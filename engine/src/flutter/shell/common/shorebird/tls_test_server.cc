// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Serves the TLS accept/reject table for the shorebird_http_client workflow
// on localhost, one port per case, so the table doesn't depend on third-party
// test hosts.
//
//   tls_test_server <dir>
//
// Makes a fresh test root, writes it to <dir>/root.pem, binds every case and
// then writes <dir>/cases.tsv (name, url, expected) and serves until killed.
// The workflow adds root.pem to the OS's trusted roots before probing, so
// the accepting cases go through the same platform trust lookup as real
// hosts. Expected is "accept", "reject" or "any" (no expectation beyond
// matching dart:io).
//
// Before serving, each case checks itself with an in-memory handshake
// against a permissive client that trusts only the test root: a rejecting
// case must fail for its own reason (its verify error, or the TLS version
// or cipher it insists on) and nothing else, so a broken case can't pass
// as a rejection.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "openssl/bn.h"
#include "openssl/ec_key.h"
#include "openssl/err.h"
#include "openssl/evp.h"
#include "openssl/nid.h"
#include "openssl/pem.h"
#include "openssl/rand.h"
#include "openssl/rsa.h"
#include "openssl/ssl.h"
#include "openssl/x509.h"
#include "openssl/x509v3.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using SocketHandle = int;
#endif

