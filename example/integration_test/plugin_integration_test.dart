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
import 'package:collection/collection.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('getPlatformVersion test', (WidgetTester tester) async {
    final JxlCoder plugin = JxlCoder();
    final String? version = await plugin.getPlatformVersion();
    // The version string depends on the host platform running the test, so
    // just assert that some non-empty string is returned.
    expect(version, isNotNull);
    if (version != null) {
      return;
    }
    expect(version?.isNotEmpty, true);
  });

  testWidgets('jxlToJpeg test', (WidgetTester tester) async {
    File file = File("integration_test/1.jxl");
    Uint8List jxlData = await file.readAsBytes();
    final Uint8List? jpegData = await JxlCoder.jxlToJpeg(jxlData);
    expect(jpegData, isNotNull);
    if (jpegData == null) {
      return;
    }
    expect(jpegData.isNotEmpty, true);
    // File output = File("integration_test/1.out.jpg");
    // output.writeAsBytesSync(jpegData);
  });

  testWidgets('jpegToJxl test', (WidgetTester tester) async {
    File file = File("integration_test/2.jpg");
    Uint8List jpegData = await file.readAsBytes();
    final Uint8List? jxlData = await JxlCoder.jpegToJxl(jpegData);
    expect(jxlData, isNotNull);
    if (jxlData == null) {
      return;
    }
    expect(jxlData.isNotEmpty, true);
    // File output = File("integration_test/1.out.jxl");
    // output.writeAsBytesSync(jxlData);
  });

  testWidgets('jpegToJxlToJpeg test', (WidgetTester tester) async {
    File file = File("integration_test/2.jpg");
    Uint8List jpegData = await file.readAsBytes();
    final Uint8List? jxlData = await JxlCoder.jpegToJxl(jpegData);
    expect(jxlData, isNotNull);
    if (jxlData == null) {
      return;
    }
    expect(jxlData.isNotEmpty, true);
    final Uint8List? jpegDataReturn = await JxlCoder.jxlToJpeg(jxlData);
    expect(jpegDataReturn, isNotNull);
    if (jpegDataReturn == null) {
      return;
    }
    expect(jpegDataReturn.isNotEmpty, true);
    expect(const ListEquality().equals(jpegData, jpegDataReturn), isTrue);
  });
}
