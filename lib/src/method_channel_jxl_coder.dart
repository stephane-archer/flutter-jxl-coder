import 'package:flutter/services.dart';

import 'jxl_types.dart';

const MethodChannel _channel = MethodChannel('jxl_coder');

Future<void> configureJxlScheduler(
  int workerCount,
  int maxActiveConversions,
) {
  return _channel.invokeMethod<void>('configureJxlScheduler', [
    workerCount,
    maxActiveConversions,
  ]);
}

Future<Uint8List> jpegBytesToJxl(
  Uint8List jpegData,
  JxlEncodeOptions encoding,
  JxlExecutionOptions execution,
) async {
  return await _channel.invokeMethod<Uint8List>('jpegBytesToJxl', [
        jpegData,
        encoding.effort,
        encoding.decodingSpeed,
        _priorityValue(execution.priority),
        _timeoutMilliseconds(execution.timeout),
      ]) ??
      (throw StateError('The native JPEG encoder returned no data'));
}

Future<Uint8List> jxlBytesToJpeg(
  Uint8List jxlData,
  JxlExecutionOptions execution,
) async {
  return await _channel.invokeMethod<Uint8List>('jxlBytesToJpeg', [
        jxlData,
        _priorityValue(execution.priority),
        _timeoutMilliseconds(execution.timeout),
      ]) ??
      (throw StateError('The native JPEG decoder returned no data'));
}

Future<void> jpegPathToJxl(
  String inputPath,
  String outputPath,
  JxlEncodeOptions encoding,
  JxlExecutionOptions execution,
  JxlMacOSSecurityScopedBookmarks? macOSSecurityScopedBookmarks,
) {
  return _channel.invokeMethod<void>('jpegPathToJxl', [
    inputPath,
    outputPath,
    encoding.effort,
    encoding.decodingSpeed,
    _priorityValue(execution.priority),
    _timeoutMilliseconds(execution.timeout),
    if (macOSSecurityScopedBookmarks != null)
      _bookmarkArguments(<JxlMacOSSecurityScopedBookmarks>[
        macOSSecurityScopedBookmarks,
      ]),
  ]);
}

Future<void> jxlPathToJpeg(
  String inputPath,
  String outputPath,
  JxlExecutionOptions execution,
  JxlMacOSSecurityScopedBookmarks? macOSSecurityScopedBookmarks,
) {
  return _channel.invokeMethod<void>('jxlPathToJpeg', [
    inputPath,
    outputPath,
    _priorityValue(execution.priority),
    _timeoutMilliseconds(execution.timeout),
    if (macOSSecurityScopedBookmarks != null)
      _bookmarkArguments(<JxlMacOSSecurityScopedBookmarks>[
        macOSSecurityScopedBookmarks,
      ]),
  ]);
}

Future<void> jpegPathsToJxl(
  List<JxlPathPair> pathPairs,
  JxlEncodeOptions encoding,
  JxlExecutionOptions execution,
  List<JxlMacOSSecurityScopedBookmarks>? macOSSecurityScopedBookmarks,
) {
  return _channel.invokeMethod<void>('jpegPathsToJxl', [
    encoding.effort,
    encoding.decodingSpeed,
    _priorityValue(execution.priority),
    _timeoutMilliseconds(execution.timeout),
    for (final pathPair in pathPairs) ...[
      pathPair.inputPath,
      pathPair.outputPath,
    ],
    if (macOSSecurityScopedBookmarks != null)
      _bookmarkArguments(macOSSecurityScopedBookmarks),
  ]);
}

Future<void> jxlPathsToJpeg(
  List<JxlPathPair> pathPairs,
  JxlExecutionOptions execution,
  List<JxlMacOSSecurityScopedBookmarks>? macOSSecurityScopedBookmarks,
) {
  return _channel.invokeMethod<void>('jxlPathsToJpeg', [
    _priorityValue(execution.priority),
    _timeoutMilliseconds(execution.timeout),
    for (final pathPair in pathPairs) ...[
      pathPair.inputPath,
      pathPair.outputPath,
    ],
    if (macOSSecurityScopedBookmarks != null)
      _bookmarkArguments(macOSSecurityScopedBookmarks),
  ]);
}

Map<String, Object> _bookmarkArguments(
  List<JxlMacOSSecurityScopedBookmarks> bookmarks,
) {
  return <String, Object>{
    'securityScopedBookmarks': <Uint8List?>[
      for (final bookmark in bookmarks) ...[bookmark.input, bookmark.output],
    ],
  };
}

int _timeoutMilliseconds(Duration? timeout) {
  if (timeout == null) return 0;
  final milliseconds = timeout.inMilliseconds;
  return milliseconds == 0 ? 1 : milliseconds;
}

int _priorityValue(JxlTaskPriority priority) => switch (priority) {
      JxlTaskPriority.low => 0,
      JxlTaskPriority.normal => 1,
      JxlTaskPriority.high => 2,
    };
