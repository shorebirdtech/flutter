#!/bin/bash
# Builds and runs the HTTP parser fuzzers under ASan and UBSan.
#
#   fuzz/run.sh [seconds per fuzzer, default 60; 0 builds without running]
#
# CXX must be a clang with libFuzzer: Linux distro clang has it; Apple's
# doesn't, so on macOS use Homebrew's (CXX=$(brew --prefix llvm)/bin/clang++). Builds without the engine: the parser has no
# dependencies beyond the C++ standard library. Fetches llhttp, pinned by
# hash, for the differential fuzzer. Crashing inputs land in fuzz/out/.

set -euo pipefail

SECONDS_PER_FUZZER="${1:-60}"
CXX="${CXX:-clang++}"
CC="${CC:-${CXX%++}}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC_ROOT="$(cd "$HERE/../../../../.." && pwd)"  # engine/src
OUT="$HERE/out"
LLHTTP_VERSION=9.4.3
LLHTTP_SHA256=1eb813c7437b31a87496a1cd3ed79f00746720f5e7e29c79b42c02cb69f36c39

mkdir -p "$OUT"
cd "$OUT"

if [ ! -d "llhttp-release-v$LLHTTP_VERSION" ]; then
  curl -sSfL -o llhttp.tar.gz \
    "https://github.com/nodejs/llhttp/archive/refs/tags/release/v$LLHTTP_VERSION.tar.gz"
  echo "$LLHTTP_SHA256  llhttp.tar.gz" | shasum -a 256 -c -
  tar xzf llhttp.tar.gz
fi
LLHTTP="$OUT/llhttp-release-v$LLHTTP_VERSION"

SYSROOT=()
if [ "$(uname)" = Darwin ]; then
  SYSROOT=(-isysroot "$(xcrun --show-sdk-path)")
fi
FLAGS=("${SYSROOT[@]}" -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
CXXFLAGS=(-std=c++20 "${FLAGS[@]}" -I"$SRC_ROOT" -I"$LLHTTP/include")
PARSER="$SRC_ROOT/flutter/shell/common/shorebird/http_parser.cc"

for f in api http llhttp; do
  "$CC" "${SYSROOT[@]}" -O1 -g -fsanitize=address,undefined -fsanitize=fuzzer-no-link \
    -I"$LLHTTP/include" -c "$LLHTTP/src/$f.c" -o "llhttp_$f.o"
done
"$CXX" "${CXXFLAGS[@]}" "$HERE/http_parser_fuzzer.cc" "$PARSER" \
  -o http_parser_fuzzer
"$CXX" "${CXXFLAGS[@]}" "$HERE/http_parser_llhttp_fuzzer.cc" "$PARSER" \
  llhttp_api.o llhttp_http.o llhttp_llhttp.o -o http_parser_llhttp_fuzzer

if [ "$SECONDS_PER_FUZZER" = 0 ]; then
  exit 0
fi
for fuzzer in http_parser_fuzzer http_parser_llhttp_fuzzer; do
  mkdir -p "corpus_$fuzzer"
  echo "== $fuzzer for ${SECONDS_PER_FUZZER}s"
  "./$fuzzer" -max_total_time="$SECONDS_PER_FUZZER" -max_len=4096 \
    -artifact_prefix="$OUT/${fuzzer}_crash_" -print_final_stats=1 \
    "corpus_$fuzzer" "$HERE/corpus"
done
