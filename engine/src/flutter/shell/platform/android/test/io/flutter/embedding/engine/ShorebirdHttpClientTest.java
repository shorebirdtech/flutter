// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package io.flutter.embedding.engine;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.util.concurrent.Executors;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public class ShorebirdHttpClientTest {
  private static final String[] NO_HEADERS = new String[0];

  private HttpServer server;
  private String base;

  @Before
  public void setUp() throws IOException {
    server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
    base = "http://127.0.0.1:" + server.getAddress().getPort();
    // Echoes the request: method, Accept-Encoding, Range and body length.
    server.createContext(
        "/echo",
        exchange -> {
          int length = exchange.getRequestBody().readAllBytes().length;
          String range = exchange.getRequestHeaders().getFirst("Range");
          exchange.getResponseHeaders().add("X-Method", exchange.getRequestMethod());
          exchange
              .getResponseHeaders()
              .add("X-Accept-Encoding", exchange.getRequestHeaders().getFirst("Accept-Encoding"));
          exchange.getResponseHeaders().add("X-Range", String.valueOf(range));
          exchange.getResponseHeaders().add("X-Body-Length", String.valueOf(length));
          respond(exchange, range == null ? 200 : 206, new byte[5000]);
        });
    server.createContext(
        "/redirect",
        exchange -> {
          exchange.getResponseHeaders().add("Location", "/echo");
          respond(exchange, 302, new byte[0]);
        });
    server.createContext(
        "/loop",
        exchange -> {
          exchange.getResponseHeaders().add("Location", "/loop");
          respond(exchange, 302, new byte[0]);
        });
    server.createContext(
        "/to-https",
        exchange -> {
          exchange.getResponseHeaders().add("Location", "https://127.0.0.1:1/");
          respond(exchange, 302, new byte[0]);
        });
    server.createContext("/missing", exchange -> respond(exchange, 404, "nope".getBytes()));
    server.createContext(
        "/slow",
        exchange -> {
          exchange.sendResponseHeaders(200, 0);
          OutputStream out = exchange.getResponseBody();
          try {
            for (int i = 0; i < 50; i++) {
              out.write(new byte[10]);
              out.flush();
              Thread.sleep(100);
            }
          } catch (IOException | InterruptedException e) {
            // The client hung up.
          }
          exchange.close();
        });
    server.setExecutor(Executors.newCachedThreadPool());
    server.start();
  }

  @After
  public void tearDown() {
    server.stop(0);
  }

  private static void respond(HttpExchange exchange, int status, byte[] body) throws IOException {
    exchange.sendResponseHeaders(status, body.length == 0 ? -1 : body.length);
    if (body.length > 0) {
      exchange.getResponseBody().write(body);
    }
    exchange.close();
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
    assertEquals(ShorebirdHttpClient.TRANSPORT_ERROR, get("/to-https").result);
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
