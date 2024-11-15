import 'package:plugin_platform_interface/plugin_platform_interface.dart';

import 'jxl_coder_method_channel.dart';

abstract class JxlCoderPlatform extends PlatformInterface {
  /// Constructs a JxlCoderPlatform.
  JxlCoderPlatform() : super(token: _token);

  static final Object _token = Object();

  static JxlCoderPlatform _instance = MethodChannelJxlCoder();

  /// The default instance of [JxlCoderPlatform] to use.
  ///
  /// Defaults to [MethodChannelJxlCoder].
  static JxlCoderPlatform get instance => _instance;

  /// Platform-specific implementations should set this with their own
  /// platform-specific class that extends [JxlCoderPlatform] when
  /// they register themselves.
  static set instance(JxlCoderPlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  Future<String?> getPlatformVersion() {
    throw UnimplementedError('platformVersion() has not been implemented.');
  }
}
