import 'dart:convert';
import 'dart:io';
import 'dart:isolate';
import 'dart:math';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:jxl_coder/jxl_coder.dart';

const MethodChannel _channel = MethodChannel('jxl_coder');
const int _configuredMaxActiveConversions = 3;
const List<int> _jpegSignature = <int>[0xff, 0xd8];
const List<int> _jxlContainerSignature = <int>[
  0x00,
  0x00,
  0x00,
  0x0c,
  0x4a,
  0x58,
  0x4c,
  0x20,
  0x0d,
  0x0a,
  0x87,
  0x0a,
];
const List<String> _jpegVariants = <String>[
  'baseline_rgb.jpg',
  'progressive_rgb.jpg',
  'grayscale.jpg',
];
const String _arithmeticJpegBase64 =
    '/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoH'
    'BwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQME'
    'BAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQU'
    'FBQUFBQUFBQUFBQUFBT/yQARCAABAAEDASIAAhEBAxEB/8wACgAQEAUBEBEF/9oA'
    'DAMBAAIRAxEAPwD/AHMPVT0N2OD/2Q==';
const String _losslessJpegBase64 =
    '/9j/7gAOQWRvYmUAZAAAAAAA/8MAEQgAAQABA1IRAEcRAEIRAP/EABUAAQEAAAAA'
    'AAAAAAAAAAAAAAcB/9oADANSAEcAQgABAAAejj//2Q==';
const String _jxlWithoutJpegReconstructionBase64 =
    '/woAkAEAE4gCAMQAtZ8gAAAVKqOMG7yc6/nyQ4fFtI3rDG21bWEJY7O9MEhIOIONi'
    '8a7OS1dDGRAQipJAA==';

Matcher _platformError(String code, {bool withDetails = false}) {
  final matcher = isA<PlatformException>()
      .having((error) => error.code, 'code', code)
      .having((error) => error.message, 'message', isNotEmpty);
  return withDetails
      ? matcher.having((error) => error.details, 'details', isNotEmpty)
      : matcher;
}

Future<Uint8List> _loadFixture(String name) async {
  final ByteData data = await rootBundle.load('integration_test/$name');
  return data.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes);
}

Future<Directory> _temporaryDirectory() async {
  return Directory.systemTemp.createTemp('jxl_coder_test_');
}

Future<File> _writeFixture(Directory directory, String name) async {
  final File file = File('${directory.path}/$name');
  await file.writeAsBytes(await _loadFixture(name), flush: true);
  return file;
}

