# JpegXL encoding and decoding for Flutter

Thin Flutter plugin that wraps [awxkee/JxlCoder](https://github.com/awxkee/jxl-coder-swift)
(`pod 'JxlCoder'`) for **JPEG ↔ JPEG XL** conversion.

## Platforms

| Platform | Status | Backend |
|----------|--------|---------|
| macOS 12+ | Supported | CocoaPods `JxlCoder` → **libjxl** (CPU / SIMD) |
| iOS 13+ | Supported (this PR) | Same CocoaPods `JxlCoder` → **libjxl** |
| Android | Not yet | Likely [awxkee/jxl-coder](https://github.com/awxkee/jxl-coder) or `libjxl` NDK |
| Windows / Linux | Not yet | Needs `dart:ffi` (or CMake plugin) binding to **libjxl** |
| Web | Not practical | No shipping browser JXL encode API for this use case |

## Important: this is not a GPU codec

- **libjxl has no production CUDA / Metal / Vulkan path** (see [libjxl#1134](https://github.com/libjxl/libjxl/issues/1134)). Speedups land as **CPU SIMD + multithreading**.
- This plugin only exposes **JPEG bytes ↔ JXL bytes** (transcode), not RGBA encode/decode helpers for custom canvases.
- Flutter/`dart:ui` image decode (`instantiateImageCodecWithSize`) is also **native CPU** codecs (often libjpeg-turbo), not a general GPU encode/decode pipeline. Impeller/Flutter GPU is for **rasterization**, not photo codecs.

If you need faster interactive previews in an app, prefer: downsample-during-decode, smaller preview long-edge, avoid JXL on the hot path, and cache framed JPEGs — not “GPU JXL”.

## Usage

```dart
Uint8List jpegData = await file.readAsBytes();
final Uint8List? jxlData = await JxlCoder.jpegToJxl(jpegData);

Uint8List jxlData = await file.readAsBytes();
final Uint8List? jpegData = await JxlCoder.jxlToJpeg(jxlData);

await JxlCoder.saveJpegAsJxl(inputFile.path, outputFile.path);
await JxlCoder.saveJxlAsJpeg(inputFile.path, outputFile.path);
```

## Depend on a fork (until a release includes iOS)

```yaml
dependencies:
  jxl_coder:
    git:
      url: https://github.com/AMDphreak/flutter-jxl-coder.git
      ref: multiplatform-ios-docs  # or main after merge
```

## Roadmap (Windows / Linux / Android)

1. Federated plugin layout (`jxl_coder_platform_interface` + per-OS impls).
2. Desktop: ship or discover `libjxl` shared libraries and call via FFI (`JxlEncoder` / `JxlDecoder`).
3. Android: wrap awxkee’s Android AAR or link libjxl via CMake.
4. Optional: expose RGBA encode/decode (not only JPEG transcode) for apps that frame canvases in Dart.
