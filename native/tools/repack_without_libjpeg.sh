#!/bin/sh
set -eu

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_directory=$(mktemp -d "${TMPDIR:-/tmp}/jxl_repack.XXXXXX")
trap 'rm -rf "$build_directory"' EXIT HUP INT TERM

strip_slice() {
  source_archive=$1
  architecture=$2
  destination=$3
  architectures=$(lipo -archs "$source_archive")
  if [ "$architectures" = "$architecture" ]; then
    cp "$source_archive" "$destination"
  else
    lipo "$source_archive" -thin "$architecture" -output "$destination"
  fi
  member_list="$destination.unused-members"
  ar -t "$destination" | while IFS= read -r member; do
    case "$member" in
      jc*.c.o|jd*.c.o|jf*.c.o|ji*.c.o|jmem*.c.o|jpeg_nbits*.c.o|jquant*.c.o|jutils*.c.o|jaricom*.c.o|jsimd*.c.o|jc*.asm.o|jd*.asm.o|jf*.asm.o|ji*.asm.o|jquant*.asm.o|jsimd*.asm.o|resizable_parallel_runner.cc.o|thread_parallel_runner.cc.o|thread_parallel_runner_internal.cc.o)
        printf '%s\n' "$member"
        ;;
    esac
  done > "$member_list"
  if [ -s "$member_list" ]; then
    xargs ar -d "$destination" < "$member_list"
  fi
  rm -f "$member_list"
  ranlib "$destination"
}

repack_fat() {
  source_archive=$1
  output_archive=$2
  arm_slice="$build_directory/$(basename "$source_archive").arm64"
  x86_slice="$build_directory/$(basename "$source_archive").x86_64"
  strip_slice "$source_archive" arm64 "$arm_slice"
  strip_slice "$source_archive" x86_64 "$x86_slice"
  lipo -create "$arm_slice" "$x86_slice" -output "$output_archive"
}

macos_archive="$repository_root/macos/jxl_coder/Frameworks/JxlCoderNative.xcframework/macos-arm64_x86_64/libjxl_coder_native.a"
ios_archive="$repository_root/ios/jxl_coder/Frameworks/JxlCoderNative.xcframework/ios-arm64/libjxl_coder_native-ios-arm64.a"
simulator_archive="$repository_root/ios/jxl_coder/Frameworks/JxlCoderNative.xcframework/ios-arm64_x86_64-simulator/libjxl_coder_native-simulator.a"

repack_fat "$macos_archive" "$build_directory/macos.a"
strip_slice "$ios_archive" arm64 "$build_directory/ios.a"
repack_fat "$simulator_archive" "$build_directory/simulator.a"

cp "$build_directory/macos.a" "$macos_archive"
cp "$build_directory/ios.a" "$ios_archive"
cp "$build_directory/simulator.a" "$simulator_archive"

echo "Apple codec archives repacked without libjpeg-turbo or libjxl runner objects"
