// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The updater's HTTP client on iOS and macOS, on NSURLSession. Requests get
// what the app's own URLSession traffic gets: the system's TLS and trust
// evaluation (including roots installed by an administrator or a
// configuration profile, such as a TLS-inspecting proxy's), the system proxy
// and PAC settings, and VPN routing.

#include "flutter/shell/common/shorebird/http_client.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "flutter/fml/logging.h"
#include "flutter/shell/common/shorebird/http_parser.h"

#if !__has_feature(objc_arc)
#error ARC must be enabled!
#endif

namespace flutter {
namespace shorebird {
namespace {

constexpr char kUserAgent[] = "Shorebird-Updater";

// Runs handler calls on the thread that called `SendHttpRequest`, as
// `HttpResponseHandler` promises. NSURLSession calls its delegate on the
// session's queue; each delegate call that reaches the handler waits there
// until the caller's thread has run it.
class CallerThread {
 public:
  // On the delegate queue: runs `call` on the caller's thread.
  bool Run(std::function<bool()> call) {
    std::unique_lock<std::mutex> lock(mutex_);
    pending_ = std::move(call);
    answered_ = false;
    changed_.notify_all();
    changed_.wait(lock, [this] { return answered_; });
    return answer_;
  }

  // On the delegate queue: the request is over.
  void Finish(HttpResult result) {
    std::lock_guard<std::mutex> lock(mutex_);
    result_ = result;
    changed_.notify_all();
  }

  // On the caller's thread: runs handler calls until the request is over.
  HttpResult Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
      changed_.wait(lock, [this] { return pending_ || result_.has_value(); });
      if (!pending_) {
        return *result_;
      }
      std::function<bool()> call = std::move(pending_);
      pending_ = nullptr;
      lock.unlock();
      bool answer = call();
      lock.lock();
      answer_ = answer;
      answered_ = true;
      changed_.notify_all();
    }
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::function<bool()> pending_;
  bool answered_ = false;
  bool answer_ = false;
  std::optional<HttpResult> result_;
};

struct RequestState {
  HttpResponseHandler* handler = nullptr;
  HttpClientOptions options;
  bool is_get = false;
  NSDictionary<NSString*, NSString*>* headers = nil;
  // Set when `trusted_roots_pem` is.
  NSArray* anchors = nil;
  std::string host;
  int redirects = 0;
  bool responded = false;
  // Why the delegate ended the request early, when it did.
  std::optional<HttpResult> failure;
  CallerThread caller;
};

NSString* ToNSString(const std::string& s) {
  return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
}

std::string ToStdString(NSString* s) {
  const char* utf8 = s.UTF8String;
  return utf8 == nullptr ? std::string() : std::string(utf8);
}

// The certificates in a PEM bundle, or nil if it holds none.
NSArray* CertificatesFromPem(const std::string& pem) {
  NSMutableArray* certificates = [NSMutableArray array];
  const std::string begin = "-----BEGIN CERTIFICATE-----";
  const std::string end = "-----END CERTIFICATE-----";
  size_t at = 0;
  while ((at = pem.find(begin, at)) != std::string::npos) {
    size_t body = at + begin.size();
    size_t stop = pem.find(end, body);
    if (stop == std::string::npos) {
      break;
    }
    NSData* der =
        [[NSData alloc] initWithBase64EncodedString:ToNSString(pem.substr(body, stop - body))
                                            options:NSDataBase64DecodingIgnoreUnknownCharacters];
    SecCertificateRef certificate =
        der ? SecCertificateCreateWithData(nullptr, (__bridge CFDataRef)der) : nullptr;
    if (certificate != nullptr) {
      [certificates addObject:(__bridge_transfer id)certificate];
    }
    at = stop + end.size();
  }
  return certificates.count > 0 ? certificates : nil;
}

HttpResult ResultForError(NSError* error, int redirects) {
  if (![error.domain isEqualToString:NSURLErrorDomain]) {
    return HttpResult::kTransportError;
  }
  switch (error.code) {
    case NSURLErrorBadURL:
    case NSURLErrorUnsupportedURL:
      // After a redirect, the bad URL came from the server.
      return redirects == 0 ? HttpResult::kInvalidRequest : HttpResult::kProtocolError;
    case NSURLErrorSecureConnectionFailed: {
      // The peer closing the connection mid-handshake is a dropped
      // connection, not a TLS failure.
      id stream_code = error.userInfo[@"_kCFStreamErrorCodeKey"];
      if ([stream_code isKindOfClass:[NSNumber class]]) {
        switch ([stream_code intValue]) {
          case errSSLClosedGraceful:
          case errSSLClosedAbort:
          case errSSLClosedNoNotify:
            return HttpResult::kTransportError;
        }
      }
      return HttpResult::kTlsError;
    }
    case NSURLErrorServerCertificateHasBadDate:
    case NSURLErrorServerCertificateUntrusted:
    case NSURLErrorServerCertificateHasUnknownRoot:
    case NSURLErrorServerCertificateNotYetValid:
    case NSURLErrorClientCertificateRejected:
    case NSURLErrorClientCertificateRequired:
      return HttpResult::kTlsError;
    case NSURLErrorBadServerResponse:
    case NSURLErrorHTTPTooManyRedirects:
    case NSURLErrorRedirectToNonExistentLocation:
    case NSURLErrorCannotParseResponse:
    case NSURLErrorCannotDecodeRawData:
    case NSURLErrorCannotDecodeContentData:
      return HttpResult::kProtocolError;
    default:
      return HttpResult::kTransportError;
  }
}

// What went wrong, for the log: NSURLSession's description and code, and
// the lower-level error under it (e.g. a Secure Transport or POSIX code).
std::string Describe(NSError* error) {
  std::string text = ToStdString(error.localizedDescription) + " (" + ToStdString(error.domain) +
                     " " + std::to_string(error.code) + ")";
  NSError* underlying = error.userInfo[NSUnderlyingErrorKey];
  if ([underlying isKindOfClass:[NSError class]]) {
    text += "; caused by " + ToStdString(underlying.domain) + " " + std::to_string(underlying.code);
  }
  // The Secure Transport or POSIX code, e.g. -9813 for a certificate with no
  // trusted root.
  id stream_code = error.userInfo[@"_kCFStreamErrorCodeKey"];
  if ([stream_code isKindOfClass:[NSNumber class]]) {
    text += "; stream error " + ToStdString([stream_code stringValue]);
  }
  // For a rejected certificate, the trust evaluation says which certificate
  // and why (e.g. "“Zscaler Root CA” certificate is not trusted").
  id peer_trust = error.userInfo[NSURLErrorFailingURLPeerTrustErrorKey];
  if (peer_trust != nil && CFGetTypeID((__bridge CFTypeRef)peer_trust) == SecTrustGetTypeID()) {
    CFErrorRef trust_error = nullptr;
    if (!SecTrustEvaluateWithError((__bridge SecTrustRef)peer_trust, &trust_error) &&
        trust_error != nullptr) {
      NSError* reason = (__bridge_transfer NSError*)trust_error;
      text += "; " + ToStdString(reason.localizedDescription);
    }
  }
  return text;
}

}  // namespace
}  // namespace shorebird
}  // namespace flutter

