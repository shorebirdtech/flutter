// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package io.flutter.embedding.engine;

import androidx.annotation.Keep;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.MalformedURLException;
import java.net.ProtocolException;
import java.net.SocketException;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import javax.net.ssl.SSLHandshakeException;
import javax.net.ssl.SSLPeerUnverifiedException;

/**
 * The Shorebird updater's HTTP client on Android, on {@link HttpURLConnection}, so updater requests
 * get what the app's own HttpURLConnection traffic gets: the app's network security config (its
 * trust anchors, including user-installed CAs where the app allows them), the system proxy, and VPN
 * routing.
 *
 * <p>Driven from native code (shell/common/shorebird/http_client_android.cc) on the updater's
 * thread: {@link #open} sends the request and reads the response head, then the caller reads the
 * body with {@link #read} and calls {@link #close}. Nothing here throws; a failure sets {@link
 * #result} and {@link #error}. Not for use by apps.
 */
@Keep
final class ShorebirdHttpClient {
  // The HttpResult values in http_client.h.
  static final int OK = 0;
  static final int TRANSPORT_ERROR = 1;
  static final int TLS_ERROR = 2;
  static final int PROTOCOL_ERROR = 3;
  static final int INVALID_REQUEST = 5;

  private static final String USER_AGENT = "Shorebird-Updater";
  private static final int MAX_REDIRECTS = 10;
  // The longest a connect or a read may take, whatever the request's overall limit.
  private static final int IDLE_TIMEOUT_MS = 60_000;

  // Ends requests that pass their overall limit. HttpURLConnection has only connect and read
  // timeouts, so a slow but steady response would otherwise never time out.
  private static final ScheduledThreadPoolExecutor deadlines = newDeadlineExecutor();

  /** Why the request failed, or {@link #OK}. */
  int result = OK;
  /** What went wrong, in the platform's words, when {@link #result} is not {@link #OK}. */
  @Nullable String error;
  /** The final response's status, once {@link #open} succeeded. */
  int status;
  /** The final response's headers, as name, value, name, value, ... */
  @Nullable String[] headers;

  @Nullable private HttpURLConnection connection;
  @Nullable private InputStream body;
  @Nullable private ScheduledFuture<?> deadline;
  private final int timeoutMs;
  private final long deadlineNanos;
  private volatile boolean timedOut = false;

  private ShorebirdHttpClient(int timeoutMs) {
    this.timeoutMs = timeoutMs;
    this.deadlineNanos = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(timeoutMs);
  }

  /**
   * Sends a request and reads the final response's status and headers, following redirects for GET
   * only, at most 10, and never from https to http.
   *
   * @param headerPairs request headers as name, value, name, value, ...
   * @param body the request body, or null for none
   * @param timeoutMs how long the whole request may take, or 0 for no limit beyond the idle timeout
   */
  @NonNull
  static ShorebirdHttpClient open(
      @NonNull String method,
      @NonNull String url,
      @NonNull String[] headerPairs,
      @Nullable byte[] body,
      int timeoutMs) {
    ShorebirdHttpClient client = new ShorebirdHttpClient(timeoutMs);
    client.send(method, url, headerPairs, body);
    return client;
  }

  /**
   * Reads the next run of body bytes, undecoded, into {@code buffer}. Returns how many, or -1 at
   * the end of the body or on failure (then {@link #result} says which).
   */
  int read(@NonNull byte[] buffer) {
    if (result != OK || body == null) {
      return -1;
    }
    try {
      int count = body.read(buffer);
      // Disconnecting at the deadline can end the stream early rather than fail the read.
      if (timedOut) {
        throw new IOException("timed out");
      }
      return count;
    } catch (IOException | RuntimeException e) {
      fail(e, true);
      return -1;
    }
  }

  /** Releases the connection. */
  void close() {
    if (deadline != null) {
      deadline.cancel(false);
    }
    HttpURLConnection current;
    synchronized (this) {
      current = connection;
    }
    if (current != null) {
      current.disconnect();
    }
  }

