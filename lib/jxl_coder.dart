import 'package:flutter/services.dart';

import 'jxl_coder_platform_interface.dart';

class JxlCoder {
  static const MethodChannel _channel = MethodChannel('jxl_coder');

  Future<String?> getPlatformVersion() {
    return JxlCoderPlatform.instance.getPlatformVersion();
  }

  /// Converts JXL data to JPEG.
  static Future<Uint8List?> inverseJXLToJPEG(Uint8List jxlData) async {
    try {
      final result =
          await _channel.invokeMethod<Uint8List>('inverseJXLToJPEG', {
        'jxlData': jxlData,
      });
      return result;
    } catch (e) {
      print("Error in inverseJXLToJPEG: $e");
      return null;
    }
  }

  /// Converts JPEG data to JXL.
  static Future<Uint8List?> transcodeJPEGToJXL(Uint8List jpegData) async {
    try {
      final result =
          await _channel.invokeMethod<Uint8List>('transcodeJPEGToJXL', {
        'jpegData': jpegData,
      });
      return result;
    } catch (e) {
      print("Error in transcodeJPEGToJXL: $e");
      return null;
    }
  }
}
