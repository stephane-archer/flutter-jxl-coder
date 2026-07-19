#!/bin/sh
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_directory=$(mktemp -d "${TMPDIR:-/tmp}/jxl_windows_scheduler_coverage.XXXXXX")
trap 'rm -rf "$build_directory"' EXIT HUP INT TERM

headers="$repository_root/macos/jxl_coder/Frameworks/JxlCoderNative.xcframework/macos-arm64_x86_64/Headers"
binary="$build_directory/windows_scheduler_test"
profile="$build_directory/windows_scheduler.profdata"

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
  -I"$headers" \
  "$repository_root/native/src/shared_parallel_runner.cc" \
  "$repository_root/native/tests/windows_scheduler_test.cc" \
  -o "$binary"

LLVM_PROFILE_FILE="$build_directory/default.profraw" "$binary"
for mode in lazy automatic configuration-race configuration-first-use-race configure-conflict lazy-conflict; do
  LLVM_PROFILE_FILE="$build_directory/$mode.profraw" "$binary" "--$mode"
done
xcrun llvm-profdata merge -sparse "$build_directory"/*.profraw -o "$profile"
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
    if (functions + 0 < 100 || lines + 0 < 97.5 || branches + 0 < 91) exit 1
  }'
