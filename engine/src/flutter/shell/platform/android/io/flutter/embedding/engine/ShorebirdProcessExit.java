// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package io.flutter.embedding.engine;

import static io.flutter.Build.API_LEVELS;

import android.annotation.SuppressLint;
import android.app.ActivityManager;
import android.app.Application;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.os.Build;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import io.flutter.BuildConfig;
import io.flutter.Log;
import java.nio.charset.StandardCharsets;
import java.util.UUID;

/**
 * Tells a kill from a crash for the Shorebird updater. On Android 11+, in release builds, this
 * process gives the OS a token as its state summary, and reads back what the OS recorded about how
 * an earlier run of this process ended. The updater writes the token into its boot breadcrumb; if
 * the next launch finds the breadcrumb still set, the record with the same token says whether the
 * patch crashed or the process was killed. Nothing here leaves the device.
 *
 * <p>The OS keeps one summary per process and the last write wins. An app or SDK that sets its own
 * summary after this replaces the token, and a death while booting then counts as a crash, as on
 * older Android.
 */
final class ShorebirdProcessExit {
  private static final String TAG = "ShorebirdProcessExit";

  @Nullable final String processToken;
  @Nullable final String previousExitProcessToken;
  final int previousExitReason;

  /** Read and recorded once per process, so a second init gives the same token. */
  @Nullable private static ShorebirdProcessExit instance;

  private ShorebirdProcessExit(
      @Nullable String processToken, @Nullable String previousExitProcessToken, int reason) {
    this.processToken = processToken;
    this.previousExitProcessToken = previousExitProcessToken;
    this.previousExitReason = reason;
  }

  @NonNull
  static synchronized ShorebirdProcessExit recordAndRead(@NonNull Context context) {
    if (instance == null) {
      // The updater only runs in release builds; don't take the app's
      // summary slot in debug or profile.
      if (!BuildConfig.RELEASE || Build.VERSION.SDK_INT < API_LEVELS.API_30) {
        instance = new ShorebirdProcessExit(null, null, 0);
      } else {
        instance = recordAndReadApi30(context);
      }
    }
    return instance;
  }

  @SuppressLint("NewApi")
  @NonNull
  private static ShorebirdProcessExit recordAndReadApi30(@NonNull Context context) {
    ActivityManager activityManager =
        (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
    if (activityManager == null) {
      return new ShorebirdProcessExit(null, null, 0);
    }
    String previousToken = null;
    int previousReason = 0;
    try {
      String processName = Application.getProcessName();
      // Newest first. Take the newest record of this process name that
      // carries a summary: a run of this process that never reached init
      // (restarted by the OS for a broadcast, then killed again) has none.
      // Each token is random per process, so only the record of the process
      // that left the breadcrumb can match it.
      for (ApplicationExitInfo info : activityManager.getHistoricalProcessExitReasons(null, 0, 0)) {
        byte[] summary = info.getProcessStateSummary();
        if (summary != null && processName.equals(info.getProcessName())) {
          previousToken = new String(summary, StandardCharsets.UTF_8);
          previousReason = info.getReason();
          break;
        }
      }
    } catch (RuntimeException e) {
      Log.w(TAG, "Could not read the previous process's exit info", e);
    }
    String token = UUID.randomUUID().toString();
    try {
      activityManager.setProcessStateSummary(token.getBytes(StandardCharsets.UTF_8));
    } catch (RuntimeException e) {
      // Without a token the updater treats any death while booting as a
      // crash, as it does on older Android.
      Log.w(TAG, "Could not record this process's token", e);
      token = null;
    }
    return new ShorebirdProcessExit(token, previousToken, previousReason);
  }
}
