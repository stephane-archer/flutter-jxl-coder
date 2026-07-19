import 'dart:async';
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:jxl_coder/jxl_coder.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const channel = MethodChannel('jxl_coder');
  late List<MethodCall> calls;
  late Future<Object?> Function(MethodCall call) handler;

  setUp(() {
    calls = [];
    handler = (call) async {
      return switch (call.method) {
        'jpegBytesToJxl' => Uint8List.fromList([3, 4]),
        'jxlBytesToJpeg' => Uint8List.fromList([1, 2]),
        'jpegPathToJxl' ||
        'jxlPathToJpeg' ||
        'jpegPathsToJxl' ||
        'jxlPathsToJpeg' ||
        'configureJxlScheduler' =>
          null,
        _ => throw MissingPluginException(),
      };
    };
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      return handler(call);
    });
  });

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  });

  test('public barrel exports only the supported API', () {
    final source = File('lib/jxl_coder.dart').readAsStringSync();
    final exports = RegExp(
      r"export '([^']+)'\s+show\s+([^;]+);",
      multiLine: true,
    ).allMatches(source);

    expect(
      <String, Set<String>>{
        for (final export in exports)
          export.group(1)!:
              export.group(2)!.split(',').map((name) => name.trim()).toSet(),
      },
      <String, Set<String>>{
        'src/jxl_functions.dart': <String>{
          'configureJxlScheduler',
          'jpegBytesToJxl',
          'jpegPathToJxl',
          'jpegPathsToJxl',
          'jxlBytesToJpeg',
          'jxlPathToJpeg',
          'jxlPathsToJpeg',
        },
        'src/jxl_types.dart': <String>{
          'JxlEncodeOptions',
          'JxlExecutionOptions',
          'JxlMacOSSecurityScopedBookmarks',
          'JxlPathPair',
          'JxlTaskPriority',
        },
      },
    );
  });

  test('option objects expose documented defaults and explicit values', () {
    expect(const JxlEncodeOptions().effort, 7);
    expect(const JxlEncodeOptions().decodingSpeed, 0);
    expect(
      const JxlEncodeOptions(effort: 9, decodingSpeed: 4).effort,
      9,
    );
    expect(
      const JxlEncodeOptions(effort: 9, decodingSpeed: 4).decodingSpeed,
      4,
    );
    expect(
      const JxlExecutionOptions().priority,
      JxlTaskPriority.normal,
    );
    expect(const JxlExecutionOptions().timeout, isNull);
    expect(
      const JxlExecutionOptions(priority: JxlTaskPriority.high).priority,
      JxlTaskPriority.high,
    );
    expect(
      const JxlExecutionOptions(timeout: Duration(seconds: 2)).timeout,
      const Duration(seconds: 2),
    );
    const execution = JxlExecutionOptions(
      priority: JxlTaskPriority.low,
      timeout: Duration(milliseconds: 750),
    );
    expect(execution.priority, JxlTaskPriority.low);
    expect(execution.timeout, const Duration(milliseconds: 750));
    final inputBookmark = Uint8List.fromList(<int>[1, 2]);
    final outputBookmark = Uint8List.fromList(<int>[3, 4]);
    final bookmarks = JxlMacOSSecurityScopedBookmarks(
      input: inputBookmark,
      output: outputBookmark,
    );
    expect(bookmarks.input, same(inputBookmark));
    expect(bookmarks.output, same(outputBookmark));
    expect(const JxlMacOSSecurityScopedBookmarks().input, isNull);
    expect(const JxlMacOSSecurityScopedBookmarks().output, isNull);
  });

  test('forwards process-wide scheduler configuration', () async {
    await configureJxlScheduler(
      workerCount: 6,
      maxActiveConversions: 3,
    );

    expect(calls.single.method, 'configureJxlScheduler');
    expect(calls.single.arguments, <Object?>[6, 3]);
  });

  test('transcodes JPEG bytes with automatic tuning', () async {
    final input = Uint8List.fromList([1, 2]);

    expect(await jpegBytesToJxl(input), Uint8List.fromList([3, 4]));
    expect(calls.single.method, 'jpegBytesToJxl');
    expect(calls.single.arguments, <Object?>[input, 7, 0, 1, 0]);
  });

  test('restores JPEG bytes with automatic tuning', () async {
    final input = Uint8List.fromList([3, 4]);

    expect(await jxlBytesToJpeg(input), Uint8List.fromList([1, 2]));
    expect(calls.single.method, 'jxlBytesToJpeg');
    expect(calls.single.arguments, <Object?>[input, 1, 0]);
  });

  test('transcodes a JPEG path with automatic tuning', () async {
    await jpegPathToJxl('input.jpg', 'output.jxl');

    expect(calls.single.method, 'jpegPathToJxl');
    expect(calls.single.arguments, <Object?>[
      'input.jpg',
      'output.jxl',
      7,
      0,
      1,
      0,
    ]);
  });

  test('restores a JPEG path with automatic tuning', () async {
    await jxlPathToJpeg('entrée.jxl', 'résultat.jpg');

    expect(calls.single.method, 'jxlPathToJpeg');
    expect(calls.single.arguments, <Object?>[
      'entrée.jxl',
      'résultat.jpg',
      1,
      0,
    ]);
  });

  test('forwards execution options', () async {
    final input = Uint8List.fromList([1, 2]);
    const byteExecution = JxlExecutionOptions(
      priority: JxlTaskPriority.high,
      timeout: Duration(milliseconds: 250),
    );

    await jpegBytesToJxl(
      input,
      encoding: const JxlEncodeOptions(effort: 1, decodingSpeed: 4),
      execution: byteExecution,
    );
    await jxlBytesToJpeg(input, execution: byteExecution);
    await jpegPathToJxl(
      'in.jpg',
      'out.jxl',
      encoding: const JxlEncodeOptions(effort: 2, decodingSpeed: 3),
      execution: const JxlExecutionOptions(
        priority: JxlTaskPriority.low,
        timeout: Duration(seconds: 2),
      ),
    );
    await jxlPathToJpeg(
      'in.jxl',
      'out.jpg',
      execution: const JxlExecutionOptions(
        priority: JxlTaskPriority.high,
        timeout: Duration(milliseconds: 750),
      ),
    );

    expect(calls[0].arguments, <Object?>[input, 1, 4, 2, 250]);
    expect(calls[1].arguments, <Object?>[input, 2, 250]);
    expect(calls[2].arguments, <Object?>['in.jpg', 'out.jxl', 2, 3, 0, 2000]);
    expect(calls[3].arguments, <Object?>['in.jxl', 'out.jpg', 2, 750]);
  });

  test('flattens path batches into one platform message', () async {
    const jobs = <JxlPathPair>[
      (inputPath: 'a.jpg', outputPath: 'a.jxl'),
      (inputPath: 'b.jpg', outputPath: 'b.jxl'),
    ];

    await jpegPathsToJxl(
      jobs,
      encoding: const JxlEncodeOptions(effort: 2, decodingSpeed: 1),
      execution: const JxlExecutionOptions(
        priority: JxlTaskPriority.high,
        timeout: Duration(seconds: 2),
      ),
    );
    await jxlPathsToJpeg(
      jobs,
      execution: const JxlExecutionOptions(
        priority: JxlTaskPriority.low,
        timeout: Duration(milliseconds: 750),
      ),
    );

    expect(calls[0].method, 'jpegPathsToJxl');
    expect(calls[0].arguments, <Object?>[
      2,
      1,
      2,
      2000,
      'a.jpg',
      'a.jxl',
      'b.jpg',
      'b.jxl',
    ]);
    expect(calls[1].method, 'jxlPathsToJpeg');
    expect(calls[1].arguments, <Object?>[
      0,
      750,
      'a.jpg',
      'a.jxl',
      'b.jpg',
      'b.jxl',
    ]);
  });

  test('forwards macOS security-scoped bookmarks for path calls', () async {
    final input = Uint8List.fromList(<int>[1, 2]);
    final output = Uint8List.fromList(<int>[3, 4]);
    final bookmarks = JxlMacOSSecurityScopedBookmarks(
      input: input,
      output: output,
    );

    await jpegPathToJxl(
      'input.jpg',
      'output.jxl',
      macOSSecurityScopedBookmarks: bookmarks,
    );
    await jxlPathToJpeg(
      'input.jxl',
      'output.jpg',
      macOSSecurityScopedBookmarks: bookmarks,
    );

    const key = 'securityScopedBookmarks';
    expect((calls[0].arguments as List<Object?>).last, <String, Object?>{
      key: <Uint8List?>[input, output],
    });
    expect((calls[1].arguments as List<Object?>).last, <String, Object?>{
      key: <Uint8List?>[input, output],
    });
  });

  test('forwards aligned macOS bookmarks for path batches', () async {
    const jobs = <JxlPathPair>[
      (inputPath: 'a.jpg', outputPath: 'a.jxl'),
      (inputPath: 'b.jpg', outputPath: 'b.jxl'),
    ];
    final firstInput = Uint8List.fromList(<int>[1]);
    final firstOutput = Uint8List.fromList(<int>[2]);
    final secondOutput = Uint8List.fromList(<int>[3]);
    final bookmarks = <JxlMacOSSecurityScopedBookmarks>[
      JxlMacOSSecurityScopedBookmarks(
        input: firstInput,
        output: firstOutput,
      ),
      JxlMacOSSecurityScopedBookmarks(output: secondOutput),
    ];

    await jpegPathsToJxl(
      jobs,
      macOSSecurityScopedBookmarks: bookmarks,
    );
    await jxlPathsToJpeg(
      jobs,
      macOSSecurityScopedBookmarks: bookmarks,
    );

    final expected = <String, Object?>{
      'securityScopedBookmarks': <Uint8List?>[
        firstInput,
        firstOutput,
        null,
        secondOutput,
      ],
    };
    expect((calls[0].arguments as List<Object?>).last, expected);
    expect((calls[1].arguments as List<Object?>).last, expected);
  });

  test('rejects misaligned macOS bookmark batches before platform calls',
      () async {
    const jobs = <JxlPathPair>[
      (inputPath: 'a.jpg', outputPath: 'a.jxl'),
    ];
    const bookmarks = <JxlMacOSSecurityScopedBookmarks>[];

    expect(
      () => jpegPathsToJxl(
        jobs,
        macOSSecurityScopedBookmarks: bookmarks,
      ),
      throwsArgumentError,
    );
    expect(
      () => jxlPathsToJpeg(
        jobs,
        macOSSecurityScopedBookmarks: bookmarks,
      ),
      throwsArgumentError,
    );
    expect(calls, isEmpty);
  });

  test('forwards all accepted tuning boundaries', () async {
    final input = Uint8List.fromList([1]);

    await configureJxlScheduler();
    await configureJxlScheduler(
      workerCount: 256,
      maxActiveConversions: 256,
    );

    await jpegBytesToJxl(
      input,
      encoding: const JxlEncodeOptions(effort: 1, decodingSpeed: 0),
      execution: const JxlExecutionOptions(
        priority: JxlTaskPriority.low,
        timeout: Duration(microseconds: 1),
      ),
    );
    await jpegBytesToJxl(
      input,
      encoding: const JxlEncodeOptions(effort: 9, decodingSpeed: 4),
      execution: const JxlExecutionOptions(
        priority: JxlTaskPriority.high,
      ),
    );
    await jpegPathsToJxl(
      const <JxlPathPair>[],
      encoding: const JxlEncodeOptions(effort: 1, decodingSpeed: 0),
      execution: const JxlExecutionOptions(
        priority: JxlTaskPriority.low,
      ),
    );
    await jxlPathsToJpeg(
      const <JxlPathPair>[],
      execution: const JxlExecutionOptions(
        priority: JxlTaskPriority.high,
        timeout: Duration(days: 1),
      ),
    );

    expect(calls[0].arguments, <Object?>[0, 0]);
    expect(calls[1].arguments, <Object?>[256, 256]);
    expect(calls[2].arguments, <Object?>[input, 1, 0, 0, 1]);
    expect(calls[3].arguments, <Object?>[input, 9, 4, 2, 0]);
    expect(calls[4].arguments, <Object?>[1, 0, 0, 0]);
    expect(calls[5].arguments, <Object?>[2, 86400000]);
  });

  test('empty batches still invoke the native batch methods once', () async {
    await jpegPathsToJxl(const <JxlPathPair>[]);
    await jxlPathsToJpeg(const <JxlPathPair>[]);

    expect(calls, hasLength(2));
    expect(calls[0].method, 'jpegPathsToJxl');
    expect(calls[0].arguments, <Object?>[7, 0, 1, 0]);
    expect(calls[1].method, 'jxlPathsToJpeg');
    expect(calls[1].arguments, <Object?>[1, 0]);
  });

  test('batch flattening preserves duplicates, empty paths, and order',
      () async {
    const jobs = <JxlPathPair>[
      (inputPath: '', outputPath: ''),
      (inputPath: 'same.jpg', outputPath: 'same.jxl'),
      (inputPath: 'same.jpg', outputPath: 'same.jxl'),
      (inputPath: 'données/entrée.jpg', outputPath: '結果.jxl'),
    ];

    await jpegPathsToJxl(jobs);

    expect(calls.single.arguments, <Object?>[
      7,
      0,
      1,
      0,
      '',
      '',
      'same.jpg',
      'same.jxl',
      'same.jpg',
      'same.jxl',
      'données/entrée.jpg',
      '結果.jxl',
    ]);
  });

  test('passes empty byte buffers to the platform unchanged', () async {
    final input = Uint8List(0);

    await jpegBytesToJxl(input);

    expect(calls.single.arguments, <Object?>[input, 7, 0, 1, 0]);
  });

  test('passes typed-data views without widening the backing buffer', () async {
    final backing = Uint8List.fromList([99, 1, 2, 3, 88]);
    final input = Uint8List.sublistView(backing, 1, 4);

    await jpegBytesToJxl(input);

    final forwarded = (calls.single.arguments as List<Object?>).first;
    expect(forwarded, isA<Uint8List>());
    expect(forwarded, orderedEquals(<int>[1, 2, 3]));
  });

  test('preserves typed-data views in both directions', () async {
    final backing = Uint8List.fromList(<int>[91, 10, 11, 12, 92]);
    final input = Uint8List.sublistView(backing, 1, 4);
    final returnedBacking = Uint8List.fromList(<int>[81, 20, 21, 22, 82]);
    handler = (call) async {
      expect(
        (call.arguments as List<Object?>).first,
        orderedEquals(<int>[10, 11, 12]),
      );
      return Uint8List.sublistView(returnedBacking, 1, 4);
    };

    final result = await jxlBytesToJpeg(input);

    expect(result, orderedEquals(<int>[20, 21, 22]));
  });

  test('forwards every priority value for encode and reconstruction', () async {
    final input = Uint8List.fromList(<int>[1]);
    for (final (priority, encoded) in <(JxlTaskPriority, int)>[
      (JxlTaskPriority.low, 0),
      (JxlTaskPriority.normal, 1),
      (JxlTaskPriority.high, 2),
    ]) {
      final execution = JxlExecutionOptions(priority: priority);
      await jpegBytesToJxl(input, execution: execution);
      await jxlBytesToJpeg(input, execution: execution);
      expect((calls[calls.length - 2].arguments as List<Object?>)[3], encoded);
      expect((calls.last.arguments as List<Object?>)[1], encoded);
    }
  });

  test('rejects missing byte results from the platform', () async {
    handler = (_) async => null;

    await expectLater(jpegBytesToJxl(Uint8List(0)), throwsA(isA<StateError>()));
    await expectLater(jxlBytesToJpeg(Uint8List(0)), throwsA(isA<StateError>()));
  });

  test('missing byte results identify the broken native operation', () async {
    handler = (_) async => null;

    await expectLater(
      jpegBytesToJxl(Uint8List(0)),
      throwsA(
        isA<StateError>().having(
          (error) => error.message,
          'message',
          'The native JPEG encoder returned no data',
        ),
      ),
    );
    await expectLater(
      jxlBytesToJpeg(Uint8List(0)),
      throwsA(
        isA<StateError>().having(
          (error) => error.message,
          'message',
          'The native JPEG decoder returned no data',
        ),
      ),
    );
  });

  test('rejects native byte results of the wrong type', () async {
    handler = (_) async => 'not bytes';

    await expectLater(
      jpegBytesToJxl(Uint8List(0)),
      throwsA(isA<TypeError>()),
    );
    await expectLater(
      jxlBytesToJpeg(Uint8List(0)),
      throwsA(isA<TypeError>()),
    );
  });

  test('propagates native codec errors without rewriting them', () async {
    handler = (_) async {
      throw PlatformException(
        code: 'TRANSCODE_ERROR',
        message: 'native failure',
        details: 'codec details',
      );
    };

    await expectLater(
      jpegBytesToJxl(Uint8List.fromList([1])),
      throwsA(
        isA<PlatformException>()
            .having((error) => error.code, 'code', 'TRANSCODE_ERROR')
            .having((error) => error.message, 'message', 'native failure')
            .having((error) => error.details, 'details', 'codec details'),
      ),
    );
  });

  test('propagates native file errors without rewriting them', () async {
    handler = (_) async {
      throw PlatformException(code: 'INVERSE_ERROR');
    };

    await expectLater(
      jxlPathToJpeg('missing.jxl', 'output.jpg'),
      throwsA(
        isA<PlatformException>().having(
          (error) => error.code,
          'code',
          'INVERSE_ERROR',
        ),
      ),
    );
  });

  test('every public operation propagates platform error metadata', () async {
    handler = (_) async {
      throw PlatformException(
        code: 'NATIVE_FAILURE',
        message: 'operation failed',
        details: <String, Object?>{'errno': 28},
      );
    };
    final operations = <Future<void> Function()>[
      () => configureJxlScheduler(),
      () async => jpegBytesToJxl(Uint8List(0)),
      () async => jxlBytesToJpeg(Uint8List(0)),
      () => jpegPathToJxl('in.jpg', 'out.jxl'),
      () => jxlPathToJpeg('in.jxl', 'out.jpg'),
      () => jpegPathsToJxl(const <JxlPathPair>[]),
      () => jxlPathsToJpeg(const <JxlPathPair>[]),
    ];
    final platformError = isA<PlatformException>()
        .having((error) => error.code, 'code', 'NATIVE_FAILURE')
        .having((error) => error.message, 'message', 'operation failed')
        .having(
      (error) => error.details,
      'details',
      <String, Object?>{'errno': 28},
    );

    for (final operation in operations) {
      await expectLater(operation(), throwsA(platformError));
    }
    expect(calls, hasLength(operations.length));
  });

  test('reports a missing native plugin', () async {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);

    await expectLater(
      jpegBytesToJxl(Uint8List(0)),
      throwsA(isA<MissingPluginException>()),
    );
  });

  test('every public operation reports a missing native plugin', () async {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
    final operations = <Future<void> Function()>[
      () => configureJxlScheduler(),
      () async => jpegBytesToJxl(Uint8List(0)),
      () async => jxlBytesToJpeg(Uint8List(0)),
      () => jpegPathToJxl('in.jpg', 'out.jxl'),
      () => jxlPathToJpeg('in.jxl', 'out.jpg'),
      () => jpegPathsToJxl(const <JxlPathPair>[]),
      () => jxlPathsToJpeg(const <JxlPathPair>[]),
    ];

    for (final operation in operations) {
      await expectLater(operation(), throwsA(isA<MissingPluginException>()));
    }
  });

  test('keeps concurrent calls and their arguments independent', () async {
    handler = (call) async {
      final arguments = call.arguments as List<Object?>;
      return arguments[0];
    };
    final inputs = List.generate(
      8,
      (index) => Uint8List.fromList([index, index + 1]),
    );

    final results = await Future.wait(inputs.map(jpegBytesToJxl));

    expect(results, hasLength(inputs.length));
    expect(calls, hasLength(inputs.length));
    for (var index = 0; index < inputs.length; index++) {
      expect(results[index], orderedEquals(inputs[index]));
      expect(calls[index].method, 'jpegBytesToJxl');
      expect((calls[index].arguments as List<Object?>)[0], inputs[index]);
    }
  });

  test('keeps out-of-order native completions paired with their calls',
      () async {
    final completions = <Completer<Object?>>[];
    handler = (call) {
      final completion = Completer<Object?>();
      completions.add(completion);
      return completion.future;
    };
    final inputs = List.generate(
      8,
      (index) => Uint8List.fromList([index]),
    );

    final pendingResults = inputs.map(jpegBytesToJxl).toList();
    for (var index = completions.length - 1; index >= 0; index--) {
      completions[index].complete(Uint8List.fromList([index + 10]));
    }
    final results = await Future.wait(pendingResults);

    expect(calls, hasLength(inputs.length));
    for (var index = 0; index < inputs.length; index++) {
      expect(results[index], orderedEquals(<int>[index + 10]));
      expect(
        (calls[index].arguments as List<Object?>).first,
        orderedEquals(inputs[index]),
      );
    }
  });

  test('validates options before platform calls', () {
    expect(
      () => jpegBytesToJxl(
        Uint8List(0),
        encoding: const JxlEncodeOptions(effort: 0),
      ),
      throwsRangeError,
    );
    expect(
      () => jpegBytesToJxl(
        Uint8List(0),
        encoding: const JxlEncodeOptions(decodingSpeed: 5),
      ),
      throwsRangeError,
    );
    expect(
      () => configureJxlScheduler(workerCount: 257),
      throwsRangeError,
    );
    expect(
      () => configureJxlScheduler(workerCount: -1),
      throwsRangeError,
    );
    expect(
      () => configureJxlScheduler(maxActiveConversions: 257),
      throwsRangeError,
    );
    expect(
      () => configureJxlScheduler(maxActiveConversions: -1),
      throwsRangeError,
    );
    expect(
      () => jpegBytesToJxl(
        Uint8List(0),
        execution: const JxlExecutionOptions(timeout: Duration.zero),
      ),
      throwsArgumentError,
    );
    expect(
      () => jpegPathsToJxl(
        const <JxlPathPair>[],
        execution: const JxlExecutionOptions(
          timeout: Duration(milliseconds: -1),
        ),
      ),
      throwsArgumentError,
    );

    expect(calls, isEmpty);
  });

  test('rejects every effort value outside the inclusive range', () {
    final invalidValues = <int>[-0x7fffffffffffffff, -1, 0, 10, 257];

    for (final effort in invalidValues) {
      final matcher = isA<RangeError>()
          .having((error) => error.name, 'name', 'effort')
          .having((error) => error.invalidValue, 'invalidValue', effort)
          .having((error) => error.start, 'start', 1)
          .having((error) => error.end, 'end', 9);
      expect(
        () => jpegBytesToJxl(
          Uint8List(0),
          encoding: JxlEncodeOptions(effort: effort),
        ),
        throwsA(matcher),
      );
      expect(
        () => jpegPathToJxl(
          'in.jpg',
          'out.jxl',
          encoding: JxlEncodeOptions(effort: effort),
        ),
        throwsA(matcher),
      );
      expect(
        () => jpegPathsToJxl(
          const <JxlPathPair>[],
          encoding: JxlEncodeOptions(effort: effort),
        ),
        throwsA(matcher),
      );
    }

    expect(calls, isEmpty);
  });

  test('rejects every decoding-speed value outside the range', () {
    final invalidValues = <int>[-0x7fffffffffffffff, -1, 5, 257];

    for (final decodingSpeed in invalidValues) {
      final matcher = isA<RangeError>()
          .having((error) => error.name, 'name', 'decodingSpeed')
          .having(
            (error) => error.invalidValue,
            'invalidValue',
            decodingSpeed,
          )
          .having((error) => error.start, 'start', 0)
          .having((error) => error.end, 'end', 4);
      for (final operation in <Object? Function()>[
        () => jpegBytesToJxl(
              Uint8List(0),
              encoding: JxlEncodeOptions(decodingSpeed: decodingSpeed),
            ),
        () => jpegPathToJxl(
              'in.jpg',
              'out.jxl',
              encoding: JxlEncodeOptions(decodingSpeed: decodingSpeed),
            ),
        () => jpegPathsToJxl(
              const <JxlPathPair>[],
              encoding: JxlEncodeOptions(decodingSpeed: decodingSpeed),
            ),
      ]) {
        expect(operation, throwsA(matcher));
      }
    }

    expect(calls, isEmpty);
  });

  test('rejects every worker-count value outside the inclusive range', () {
    final invalidValues = <int>[-0x7fffffffffffffff, -1, 257, 1000];

    for (final workerCount in invalidValues) {
      final matcher = isA<RangeError>()
          .having((error) => error.name, 'name', 'workerCount')
          .having((error) => error.invalidValue, 'invalidValue', workerCount)
          .having((error) => error.start, 'start', 0)
          .having((error) => error.end, 'end', 256);
      expect(
        () => configureJxlScheduler(workerCount: workerCount),
        throwsA(matcher),
      );
    }

    expect(calls, isEmpty);
  });

  test('rejects every active-conversion value outside the range', () {
    final invalidValues = <int>[-0x7fffffffffffffff, -1, 257, 1000];

    for (final maxActiveConversions in invalidValues) {
      final matcher = isA<RangeError>()
          .having((error) => error.name, 'name', 'maxActiveConversions')
          .having(
            (error) => error.invalidValue,
            'invalidValue',
            maxActiveConversions,
          )
          .having((error) => error.start, 'start', 0)
          .having((error) => error.end, 'end', 256);
      expect(
        () => configureJxlScheduler(
          maxActiveConversions: maxActiveConversions,
        ),
        throwsA(matcher),
      );
    }

    expect(calls, isEmpty);
  });

  test('rejects non-positive deadlines for every operation kind', () {
    for (final timeout in <Duration>[Duration.zero, const Duration(days: -1)]) {
      final execution = JxlExecutionOptions(timeout: timeout);
      final operations = <Object? Function()>[
        () => jpegBytesToJxl(Uint8List(0), execution: execution),
        () => jxlBytesToJpeg(Uint8List(0), execution: execution),
        () => jpegPathToJxl('in.jpg', 'out.jxl', execution: execution),
        () => jxlPathToJpeg('in.jxl', 'out.jpg', execution: execution),
        () => jpegPathsToJxl(
              const <JxlPathPair>[],
              execution: execution,
            ),
        () => jxlPathsToJpeg(
              const <JxlPathPair>[],
              execution: execution,
            ),
      ];

      for (final operation in operations) {
        expect(
          operation,
          throwsA(
            isA<ArgumentError>()
                .having((error) => error.name, 'name', 'timeout')
                .having(
                  (error) => error.invalidValue,
                  'invalidValue',
                  timeout,
                ),
          ),
        );
      }
    }

    expect(calls, isEmpty);
  });
}
