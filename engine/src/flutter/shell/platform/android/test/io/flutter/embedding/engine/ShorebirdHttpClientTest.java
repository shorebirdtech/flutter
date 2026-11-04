// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package io.flutter.embedding.engine;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public class ShorebirdHttpClientTest {
  private static final String[] NO_HEADERS = new String[0];

  // A minimal HTTP/1.1 server on a ServerSocket: the unit tests compile against
  // android.jar, which has no com.sun.net.httpserver. One request per
  // connection, answered with Connection: close.
  private ServerSocket server;
  private ExecutorService executor;
  private String base;

  @Before
  public void setUp() throws IOException {
    server = new ServerSocket(0, 50, InetAddress.getByName("127.0.0.1"));
    base = "http://127.0.0.1:" + server.getLocalPort();
    executor = Executors.newCachedThreadPool();
    executor.execute(
        () -> {
          while (!server.isClosed()) {
            try {
              Socket socket = server.accept();
              executor.execute(() -> serve(socket));
            } catch (IOException | RuntimeException e) {
              return; // Closed or shut down by tearDown.
            }
          }
        });
  }

  @After
  public void tearDown() throws IOException {
    server.close();
    executor.shutdownNow();
  }

  private static void serve(Socket socket) {
    try (Socket s = socket) {
      InputStream in = new BufferedInputStream(s.getInputStream());
      String[] requestLine = readLine(in).split(" ");
      Map<String, String> headers = new HashMap<>();
      String line;
      while (!(line = readLine(in)).isEmpty()) {
        int colon = line.indexOf(':');
        String name = line.substring(0, colon).trim().toLowerCase(Locale.ROOT);
        headers.put(name, line.substring(colon + 1).trim());
      }
      int length = Integer.parseInt(headers.getOrDefault("content-length", "0"));
      for (int i = 0; i < length; i++) {
        in.read();
      }
      route(requestLine[0], requestLine[1], headers, length, s.getOutputStream());
    } catch (IOException | RuntimeException e) {
      // The client hung up, or sent something the test doesn't expect.
    }
  }

  private static void route(
      String method, String path, Map<String, String> headers, int length, OutputStream out)
      throws IOException {
    switch (path) {
      case "/echo":
        // Echoes the request: method, Accept-Encoding, Range and body length.
        String range = headers.get("range");
        respond(
            out,
            range == null ? 200 : 206,
            new byte[5000],
            "X-Method: " + method,
            "X-Accept-Encoding: " + headers.get("accept-encoding"),
            "X-Range: " + range,
            "X-Body-Length: " + length);
        break;
      case "/redirect":
        respond(out, 302, new byte[0], "Location: /echo");
        break;
      case "/loop":
        respond(out, 302, new byte[0], "Location: /loop");
        break;
      case "/to-https":
        respond(out, 302, new byte[0], "Location: https://127.0.0.1:1/");
        break;
      case "/missing":
        respond(out, 404, "nope".getBytes(StandardCharsets.UTF_8));
        break;
      case "/slow":
        // Chunked: a chunked body ends cleanly when the client's deadline
        // closes it, so this fails if the client doesn't report that as a
        // timeout. A body that runs until close makes the read throw instead.
        String head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
        out.write(head.getBytes(StandardCharsets.UTF_8));
        byte[] chunk = new byte[15];
        System.arraycopy("a\r\n".getBytes(StandardCharsets.UTF_8), 0, chunk, 0, 3);
        chunk[13] = '\r';
        chunk[14] = '\n';
        try {
          for (int i = 0; i < 50; i++) {
            out.write(chunk);
            out.flush();
            Thread.sleep(100);
          }
          out.write("0\r\n\r\n".getBytes(StandardCharsets.UTF_8));
          out.flush();
        } catch (InterruptedException e) {
          // Shut down by tearDown.
        }
        break;
      default:
        respond(out, 404, new byte[0]);
    }
  }

  private static void respond(OutputStream out, int status, byte[] body, String... headers)
      throws IOException {
    StringBuilder head = new StringBuilder("HTTP/1.1 " + status + " X\r\n");
    for (String header : headers) {
      head.append(header).append("\r\n");
    }
    head.append("Content-Length: ").append(body.length).append("\r\n");
    head.append("Connection: close\r\n\r\n");
    out.write(head.toString().getBytes(StandardCharsets.UTF_8));
    out.write(body);
    out.flush();
  }

  private static String readLine(InputStream in) throws IOException {
    ByteArrayOutputStream line = new ByteArrayOutputStream();
    int c;
    while ((c = in.read()) != '\n') {
      if (c < 0) {
        throw new IOException("connection closed mid-request");
      }
      if (c != '\r') {
        line.write(c);
      }
    }
    return line.toString("UTF-8");
  }

  private static String header(ShorebirdHttpClient client, String name) {
    for (int i = 0; i + 1 < client.headers.length; i += 2) {
      if (client.headers[i].equalsIgnoreCase(name)) {
        return client.headers[i + 1];
      }
    }
    return null;
  }

  private static int readBody(ShorebirdHttpClient client) {
    byte[] buffer = new byte[1024];
    int total = 0;
    int count;
    while ((count = client.read(buffer)) >= 0) {
      total += count;
    }
    client.close();
    return total;
  }

  private ShorebirdHttpClient get(String path, String... headers) {
    return ShorebirdHttpClient.open("GET", base + path, headers, null, 0);
  }

  @Test
  public void getDeliversStatusHeadersAndBody() {
    ShorebirdHttpClient client = get("/echo");
    assertEquals(ShorebirdHttpClient.OK, client.result);
    assertEquals(200, client.status);
    assertEquals("identity", header(client, "X-Accept-Encoding"));
    assertEquals(5000, readBody(client));
    assertEquals(ShorebirdHttpClient.OK, client.result);
  }

  @Test
  public void rangeIsSentAndPartialContentDelivered() {
    ShorebirdHttpClient client = get("/echo", "Range", "bytes=10-");
    assertEquals(206, client.status);
    assertEquals("bytes=10-", header(client, "X-Range"));
    readBody(client);
  }

  @Test
  public void postSendsItsBody() {
    ShorebirdHttpClient client =
        ShorebirdHttpClient.open("POST", base + "/echo", NO_HEADERS, new byte[7], 30000);
    assertEquals(200, client.status);
    assertEquals("POST", header(client, "X-Method"));
    assertEquals("7", header(client, "X-Body-Length"));
    readBody(client);
  }

  @Test
  public void getFollowsRedirects() {
    ShorebirdHttpClient client = get("/redirect");
    assertEquals(200, client.status);
    assertEquals("GET", header(client, "X-Method"));
    readBody(client);
  }

  @Test
  public void postDeliversRedirectAsTheResponse() {
    ShorebirdHttpClient client =
        ShorebirdHttpClient.open("POST", base + "/redirect", NO_HEADERS, new byte[1], 0);
    assertEquals(ShorebirdHttpClient.OK, client.result);
    assertEquals(302, client.status);
    readBody(client);
  }

  @Test
  public void redirectLoopIsAProtocolError() {
    assertEquals(ShorebirdHttpClient.PROTOCOL_ERROR, get("/loop").result);
  }

  @Test
  public void httpToHttpsRedirectIsFollowed() {
    // Followed to a closed https port, so it fails connecting rather than on the redirect.
    ShorebirdHttpClient client = get("/to-https");
    assertEquals(ShorebirdHttpClient.TRANSPORT_ERROR, client.result);
    // A refused connect proves the redirect was followed; a dropped /to-https
    // response would be an unexpected end of stream instead.
    assertTrue(client.error, client.error.contains("ConnectException"));
  }

  @Test
  public void errorStatusDeliversItsBody() {
    ShorebirdHttpClient client = get("/missing");
    assertEquals(404, client.status);
    assertEquals(4, readBody(client));
  }

  @Test
  public void timeoutEndsASlowResponse() {
    long start = System.nanoTime();
    ShorebirdHttpClient client =
        ShorebirdHttpClient.open("GET", base + "/slow", NO_HEADERS, null, 500);
    readBody(client);
    long elapsedMs = (System.nanoTime() - start) / 1_000_000;
    assertEquals(ShorebirdHttpClient.TRANSPORT_ERROR, client.result);
    assertTrue(client.error, client.error.contains("timed out"));
    assertTrue("took " + elapsedMs + " ms", elapsedMs < 3000);
  }

  @Test
  public void rejectsUnusableRequests() {
    assertEquals(
        ShorebirdHttpClient.INVALID_REQUEST,
        ShorebirdHttpClient.open("PUT", base + "/echo", NO_HEADERS, null, 0).result);
    assertEquals(
        ShorebirdHttpClient.INVALID_REQUEST,
        ShorebirdHttpClient.open("GET", base + "/echo", NO_HEADERS, new byte[1], 0).result);
    assertEquals(
        ShorebirdHttpClient.INVALID_REQUEST,
        ShorebirdHttpClient.open("GET", "ftp://example.com/", NO_HEADERS, null, 0).result);
    assertEquals(
        ShorebirdHttpClient.INVALID_REQUEST,
        ShorebirdHttpClient.open("GET", "not a url", NO_HEADERS, null, 0).result);
    assertEquals(ShorebirdHttpClient.INVALID_REQUEST, get("/echo", "X-A", "b\r\nX-B: c").result);
    assertEquals(ShorebirdHttpClient.INVALID_REQUEST, get("/echo", "Host", "elsewhere").result);
  }

  @Test
  public void closedPortIsATransportError() {
    ShorebirdHttpClient client =
        ShorebirdHttpClient.open("GET", "http://127.0.0.1:1/", NO_HEADERS, null, 0);
    assertEquals(ShorebirdHttpClient.TRANSPORT_ERROR, client.result);
    assertTrue(client.error != null && !client.error.isEmpty());
  }
}