@pragma('vm:entry-point')
Future<void> _transcodeInBackgroundIsolate(List<Object> message) async {
  final RootIsolateToken token = message[0] as RootIsolateToken;
  final SendPort sendPort = message[1] as SendPort;
  final TransferableTypedData input = message[2] as TransferableTypedData;
  BackgroundIsolateBinaryMessenger.ensureInitialized(token);

  try {
    final jpeg = input.materialize().asUint8List();
    final jxl = await jpegBytesToJxl(jpeg);
    sendPort.send(TransferableTypedData.fromList(<Uint8List>[jxl]));
  } catch (error, stackTrace) {
    sendPort.send('$error\n$stackTrace');
  }
}

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  setUpAll(() async {
    await expectLater(
      _channel.invokeMethod<Object?>('jpegBytesToJxl', <Object?>[
        Uint8List(1),
        10,
        0,
        0,
        0,
      ]),
      throwsA(_platformError('INVALID_ARGUMENTS', withDetails: false)),
      reason: 'A malformed first call must not initialize the scheduler.',
    );
    await configureJxlScheduler(
      maxActiveConversions: _configuredMaxActiveConversions,
    );
    await configureJxlScheduler(
      maxActiveConversions: _configuredMaxActiveConversions,
    );
  });

  testWidgets('macOS integration host is confined by App Sandbox', (
    WidgetTester tester,
  ) async {
    if (!Platform.isMacOS) return;

    const sharedDirectory = '/Users/Shared';
    expect(await Directory(sharedDirectory).exists(), isTrue);
    final probe = File('$sharedDirectory/jxl_coder_sandbox_probe_$pid');
    var denied = false;
    try {
      await probe.writeAsString('sandbox probe', flush: true);
    } on FileSystemException {
      denied = true;
    } finally {
      if (await probe.exists()) await probe.delete();
    }
    expect(
      denied,
      isTrue,
      reason: 'A non-sandboxed debug host can write to /Users/Shared.',
    );
  });

  testWidgets('converts JXL bytes to JPEG bytes', (WidgetTester tester) async {
    final Uint8List jxlData = await _loadFixture('1.jxl');
    final Uint8List jpegData = await jxlBytesToJpeg(jxlData);

    expect(jpegData, isNotEmpty);
    expect(jpegData.take(2), orderedEquals(_jpegSignature));
  });

  testWidgets('rejects scheduler changes after conversion starts', (
    WidgetTester tester,
  ) async {
    await expectLater(
      configureJxlScheduler(maxActiveConversions: 4),
      throwsA(_platformError('SCHEDULER_ALREADY_STARTED')),
    );
  });

  testWidgets('converts JPEG bytes to JXL bytes', (WidgetTester tester) async {
    final Uint8List jpegData = await _loadFixture('2.jpg');
    final Uint8List jxlData = await jpegBytesToJxl(jpegData);

    expect(jxlData, isNotEmpty);
    expect(
      jxlData.take(_jxlContainerSignature.length),
      orderedEquals(_jxlContainerSignature),
    );
    expect(
      jxlData.length,
      lessThanOrEqualTo(366334),
      reason: 'Default decoding speed 0 must stay within 1% of the '
          '362,707-byte cjxl baseline.',
    );
  });

  testWidgets('round-trips JPEG bytes losslessly', (WidgetTester tester) async {
    final Uint8List jpegData = await _loadFixture('2.jpg');
    final Uint8List jxlData = await jpegBytesToJxl(jpegData);
    final Uint8List roundTripData = await jxlBytesToJpeg(jxlData);

    expect(roundTripData, orderedEquals(jpegData));
  });

  testWidgets('round-trips only the addressed bytes of a typed-data view', (
    WidgetTester tester,
  ) async {
    final Uint8List jpegData = await _loadFixture('2.jpg');
    final Uint8List padded = Uint8List(jpegData.length + 64)
      ..fillRange(0, 32, 0xa5)
      ..setRange(32, 32 + jpegData.length, jpegData)
      ..fillRange(32 + jpegData.length, 64 + jpegData.length, 0x5a);
    final Uint8List view = Uint8List.sublistView(
      padded,
      32,
      32 + jpegData.length,
    );

    final Uint8List jxlData = await jpegBytesToJxl(view);
    final Uint8List restored = await jxlBytesToJpeg(jxlData);

    expect(restored, orderedEquals(jpegData));
  });

  testWidgets('keeps every tuning extreme lossless', (
    WidgetTester tester,
  ) async {
    final Uint8List jpegData = await _loadFixture('2.jpg');
    const options = <(JxlEncodeOptions, JxlExecutionOptions)>[
      (
        JxlEncodeOptions(effort: 1),
        JxlExecutionOptions(priority: JxlTaskPriority.low),
      ),
      (
        JxlEncodeOptions(effort: 9),
        JxlExecutionOptions(priority: JxlTaskPriority.high),
      ),
    ];

    for (final (encode, execution) in options) {
      final jxlData = await jpegBytesToJxl(
        jpegData,
        encoding: encode,
        execution: execution,
      );
      final restored = await jxlBytesToJpeg(jxlData, execution: execution);

      expect(restored, orderedEquals(jpegData));
    }
  });

  testWidgets('saves JPEG files as JXL files', (WidgetTester tester) async {
    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));
    final File inputFile = await _writeFixture(tmpDir, '2.jpg');
    final File outputFile = File('${tmpDir.path}/out.jxl');

    await jpegPathToJxl(inputFile.path, outputFile.path);

    expect(await outputFile.exists(), isTrue);
    expect(await outputFile.length(), greaterThan(0));
  });

  testWidgets('saves JXL files as JPEG files', (WidgetTester tester) async {
    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));
    final File inputFile = await _writeFixture(tmpDir, '1.jxl');
    final File outputFile = File('${tmpDir.path}/out.jpg');

    await jxlPathToJpeg(inputFile.path, outputFile.path);

    expect(await outputFile.exists(), isTrue);
    expect(
      (await outputFile.readAsBytes()).take(2),
      orderedEquals(_jpegSignature),
    );
  });

  testWidgets('round-trips JPEG files losslessly', (WidgetTester tester) async {
    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));
    final File jpegFile = await _writeFixture(tmpDir, '2.jpg');
    final File jxlFile = File('${tmpDir.path}/out.jxl');
    final File outputFile = File('${tmpDir.path}/out.jpg');

    await jpegPathToJxl(jpegFile.path, jxlFile.path);
    await jxlPathToJpeg(jxlFile.path, outputFile.path);

    expect(
      await outputFile.readAsBytes(),
      orderedEquals(await jpegFile.readAsBytes()),
    );
  });

  testWidgets('honors cooperative path deadlines', (WidgetTester tester) async {
    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));
    final File input = await _writeFixture(tmpDir, '2.jpg');
    final File output = File('${tmpDir.path}/timed-out.jxl');
    final Uint8List sentinel = Uint8List.fromList(<int>[9, 8, 7, 6]);
    await output.writeAsBytes(sentinel);

    await expectLater(
      jpegPathToJxl(
        input.path,
        output.path,
        encoding: const JxlEncodeOptions(effort: 9),
        execution: const JxlExecutionOptions(
          timeout: Duration(milliseconds: 1),
        ),
      ),
      throwsA(_platformError('TIMEOUT')),
    );
    expect(await output.readAsBytes(), orderedEquals(sentinel));
  });

  testWidgets('honors cooperative byte deadlines', (WidgetTester tester) async {
    final Uint8List jpeg = await _loadFixture('2.jpg');

    await expectLater(
      jpegBytesToJxl(
        jpeg,
        encoding: const JxlEncodeOptions(effort: 9),
        execution: const JxlExecutionOptions(
          timeout: Duration(milliseconds: 1),
        ),
      ),
      throwsA(_platformError('TIMEOUT')),
    );
  });

  testWidgets('very large deadlines do not overflow into immediate expiry', (
    WidgetTester tester,
  ) async {
    final Uint8List jpeg = await _loadFixture('baseline_rgb.jpg');
    const execution = JxlExecutionOptions(
      timeout: Duration(microseconds: 0x7fffffffffffffff),
    );

    final jxl = await jpegBytesToJxl(jpeg, execution: execution);
    final restored = await jxlBytesToJpeg(jxl, execution: execution);

    expect(restored, orderedEquals(jpeg));
  });

  testWidgets('inverse deadlines preserve existing path output', (
    WidgetTester tester,
  ) async {
    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));
    final File input = await _writeFixture(tmpDir, '1.jxl');
    final File output = File('${tmpDir.path}/existing.jpg');
    final Uint8List sentinel = Uint8List.fromList(<int>[4, 3, 2, 1]);
    await output.writeAsBytes(sentinel);

    await expectLater(
      jxlPathToJpeg(
        input.path,
        output.path,
        execution: const JxlExecutionOptions(
          timeout: Duration(milliseconds: 1),
        ),
      ),
      throwsA(_platformError('TIMEOUT')),
    );
    expect(await output.readAsBytes(), orderedEquals(sentinel));
  });

  testWidgets('starts a fresh deadline for every queued batch image', (
    WidgetTester tester,
  ) async {
    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));
    final Uint8List jpeg = await _loadFixture('2.jpg');
    final probeJobs = <JxlPathPair>[];
    for (var index = 0; index < _configuredMaxActiveConversions; index++) {
      final input = File('${tmpDir.path}/probe_$index.jpg');
      await input.writeAsBytes(jpeg);
      probeJobs.add((
        inputPath: input.path,
        outputPath: '${tmpDir.path}/probe_$index.jxl',
      ));
    }

    final probe = Stopwatch()..start();
    await jpegPathsToJxl(
      probeJobs,
      encoding: const JxlEncodeOptions(effort: 9),
    );
    probe.stop();
    final timeout = Duration(
      microseconds: max(500000, probe.elapsedMicroseconds * 2),
    );

    final jobs = <JxlPathPair>[];
    for (var index = 0; index < 12; index++) {
      final input = File('${tmpDir.path}/queued_$index.jpg');
      await input.writeAsBytes(jpeg);
      jobs.add((
        inputPath: input.path,
        outputPath: '${tmpDir.path}/queued_$index.jxl',
      ));
    }

    final batch = Stopwatch()..start();
    await jpegPathsToJxl(
      jobs,
      encoding: const JxlEncodeOptions(effort: 9),
      execution: JxlExecutionOptions(
        timeout: timeout,
      ),
    );
    batch.stop();

    expect(batch.elapsed, greaterThan(timeout));
    for (final job in jobs) {
      expect(await File(job.outputPath).length(), greaterThan(0));
    }
  });

  testWidgets('byte and file transcoding produce the same JXL data', (
    WidgetTester tester,
  ) async {
    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));
    final File jpegFile = await _writeFixture(tmpDir, '2.jpg');
    final File jxlFile = File('${tmpDir.path}/out.jxl');
    final Uint8List jpegData = await jpegFile.readAsBytes();

    final Uint8List byteResult = await jpegBytesToJxl(jpegData);
    await jpegPathToJxl(jpegFile.path, jxlFile.path);

    expect(await jxlFile.readAsBytes(), orderedEquals(byteResult));
  });

  testWidgets('overwrites existing output files', (WidgetTester tester) async {
    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));
    final File jpegFile = await _writeFixture(tmpDir, '2.jpg');
    final File jxlFile = File('${tmpDir.path}/out.jxl');
    await jxlFile.writeAsBytes(<int>[1, 2, 3]);

    await jpegPathToJxl(jpegFile.path, jxlFile.path);

    final Uint8List output = await jxlFile.readAsBytes();
    expect(output.length, greaterThan(3));
    expect(
      output.take(_jxlContainerSignature.length),
      orderedEquals(_jxlContainerSignature),
    );
  });

  testWidgets('rejects malformed JPEG bytes', (WidgetTester tester) async {
    await expectLater(
      jpegBytesToJxl(Uint8List.fromList(<int>[0xff, 0xd8, 0xff])),
      throwsA(_platformError('UNSUPPORTED_INPUT')),
    );
  });

  testWidgets('rejects malformed JXL bytes', (WidgetTester tester) async {
    await expectLater(
      jxlBytesToJpeg(Uint8List.fromList(<int>[0x00, 0x01, 0x02])),
      throwsA(_platformError('UNSUPPORTED_INPUT')),
    );
  });

  testWidgets('rejects empty byte inputs', (WidgetTester tester) async {
    await expectLater(
      jpegBytesToJxl(Uint8List(0)),
      throwsA(_platformError('INVALID_ARGUMENTS')),
    );
    await expectLater(
      jxlBytesToJpeg(Uint8List(0)),
      throwsA(_platformError('INVALID_ARGUMENTS')),
    );
  });

  testWidgets('reports missing input files', (WidgetTester tester) async {
    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));

    await expectLater(
      jpegPathToJxl('${tmpDir.path}/missing.jpg', '${tmpDir.path}/out.jxl'),
      throwsA(_platformError('IO_ERROR')),
    );
    await expectLater(
      jxlPathToJpeg('${tmpDir.path}/missing.jxl', '${tmpDir.path}/out.jpg'),
      throwsA(_platformError('IO_ERROR')),
    );
  });

  testWidgets('reports output write failures', (WidgetTester tester) async {
    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));
    final File jpegFile = await _writeFixture(tmpDir, '2.jpg');
    final File jxlFile = await _writeFixture(tmpDir, '1.jxl');

    await expectLater(
      jpegPathToJxl(jpegFile.path, tmpDir.path),
      throwsA(_platformError('IO_ERROR')),
    );
    await expectLater(
      jxlPathToJpeg(jxlFile.path, tmpDir.path),
      throwsA(_platformError('IO_ERROR')),
    );
  });

  testWidgets('validates every native method argument shape', (
    WidgetTester tester,
  ) async {
    final invalidCalls = <({String method, Object? arguments})>[
      (method: 'jpegBytesToJxl', arguments: null),
      (
        method: 'jxlBytesToJpeg',
        arguments: <String, Object?>{'jxlData': 'wrong'},
      ),
      (
        method: 'jpegPathToJxl',
        arguments: <String, Object?>{'inputPath': 'input.jpg'},
      ),
      (
        method: 'jxlPathToJpeg',
        arguments: <String, Object?>{
          'inputPath': 42,
          'outputPath': 'output.jpg',
        },
      ),
      (method: 'jpegPathsToJxl', arguments: <Object?>[3, 0, 0, 'unpaired.jpg']),
      (method: 'jxlPathsToJpeg', arguments: <Object?>[0, 0, 'unpaired.jxl']),
      (method: 'jpegBytesToJxl', arguments: <Object?>[Uint8List(0), 10, 0]),
      (method: 'jxlBytesToJpeg', arguments: <Object?>[Uint8List(0), -1]),
      (method: 'jpegPathsToJxl', arguments: <Object?>[3, 0, 257]),
      (
        method: 'jpegBytesToJxl',
        arguments: <Object?>[Uint8List(1), 7, 5, 0, 0],
      ),
      (
        method: 'jxlBytesToJpeg',
        arguments: <Object?>[Uint8List(1), 0, -1],
      ),
      (
        method: 'jpegPathsToJxl',
        arguments: <Object?>[7, 0, 0, 257, 0],
      ),
      (method: 'jxlPathsToJpeg', arguments: <Object?>[0, 257, 0]),
    ];

    for (final call in invalidCalls) {
      await expectLater(
        _channel.invokeMethod<Object?>(call.method, call.arguments),
        throwsA(_platformError('INVALID_ARGUMENTS', withDetails: false)),
        reason: call.method,
      );
    }
  });

  testWidgets('reports unknown native methods', (WidgetTester tester) async {
    await expectLater(
      _channel.invokeMethod<void>('unknownMethod'),
      throwsA(isA<MissingPluginException>()),
    );
  });

  testWidgets('handles repeated byte round trips independently', (
    WidgetTester tester,
  ) async {
    final Uint8List jpegData = await _loadFixture('2.jpg');

    for (var iteration = 0; iteration < 3; iteration++) {
      final Uint8List jxlData = await jpegBytesToJxl(jpegData);
      final Uint8List restored = await jxlBytesToJpeg(jxlData);
      expect(restored, orderedEquals(jpegData), reason: 'iteration $iteration');
    }
  });

  testWidgets('handles multiple queued conversions without shared state', (
    WidgetTester tester,
  ) async {
    final Uint8List jpegData = await _loadFixture('2.jpg');

    final results = await Future.wait(
      List.generate(3, (_) => jpegBytesToJxl(jpegData)),
    );

    for (final jxlData in results) {
      expect(await jxlBytesToJpeg(jxlData), orderedEquals(jpegData));
    }
  });

  testWidgets('runs file batches natively and losslessly', (
    WidgetTester tester,
  ) async {
    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));
    final Uint8List jpeg = await _loadFixture('2.jpg');
    final encodeJobs = <JxlPathPair>[];
    final decodeJobs = <JxlPathPair>[];

    for (var index = 0; index < 4; index++) {
      final input = File('${tmpDir.path}/input_$index.jpg');
      final jxl = File('${tmpDir.path}/output_$index.jxl');
      final restored = File('${tmpDir.path}/restored_$index.jpg');
      await input.writeAsBytes(jpeg);
      encodeJobs.add((inputPath: input.path, outputPath: jxl.path));
      decodeJobs.add((inputPath: jxl.path, outputPath: restored.path));
    }

    await jpegPathsToJxl(encodeJobs);
    await jxlPathsToJpeg(decodeJobs);

    for (final job in decodeJobs) {
      expect(await File(job.outputPath).readAsBytes(), orderedEquals(jpeg));
    }
    await jpegPathsToJxl(const <JxlPathPair>[]);
    await jxlPathsToJpeg(const <JxlPathPair>[]);
  });

  testWidgets('reports native batch failures', (WidgetTester tester) async {
    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));

    await expectLater(
      jpegPathsToJxl(<JxlPathPair>[
        (
          inputPath: '${tmpDir.path}/missing.jpg',
          outputPath: '${tmpDir.path}/out.jxl',
        ),
      ]),
      throwsA(_platformError('IO_ERROR')),
    );
    await expectLater(
      jxlPathsToJpeg(<JxlPathPair>[
        (
          inputPath: '${tmpDir.path}/missing.jxl',
          outputPath: '${tmpDir.path}/out.jpg',
        ),
      ]),
      throwsA(_platformError('IO_ERROR')),
    );
  });

  testWidgets('finishes valid batch jobs before reporting another failure', (
    WidgetTester tester,
  ) async {
    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));
    final Uint8List jpeg = await _loadFixture('2.jpg');
    final File validInput = File('${tmpDir.path}/valid.jpg');
    final File validJxl = File('${tmpDir.path}/valid.jxl');
    final File validOutput = File('${tmpDir.path}/valid-restored.jpg');
    await validInput.writeAsBytes(jpeg);

    await expectLater(
      jpegPathsToJxl(
        <JxlPathPair>[
          (inputPath: validInput.path, outputPath: validJxl.path),
          (
            inputPath: '${tmpDir.path}/missing.jpg',
            outputPath: '${tmpDir.path}/missing.jxl',
          ),
        ],
        execution: const JxlExecutionOptions(),
      ),
      throwsA(_platformError('IO_ERROR')),
    );
    expect(await validJxl.exists(), isTrue);
    expect(await validJxl.length(), greaterThan(0));

    await expectLater(
      jxlPathsToJpeg(
        <JxlPathPair>[
          (inputPath: validJxl.path, outputPath: validOutput.path),
          (
            inputPath: '${tmpDir.path}/missing.jxl',
            outputPath: '${tmpDir.path}/missing.jpg',
          ),
        ],
        execution: const JxlExecutionOptions(),
      ),
      throwsA(_platformError('IO_ERROR')),
    );
    expect(await validOutput.readAsBytes(), orderedEquals(jpeg));
  });

  testWidgets('selects batch errors by input order, not completion order', (
    WidgetTester tester,
  ) async {
    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));
    final File valid = await _writeFixture(tmpDir, '2.jpg');
    final File malformed = File('${tmpDir.path}/malformed.jpg');
    await malformed.writeAsBytes(<int>[0xff, 0xd8]);

    await expectLater(
      jpegPathsToJxl(
        <JxlPathPair>[
          (inputPath: valid.path, outputPath: tmpDir.path),
          (
            inputPath: malformed.path,
            outputPath: '${tmpDir.path}/unused.jxl',
          ),
        ],
        encoding: const JxlEncodeOptions(effort: 9),
      ),
      throwsA(_platformError('IO_ERROR')),
    );
  });

  testWidgets('accepts work from multiple background isolates', (
    WidgetTester tester,
  ) async {
    final Uint8List jpeg = await _loadFixture('2.jpg');
    final RootIsolateToken token = ServicesBinding.rootIsolateToken!;
    final ReceivePort receivePort = ReceivePort();
    addTearDown(receivePort.close);

    for (var index = 0; index < 4; index++) {
      await Isolate.spawn<List<Object>>(_transcodeInBackgroundIsolate, <Object>[
        token,
        receivePort.sendPort,
        TransferableTypedData.fromList(<Uint8List>[jpeg]),
      ]);
    }

    final responses = await receivePort.take(4).toList();
    for (final response in responses) {
      expect(response, isA<TransferableTypedData>(), reason: '$response');
      final jxl =
          (response as TransferableTypedData).materialize().asUint8List();
      expect(await jxlBytesToJpeg(jxl), orderedEquals(jpeg));
    }
  });

  for (final fixtureName in _jpegVariants) {
    testWidgets('round-trips $fixtureName losslessly', (
      WidgetTester tester,
    ) async {
      final Uint8List jpegData = await _loadFixture(fixtureName);
      final Uint8List jxlData = await jpegBytesToJxl(jpegData);
      final Uint8List restored = await jxlBytesToJpeg(jxlData);

      expect(restored, orderedEquals(jpegData), reason: fixtureName);
    });
  }

  testWidgets('rejects CMYK JPEG transcoding', (WidgetTester tester) async {
    await expectLater(
      jpegBytesToJxl(await _loadFixture('cmyk.jpg')),
      throwsA(_platformError('UNSUPPORTED_INPUT')),
    );
  });

  testWidgets('rejects arithmetic-coded and lossless JPEG transcoding', (
    WidgetTester tester,
  ) async {
    for (final input in <Uint8List>[
      base64Decode(_arithmeticJpegBase64),
      base64Decode(_losslessJpegBase64),
    ]) {
      await expectLater(
        jpegBytesToJxl(input),
        throwsA(_platformError('UNSUPPORTED_INPUT')),
      );
    }
  });

  testWidgets('rejects JXL without JPEG reconstruction data', (
    WidgetTester tester,
  ) async {
    await expectLater(
      jxlBytesToJpeg(base64Decode(_jxlWithoutJpegReconstructionBase64)),
      throwsA(_platformError('UNSUPPORTED_INPUT')),
    );
  });

  testWidgets('rejects truncated JPEG and JXL streams', (
    WidgetTester tester,
  ) async {
    final Uint8List jpegData = await _loadFixture('2.jpg');
    final Uint8List jxlData = await _loadFixture('1.jxl');

    for (final length in <int>[1, 16, 256, jpegData.length ~/ 2]) {
      await expectLater(
        jpegBytesToJxl(Uint8List.sublistView(jpegData, 0, length)),
        throwsA(_platformError('UNSUPPORTED_INPUT')),
        reason: 'JPEG length $length',
      );
    }
    for (final length in <int>[1, 16, 256, jxlData.length ~/ 2]) {
      await expectLater(
        jxlBytesToJpeg(Uint8List.sublistView(jxlData, 0, length)),
        throwsA(_platformError('UNSUPPORTED_INPUT')),
        reason: 'JXL length $length',
      );
    }
  });

  testWidgets('rejects deterministic random payloads across buffer sizes', (
    WidgetTester tester,
  ) async {
    final Random random = Random(0x4a584c);

    for (final length in <int>[1, 2, 3, 15, 64, 255, 1024, 4096]) {
      final Uint8List payload = Uint8List.fromList(
        List<int>.generate(length, (_) => random.nextInt(256)),
      )..first = 0;

      await expectLater(
        jpegBytesToJxl(payload),
        throwsA(_platformError('UNSUPPORTED_INPUT')),
        reason: 'random JPEG payload length $length',
      );
      await expectLater(
        jxlBytesToJpeg(payload),
        throwsA(_platformError('UNSUPPORTED_INPUT')),
        reason: 'random JXL payload length $length',
      );
    }
  });

  testWidgets('never corrupts pre-existing outputs after codec failures', (
    WidgetTester tester,
  ) async {
    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));
    final File badJpeg = File('${tmpDir.path}/bad.jpg');
    final File badJxl = File('${tmpDir.path}/bad.jxl');
    final File jxlOutput = File('${tmpDir.path}/existing.jxl');
    final File jpegOutput = File('${tmpDir.path}/existing.jpg');
    final Uint8List sentinel = Uint8List.fromList(<int>[9, 8, 7, 6]);
    await badJpeg.writeAsBytes(<int>[0xff, 0xd8]);
    await badJxl.writeAsBytes(<int>[0x00, 0x01]);
    await jxlOutput.writeAsBytes(sentinel);
    await jpegOutput.writeAsBytes(sentinel);

    await expectLater(
      jpegPathToJxl(badJpeg.path, jxlOutput.path),
      throwsA(_platformError('UNSUPPORTED_INPUT')),
    );
    await expectLater(
      jxlPathToJpeg(badJxl.path, jpegOutput.path),
      throwsA(_platformError('UNSUPPORTED_INPUT')),
    );

    expect(await jxlOutput.readAsBytes(), orderedEquals(sentinel));
    expect(await jpegOutput.readAsBytes(), orderedEquals(sentinel));
  });

  testWidgets('runs mixed fixtures and tuning concurrently without cross-talk',
      (
    WidgetTester tester,
  ) async {
    final fixtures = <Uint8List>[
      for (final name in _jpegVariants) await _loadFixture(name),
      await _loadFixture('2.jpg'),
    ];

    final roundTrips = <Future<({Uint8List expected, Uint8List actual})>>[];
    for (var repetition = 0; repetition < 3; repetition++) {
      for (var index = 0; index < fixtures.length; index++) {
        final jpeg = fixtures[index];
        roundTrips.add(() async {
          final jxl = await jpegBytesToJxl(
            jpeg,
            encoding: JxlEncodeOptions(
              effort: index.isEven ? 1 : 9,
              decodingSpeed: repetition.isEven ? 0 : 4,
            ),
            execution: JxlExecutionOptions(
              priority: repetition.isEven
                  ? JxlTaskPriority.low
                  : JxlTaskPriority.high,
            ),
          );
          return (expected: jpeg, actual: await jxlBytesToJpeg(jxl));
        }());
      }
    }

    for (final result in await Future.wait(roundTrips)) {
      expect(result.actual, orderedEquals(result.expected));
    }
  });

  testWidgets('rejects malformed input files', (WidgetTester tester) async {
    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));
    final File badJpeg = File('${tmpDir.path}/bad.jpg');
    final File badJxl = File('${tmpDir.path}/bad.jxl');
    await badJpeg.writeAsBytes(<int>[0xff, 0xd8, 0xff]);
    await badJxl.writeAsBytes(<int>[0x00, 0x01, 0x02]);

    await expectLater(
      jpegPathToJxl(badJpeg.path, '${tmpDir.path}/out.jxl'),
      throwsA(_platformError('UNSUPPORTED_INPUT')),
    );
    await expectLater(
      jxlPathToJpeg(badJxl.path, '${tmpDir.path}/out.jpg'),
      throwsA(_platformError('UNSUPPORTED_INPUT')),
    );
  });

  testWidgets('supports Unicode paths and in-place conversion', (
    WidgetTester tester,
  ) async {
    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));
    final Directory unicodeDir = Directory('${tmpDir.path}/données 🖼️');
    await unicodeDir.create();
    final File file = File('${unicodeDir.path}/entrée.jpg');
    final Uint8List original = await _loadFixture('baseline_rgb.jpg');
    await file.writeAsBytes(original);

    await jpegPathToJxl(file.path, file.path);
    expect(
      (await file.readAsBytes()).take(_jxlContainerSignature.length),
      orderedEquals(_jxlContainerSignature),
    );

    await jxlPathToJpeg(file.path, file.path);
    expect(await file.readAsBytes(), orderedEquals(original));
  });

  testWidgets('runs a 100-image native path stress batch', (
    WidgetTester tester,
  ) async {
    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));
    final Uint8List original = await _loadFixture('baseline_rgb.jpg');
    final encodeJobs = <JxlPathPair>[];
    final decodeJobs = <JxlPathPair>[];

    for (var index = 0; index < 100; index++) {
      final input = File('${tmpDir.path}/input_$index.jpg');
      final encoded = File('${tmpDir.path}/encoded_$index.jxl');
      final restored = File('${tmpDir.path}/restored_$index.jpg');
      await input.writeAsBytes(original);
      encodeJobs.add((inputPath: input.path, outputPath: encoded.path));
      decodeJobs.add((inputPath: encoded.path, outputPath: restored.path));
    }

    await jpegPathsToJxl(encodeJobs);
    await jxlPathsToJpeg(decodeJobs);

    for (final job in decodeJobs) {
      final restored = File(job.outputPath);
      expect(await restored.readAsBytes(), orderedEquals(original));
    }
  });

  testWidgets('supports Windows paths longer than 260 characters', (
    WidgetTester tester,
  ) async {
    if (!Platform.isWindows) return;

    final Directory tmpDir = await _temporaryDirectory();
    addTearDown(() => tmpDir.delete(recursive: true));
    var longPath = tmpDir.path;
    for (var index = 0; index < 7; index++) {
      final segment = List.filled(4, 'segment_${index}_').join();
      longPath = '$longPath/$segment';
    }
    final directory = Directory(longPath);
    await directory.create(recursive: true);
    expect(directory.path.length, greaterThan(260));

    final original = await _loadFixture('baseline_rgb.jpg');
    final input = File('${directory.path}/entrée.jpg');
    final encoded = File('${directory.path}/sortie.jxl');
    final restored = File('${directory.path}/restaurée.jpg');
    await input.writeAsBytes(original);

    await jpegPathToJxl(input.path, encoded.path);
    await jxlPathToJpeg(encoded.path, restored.path);
    expect(await restored.readAsBytes(), orderedEquals(original));
  });
}
