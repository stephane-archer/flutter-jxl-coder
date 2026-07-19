#!/bin/sh
set -eu

if ! flutter_executable=$(command -v flutter); then
  echo "Flutter is required for the Windows adapter tests" >&2
  exit 1
fi
derived_flutter_root=$(CDPATH= cd -- "$(dirname -- "$flutter_executable")/.." && pwd)

for flutter_root_candidate in "${FLUTTER_ROOT:-}" "$derived_flutter_root"; do
  if [ -z "$flutter_root_candidate" ]; then
    continue
  fi
  for header_candidate in \
    "$flutter_root_candidate/engine/src/flutter/shell/platform/common/client_wrapper/include" \
    "$flutter_root_candidate/bin/cache/artifacts/engine/windows-x64/cpp_client_wrapper/include"; do
    if [ -f "$header_candidate/flutter/encodable_value.h" ] &&
       [ -f "$header_candidate/flutter/method_result.h" ]; then
      printf '%s\n' "$header_candidate"
      exit 0
    fi
  done
done

echo "Flutter C++ client-wrapper headers were not found" >&2
exit 1
