#!/bin/sh
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_directory=$(mktemp -d "${TMPDIR:-/tmp}/jxl_windows_adapter_sanitizers.XXXXXX")
trap 'rm -rf "$build_directory"' EXIT HUP INT TERM

flutter_headers=$("$repository_root/native/tests/find_flutter_cpp_headers.sh")
jxl_headers="$repository_root/macos/jxl_coder/Frameworks/JxlCoderNative.xcframework/macos-arm64_x86_64/Headers"
jxl_archive="$repository_root/macos/jxl_coder/Frameworks/JxlCoderNative.xcframework/macos-arm64_x86_64/libjxl_coder_native.a"
binary="$build_directory/windows_adapter_test"

clang++ \
  -std=c++17 \
  -Wall \
  -Wextra \
  -Wpedantic \
  -Werror \
  -O1 \
  -g \
  -fsanitize=address,undefined \
  -fno-omit-frame-pointer \
  -I"$flutter_headers" \
  -I"$jxl_headers" \
  "$repository_root/native/src/jxl_codec.cc" \
  "$repository_root/native/src/shared_parallel_runner.cc" \
  "$repository_root/native/tests/windows_adapter_test.cc" \
  "$jxl_archive" \
  -o "$binary"

ASAN_OPTIONS=detect_leaks=0 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  "$binary" \
  "$repository_root/example/integration_test/2.jpg" \
  "$repository_root/example/integration_test/1.jxl"
