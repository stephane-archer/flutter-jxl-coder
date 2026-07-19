#!/bin/sh
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_directory=$(mktemp -d "${TMPDIR:-/tmp}/jxl_windows_scheduler_tsan.XXXXXX")
trap 'rm -rf "$build_directory"' EXIT HUP INT TERM

headers="$repository_root/macos/jxl_coder/Frameworks/JxlCoderNative.xcframework/macos-arm64_x86_64/Headers"
binary="$build_directory/windows_scheduler_test"

clang++ \
  -std=c++17 \
  -Wall \
  -Wextra \
  -Wpedantic \
  -Werror \
  -O1 \
  -g \
  -fsanitize=thread \
  -fno-omit-frame-pointer \
  -I"$headers" \
  "$repository_root/native/src/shared_parallel_runner.cc" \
  "$repository_root/native/tests/windows_scheduler_test.cc" \
  -o "$binary"

TSAN_OPTIONS=halt_on_error=1 \
  "$binary"
for mode in --lazy --automatic --configuration-race --configuration-first-use-race --configure-conflict --lazy-conflict; do
  TSAN_OPTIONS=halt_on_error=1 "$binary" "$mode"
done
