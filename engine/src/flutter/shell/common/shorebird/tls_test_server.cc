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

#include <cstdio>
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

enum class KeyType { kRsa1024, kRsa2048, kRsa4096, kRsa8192, kEc256, kEc384 };

bssl::UniquePtr<EVP_PKEY> MakeKey(KeyType type) {
  bssl::UniquePtr<EVP_PKEY> key(EVP_PKEY_new());
  if (type == KeyType::kEc256 || type == KeyType::kEc384) {
    bssl::UniquePtr<EC_KEY> ec(EC_KEY_new_by_curve_name(
        type == KeyType::kEc256 ? NID_X9_62_prime256v1 : NID_secp384r1));
    EC_KEY_generate_key(ec.get());
    EVP_PKEY_assign_EC_KEY(key.get(), ec.release());
    return key;
  }
  int bits = type == KeyType::kRsa1024   ? 1024
             : type == KeyType::kRsa4096 ? 4096
             : type == KeyType::kRsa8192 ? 8192
                                         : 2048;
  bssl::UniquePtr<RSA> rsa(RSA_new());
  bssl::UniquePtr<BIGNUM> exponent(BN_new());
  BN_set_word(exponent.get(), RSA_F4);
  RSA_generate_key_ex(rsa.get(), bits, exponent.get(), nullptr);
  EVP_PKEY_assign_RSA(key.get(), rsa.release());
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
  X509_add_ext(cert, extension.get(), -1);
}

// Signed by `issuer`, or self-signed when it's null.
Cert MakeCert(const CertSpec& spec, const Cert* issuer) {
  static long serial = 1;
  Cert result;
  result.key = MakeKey(spec.key);
  result.cert.reset(X509_new());
  X509* cert = result.cert.get();
  X509_set_version(cert, X509_VERSION_3);
  ASN1_INTEGER_set(X509_get_serialNumber(cert), serial++);
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
    AddExtension(cert, &context, NID_basic_constraints, "critical,CA:TRUE");
    AddExtension(cert, &context, NID_key_usage, "critical,keyCertSign,cRLSign");
  } else {
    AddExtension(cert, &context, NID_basic_constraints, "critical,CA:FALSE");
    AddExtension(cert, &context, NID_key_usage,
                 "critical,digitalSignature,keyEncipherment");
    AddExtension(cert, &context, NID_ext_key_usage,
                 spec.extended_key_usage.c_str());
  }
  if (!spec.subject_alt_names.empty()) {
    AddExtension(cert, &context, NID_subject_alt_name,
                 spec.subject_alt_names.c_str());
  }
  X509_sign(cert, issuer_key, spec.digest());
  return result;
}

CertSpec CaSpec(const char* name) {
  CertSpec spec;
  spec.common_name = name;
  spec.is_ca = true;
  spec.not_after = 30 * kDay;
  return spec;
}

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
  bssl::UniquePtr<SSL_CTX> context;  // Null for plain http.
  SocketHandle listener;
  int port = 0;
};

// A server context presenting `leaf`, followed by `chain`.
bssl::UniquePtr<SSL_CTX> Context(const Cert& leaf,
                                 const std::vector<const Cert*>& chain) {
  bssl::UniquePtr<SSL_CTX> context(SSL_CTX_new(TLS_server_method()));
  SSL_CTX_use_certificate(context.get(), leaf.cert.get());
  SSL_CTX_use_PrivateKey(context.get(), leaf.key.get());
  for (const Cert* cert : chain) {
    SSL_CTX_add1_chain_cert(context.get(), cert->cert.get());
  }
  return context;
}

void CloseSocket(SocketHandle socket) {
#if defined(_WIN32)
  closesocket(socket);
#else
  close(socket);
#endif
}

bool Listen(Case& c) {
  c.listener = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(c.listener, reinterpret_cast<sockaddr*>(&address),
           sizeof(address)) != 0 ||
      listen(c.listener, 16) != 0) {
    return false;
  }
  socklen_t length = sizeof(address);
  getsockname(c.listener, reinterpret_cast<sockaddr*>(&address), &length);
  c.port = ntohs(address.sin_port);
  return true;
}

