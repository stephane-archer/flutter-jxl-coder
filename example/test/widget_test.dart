import 'package:flutter_test/flutter_test.dart';

import 'package:jxl_coder_example/main.dart';

void main() {
  testWidgets('describes the package purpose', (tester) async {
    await tester.pumpWidget(const MyApp());

    expect(find.text('JXL Coder example'), findsOneWidget);
    expect(find.text('JPEG ↔ JPEG XL lossless transcoding'), findsOneWidget);
  });
}
