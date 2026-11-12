#!/bin/bash -e

# cspell:words cmdline sdkmanager Seekable

# Installs Android SDK packages with sdkmanager, retrying a failed download.
#
# An install normally takes about 20 seconds. Its download from dl.google.com
# has failed jobs that tested nothing: a corrupt NDK zip ("Error reading Zip
# content from a SeekableByteChannel") failed shorebird_release_tests'
# build_tests 4_5 in run 43264479347, and shorebird/shorebird's e2e.yaml the
# same way (runs 41553186726, 43237646183). Each attempt gets a deadline, and
# a failed attempt's scratch files (partial downloads, unpacked pieces) are
# deleted, so the next one starts clean. A copy of shorebirdtech/shorebird's
# scripts/install_android_sdk_packages.sh.
#
# Usage: shorebird/ci/install_android_sdk_packages.sh <package> [package...]
# e.g. shorebird/ci/install_android_sdk_packages.sh "ndk;28.2.13676358"

if [[ -z "$ANDROID_HOME" ]]; then
    echo "❌ ANDROID_HOME is not set."
    exit 1
fi
if [[ $# -eq 0 ]]; then
    echo "❌ Usage: $0 <package> [package...]"
    exit 1
fi

sdkmanager="${ANDROID_HOME}/cmdline-tools/latest/bin/sdkmanager"
attempts=3
# The slowest of 235 successful installs in shorebird's e2e took 207 seconds.
deadline=5m

for attempt in $(seq 1 "$attempts"); do
    status=0
    timeout --kill-after=10s "$deadline" "$sdkmanager" --install "$@" ||
        status=$?
    if [[ $status -eq 0 ]]; then
        exit 0
    fi
    # 124 from the deadline, 137 if it then had to be killed.
    if [[ $status -eq 124 || $status -eq 137 ]]; then
        echo "⚠️ sdkmanager timed out after $deadline (attempt $attempt of $attempts)."
    else
        echo "⚠️ sdkmanager exited $status (attempt $attempt of $attempts)."
    fi
    # Where sdkmanager keeps downloads and unpacks them (which one depends on
    # its version); a package moves into place only once unpacked.
    rm -rf "${ANDROID_HOME}/.downloadIntermediates" "${ANDROID_HOME}/.temp" \
        "${HOME}/.android/cache"
done

echo "❌ Could not install $* after $attempts attempts."
exit 1
