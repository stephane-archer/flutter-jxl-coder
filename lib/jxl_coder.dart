
import 'jxl_coder_platform_interface.dart';

class JxlCoder {
  Future<String?> getPlatformVersion() {
    return JxlCoderPlatform.instance.getPlatformVersion();
  }
}
