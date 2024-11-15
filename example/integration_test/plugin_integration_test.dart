// This is a basic Flutter integration test.
//
// Since integration tests run in a full Flutter application, they can interact
// with the host side of a plugin implementation, unlike Dart unit tests.
//
// For more information about Flutter integration tests, please see
// https://flutter.dev/to/integration-testing

import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

import 'package:jxl_coder/jxl_coder.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('getPlatformVersion test', (WidgetTester tester) async {
    final JxlCoder plugin = JxlCoder();
    final String? version = await plugin.getPlatformVersion();
    // The version string depends on the host platform running the test, so
    // just assert that some non-empty string is returned.
    expect(version?.isNotEmpty, true);
  });

  testWidgets('inverseJXLToJPEG test', (WidgetTester tester) async {
    File file = File("integration_test/1.jxl");
    Uint8List jxlData = await file.readAsBytes();
    final Uint8List? jpegData = await JxlCoder.inverseJXLToJPEG(jxlData);
    expect(jpegData?.isNotEmpty, true);
    File output = File("integration_test/1.out.jpg");
    output.writeAsBytesSync(jxlData);
  });
}
