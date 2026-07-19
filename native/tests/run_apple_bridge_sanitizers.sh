#!/bin/sh
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_directory=$(mktemp -d "${TMPDIR:-/tmp}/jxl_bridge_sanitizers.XXXXXX")
trap 'rm -rf "$build_directory"' EXIT HUP INT TERM

headers="$repository_root/macos/jxl_coder/Frameworks/JxlCoderNative.xcframework/macos-arm64_x86_64/Headers"
archive="$repository_root/macos/jxl_coder/Frameworks/JxlCoderNative.xcframework/macos-arm64_x86_64/libjxl_coder_native.a"
binary="$build_directory/jxl_coder_core_test"

clang++ \
  -std=c++17 \
  -Wall \
  -Wextra \
  -Wpedantic \
  -Werror \
  -O1 \
  -g \
  -fobjc-arc \
  -fsanitize=address,undefined \
  -fno-omit-frame-pointer \
  -I"$headers" \
  -I"$repository_root/macos/jxl_coder/Sources/JxlCoderCore/include" \
  "$repository_root/native/tests/jxl_coder_core_test.mm" \
  "$archive" \
  -framework Foundation \
  -o "$binary"

ASAN_OPTIONS=detect_leaks=0 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  "$binary" \
  "$repository_root/example/integration_test/2.jpg" \
  "$repository_root/example/integration_test/1.jxl"