using flutter::shorebird::HttpHeader;
using flutter::shorebird::HttpResult;
using flutter::shorebird::RequestState;

@interface FLTShorebirdHttpDelegate : NSObject <NSURLSessionDataDelegate>
- (instancetype)initWithState:(std::shared_ptr<RequestState>)state;
@end

@implementation FLTShorebirdHttpDelegate {
  // Shared with the caller, so whichever finishes last frees it.
  std::shared_ptr<RequestState> _state;
}

- (instancetype)initWithState:(std::shared_ptr<RequestState>)state {
  self = [super init];
  if (self) {
    _state = std::move(state);
  }
  return self;
}

- (void)failTask:(NSURLSessionTask*)task with:(HttpResult)result {
  if (!_state->failure.has_value()) {
    _state->failure = result;
  }
  [task cancel];
}

- (void)URLSession:(NSURLSession*)session
                          task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)request
             completionHandler:(void (^)(NSURLRequest*))completionHandler {
  // Redirects are followed for GET only. Nothing the updater POSTs is
  // expected to redirect, and replaying a POST elsewhere is not safe; the
  // redirect itself is delivered as the response.
  if (!_state->is_get) {
    completionHandler(nil);
    return;
  }
  std::optional<flutter::shorebird::http_internal::Url> next =
      flutter::shorebird::http_internal::ParseUrl(
          flutter::shorebird::ToStdString(request.URL.absoluteString));
  bool from_https = [response.URL.scheme caseInsensitiveCompare:@"https"] == NSOrderedSame;
  // Never from https to plain http: the request (and the headers it carries)
  // would leave TLS.
  if (_state->redirects >= _state->options.max_redirects || !next.has_value() ||
      (from_https && !next->https)) {
    [self failTask:task with:HttpResult::kProtocolError];
    completionHandler(nil);
    return;
  }
  _state->redirects++;
  NSMutableURLRequest* follow = [request mutableCopy];
  [_state->headers
      enumerateKeysAndObjectsUsingBlock:^(NSString* name, NSString* value, BOOL* stop) {
        [follow setValue:value forHTTPHeaderField:name];
      }];
  completionHandler(follow);
}

