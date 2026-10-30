// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The updater's HTTP client on Android: the embedding's ShorebirdHttpClient,
// on HttpURLConnection, driven over JNI from the updater's thread. Requests
// get what the app's own HttpURLConnection traffic gets: its network security
// config, the system proxy, and VPN routing.

#include "flutter/shell/common/shorebird/http_client_android.h"

#include <jni.h>

#include <cstdint>
#include <vector>

#include "flutter/fml/logging.h"
#include "flutter/fml/platform/android/jni_util.h"
#include "flutter/fml/platform/android/scoped_java_ref.h"
#include "flutter/shell/common/shorebird/http_client.h"

namespace flutter {
namespace shorebird {
namespace {

constexpr size_t kReadBufferSize = 64 * 1024;

// Cached in RegisterHttpClient, on the thread that loaded the library: the
// updater's thread is attached to the VM by native code, so FindClass there
// would search the system class loader, which does not have the embedding.
fml::jni::ScopedJavaGlobalRef<jclass>* g_client_class = nullptr;
jmethodID g_open_method = nullptr;
jmethodID g_read_method = nullptr;
jmethodID g_close_method = nullptr;
jfieldID g_result_field = nullptr;
jfieldID g_error_field = nullptr;
jfieldID g_status_field = nullptr;
jfieldID g_headers_field = nullptr;

// Clears a pending Java exception, which nothing in ShorebirdHttpClient
// should throw, and says whether there was one.
bool Threw(JNIEnv* env, std::string* error) {
  if (!env->ExceptionCheck()) {
    return false;
  }
  // Cleared before anything else touches JNI, which CheckJNI forbids with
  // an exception pending.
  jthrowable exception = env->ExceptionOccurred();
  env->ExceptionClear();
  *error = fml::jni::GetJavaExceptionInfo(env, exception);
  env->DeleteLocalRef(exception);
  return true;
}

HttpResult ToResult(jint code) {
  switch (code) {
    case 0:
      return HttpResult::kOk;
    case 2:
      return HttpResult::kTlsError;
    case 3:
      return HttpResult::kProtocolError;
    case 5:
      return HttpResult::kInvalidRequest;
    default:
      return HttpResult::kTransportError;
  }
}

// The client's result, with its error, once it failed.
HttpResult Failure(JNIEnv* env, jobject client, std::string* error) {
  fml::jni::ScopedJavaLocalRef<jstring> message(
      env, static_cast<jstring>(env->GetObjectField(client, g_error_field)));
  *error = fml::jni::JavaStringToString(env, message.obj());
  return ToResult(env->GetIntField(client, g_result_field));
}

HttpResult Send(JNIEnv* env,
                const HttpRequest& request,
                HttpResponseHandler& handler,
                std::string* error) {
  std::vector<std::string> header_pairs;
  header_pairs.reserve(request.headers.size() * 2);
  for (const auto& header : request.headers) {
    header_pairs.push_back(header.name);
    header_pairs.push_back(header.value);
  }
  fml::jni::ScopedJavaLocalRef<jstring> method =
      fml::jni::StringToJavaString(env, request.method);
  fml::jni::ScopedJavaLocalRef<jstring> url =
      fml::jni::StringToJavaString(env, request.url);
  fml::jni::ScopedJavaLocalRef<jobjectArray> headers =
      fml::jni::VectorToStringArray(env, header_pairs);
  fml::jni::ScopedJavaLocalRef<jbyteArray> body(
      env,
      request.body.empty() ? nullptr : env->NewByteArray(request.body.size()));
  if (!request.body.empty()) {
    if (body.is_null()) {
      Threw(env, error);
      return HttpResult::kInvalidRequest;
    }
    env->SetByteArrayRegion(
        body.obj(), 0, request.body.size(),
        reinterpret_cast<const jbyte*>(request.body.data()));
  }
  if (Threw(env, error)) {
    return HttpResult::kInvalidRequest;
  }
  // The timeout is a whole number of milliseconds the Java int can hold.
  jint timeout_ms = request.timeout_ms > INT32_MAX
                        ? INT32_MAX
                        : static_cast<jint>(request.timeout_ms);

  fml::jni::ScopedJavaLocalRef<jobject> client(
      env, env->CallStaticObjectMethod(g_client_class->obj(), g_open_method,
                                       method.obj(), url.obj(), headers.obj(),
                                       body.obj(), timeout_ms));
  if (Threw(env, error) || client.is_null()) {
    return HttpResult::kTransportError;
  }

  HttpResult result = HttpResult::kOk;
  if (env->GetIntField(client.obj(), g_result_field) != 0) {
    result = Failure(env, client.obj(), error);
  } else {
    int status = env->GetIntField(client.obj(), g_status_field);
    fml::jni::ScopedJavaLocalRef<jobjectArray> response_pairs(
        env, static_cast<jobjectArray>(
                 env->GetObjectField(client.obj(), g_headers_field)));
    std::vector<std::string> pairs =
        fml::jni::StringArrayToVector(env, response_pairs.obj());
    std::vector<HttpHeader> response_headers;
    response_headers.reserve(pairs.size() / 2);
    for (size_t i = 0; i + 1 < pairs.size(); i += 2) {
      response_headers.push_back({pairs[i], pairs[i + 1]});
    }
    if (!handler.OnResponse(status, response_headers)) {
      result = HttpResult::kAborted;
    } else {
      fml::jni::ScopedJavaLocalRef<jbyteArray> buffer(
          env, env->NewByteArray(kReadBufferSize));
      if (buffer.is_null()) {
        Threw(env, error);
        result = HttpResult::kTransportError;
      }
      std::vector<uint8_t> bytes(kReadBufferSize);
      while (result == HttpResult::kOk) {
        jint count =
            env->CallIntMethod(client.obj(), g_read_method, buffer.obj());
        if (Threw(env, error)) {
          result = HttpResult::kTransportError;
        } else if (count < 0) {
          if (env->GetIntField(client.obj(), g_result_field) != 0) {
            result = Failure(env, client.obj(), error);
          }
          break;
        } else if (count > 0) {
          env->GetByteArrayRegion(buffer.obj(), 0, count,
                                  reinterpret_cast<jbyte*>(bytes.data()));
          if (!handler.OnBody(bytes.data(), count)) {
            result = HttpResult::kAborted;
          }
        }
      }
    }
  }
  env->CallVoidMethod(client.obj(), g_close_method);
  env->ExceptionClear();
  return result;
}

}  // namespace

bool RegisterHttpClient(JNIEnv* env) {
  fml::jni::ScopedJavaLocalRef<jclass> clazz(
      env, env->FindClass("io/flutter/embedding/engine/ShorebirdHttpClient"));
  if (clazz.is_null()) {
    env->ExceptionClear();
    return false;
  }
  // Each lookup is checked before the next, as JNI must not be called with
  // an exception pending (CheckJNI aborts the app).
  auto method = [&](jmethodID* id, const char* name, const char* signature,
                    bool is_static) {
    *id = is_static ? env->GetStaticMethodID(clazz.obj(), name, signature)
                    : env->GetMethodID(clazz.obj(), name, signature);
    return !env->ExceptionCheck() && *id != nullptr;
  };
  auto field = [&](jfieldID* id, const char* name, const char* signature) {
    *id = env->GetFieldID(clazz.obj(), name, signature);
    return !env->ExceptionCheck() && *id != nullptr;
  };
  if (!method(&g_open_method, "open",
              "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;[BI)"
              "Lio/flutter/embedding/engine/ShorebirdHttpClient;",
              true) ||
      !method(&g_read_method, "read", "([B)I", false) ||
      !method(&g_close_method, "close", "()V", false) ||
      !field(&g_result_field, "result", "I") ||
      !field(&g_error_field, "error", "Ljava/lang/String;") ||
      !field(&g_status_field, "status", "I") ||
      !field(&g_headers_field, "headers", "[Ljava/lang/String;")) {
    env->ExceptionClear();
    return false;
  }
  g_client_class = new fml::jni::ScopedJavaGlobalRef<jclass>(env, clazz.obj());
  return true;
}

HttpResult SendHttpRequest(const HttpRequest& request,
                           HttpResponseHandler& handler,
                           std::string* error) {
  if (g_client_class == nullptr) {
    *error = "The Android HTTP client is not registered.";
    return HttpResult::kTransportError;
  }
  // Attaches the updater's thread to the VM, if it is not already; it stays
  // attached until the thread exits.
  JNIEnv* env = fml::jni::AttachCurrentThread();
  // Frees the local references this request makes, as the thread may never
  // return to Java.
  if (env->PushLocalFrame(16) != 0) {
    Threw(env, error);
    return HttpResult::kTransportError;
  }
  HttpResult result = Send(env, request, handler, error);
  env->PopLocalFrame(nullptr);
  return result;
}

}  // namespace shorebird
}  // namespace flutter