namespace {

constexpr long kDay = 24 * 60 * 60;

// Generation and setup failures stop the server: a case built wrong would
// otherwise be rejected for the wrong reason.
void Require(bool ok, const char* what) {
  if (!ok) {
    fprintf(stderr, "tls_test_server: %s failed\n", what);
    std::_Exit(1);
  }
}

enum class KeyType { kRsa1024, kRsa2048, kRsa4096, kRsa8192, kEc256, kEc384 };

bssl::UniquePtr<EVP_PKEY> MakeKey(KeyType type) {
  bssl::UniquePtr<EVP_PKEY> key(EVP_PKEY_new());
  if (type == KeyType::kEc256 || type == KeyType::kEc384) {
    bssl::UniquePtr<EC_KEY> ec(EC_KEY_new_by_curve_name(
        type == KeyType::kEc256 ? NID_X9_62_prime256v1 : NID_secp384r1));
    Require(EC_KEY_generate_key(ec.get()) &&
                EVP_PKEY_assign_EC_KEY(key.get(), ec.release()),
            "EC keygen");
    return key;
  }
  int bits = type == KeyType::kRsa1024   ? 1024
             : type == KeyType::kRsa4096 ? 4096
             : type == KeyType::kRsa8192 ? 8192
                                         : 2048;
  bssl::UniquePtr<RSA> rsa(RSA_new());
  bssl::UniquePtr<BIGNUM> exponent(BN_new());
  BN_set_word(exponent.get(), RSA_F4);
  Require(RSA_generate_key_ex(rsa.get(), bits, exponent.get(), nullptr) &&
              EVP_PKEY_assign_RSA(key.get(), rsa.release()),
          "RSA keygen");
  return key;
}

struct Cert {
  bssl::UniquePtr<EVP_PKEY> key;
  bssl::UniquePtr<X509> cert;
};

struct CertSpec {
  // Empty for no subject at all.
  std::string common_name;
  KeyType key = KeyType::kEc256;
  bool is_ca = false;
  std::string ca_constraints = "critical,CA:TRUE";
  // A leaf's key usage.
  std::string key_usage = "critical,digitalSignature,keyEncipherment";
  // Comma-separated, in X509V3 config syntax; empty for none.
  std::string subject_alt_names;
  std::string extended_key_usage = "serverAuth";
  // Validity, in seconds from now.
  long not_before = -kDay;
  long not_after = kDay;
  const EVP_MD* (*digest)() = EVP_sha256;
};

void AddExtension(X509* cert, X509V3_CTX* context, int nid, const char* value) {
  bssl::UniquePtr<X509_EXTENSION> extension(
      X509V3_EXT_nconf_nid(nullptr, context, nid, value));
  Require(extension && X509_add_ext(cert, extension.get(), -1), value);
}

// Signed by `issuer`, or self-signed when it's null.
Cert MakeCert(const CertSpec& spec, const Cert* issuer) {
  Cert result;
  result.key = MakeKey(spec.key);
  result.cert.reset(X509_new());
  X509* cert = result.cert.get();
  X509_set_version(cert, X509_VERSION_3);
  uint64_t serial;
  RAND_bytes(reinterpret_cast<uint8_t*>(&serial), sizeof(serial));
  ASN1_INTEGER_set_uint64(X509_get_serialNumber(cert), serial >> 1);
  X509_gmtime_adj(X509_getm_notBefore(cert), spec.not_before);
  X509_gmtime_adj(X509_getm_notAfter(cert), spec.not_after);
  if (!spec.common_name.empty()) {
    X509_NAME_add_entry_by_txt(
        X509_get_subject_name(cert), "CN", MBSTRING_ASC,
        reinterpret_cast<const uint8_t*>(spec.common_name.c_str()), -1, -1, 0);
  }
  X509* issuer_cert = issuer ? issuer->cert.get() : cert;
  EVP_PKEY* issuer_key = issuer ? issuer->key.get() : result.key.get();
  X509_set_issuer_name(cert, X509_get_subject_name(issuer_cert));
  X509_set_pubkey(cert, result.key.get());

  X509V3_CTX context;
  X509V3_set_ctx(&context, issuer_cert, cert, nullptr, nullptr, 0);
  AddExtension(cert, &context, NID_subject_key_identifier, "hash");
  if (issuer) {
    AddExtension(cert, &context, NID_authority_key_identifier, "keyid");
  }
  if (spec.is_ca) {
    AddExtension(cert, &context, NID_basic_constraints,
                 spec.ca_constraints.c_str());
    AddExtension(cert, &context, NID_key_usage,
                 "critical,digitalSignature,keyCertSign,cRLSign");
  } else {
    AddExtension(cert, &context, NID_basic_constraints, "critical,CA:FALSE");
    AddExtension(cert, &context, NID_key_usage, spec.key_usage.c_str());
    AddExtension(cert, &context, NID_ext_key_usage,
                 spec.extended_key_usage.c_str());
  }
  if (!spec.subject_alt_names.empty()) {
    // Critical when there's no subject, as RFC 5280 requires.
    std::string names =
        (spec.common_name.empty() ? "critical," : "") + spec.subject_alt_names;
    AddExtension(cert, &context, NID_subject_alt_name, names.c_str());
  }
  Require(X509_sign(cert, issuer_key, spec.digest()) != 0, "X509_sign");
  return result;
}

CertSpec CaSpec(const char* name) {
  CertSpec spec;
  spec.common_name = name;
  spec.is_ca = true;
  spec.not_after = 30 * kDay;
  return spec;
}

// What a case's self-check expects of the permissive client.
struct Check {
  // The client's verify result; kSkip for an "any" case.
  long verify = X509_V_OK;
  // The negotiated version, or 0 for any.
  uint16_t version = 0;
  bool triple_des = false;
};
constexpr long kSkip = -1;

CertSpec LeafSpec(KeyType key = KeyType::kEc256) {
  CertSpec spec;
  spec.common_name = "localhost";
  spec.subject_alt_names = "DNS:localhost";
  spec.key = key;
  return spec;
}

struct Case {
  std::string name;
  std::string expected;
  std::string host;
  bssl::UniquePtr<SSL_CTX> context;  // Null for plain http.
  Check check;
  std::vector<SocketHandle> listeners;
  int port = 0;
};

// A server context presenting `leaf`, followed by `chain`.
bssl::UniquePtr<SSL_CTX> Context(const Cert& leaf,
                                 const std::vector<const Cert*>& chain) {
  bssl::UniquePtr<SSL_CTX> context(SSL_CTX_new(TLS_server_method()));
  Require(context && SSL_CTX_use_certificate(context.get(), leaf.cert.get()) &&
              SSL_CTX_use_PrivateKey(context.get(), leaf.key.get()),
          "server context");
  for (const Cert* cert : chain) {
    Require(SSL_CTX_add1_chain_cert(context.get(), cert->cert.get()),
            "chain cert");
  }
  return context;
}

bssl::UniquePtr<SSL_CTX> Versions(bssl::UniquePtr<SSL_CTX> context,
                                  uint16_t min,
                                  uint16_t max) {
  Require(SSL_CTX_set_min_proto_version(context.get(), min) &&
              SSL_CTX_set_max_proto_version(context.get(), max),
          "TLS versions");
  return context;
}

// Handshakes with `c` in memory, as a client that trusts only `root`,
// accepts TLS 1.0 and up and every cipher BoringSSL has, and checks the
// result against `c.check`. Returns an error, or empty.
std::string SelfCheck(const Case& c, X509* root) {
  bssl::UniquePtr<SSL_CTX> client_context(SSL_CTX_new(TLS_client_method()));
  Require(SSL_CTX_set_min_proto_version(client_context.get(), TLS1_VERSION) &&
              SSL_CTX_set_cipher_list(client_context.get(), "ALL:3DES") &&
              X509_STORE_add_cert(SSL_CTX_get_cert_store(client_context.get()),
                                  root),
          "client context");
  SSL_CTX_set_verify(client_context.get(), SSL_VERIFY_PEER, nullptr);
  bssl::UniquePtr<SSL> client(SSL_new(client_context.get()));
  bssl::UniquePtr<SSL> server(SSL_new(c.context.get()));
  X509_VERIFY_PARAM* param = SSL_get0_param(client.get());
  Require(
      c.host == "localhost"
          ? X509_VERIFY_PARAM_set1_host(param, c.host.c_str(), c.host.size())
          : X509_VERIFY_PARAM_set1_ip_asc(param, c.host.c_str()),
      "client host");
  BIO* client_bio;
  BIO* server_bio;
  Require(BIO_new_bio_pair(&client_bio, 0, &server_bio, 0), "BIO pair");
  SSL_set_bio(client.get(), client_bio, client_bio);
  SSL_set_bio(server.get(), server_bio, server_bio);
  SSL_set_connect_state(client.get());
  SSL_set_accept_state(server.get());
  int client_result = -1;
  int server_result = -1;
  for (int i = 0; i < 100 && (client_result != 1 || server_result != 1); i++) {
    if (client_result != 1) {
      client_result = SSL_do_handshake(client.get());
      if (client_result != 1 &&
          SSL_get_error(client.get(), client_result) != SSL_ERROR_WANT_READ) {
        break;
      }
    }
    if (server_result != 1) {
      server_result = SSL_do_handshake(server.get());
      if (server_result != 1 &&
          SSL_get_error(server.get(), server_result) != SSL_ERROR_WANT_READ) {
        break;
      }
    }
  }
  ERR_clear_error();
  long verify = SSL_get_verify_result(client.get());
  bool connected = client_result == 1 && server_result == 1;
  if (verify != c.check.verify) {
    return std::string("verify result ") +
           X509_verify_cert_error_string(verify) + ", expected " +
           X509_verify_cert_error_string(c.check.verify);
  }
  if (connected != (verify == X509_V_OK)) {
    return connected ? "connected despite the verify error"
                     : "handshake failed";
  }
  if (c.check.version != 0 && SSL_version(client.get()) != c.check.version) {
    return "negotiated the wrong TLS version";
  }
  if (c.check.triple_des && SSL_CIPHER_get_cipher_nid(SSL_get_current_cipher(
                                client.get())) != NID_des_ede3_cbc) {
    return "negotiated a cipher other than 3DES";
  }
  return "";
}

void CloseSocket(SocketHandle socket) {
#if defined(_WIN32)
  closesocket(socket);
#else
  close(socket);
#endif
}

// Listens on 127.0.0.1 and, where the OS has it, on ::1 at the same port:
// "localhost" usually resolves to ::1 first, and a refused connect there
// costs a client seconds on Windows.
bool Listen(Case& c) {
  SocketHandle v4 = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(v4, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(v4, 16) != 0) {
    return false;
  }
  socklen_t length = sizeof(address);
  getsockname(v4, reinterpret_cast<sockaddr*>(&address), &length);
  c.port = ntohs(address.sin_port);
  c.listeners.push_back(v4);

  SocketHandle v6 = socket(AF_INET6, SOCK_STREAM, 0);
  sockaddr_in6 address6 = {};
  address6.sin6_family = AF_INET6;
  address6.sin6_addr = in6addr_loopback;
  address6.sin6_port = htons(static_cast<uint16_t>(c.port));
  if (bind(v6, reinterpret_cast<sockaddr*>(&address6), sizeof(address6)) == 0 &&
      listen(v6, 16) == 0) {
    c.listeners.push_back(v6);
  } else {
    CloseSocket(v6);
  }
  return true;
}

// Answers every request with a 200 and a two-byte body. One connection at a
// time per case; a client that stalls is dropped after a few seconds.
void Serve(const Case& c, SocketHandle listener) {
  const char kResponse[] =
      "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
  while (true) {
    SocketHandle connection = accept(listener, nullptr, nullptr);
#if defined(_WIN32)
    if (connection == INVALID_SOCKET) {
      return;
    }
    DWORD timeout = 5000;
#else
    if (connection < 0) {
      return;
    }
    timeval timeout = {5, 0};
#endif
    setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    bssl::UniquePtr<SSL> ssl;
    if (c.context) {
      ssl.reset(SSL_new(c.context.get()));
      SSL_set_fd(ssl.get(), static_cast<int>(connection));
      if (SSL_accept(ssl.get()) != 1) {
        ERR_clear_error();
        CloseSocket(connection);
        continue;
      }
    }
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos) {
      int n =
          ssl ? SSL_read(ssl.get(), buffer, sizeof(buffer))
              : static_cast<int>(recv(connection, buffer, sizeof(buffer), 0));
      if (n <= 0) {
        break;
      }
      request.append(buffer, n);
    }
    if (request.find("\r\n\r\n") != std::string::npos) {
      if (ssl) {
        SSL_write(ssl.get(), kResponse, sizeof(kResponse) - 1);
        SSL_shutdown(ssl.get());
      } else {
        send(connection, kResponse, sizeof(kResponse) - 1, 0);
      }
    }
    ERR_clear_error();
    CloseSocket(connection);
  }
}