  private void send(String method, String url, String[] headerPairs, @Nullable byte[] requestBody) {
    if (!method.equals("GET") && !method.equals("POST")) {
      invalid("The method is not GET or POST.");
      return;
    }
    // HttpURLConnection would send a GET with a body as a POST.
    if (method.equals("GET") && requestBody != null && requestBody.length > 0) {
      invalid("A GET request has a body.");
      return;
    }
    // Replace a default whatever its case.
    Map<String, String> requestHeaders = new TreeMap<>(String.CASE_INSENSITIVE_ORDER);
    requestHeaders.put("User-Agent", USER_AGENT);
    requestHeaders.put("Accept", "*/*");
    // Bodies arrive as sent, so Content-Length counts the bytes delivered. HttpURLConnection
    // decodes gzip only when it added Accept-Encoding itself.
    requestHeaders.put("Accept-Encoding", "identity");
    for (int i = 0; i + 1 < headerPairs.length; i += 2) {
      String name = headerPairs[i];
      String value = headerPairs[i + 1];
      if (!isHeaderText(name, false)
          || !isHeaderText(value, true)
          || name.indexOf(':') >= 0
          || isReservedHeader(name)) {
        invalid("A request header is not valid.");
        return;
      }
      requestHeaders.put(name, value);
    }
    URL current;
    try {
      current = new URL(url);
    } catch (MalformedURLException e) {
      invalid("The URL is not an absolute http or https URL.");
      return;
    }
    if (!isHttpScheme(current) || current.getHost().isEmpty()) {
      invalid("The URL is not an absolute http or https URL.");
      return;
    }
    if (timeoutMs > 0) {
      deadline = deadlines.schedule(this::timeOut, timeoutMs, TimeUnit.MILLISECONDS);
    }

    boolean isGet = method.equals("GET");
    for (int redirects = 0; ; redirects++) {
      HttpURLConnection next;
      try {
        next = (HttpURLConnection) current.openConnection();
        synchronized (this) {
          connection = next;
        }
        if (timedOut) {
          next.disconnect();
          throw new IOException("timed out");
        }
        int idle = idleTimeoutMs();
        next.setConnectTimeout(idle);
        next.setReadTimeout(idle);
        // Redirects are checked here, so their rules hold on every Android version.
        next.setInstanceFollowRedirects(false);
        next.setUseCaches(false);
        next.setRequestMethod(method);
        for (Map.Entry<String, String> header : requestHeaders.entrySet()) {
          next.setRequestProperty(header.getKey(), header.getValue());
        }
        if (!isGet) {
          byte[] bytes = requestBody == null ? new byte[0] : requestBody;
          next.setDoOutput(true);
          next.setFixedLengthStreamingMode(bytes.length);
          try (OutputStream out = next.getOutputStream()) {
            out.write(bytes);
          }
        }
        status = next.getResponseCode();
        if (timedOut) {
          throw new IOException("timed out");
        }
      } catch (IOException | RuntimeException e) {
        fail(e, false);
        return;
      }
      if (status < 100 || status > 599) {
        fail(PROTOCOL_ERROR, "The response had no valid HTTP status.");
        return;
      }
      String location = next.getHeaderField("Location");
      // Nothing the updater POSTs is expected to redirect, and replaying a POST elsewhere is not
      // safe, so a POST's redirect is delivered as the response.
      if (isGet && isRedirect(status) && location != null) {
        URL target = null;
        try {
          target = new URL(current, location);
        } catch (MalformedURLException e) {
          // Rejected below.
        }
        // Never from https to plain http: the request (and the headers it carries) would leave
        // TLS.
        if (redirects >= MAX_REDIRECTS
            || target == null
            || !isHttpScheme(target)
            || target.getHost().isEmpty()
            || (isHttps(current) && !isHttps(target))) {
          next.disconnect();
          fail(
              PROTOCOL_ERROR,
              "Too many redirects, or a redirect to an unsupported or less secure URL.");
          return;
        }
        next.disconnect();
        current = target;
        continue;
      }
      headers = responseHeaders(next);
      try {
        body = status >= 400 ? next.getErrorStream() : next.getInputStream();
      } catch (IOException | RuntimeException e) {
        fail(e, true);
      }
      return;
    }
  }

  private int idleTimeoutMs() throws IOException {
    if (timeoutMs == 0) {
      return IDLE_TIMEOUT_MS;
    }
    long remaining = TimeUnit.NANOSECONDS.toMillis(deadlineNanos - System.nanoTime());
    if (remaining <= 0) {
      timedOut = true;
      throw new IOException("timed out");
    }
    return (int) Math.min(remaining, IDLE_TIMEOUT_MS);
  }

  // On the deadline thread: ends a request that is still running. Disconnecting closes the
  // socket, so a blocked connect, write or read fails at once.
  private void timeOut() {
    timedOut = true;
    HttpURLConnection current;
    synchronized (this) {
      current = connection;
    }
    if (current != null) {
      current.disconnect();
    }
  }

