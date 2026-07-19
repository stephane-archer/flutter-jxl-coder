import 'dart:typed_data';

import 'jxl_types.dart';
import 'method_channel_jxl_coder.dart' as native;

/// Configures the process-wide native scheduler.
///
/// Calling this function is not required. A custom configuration must be
/// installed before the first conversion starts. Repeating an effectively
/// identical configuration is allowed.
///
/// [workerCount] accepts values from 0 through 256. Zero uses the platform's
/// logical CPU count, capped at 256.
///
/// [maxActiveConversions] accepts values from 0 through 256. Zero selects the
/// platform policy. Unlike [workerCount], this bounds complete image pipelines
/// including input access, codec instances, output buffers, and writing.
Future<void> configureJxlScheduler({
  int workerCount = 0,
  int maxActiveConversions = 0,
}) {
  _validateSchedulerConfiguration(workerCount, maxActiveConversions);
  return native.configureJxlScheduler(workerCount, maxActiveConversions);
}

/// Converts JPEG data to JXL without re-encoding the image pixels.
///
/// Use this function when the JPEG is already in memory. For file-backed data,
/// [jpegPathToJxl] avoids transferring the image through the Dart heap and
/// platform channel.
///
/// [encoding] controls encoder effort and the future JXL decoding-speed
/// trade-off. [execution] controls scheduling priority and the deadline.
///
/// Four-component CMYK JPEGs are not supported by the native lossless
/// reconstruction path. They produce a `PlatformException` whose code is
/// `UNSUPPORTED_INPUT`.
Future<Uint8List> jpegBytesToJxl(
  Uint8List jpegData, {
  JxlEncodeOptions encoding = const JxlEncodeOptions(),
  JxlExecutionOptions execution = const JxlExecutionOptions(),
}) {
  _validateEncodeOptions(encoding);
  _validateExecution(execution);
  return native.jpegBytesToJxl(jpegData, encoding, execution);
}

/// Converts in-memory JXL data to the original JPEG bytes.
///
/// The JXL input must contain JPEG reconstruction data. Arbitrary pixel-encoded
/// JXL images are rejected with `UNSUPPORTED_INPUT`; this package never creates
/// a new lossy JPEG from decoded pixels.
///
/// Use [jxlPathToJpeg] when both the input and output are files.
Future<Uint8List> jxlBytesToJpeg(
  Uint8List jxlData, {
  JxlExecutionOptions execution = const JxlExecutionOptions(),
}) {
  _validateExecution(execution);
  return native.jxlBytesToJpeg(jxlData, execution);
}

/// Converts a JPEG file to JXL and saves it to the specified output path.
///
/// This is the preferred function for file-backed data because native code can
/// map the input and write the output without copying the full image through
/// Dart.
///
/// [encoding] controls encoder effort and the future JXL decoding-speed
/// trade-off. [execution] controls scheduling priority and the deadline.
///
/// In a sandboxed macOS app, pass [macOSSecurityScopedBookmarks] for URLs
/// restored from security-scoped bookmarks. For a URL received directly from
/// a system picker, the host may instead keep that original URL's security
/// scope active until this future completes. A path string alone cannot carry
/// a security scope. The host app needs the User Selected File Read/Write
/// entitlement in either case.
///
/// Four-component CMYK JPEGs are not supported by the native lossless
/// reconstruction path. They produce a `PlatformException` whose code is
/// `UNSUPPORTED_INPUT`.
Future<void> jpegPathToJxl(
  String inputPath,
  String outputPath, {
  JxlEncodeOptions encoding = const JxlEncodeOptions(),
  JxlExecutionOptions execution = const JxlExecutionOptions(),
  JxlMacOSSecurityScopedBookmarks? macOSSecurityScopedBookmarks,
}) {
  _validateEncodeOptions(encoding);
  _validateExecution(execution);
  return native.jpegPathToJxl(
    inputPath,
    outputPath,
    encoding,
    execution,
    macOSSecurityScopedBookmarks,
  );
}