- (void)URLSession:(NSURLSession*)session
                   task:(NSURLSessionTask*)task
    didReceiveChallenge:(NSURLAuthenticationChallenge*)challenge
      completionHandler:
          (void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential*))completionHandler {
  SecTrustRef trust = challenge.protectionSpace.serverTrust;
  if (_state->anchors == nil ||
      ![challenge.protectionSpace.authenticationMethod
          isEqualToString:NSURLAuthenticationMethodServerTrust] ||
      trust == nullptr) {
    completionHandler(NSURLSessionAuthChallengePerformDefaultHandling, nil);
    return;
  }
  // Tests: trust only the given roots. The trust object already carries
  // NSURLSession's SSL policy for the host.
  SecTrustSetAnchorCertificates(trust, (__bridge CFArrayRef)_state->anchors);
  SecTrustSetAnchorCertificatesOnly(trust, true);
  CFErrorRef error = nullptr;
  if (SecTrustEvaluateWithError(trust, &error)) {
    completionHandler(NSURLSessionAuthChallengeUseCredential,
                      [NSURLCredential credentialForTrust:trust]);
    return;
  }
  NSError* reason = (__bridge_transfer NSError*)error;
  FML_LOG(ERROR) << "Shorebird updater could not verify " << _state->host << ": "
                 << flutter::shorebird::Describe(reason);
  _state->failure = HttpResult::kTlsError;
  completionHandler(NSURLSessionAuthChallengeCancelAuthenticationChallenge, nil);
}

- (void)URLSession:(NSURLSession*)session
              dataTask:(NSURLSessionDataTask*)task
    didReceiveResponse:(NSURLResponse*)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
  // One response per request: a multipart/x-mixed-replace server could send
  // several.
  if (_state->failure.has_value() || _state->responded ||
      ![response isKindOfClass:[NSHTTPURLResponse class]]) {
    if (!_state->failure.has_value()) {
      _state->failure = HttpResult::kProtocolError;
    }
    completionHandler(NSURLSessionResponseCancel);
    return;
  }
  NSHTTPURLResponse* http = (NSHTTPURLResponse*)response;
  int status = static_cast<int>(http.statusCode);
  std::vector<HttpHeader> headers;
  NSDictionary* fields = http.allHeaderFields;
  // NSURLSession decodes a content-encoded body but keeps the encoded
  // Content-Length, which would then not count the bytes delivered.
  NSString* encoding = [http valueForHTTPHeaderField:@"Content-Encoding"];
  bool decoded =
      encoding.length > 0 && [encoding caseInsensitiveCompare:@"identity"] != NSOrderedSame;
  for (id name in fields) {
    id value = fields[name];
    if (decoded && [name isKindOfClass:[NSString class]] &&
        [name caseInsensitiveCompare:@"Content-Length"] == NSOrderedSame) {
      continue;
    }
    if ([name isKindOfClass:[NSString class]] && [value isKindOfClass:[NSString class]]) {
      headers.push_back(
          {flutter::shorebird::ToStdString(name), flutter::shorebird::ToStdString(value)});
    }
  }
  RequestState* state = _state.get();
  _state->responded = true;
  if (!_state->caller.Run([&] { return state->handler->OnResponse(status, headers); })) {
    _state->failure = HttpResult::kAborted;
    completionHandler(NSURLSessionResponseCancel);
    return;
  }
  completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session
          dataTask:(NSURLSessionDataTask*)task
    didReceiveData:(NSData*)data {
  if (_state->failure.has_value()) {
    return;
  }
  RequestState* state = _state.get();
  __block bool keep_going = true;
  [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range, BOOL* stop) {
    keep_going = state->caller.Run(
        [&] { return state->handler->OnBody(static_cast<const uint8_t*>(bytes), range.length); });
    *stop = !keep_going;
  }];
  if (!keep_going) {
    [self failTask:task with:HttpResult::kAborted];
  }
}

