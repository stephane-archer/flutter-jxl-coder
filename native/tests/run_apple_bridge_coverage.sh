#!/bin/sh
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_directory=$(mktemp -d "${TMPDIR:-/tmp}/jxl_bridge_coverage.XXXXXX")
trap 'rm -rf "$build_directory"' EXIT HUP INT TERM

headers="$repository_root/macos/jxl_coder/Frameworks/JxlCoderNative.xcframework/macos-arm64_x86_64/Headers"
archive="$repository_root/macos/jxl_coder/Frameworks/JxlCoderNative.xcframework/macos-arm64_x86_64/libjxl_coder_native.a"
binary="$build_directory/jxl_coder_core_test"
raw_profile="$build_directory/bridge.profraw"
profile="$build_directory/bridge.profdata"

clang++ \
  -std=c++17 \
  -Wall \
  -Wextra \
  -Wpedantic \
  -Werror \
  -O0 \
  -g \
  -fobjc-arc \
  -fprofile-instr-generate \
  -fcoverage-mapping \
  -I"$headers" \
  -I"$repository_root/macos/jxl_coder/Sources/JxlCoderCore/include" \
  "$repository_root/native/tests/jxl_coder_core_test.mm" \
  "$archive" \
  -framework Foundation \
  -o "$binary"

LLVM_PROFILE_FILE="$raw_profile" \
  "$binary" \
  "$repository_root/example/integration_test/2.jpg" \
  "$repository_root/example/integration_test/1.jxl"
xcrun llvm-profdata merge -sparse "$raw_profile" -o "$profile"

report=$(xcrun llvm-cov report "$binary" \
  -instr-profile="$profile" \
  "$repository_root/macos/jxl_coder/Sources/JxlCoderCore/JxlCoderCore.mm")
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
    if (functions + 0 < 100 || lines + 0 < 100 || branches + 0 < 97) exit 1
  }'
