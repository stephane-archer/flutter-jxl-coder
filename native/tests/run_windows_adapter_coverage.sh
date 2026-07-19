#!/bin/sh
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_directory=$(mktemp -d "${TMPDIR:-/tmp}/jxl_windows_adapter_coverage.XXXXXX")
trap 'rm -rf "$build_directory"' EXIT HUP INT TERM

flutter_headers=$("$repository_root/native/tests/find_flutter_cpp_headers.sh")
jxl_headers="$repository_root/macos/jxl_coder/Frameworks/JxlCoderNative.xcframework/macos-arm64_x86_64/Headers"
jxl_archive="$repository_root/macos/jxl_coder/Frameworks/JxlCoderNative.xcframework/macos-arm64_x86_64/libjxl_coder_native.a"
binary="$build_directory/windows_adapter_test"
raw_profile="$build_directory/windows_adapter.profraw"
profile="$build_directory/windows_adapter.profdata"

clang++ \
  -std=c++17 \
  -Wall \
  -Wextra \
  -Wpedantic \
  -Werror \
  -O0 \
  -g \
  -fprofile-instr-generate \
  -fcoverage-mapping \
  -I"$flutter_headers" \
  -I"$jxl_headers" \
  "$repository_root/native/src/jxl_codec.cc" \
  "$repository_root/native/src/shared_parallel_runner.cc" \
  "$repository_root/native/tests/windows_adapter_test.cc" \
  "$jxl_archive" \
  -o "$binary"

LLVM_PROFILE_FILE="$raw_profile" \
  "$binary" \
  "$repository_root/example/integration_test/2.jpg" \
  "$repository_root/example/integration_test/1.jxl"
xcrun llvm-profdata merge -sparse "$raw_profile" -o "$profile"
report=$(xcrun llvm-cov report "$binary" \
  -instr-profile="$profile" \
  "$repository_root/windows/jxl_coder_plugin.cpp")
printf '%s\n' "$report"

set -- $(printf '%s\n' "$report" | awk '
  $1 == "TOTAL" {
    gsub("%", "", $7)
    gsub("%", "", $10)
    gsub("%", "", $13)
    print $7, $10, $13
  }
')
function_coverage=${1:-0}
line_coverage=${2:-0}
branch_coverage=${3:-0}
awk \
  -v functions="$function_coverage" \
  -v lines="$line_coverage" \
  -v branches="$branch_coverage" \
  'BEGIN {
    if (functions + 0 < 100 || lines + 0 < 90 || branches + 0 < 84) exit 1
  }'
