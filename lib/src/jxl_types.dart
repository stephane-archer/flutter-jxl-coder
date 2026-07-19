import 'dart:typed_data';

/// Compression controls for lossless JPEG to JPEG XL transcoding.
final class JxlEncodeOptions {
  const JxlEncodeOptions({this.effort = 7, this.decodingSpeed = 0});

  /// libjxl encoder effort from 1 (least work) through 9 (most work).
  ///
  /// Defaults to 7, matching libjxl and `cjxl`.
  ///
  /// Reconstructing the original JPEG remains byte-for-byte lossless at every
  /// effort. Lower values are faster and generally produce a larger JXL file,
  /// but output size is not guaranteed to improve monotonically with effort.
  final int effort;

  /// Decoder speed trade-off from 0 (smallest output) through 4 (fastest).
  ///
  /// Defaults to 0, matching `cjxl` and minimizing the lossless JXL size.
  final int decodingSpeed;
}

/// Relative scheduling priority for one image conversion.
///
/// Priority only affects work competing inside this package. It does not
/// change the operating system's thread priority, and a lone low-priority
/// conversion may use the complete configured worker pool.
enum JxlTaskPriority { low, normal, high }

/// Scheduling and deadline controls for conversion work.
///
/// For a single conversion, [timeout] starts at native submission and includes
/// admission waiting. For a batch, every image receives a fresh deadline when
/// it is admitted, so time in the batch queue is excluded.
final class JxlExecutionOptions {
  const JxlExecutionOptions({
    this.priority = JxlTaskPriority.normal,
    this.timeout,
  });

  /// Relative priority used when this work competes with other conversions.
  final JxlTaskPriority priority;

  /// Optional cooperative deadline for conversion work.
  ///
  /// A null value disables the deadline. Codec work cannot be forcibly
  /// terminated; cancellation is observed at safe points.
  final Duration? timeout;
}

/// Input and output filesystem paths for one native batch operation.
typedef JxlPathPair = ({String inputPath, String outputPath});

/// macOS security-scoped bookmark data for a path conversion.
///
/// Create each bookmark from the original URL returned by an `NSOpenPanel`,
/// `NSSavePanel`, or equivalent system picker. A path reconstructed from the
/// URL does not retain its security scope.
///
/// When bookmarks are supplied to a path API, the macOS implementation
/// resolves them, starts access before touching either file, and stops access
/// after all reading, conversion, and writing has finished.
final class JxlMacOSSecurityScopedBookmarks {
  const JxlMacOSSecurityScopedBookmarks({this.input, this.output});

  /// A read/write security-scoped bookmark for the input file or its folder.
  final Uint8List? input;

  /// A read/write security-scoped bookmark for the output file or its folder.
  final Uint8List? output;
}
