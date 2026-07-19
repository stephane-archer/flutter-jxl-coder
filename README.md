# JPEG-to-JPEG XL compression for Flutter

Compress JPEG files and bytes to JPEG XL directly from your Flutter app.
`jxl_coder` is for apps that want to store or send their JPEG assets as JPEG XL
and later convert that JPEG XL back to the original JPEG without any loss.

Unlike an ordinary image conversion, JPEG-to-JPEG XL compression in
`jxl_coder` is reversible: when it converts a JPEG to JPEG XL, it stores the
information needed to recreate the original JPEG file inside the generated
JPEG XL file. Converting that JPEG XL back to JPEG restores the **exact original
JPEG file byte-for-byte, including its metadata**. The restored file is more
than visually or pixel-identical. It is an exact copy of the original JPEG.
This makes it suitable for JPEG archives, asset pipelines, and
bandwidth-conscious transfers that still need a compatible JPEG later.

## Platforms

| Platform | Architectures | Minimum | Packaging |
|---|---|---:|---|
| macOS | universal arm64/x86_64 | 12 | static archive in XCFramework |
| iOS device | arm64 | 13 | static archive in XCFramework |
| iOS simulator | arm64/x86_64 | 13 | static archive in XCFramework |
| Windows (provisional) | x64 | Windows 10 | static codec linked into `jxl_coder_plugin.dll` |
| Windows Arm64 | unsupported | N/A | N/A |
| Android, Linux, Web | unsupported | N/A | N/A |

The native codec is libjxl 0.12.0. Source versions, hashes, build flags,
license locations, and the SBOM are recorded in
[native/PROVENANCE.md](native/PROVENANCE.md).

## Usage

Convert bytes already held in memory:

```dart
import 'dart:typed_data';

import 'package:jxl_coder/jxl_coder.dart';

final Uint8List encoded = await jpegBytesToJxl(jpegBytes);
final Uint8List restored = await jxlBytesToJpeg(encoded);
```

No initialization is required. Applications that need explicit process-wide
limits may configure them before the first conversion while retaining the same
top-level functions:

```dart
await configureJxlScheduler(
  workerCount: 6,
  maxActiveConversions: 3,
);
```

Repeating an effectively identical configuration is allowed. Changing it after
conversion work begins throws `SCHEDULER_ALREADY_STARTED`.

Prefer path APIs for files so image data does not cross the Dart platform
channel:

```dart
await jpegPathToJxl(
  inputJpeg.path,
  outputJxl.path,
  encoding: const JxlEncodeOptions(
    effort: 7,
    decodingSpeed: 0,
  ),
  execution: const JxlExecutionOptions(
    priority: JxlTaskPriority.high,
    timeout: Duration(minutes: 2),
  ),
);

await jxlPathToJpeg(
  outputJxl.path,
  restoredJpeg.path,
  execution: const JxlExecutionOptions(
    timeout: Duration(minutes: 2),
  ),
);
```

`effort` accepts 1–9 and defaults to 7. `decodingSpeed` accepts 0–4 and
defaults to 0, matching the size-optimized `cjxl` behavior. Every accepted
setting still reconstructs the source JPEG exactly.

Batch path APIs cross the channel once:

```dart
await jpegPathsToJxl(
  const [
    (inputPath: r'C:\images\été.jpg', outputPath: r'C:\images\été.jxl'),
    (inputPath: r'C:\images\日本語.jpg', outputPath: r'C:\images\日本語.jxl'),
  ],
  execution: const JxlExecutionOptions(
    priority: JxlTaskPriority.normal,
  ),
);
```

Windows paths are converted from UTF-8 to UTF-16 and normalized with the
extended-length prefix, preserving Unicode and long paths.

### macOS App Sandbox

Mac App Store hosts must enable both `com.apple.security.app-sandbox` and
`com.apple.security.files.user-selected.read-write`. Keep Debug and Profile
sandboxed too so development and CI exercise the same filesystem boundary as
Release.

A path string does not preserve the security scope of the URL returned by an
open or save panel. For a selection used immediately, retain the original URL
and its granted access until the complete path conversion has been awaited.
Follow the picker API's contract for implicit versus explicit start, and
balance every started access with one stop in `defer`/`finally`. For persisted
selections, create a read/write security-scoped bookmark from that original URL
and pass its bytes to the path operation:

```dart
await jpegPathToJxl(
  inputPath,
  outputPath,
  macOSSecurityScopedBookmarks: JxlMacOSSecurityScopedBookmarks(
    input: inputBookmarkData,
    output: outputBookmarkData,
  ),
);
```

The plugin resolves supplied bookmarks on macOS, verifies that each bookmark
contains its requested path, starts access before filesystem I/O, and always
stops access after the asynchronous read, codec, and atomic write have all
finished. A folder bookmark may be used for a child input or output. Callers
that do not supply bookmarks remain responsible for keeping the original
security-scoped URLs active until the returned future completes. See Apple's
[App Sandbox file-access guidance](https://developer.apple.com/documentation/security/accessing-files-from-the-macos-app-sandbox).

## Packaging and signing

This package owns and versions the static codec artifacts. Applications should
not resolve or bundle loose codec executables.

Do not sign the `.a` or `.lib` inputs. Verify their source, release hashes, and
provenance. On Windows the static code becomes part of
`jxl_coder_plugin.dll`. A Store MSIX signature covers that plugin with the rest
of the final package. A directly distributed installer or MSIX must be signed
by the application publisher, not by this package with a generic identity.