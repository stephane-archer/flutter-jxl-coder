#!/bin/sh
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_directory=$(mktemp -d "${TMPDIR:-/tmp}/jxl_codec_coverage.XXXXXX")
trap 'rm -rf "$build_directory"' EXIT HUP INT TERM

headers="$repository_root/macos/jxl_coder/Frameworks/JxlCoderNative.xcframework/macos-arm64_x86_64/Headers"
archive="$repository_root/macos/jxl_coder/Frameworks/JxlCoderNative.xcframework/macos-arm64_x86_64/libjxl_coder_native.a"
binary="$build_directory/jxl_codec_test"
profile="$build_directory/jxl_codec.profdata"

clang++ \
  -std=c++17 \
  -Wall \
  -Wextra \
  -Wpedantic \
  -Werror \
  -O0 \
  -g \
  -DJXL_CODER_ENABLE_TEST_HOOKS=1 \
  -fprofile-instr-generate \
  -fcoverage-mapping \
  -I"$headers" \
  "$repository_root/native/src/jxl_codec.cc" \
  "$repository_root/native/src/shared_parallel_runner.cc" \
  "$repository_root/native/tests/jxl_codec_test.cc" \
  "$archive" \
  -o "$binary"

LLVM_PROFILE_FILE="$build_directory/default.profraw" \
  "$binary" \
  "$repository_root/example/integration_test/2.jpg" \
  "$repository_root/example/integration_test/1.jxl"
for mode in lazy-configuration configuration-race configuration-first-use-race; do
  LLVM_PROFILE_FILE="$build_directory/$mode.profraw" "$binary" "--$mode"
done

xcrun llvm-profdata merge -sparse "$build_directory"/*.profraw -o "$profile"
coverage_report=$(xcrun llvm-cov report \
  "$binary" \
  -instr-profile="$profile" \
  "$repository_root/native/src/jxl_codec.cc" \
  "$repository_root/native/src/shared_parallel_runner.cc")
printf '%s\n' "$coverage_report"

set -- $(printf '%s\n' "$coverage_report" | awk '
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
    if (functions + 0 < 100 || lines + 0 < 98 || branches + 0 < 86.5) exit 1
  }'