bool WriteFile(const std::string& path, const std::string& contents) {
  FILE* file = fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  bool ok =
      fwrite(contents.data(), 1, contents.size(), file) == contents.size();
  return fclose(file) == 0 && ok;
}

std::string Pem(X509* cert) {
  bssl::UniquePtr<BIO> bio(BIO_new(BIO_s_mem()));
  PEM_write_bio_X509(bio.get(), cert);
  const uint8_t* data;
  size_t length;
  BIO_mem_contents(bio.get(), &data, &length);
  return std::string(reinterpret_cast<const char*>(data), length);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: tls_test_server <dir>\n");
    return 2;
  }
  std::string dir = argv[1];
#if defined(_WIN32)
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

  Cert root = MakeCert(CaSpec("Shorebird TLS Table Test Root"), nullptr);
  Cert intermediate =
      MakeCert(CaSpec("Shorebird TLS Table Test Intermediate"), &root);
  Cert other_root = MakeCert(CaSpec("Untrusted Test Root"), nullptr);
  CertSpec sha1_spec = CaSpec("SHA-1 Test Intermediate");
  sha1_spec.digest = EVP_sha1;
  Cert sha1_intermediate = MakeCert(sha1_spec, &root);
  CertSpec expired_spec = CaSpec("Expired Test Intermediate");
  expired_spec.not_before = -2 * kDay;
  expired_spec.not_after = -kDay;
  Cert expired_intermediate = MakeCert(expired_spec, &root);
  CertSpec no_sub_ca_spec = CaSpec("Path Length 0 Test Intermediate");
  no_sub_ca_spec.ca_constraints = "critical,CA:TRUE,pathlen:0";
  Cert no_sub_ca = MakeCert(no_sub_ca_spec, &root);
  Cert sub_ca = MakeCert(CaSpec("Too Deep Test Intermediate"), &no_sub_ca);

  auto leaf = [&](const CertSpec& spec) {
    return MakeCert(spec, &intermediate);
  };

  std::vector<Case> cases;
  auto add = [&](const char* name, const char* expected,
                 bssl::UniquePtr<SSL_CTX> context, Check check = {},
                 const char* host = "localhost") {
    Case c;
    c.name = name;
    c.expected = expected;
    c.host = host;
    c.context = std::move(context);
    c.check = std::move(check);
    if (c.expected == "any") {
      c.check.verify = kSkip;
    }
    cases.push_back(std::move(c));
  };
  auto rejects = [](long verify) {
    Check check;
    check.verify = verify;
    return check;
  };

  Cert good = leaf(LeafSpec());
  add("ecc256", "accept", Context(good, {&intermediate}));
  add("ecc384", "accept",
      Context(leaf(LeafSpec(KeyType::kEc384)), {&intermediate}));
  add("rsa2048", "accept",
      Context(leaf(LeafSpec(KeyType::kRsa2048)), {&intermediate}));
  add("rsa4096", "accept",
      Context(leaf(LeafSpec(KeyType::kRsa4096)), {&intermediate}));
  add("rsa8192", "any",
      Context(leaf(LeafSpec(KeyType::kRsa8192)), {&intermediate}));
  add("rsa1024", "any",
      Context(leaf(LeafSpec(KeyType::kRsa1024)), {&intermediate}));

  CertSpec many_names = LeafSpec();
  for (int i = 0; i < 1000; i++) {
    many_names.subject_alt_names += ",DNS:n" + std::to_string(i) + ".test";
  }
  add("1000-sans", "accept", Context(leaf(many_names), {&intermediate}));

  CertSpec no_subject = LeafSpec();
  no_subject.common_name.clear();
  add("no-subject", "any", Context(leaf(no_subject), {&intermediate}));

  CertSpec ip_names = LeafSpec();
  ip_names.subject_alt_names = "IP:127.0.0.1";
  add("ip-in-san", "accept", Context(leaf(ip_names), {&intermediate}), {},
      "127.0.0.1");
  add("ip-not-in-san", "reject", Context(good, {&intermediate}),
      rejects(X509_V_ERR_IP_ADDRESS_MISMATCH), "127.0.0.1");

  // TLS versions and ciphers: the client requires TLS 1.2 or later and
  // offers no 3DES, RC4, NULL or DHE suites, so a server that insists on
  // one of those has nothing in common with it.
  Check tls12;
  tls12.version = TLS1_2_VERSION;
  add("tls-v1-2", "accept",
      Versions(Context(good, {&intermediate}), TLS1_VERSION, TLS1_2_VERSION),
      tls12);
  Check tls13;
  tls13.version = TLS1_3_VERSION;
  add("tls-v1-3", "accept",
      Versions(Context(good, {&intermediate}), TLS1_3_VERSION, TLS1_3_VERSION),
      tls13);
  Check tls10;
  tls10.version = TLS1_VERSION;
  add("tls-v1-0", "reject",
      Versions(Context(good, {&intermediate}), TLS1_VERSION, TLS1_VERSION),
      tls10);
  Check tls11;
  tls11.version = TLS1_1_VERSION;
  add("tls-v1-1", "reject",
      Versions(Context(good, {&intermediate}), TLS1_1_VERSION, TLS1_1_VERSION),
      tls11);
  auto triple_des =
      Versions(Context(leaf(LeafSpec(KeyType::kRsa2048)), {&intermediate}),
               TLS1_VERSION, TLS1_2_VERSION);
  Require(SSL_CTX_set_strict_cipher_list(triple_des.get(), "3DES"),
          "3DES cipher list");
  Check only_3des;
  only_3des.triple_des = true;
  add("3des", "reject", std::move(triple_des), only_3des);

  // Certificates that must not be trusted.
  CertSpec expired = LeafSpec();
  expired.not_before = -2 * kDay;
  expired.not_after = -kDay;
  add("expired", "reject", Context(leaf(expired), {&intermediate}),
      rejects(X509_V_ERR_CERT_HAS_EXPIRED));
  CertSpec not_yet_valid = LeafSpec();
  not_yet_valid.not_before = kDay;
  not_yet_valid.not_after = 2 * kDay;
  add("not-yet-valid", "reject", Context(leaf(not_yet_valid), {&intermediate}),
      rejects(X509_V_ERR_CERT_NOT_YET_VALID));
  add("expired-intermediate", "reject",
      Context(MakeCert(LeafSpec(), &expired_intermediate),
              {&expired_intermediate}),
      rejects(X509_V_ERR_CERT_HAS_EXPIRED));
  CertSpec wrong_host = LeafSpec();
  wrong_host.common_name = "wrong.host.test";
  wrong_host.subject_alt_names = "DNS:wrong.host.test";
  add("wrong-host", "reject", Context(leaf(wrong_host), {&intermediate}),
      rejects(X509_V_ERR_HOSTNAME_MISMATCH));
  CertSpec name_in_cn_only = LeafSpec();
  name_in_cn_only.subject_alt_names.clear();
  add("cn-only", "any", Context(leaf(name_in_cn_only), {&intermediate}));
  CertSpec client_auth = LeafSpec();
  client_auth.extended_key_usage = "clientAuth";
  add("client-auth-only", "reject", Context(leaf(client_auth), {&intermediate}),
      rejects(X509_V_ERR_INVALID_PURPOSE));
  // Shaped like `openssl req -x509`'s: a CA certificate for the host.
  CertSpec self_signed = LeafSpec();
  self_signed.is_ca = true;
  add("self-signed", "reject", Context(MakeCert(self_signed, nullptr), {}),
      rejects(X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT));
  add("untrusted-root", "reject",
      Context(MakeCert(LeafSpec(), &other_root), {&other_root}),
      rejects(X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN));
  add("incomplete-chain", "reject", Context(good, {}),
      rejects(X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY));
  // An issuer that may sign certificates but isn't a CA.
  CertSpec signing_leaf = LeafSpec();
  signing_leaf.key_usage = "critical,digitalSignature,keyCertSign";
  Cert not_a_ca = leaf(signing_leaf);
  add("leaf-signs-leaf", "reject",
      Context(MakeCert(LeafSpec(), &not_a_ca), {&not_a_ca, &intermediate}),
      rejects(X509_V_ERR_INVALID_CA));
  add("path-too-long", "reject",
      Context(MakeCert(LeafSpec(), &sub_ca), {&sub_ca, &no_sub_ca}),
      rejects(X509_V_ERR_PATH_LENGTH_EXCEEDED));
  add("sha1-intermediate", "any",
      Context(MakeCert(LeafSpec(), &sha1_intermediate), {&sha1_intermediate}));

  add("http", "accept", nullptr);

  bool broken = false;
  for (const Case& c : cases) {
    if (c.context && c.check.verify != kSkip) {
      std::string error = SelfCheck(c, root.cert.get());
      if (!error.empty()) {
        fprintf(stderr, "tls_test_server: case %s: %s\n", c.name.c_str(),
                error.c_str());
        broken = true;
      }
    }
  }
  if (broken) {
    return 1;
  }

  if (!WriteFile(dir + "/root.pem", Pem(root.cert.get()))) {
    fprintf(stderr, "can't write %s/root.pem\n", dir.c_str());
    return 1;
  }
  std::string table;
  for (Case& c : cases) {
    if (!Listen(c)) {
      fprintf(stderr, "can't listen for %s\n", c.name.c_str());
      return 1;
    }
    table += c.name + "\t" + (c.context ? "https" : "http") + "://" + c.host +
             ":" + std::to_string(c.port) + "/\t" + c.expected + "\n";
  }
  // Before serving: connections wait in the listen backlog meanwhile.
  if (!WriteFile(dir + "/cases.tsv.tmp", table) ||
      rename((dir + "/cases.tsv.tmp").c_str(), (dir + "/cases.tsv").c_str()) !=
          0) {
    fprintf(stderr, "can't write %s/cases.tsv\n", dir.c_str());
    return 1;
  }
  printf("%s", table.c_str());
  fflush(stdout);
  std::vector<std::thread> threads;
  for (const Case& c : cases) {
    for (SocketHandle listener : c.listeners) {
      threads.emplace_back(Serve, std::cref(c), listener);
    }
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  return 0;
}
