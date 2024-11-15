import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'jxl_coder_platform_interface.dart';

/// An implementation of [JxlCoderPlatform] that uses method channels.
class MethodChannelJxlCoder extends JxlCoderPlatform {
  /// The method channel used to interact with the native platform.
  @visibleForTesting
  final methodChannel = const MethodChannel('jxl_coder');

  @override
  Future<String?> getPlatformVersion() async {
    final version = await methodChannel.invokeMethod<String>('getPlatformVersion');
    return version;
  }
}