/// Converts a JXL file to JPEG and saves it to the specified output path.
///
/// The JXL input must contain JPEG reconstruction data. Arbitrary pixel-encoded
/// JXL images are rejected with `UNSUPPORTED_INPUT`.
///
/// This is the preferred inverse function when both endpoints are files.
///
/// In a sandboxed macOS app, use [macOSSecurityScopedBookmarks] or keep the
/// original user-selected URLs' security scopes active until this future
/// completes. See [jpegPathToJxl] for the complete host-app requirements.
Future<void> jxlPathToJpeg(
  String inputPath,
  String outputPath, {
  JxlExecutionOptions execution = const JxlExecutionOptions(),
  JxlMacOSSecurityScopedBookmarks? macOSSecurityScopedBookmarks,
}) {
  _validateExecution(execution);
  return native.jxlPathToJpeg(
    inputPath,
    outputPath,
    execution,
    macOSSecurityScopedBookmarks,
  );
}

/// Converts [pathPairs] in one platform message and runs them concurrently
/// in the native worker pool.
///
/// For multiple in-memory inputs, use concurrent [jpegBytesToJxl] calls.
///
/// [encoding] applies to every entry in the batch. [execution] controls the
/// priority and per-image deadline.
///
/// All scheduled conversions finish before an error is reported. Outputs
/// completed before that error are not removed.
///
/// On macOS, [macOSSecurityScopedBookmarks] must either be null or contain one
/// entry per path pair in the same order. See [jpegPathToJxl] for scope rules.
Future<void> jpegPathsToJxl(
  List<JxlPathPair> pathPairs, {
  JxlEncodeOptions encoding = const JxlEncodeOptions(),
  JxlExecutionOptions execution = const JxlExecutionOptions(),
  List<JxlMacOSSecurityScopedBookmarks>? macOSSecurityScopedBookmarks,
}) {
  _validateEncodeOptions(encoding);
  _validateExecution(execution);
  _validateSecurityScopedBookmarks(pathPairs, macOSSecurityScopedBookmarks);
  return native.jpegPathsToJxl(
    pathPairs,
    encoding,
    execution,
    macOSSecurityScopedBookmarks,
  );
}

/// Restores [pathPairs] in one platform message and runs them concurrently
/// in the native worker pool.
///
/// Every JXL input must contain JPEG reconstruction data.
///
/// All scheduled conversions finish before an error is reported. Outputs
/// completed before that error are not removed.
///
/// On macOS, [macOSSecurityScopedBookmarks] must either be null or contain one
/// entry per path pair in the same order. See [jpegPathToJxl] for scope rules.
Future<void> jxlPathsToJpeg(
  List<JxlPathPair> pathPairs, {
  JxlExecutionOptions execution = const JxlExecutionOptions(),
  List<JxlMacOSSecurityScopedBookmarks>? macOSSecurityScopedBookmarks,
}) {
  _validateExecution(execution);
  _validateSecurityScopedBookmarks(pathPairs, macOSSecurityScopedBookmarks);
  return native.jxlPathsToJpeg(
    pathPairs,
    execution,
    macOSSecurityScopedBookmarks,
  );
}

void _validateSecurityScopedBookmarks(
  List<JxlPathPair> pathPairs,
  List<JxlMacOSSecurityScopedBookmarks>? bookmarks,
) {
  if (bookmarks != null && bookmarks.length != pathPairs.length) {
    throw ArgumentError.value(
      bookmarks,
      'macOSSecurityScopedBookmarks',
      'must contain one entry per path pair',
    );
  }
}

void _validateEncodeOptions(JxlEncodeOptions encoding) {
  _validateEffort(encoding.effort);
  if (encoding.decodingSpeed < 0 || encoding.decodingSpeed > 4) {
    throw RangeError.range(encoding.decodingSpeed, 0, 4, 'decodingSpeed');
  }
}

void _validateEffort(int effort) {
  if (effort < 1 || effort > 9) {
    throw RangeError.range(effort, 1, 9, 'effort');
  }
}

void _validateSchedulerConfiguration(
  int workerCount,
  int maxActiveConversions,
) {
  if (workerCount < 0 || workerCount > 256) {
    throw RangeError.range(workerCount, 0, 256, 'workerCount');
  }
  if (maxActiveConversions < 0 || maxActiveConversions > 256) {
    throw RangeError.range(
      maxActiveConversions,
      0,
      256,
      'maxActiveConversions',
    );
  }
}

void _validateExecution(JxlExecutionOptions execution) {
  _validateTimeout(execution.timeout);
}

void _validateTimeout(Duration? timeout) {
  if (timeout == null) return;
  if (timeout <= Duration.zero) {
    throw ArgumentError.value(timeout, 'timeout', 'must be positive');
  }
}