  private void fail(Throwable e, boolean afterResponse) {
    if (timedOut) {
      fail(TRANSPORT_ERROR, "The request timed out after " + timeoutMs + " ms.");
    } else {
      fail(resultFor(e, afterResponse), describe(e));
    }
  }

  private void fail(int result, String error) {
    if (this.result == OK) {
      this.result = result;
      this.error = error;
    }
    close();
  }

  private void invalid(String error) {
    fail(INVALID_REQUEST, error);
  }

  private static int resultFor(Throwable e, boolean afterResponse) {
    if (e instanceof SSLHandshakeException || e instanceof SSLPeerUnverifiedException) {
      // The peer closing the connection mid-handshake is a dropped connection, not a TLS failure.
      for (Throwable cause = e.getCause(); cause != null; cause = cause.getCause()) {
        if (cause instanceof SocketException || cause instanceof EOFException) {
          return TRANSPORT_ERROR;
        }
      }
      return TLS_ERROR;
    }
    // In the body, a ProtocolException is a short read ("unexpected end of stream"): the
    // connection dropped, and the updater can resume.
    if (e instanceof ProtocolException && !afterResponse) {
      return PROTOCOL_ERROR;
    }
    return TRANSPORT_ERROR;
  }

  // What went wrong, for the log: the exception and its causes, e.g.
  // "javax.net.ssl.SSLHandshakeException: ...; caused by
  // java.security.cert.CertPathValidatorException: Trust anchor for certification path not found."
  private static String describe(Throwable e) {
    StringBuilder text = new StringBuilder(e.toString());
    int depth = 0;
    for (Throwable cause = e.getCause(); cause != null && depth < 4; cause = cause.getCause()) {
      // Wrappers often repeat their cause's message.
      String message = cause.getMessage();
      if (message == null || text.indexOf(message) < 0) {
        text.append("; caused by ").append(cause);
      }
      depth++;
    }
    return text.toString();
  }

  private static String[] responseHeaders(HttpURLConnection connection) {
    List<String> pairs = new ArrayList<>();
    for (Map.Entry<String, List<String>> field : connection.getHeaderFields().entrySet()) {
      // The null key is the status line; X-Android-* are Android's own notes (timings, protocol),
      // not from the server.
      if (field.getKey() == null
          || field.getValue() == null
          || field.getKey().regionMatches(true, 0, "X-Android-", 0, 10)) {
        continue;
      }
      for (String value : field.getValue()) {
        if (value != null) {
          pairs.add(field.getKey());
          pairs.add(value);
        }
      }
    }
    return pairs.toArray(new String[0]);
  }

  private static boolean isRedirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
  }

  private static boolean isHttps(URL url) {
    return url.getProtocol().equalsIgnoreCase("https");
  }

  private static boolean isHttpScheme(URL url) {
    return isHttps(url) || url.getProtocol().equalsIgnoreCase("http");
  }

  // Whether {@code s} can be a header name or value: nonempty (for a name), with no control
  // characters, so it cannot end the header or smuggle another.
  private static boolean isHeaderText(String s, boolean allowEmpty) {
    if (s == null) {
      return false;
    }
    if (s.isEmpty()) {
      return allowEmpty;
    }
    for (int i = 0; i < s.length(); i++) {
      char c = s.charAt(i);
      if (c == '\r' || c == '\n' || c == '\0') {
        return false;
      }
    }
    return true;
  }

  // Headers the client sets itself.
  private static boolean isReservedHeader(String name) {
    for (String reserved :
        new String[] {"Host", "Content-Length", "Transfer-Encoding", "Connection"}) {
      if (reserved.equalsIgnoreCase(name)) {
        return true;
      }
    }
    return false;
  }

  private static ScheduledThreadPoolExecutor newDeadlineExecutor() {
    ScheduledThreadPoolExecutor executor =
        new ScheduledThreadPoolExecutor(
            1,
            runnable -> {
              Thread thread = new Thread(runnable, "ShorebirdHttpDeadline");
              thread.setDaemon(true);
              return thread;
            });
    executor.setRemoveOnCancelPolicy(true);
    // No thread left behind between update checks.
    executor.setKeepAliveTime(30, TimeUnit.SECONDS);
    executor.allowCoreThreadTimeOut(true);
    return executor;
  }
}
