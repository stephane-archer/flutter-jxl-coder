#!/bin/sh
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
verification_directory=$(mktemp -d "${TMPDIR:-/tmp}/jxl_verify.XXXXXX")
trap 'rm -rf "$verification_directory"' EXIT HUP INT TERM

macos_archive="$repository_root/macos/jxl_coder/Frameworks/JxlCoderNative.xcframework/macos-arm64_x86_64/libjxl_coder_native.a"
ios_archive="$repository_root/ios/jxl_coder/Frameworks/JxlCoderNative.xcframework/ios-arm64/libjxl_coder_native-ios-arm64.a"
simulator_archive="$repository_root/ios/jxl_coder/Frameworks/JxlCoderNative.xcframework/ios-arm64_x86_64-simulator/libjxl_coder_native-simulator.a"

verify_hash() {
  archive=$1
  expected=$2
  actual=$(shasum -a 256 "$archive" | awk '{print $1}')
  if [ "$actual" != "$expected" ]; then
    echo "Hash mismatch: $archive"
    exit 1
  fi
}

verify_slice() {
  archive=$1
  architecture=$2
  slice="$verification_directory/$(basename "$archive").$architecture"
  architectures=$(lipo -archs "$archive")
  if [ "$architectures" = "$architecture" ]; then
    cp "$archive" "$slice"
  else
    lipo "$archive" -thin "$architecture" -output "$slice"
  fi
  if ar -t "$slice" | grep -Eq '^(jc.*\.(c|asm)\.o|jd.*\.(c|asm)\.o|jf.*\.(c|asm)\.o|ji.*\.(c|asm)\.o|jmem.*\.c\.o|jpeg_nbits.*\.c\.o|jquant.*\.(c|asm)\.o|jutils.*\.c\.o|jaricom.*\.c\.o|jsimd.*\.(c|asm)\.o)$'; then
    echo "libjpeg-turbo object found in $archive ($architecture)"
    exit 1
  fi
  if ar -t "$slice" | grep -Eq '^(resizable_parallel_runner|thread_parallel_runner|thread_parallel_runner_internal)\.cc\.o$'; then
    echo "Unused libjxl runner object found in $archive ($architecture)"
    exit 1
  fi
}

verify_hash "$macos_archive" 30d2fa76da17d8650bc0217d7f6d0175a50f1d5c9c34fc360ff0d012db0dd596
verify_hash "$ios_archive" 873a792d5e583f9ba263c3dd3fcbfa9af8b443e4c4abc4d55092e894a3e57c5d
verify_hash "$simulator_archive" 065405d233345a0fc5a4dd64949e2386e777b0ab98dc4d80f0f4ec0c1a5eb5c6

verify_slice "$macos_archive" arm64
verify_slice "$macos_archive" x86_64
verify_slice "$ios_archive" arm64
verify_slice "$simulator_archive" arm64
verify_slice "$simulator_archive" x86_64

if find "$repository_root/ios/jxl_coder/Frameworks" \
    "$repository_root/macos/jxl_coder/Frameworks" \
    -type f \( -name jpeglib.h -o -name turbojpeg.h \) | grep -q .; then
  echo "Unused libjpeg header found in an Apple framework"
  exit 1
fi

if find "$repository_root/ios/jxl_coder/Frameworks" \
    "$repository_root/macos/jxl_coder/Frameworks" \
    -type f \( -name jxl_threads_export.h \
      -o -name resizable_parallel_runner.h \
      -o -name resizable_parallel_runner_cxx.h \
      -o -name thread_parallel_runner.h \
      -o -name thread_parallel_runner_cxx.h \) | grep -q .; then
  echo "Unused libjxl runner header found in an Apple framework"
  exit 1
fi

echo "Apple static artifacts and unused codec exclusions verified"