// Answers every request with a 200 and a two-byte body. One connection at a
// time per case; a client that stalls is dropped after a few seconds.
void Serve(const Case& c) {
  const char kResponse[] =
      "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
  while (true) {
    SocketHandle connection = accept(c.listener, nullptr, nullptr);
#if defined(_WIN32)
    if (connection == INVALID_SOCKET) {
      continue;
    }
    DWORD timeout = 5000;
#else
    if (connection < 0) {
      continue;
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

  auto leaf = [&](const CertSpec& spec) {
    return MakeCert(spec, &intermediate);
  };

  std::vector<Case> cases;
  auto add = [&](const char* name, const char* expected,
                 bssl::UniquePtr<SSL_CTX> context) {
    Case c;
    c.name = name;
    c.expected = expected;
    c.context = std::move(context);
    cases.push_back(std::move(c));
  };
  {
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

    CertSpec no_common_name = LeafSpec();
    no_common_name.common_name.clear();
    add("no-subject", "any", Context(leaf(no_common_name), {&intermediate}));

    // TLS versions and ciphers: the client requires TLS 1.2 or later and
    // offers no 3DES, RC4, NULL or DHE suites, so a server that insists on
    // one of those has nothing in common with it.
    auto tls12 = Context(good, {&intermediate});
    SSL_CTX_set_max_proto_version(tls12.get(), TLS1_2_VERSION);
    add("tls-v1-2", "accept", std::move(tls12));
    auto tls13 = Context(good, {&intermediate});
    SSL_CTX_set_min_proto_version(tls13.get(), TLS1_3_VERSION);
    add("tls-v1-3", "accept", std::move(tls13));
    auto tls10 = Context(good, {&intermediate});
    SSL_CTX_set_min_proto_version(tls10.get(), TLS1_VERSION);
    SSL_CTX_set_max_proto_version(tls10.get(), TLS1_VERSION);
    add("tls-v1-0", "reject", std::move(tls10));
    auto tls11 = Context(good, {&intermediate});
    SSL_CTX_set_min_proto_version(tls11.get(), TLS1_1_VERSION);
    SSL_CTX_set_max_proto_version(tls11.get(), TLS1_1_VERSION);
    add("tls-v1-1", "reject", std::move(tls11));
    auto triple_des =
        Context(leaf(LeafSpec(KeyType::kRsa2048)), {&intermediate});
    SSL_CTX_set_max_proto_version(triple_des.get(), TLS1_2_VERSION);
    if (!SSL_CTX_set_strict_cipher_list(triple_des.get(), "3DES")) {
      fprintf(stderr, "BoringSSL has no 3DES suite\n");
      return 1;
    }
    add("3des", "reject", std::move(triple_des));

    // Certificates that must not be trusted.
    CertSpec expired = LeafSpec();
    expired.not_before = -2 * kDay;
    expired.not_after = -kDay;
    add("expired", "reject", Context(leaf(expired), {&intermediate}));
    CertSpec not_yet_valid = LeafSpec();
    not_yet_valid.not_before = kDay;
    not_yet_valid.not_after = 2 * kDay;
    add("not-yet-valid", "reject",
        Context(leaf(not_yet_valid), {&intermediate}));
    CertSpec wrong_host = LeafSpec();
    wrong_host.common_name = "wrong.host.test";
    wrong_host.subject_alt_names = "DNS:wrong.host.test";
    add("wrong-host", "reject", Context(leaf(wrong_host), {&intermediate}));
    CertSpec name_in_cn_only = LeafSpec();
    name_in_cn_only.subject_alt_names.clear();
    add("cn-only", "any", Context(leaf(name_in_cn_only), {&intermediate}));
    CertSpec client_auth = LeafSpec();
    client_auth.extended_key_usage = "clientAuth";
    add("client-auth-only", "reject",
        Context(leaf(client_auth), {&intermediate}));
    add("self-signed", "reject", Context(MakeCert(LeafSpec(), nullptr), {}));
    add("untrusted-root", "reject",
        Context(MakeCert(LeafSpec(), &other_root), {&other_root}));
    add("incomplete-chain", "reject", Context(good, {}));
    add("leaf-signs-leaf", "reject",
        Context(MakeCert(LeafSpec(), &good), {&good, &intermediate}));
    add("sha1-intermediate", "any",
        Context(MakeCert(LeafSpec(), &sha1_intermediate),
                {&sha1_intermediate}));

    add("http", "accept", nullptr);
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
    table += c.name + "\t" + (c.context ? "https" : "http") +
             "://localhost:" + std::to_string(c.port) + "/\t" + c.expected +
             "\n";
  }
  std::vector<std::thread> threads;
  for (const Case& c : cases) {
    threads.emplace_back(Serve, std::cref(c));
  }
  // Last, so a reader that sees the file can connect to every case.
  if (!WriteFile(dir + "/cases.tsv.tmp", table) ||
      rename((dir + "/cases.tsv.tmp").c_str(), (dir + "/cases.tsv").c_str()) !=
          0) {
    fprintf(stderr, "can't write %s/cases.tsv\n", dir.c_str());
    return 1;
  }
  printf("%s", table.c_str());
  fflush(stdout);
  for (std::thread& thread : threads) {
    thread.join();
  }
  return 0;
}