- (void)URLSession:(NSURLSession*)session
                    task:(NSURLSessionTask*)task
    didCompleteWithError:(NSError*)error {
  HttpResult result;
  if (_state->failure.has_value()) {
    result = *_state->failure;
  } else if (error != nil) {
    result = flutter::shorebird::ResultForError(error, _state->redirects);
    FML_LOG(ERROR) << "Shorebird updater request to " << _state->host
                   << " failed: " << flutter::shorebird::Describe(error);
  } else {
    result = _state->responded ? HttpResult::kOk : HttpResult::kProtocolError;
  }
  _state->caller.Finish(result);
}

@end

namespace flutter {
namespace shorebird {

HttpResult SendHttpRequest(const HttpRequest& request,
                           HttpResponseHandler& handler,
                           const HttpClientOptions& options) {
  @autoreleasepool {
    if (request.method != "GET" && request.method != "POST") {
      return HttpResult::kInvalidRequest;
    }
    std::optional<http_internal::Url> url = http_internal::ParseUrl(request.url);
    NSURL* ns_url = [NSURL URLWithString:ToNSString(request.url)];
    if (!url.has_value() || ns_url == nil) {
      return HttpResult::kInvalidRequest;
    }

    auto state = std::make_shared<RequestState>();
    state->handler = &handler;
    state->options = options;
    state->is_get = request.method == "GET";
    state->host = url->host;
    if (options.trusted_roots_pem.has_value()) {
      state->anchors = CertificatesFromPem(*options.trusted_roots_pem);
      if (state->anchors == nil) {
        return HttpResult::kInvalidRequest;
      }
    }

    NSMutableDictionary<NSString*, NSString*>* headers = [NSMutableDictionary dictionary];
    headers[@"User-Agent"] = @(kUserAgent);
    headers[@"Accept"] = @"*/*";
    // Bodies arrive as sent, so Content-Length counts the bytes delivered.
    headers[@"Accept-Encoding"] = @"identity";
    for (const auto& header : request.headers) {
      if (!http_internal::IsToken(header.name) || !http_internal::IsFieldValue(header.value) ||
          http_internal::IsReservedHeader(header.name)) {
        return HttpResult::kInvalidRequest;
      }
      NSString* name = ToNSString(header.name);
      NSString* value = ToNSString(header.value);
      if (name == nil || value == nil) {
        return HttpResult::kInvalidRequest;
      }
      // Replace a default whatever its case.
      for (NSString* existing in headers.allKeys) {
        if ([existing caseInsensitiveCompare:name] == NSOrderedSame) {
          [headers removeObjectForKey:existing];
        }
      }
      headers[name] = value;
    }
    state->headers = headers;

    NSTimeInterval timeout = std::chrono::duration<double>(options.io_timeout).count();
    NSMutableURLRequest* ns_request =
        [NSMutableURLRequest requestWithURL:ns_url
                                cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                            timeoutInterval:timeout];
    ns_request.HTTPMethod = ToNSString(request.method);
    ns_request.allHTTPHeaderFields = headers;
    ns_request.HTTPShouldHandleCookies = NO;
    if (request.method == "POST" || !request.body.empty()) {
      ns_request.HTTPBody = [NSData dataWithBytes:request.body.data() length:request.body.size()];
    }

    NSURLSessionConfiguration* configuration =
        [NSURLSessionConfiguration ephemeralSessionConfiguration];
    configuration.URLCache = nil;
    configuration.HTTPCookieStorage = nil;
    configuration.timeoutIntervalForRequest = timeout;

    NSOperationQueue* queue = [[NSOperationQueue alloc] init];
    queue.maxConcurrentOperationCount = 1;
    FLTShorebirdHttpDelegate* delegate = [[FLTShorebirdHttpDelegate alloc] initWithState:state];
    NSURLSession* session = [NSURLSession sessionWithConfiguration:configuration
                                                          delegate:delegate
                                                     delegateQueue:queue];
    NSURLSessionDataTask* task = [session dataTaskWithRequest:ns_request];
    if (task == nil) {
      [session invalidateAndCancel];
      return HttpResult::kInvalidRequest;
    }
    [task resume];
    HttpResult result = state->caller.Wait();
    // The session holds its delegate until it is invalidated.
    [session invalidateAndCancel];
    return result;
  }
}

}  // namespace shorebird
}  // namespace flutter
