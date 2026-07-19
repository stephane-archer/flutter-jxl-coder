#!/bin/sh
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_directory=$(mktemp -d "${TMPDIR:-/tmp}/jxl_codec_tsan.XXXXXX")
trap 'rm -rf "$build_directory"' EXIT HUP INT TERM

headers="$repository_root/macos/jxl_coder/Frameworks/JxlCoderNative.xcframework/macos-arm64_x86_64/Headers"
archive="$repository_root/macos/jxl_coder/Frameworks/JxlCoderNative.xcframework/macos-arm64_x86_64/libjxl_coder_native.a"
binary="$build_directory/jxl_codec_test"

clang++ \
  -std=c++17 \
  -Wall \
  -Wextra \
  -Wpedantic \
  -Werror \
  -O1 \
  -g \
  -DJXL_CODER_ENABLE_TEST_HOOKS=1 \
  -fsanitize=thread \
  -fno-omit-frame-pointer \
  -I"$headers" \
  "$repository_root/native/src/jxl_codec.cc" \
  "$repository_root/native/src/shared_parallel_runner.cc" \
  "$repository_root/native/tests/jxl_codec_test.cc" \
  "$archive" \
  -o "$binary"

TSAN_OPTIONS=halt_on_error=1 \
  "$binary" \
  "$repository_root/example/integration_test/2.jpg" \
  "$repository_root/example/integration_test/1.jxl"
for mode in --lazy-configuration --configuration-race --configuration-first-use-race; do
  TSAN_OPTIONS=halt_on_error=1 "$binary" "$mode"
done
