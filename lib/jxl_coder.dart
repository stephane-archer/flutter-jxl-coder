import 'package:flutter/services.dart';

import 'jxl_coder_platform_interface.dart';

class JxlCoder {
  static const MethodChannel _channel = MethodChannel('jxl_coder');

  Future<String?> getPlatformVersion() {
    return JxlCoderPlatform.instance.getPlatformVersion();
  }

  /// Converts JPEG data to JXL.
  static Future<Uint8List?> jpegToJxl(Uint8List jpegData) async {
    final result = await _channel.invokeMethod<Uint8List>('jpegToJxl', {
      'jpegData': jpegData,
    });
    return result;
  }

  /// Converts JXL data to JPEG.
  static Future<Uint8List?> jxlToJpeg(Uint8List jxlData) async {
    final result = await _channel.invokeMethod<Uint8List>('jxlToJpeg', {
      'jxlData': jxlData,
    });
    return result;
  }
}
