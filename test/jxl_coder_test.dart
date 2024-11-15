import 'package:flutter_test/flutter_test.dart';
import 'package:jxl_coder/jxl_coder.dart';
import 'package:jxl_coder/jxl_coder_platform_interface.dart';
import 'package:jxl_coder/jxl_coder_method_channel.dart';
import 'package:plugin_platform_interface/plugin_platform_interface.dart';

class MockJxlCoderPlatform
    with MockPlatformInterfaceMixin
    implements JxlCoderPlatform {

  @override
  Future<String?> getPlatformVersion() => Future.value('42');
}

void main() {
  final JxlCoderPlatform initialPlatform = JxlCoderPlatform.instance;

  test('$MethodChannelJxlCoder is the default instance', () {
    expect(initialPlatform, isInstanceOf<MethodChannelJxlCoder>());
  });

  test('getPlatformVersion', () async {
    JxlCoder jxlCoderPlugin = JxlCoder();
    MockJxlCoderPlatform fakePlatform = MockJxlCoderPlatform();
    JxlCoderPlatform.instance = fakePlatform;

    expect(await jxlCoderPlugin.getPlatformVersion(), '42');
  });
}
