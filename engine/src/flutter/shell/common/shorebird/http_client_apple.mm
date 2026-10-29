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

#if !__has_feature(objc_arc)
#error ARC must be enabled!
#endif

namespace flutter {
namespace shorebird {
namespace {

constexpr char kUserAgent[] = "Shorebird-Updater";
constexpr int kMaxRedirects = 10;
// The longest the connection may sit idle, whatever the request's overall
// limit.
constexpr NSTimeInterval kIdleTimeout = 60;

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
  bool is_get = false;
  NSDictionary<NSString*, NSString*>* headers = nil;
  int redirects = 0;
  bool responded = false;
  // Why the delegate ended the request early, when it did.
  std::optional<HttpResult> failure;
  // Why the request failed, for the caller to log.
  std::string error;
  CallerThread caller;
};

NSString* ToNSString(const std::string& s) {
  return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
}

std::string ToStdString(NSString* s) {
  const char* utf8 = s.UTF8String;
  return utf8 == nullptr ? std::string() : std::string(utf8);
}

// Whether `s` can be a header name or value: nonempty (for a name), with no
// control characters, so it cannot end the header or smuggle another.
bool IsHeaderText(const std::string& s, bool allow_empty) {
  if (s.empty()) {
    return allow_empty;
  }
  for (unsigned char c : s) {
    if (c == '\r' || c == '\n' || c == '\0') {
      return false;
    }
  }
  return true;
}

// Headers the client sets itself.
bool IsReservedHeader(const std::string& name) {
  for (const char* reserved : {"Host", "Content-Length", "Transfer-Encoding", "Connection"}) {
    if ([ToNSString(name) caseInsensitiveCompare:@(reserved)] == NSOrderedSame) {
      return true;
    }
  }
  return false;
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

- (void)failTask:(NSURLSessionTask*)task with:(HttpResult)result because:(const char*)reason {
  if (!_state->failure.has_value()) {
    _state->failure = result;
    _state->error = reason;
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
  NSString* next_scheme = request.URL.scheme;
  bool to_https = [next_scheme caseInsensitiveCompare:@"https"] == NSOrderedSame;
  bool to_http = [next_scheme caseInsensitiveCompare:@"http"] == NSOrderedSame;
  bool from_https = [response.URL.scheme caseInsensitiveCompare:@"https"] == NSOrderedSame;
  // Never from https to plain http: the request (and the headers it carries)
  // would leave TLS.
  if (_state->redirects >= flutter::shorebird::kMaxRedirects || !(to_https || to_http) ||
      (from_https && !to_https)) {
    [self failTask:task
              with:HttpResult::kProtocolError
           because:"Too many redirects, or a redirect to an unsupported or less secure URL."];
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
              dataTask:(NSURLSessionDataTask*)task
    didReceiveResponse:(NSURLResponse*)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
  // One response per request: a multipart/x-mixed-replace server could send
  // several.
  if (_state->failure.has_value() || _state->responded ||
      ![response isKindOfClass:[NSHTTPURLResponse class]]) {
    if (!_state->failure.has_value()) {
      _state->failure = HttpResult::kProtocolError;
      _state->error = "The server sent more than one response, or one that was not HTTP.";
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
    [self failTask:task with:HttpResult::kAborted because:""];
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
    _state->error = flutter::shorebird::Describe(error);
  } else if (_state->responded) {
    result = HttpResult::kOk;
  } else {
    result = HttpResult::kProtocolError;
    _state->error = "The server sent no response.";
  }
  _state->caller.Finish(result);
}

@end

namespace flutter {
namespace shorebird {

HttpResult SendHttpRequest(const HttpRequest& request,
                           HttpResponseHandler& handler,
                           std::string* error) {
  @autoreleasepool {
    if (request.method != "GET" && request.method != "POST") {
      *error = "The method is not GET or POST.";
      return HttpResult::kInvalidRequest;
    }
    NSString* url_string = ToNSString(request.url);
    NSURL* ns_url = url_string == nil ? nil : [NSURL URLWithString:url_string];
    if (ns_url == nil || ns_url.host.length == 0 ||
        !([ns_url.scheme caseInsensitiveCompare:@"https"] == NSOrderedSame ||
          [ns_url.scheme caseInsensitiveCompare:@"http"] == NSOrderedSame)) {
      *error = "The URL is not an absolute http or https URL.";
      return HttpResult::kInvalidRequest;
    }

    auto state = std::make_shared<RequestState>();
    state->handler = &handler;
    state->is_get = request.method == "GET";

    NSMutableDictionary<NSString*, NSString*>* headers = [NSMutableDictionary dictionary];
    headers[@"User-Agent"] = @(kUserAgent);
    headers[@"Accept"] = @"*/*";
    // Bodies arrive as sent, so Content-Length counts the bytes delivered.
    headers[@"Accept-Encoding"] = @"identity";
    for (const auto& header : request.headers) {
      if (!IsHeaderText(header.name, false) || !IsHeaderText(header.value, true) ||
          header.name.find(':') != std::string::npos || IsReservedHeader(header.name)) {
        *error = "A request header is not valid.";
        return HttpResult::kInvalidRequest;
      }
      NSString* name = ToNSString(header.name);
      NSString* value = ToNSString(header.value);
      if (name == nil || value == nil) {
        *error = "A request header is not valid UTF-8.";
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

    NSMutableURLRequest* ns_request =
        [NSMutableURLRequest requestWithURL:ns_url
                                cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                            timeoutInterval:kIdleTimeout];
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
    configuration.timeoutIntervalForRequest = kIdleTimeout;
    if (request.timeout_ms > 0) {
      // The whole request, redirects included.
      configuration.timeoutIntervalForResource = request.timeout_ms / 1000.0;
    }

    NSOperationQueue* queue = [[NSOperationQueue alloc] init];
    queue.maxConcurrentOperationCount = 1;
    FLTShorebirdHttpDelegate* delegate = [[FLTShorebirdHttpDelegate alloc] initWithState:state];
    NSURLSession* session = [NSURLSession sessionWithConfiguration:configuration
                                                          delegate:delegate
                                                     delegateQueue:queue];
    NSURLSessionDataTask* task = [session dataTaskWithRequest:ns_request];
    if (task == nil) {
      [session invalidateAndCancel];
      *error = "NSURLSession could not make a task for the request.";
      return HttpResult::kInvalidRequest;
    }
    [task resume];
    HttpResult result = state->caller.Wait();
    *error = state->error;
    // The session holds its delegate until it is invalidated.
    [session invalidateAndCancel];
    return result;
  }
}

}  // namespace shorebird
}  // namespace flutter
