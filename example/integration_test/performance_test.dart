import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:jxl_coder/jxl_coder.dart';

const int _warmUpIterations = 2;
const int _measuredIterations = 8;
const int _concurrentJobs = 4;
const int _configuredWorkers = int.fromEnvironment('JXL_WORKER_COUNT');
const int _configuredMaximum = int.fromEnvironment('JXL_MAX_ACTIVE');

Future<Uint8List> _fixture(String name) async {
  final data = await rootBundle.load('integration_test/$name');
  return data.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes);
}

Future<int> _measure(Future<void> Function() action) async {
  final stopwatch = Stopwatch()..start();
  await action();
  stopwatch.stop();
  return stopwatch.elapsedMicroseconds;
}

double _milliseconds(int microseconds) => microseconds / 1000;

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('reports JPEG transcoding performance', (tester) async {
    await configureJxlScheduler(
      workerCount: _configuredWorkers,
      maxActiveConversions: _configuredMaximum,
    );
    final jpeg = await _fixture('2.jpg');

    for (var index = 0; index < _warmUpIterations; index++) {
      expect(await jpegBytesToJxl(jpeg), isNotEmpty);
    }

    final sequentialMicros = await _measure(() async {
      for (var index = 0; index < _measuredIterations; index++) {
        expect(await jpegBytesToJxl(jpeg), isNotEmpty);
      }
    });

    final concurrentMicros = await _measure(() async {
      final results = await Future.wait(
        List.generate(_concurrentJobs, (_) => jpegBytesToJxl(jpeg)),
      );
      expect(results, everyElement(isNotEmpty));
    });

    final encoded = await jpegBytesToJxl(jpeg);
    final inverseMicros = await _measure(() async {
      for (var index = 0; index < _measuredIterations; index++) {
        expect(await jxlBytesToJpeg(encoded), orderedEquals(jpeg));
      }
    });

    final directory = await Directory.systemTemp.createTemp('jxl_benchmark_');
    addTearDown(() => directory.delete(recursive: true));
    final input = File('${directory.path}/input.jpg');
    await input.writeAsBytes(jpeg, flush: true);

    final fileMicros = await _measure(() async {
      for (var index = 0; index < _measuredIterations; index++) {
        await jpegPathToJxl(
          input.path,
          '${directory.path}/output_$index.jxl',
        );
      }
    });

    final batchInputs = <JxlPathPair>[];
    for (var index = 0; index < _concurrentJobs; index++) {
      final batchInput = File('${directory.path}/batch_input_$index.jpg');
      await batchInput.writeAsBytes(jpeg);
      batchInputs.add((
        inputPath: batchInput.path,
        outputPath: '${directory.path}/batch_output_$index.jxl',
      ));
    }
    final batchMicros = await _measure(
      () => jpegPathsToJxl(batchInputs),
    );

    final separateFileMicros = await _measure(() async {
      await Future.wait([
        for (var index = 0; index < batchInputs.length; index++)
          jpegPathToJxl(
            batchInputs[index].inputPath,
            '${directory.path}/separate_output_$index.jxl',
          ),
      ]);
    });

    final effortResults = <String, Object>{};
    for (final effort in <int>[1, 3, 7]) {
      final encoding = JxlEncodeOptions(effort: effort);
      final output = await jpegBytesToJxl(jpeg, encoding: encoding);
      final micros = await _measure(() async {
        for (var index = 0; index < _measuredIterations; index++) {
          await jpegBytesToJxl(jpeg, encoding: encoding);
        }
      });
      effortResults['effort$effort'] = <String, Object>{
        'milliseconds': _milliseconds(micros) / _measuredIterations,
        'outputBytes': output.length,
      };
    }

    final priorityResults = <String, Object>{};
    for (final priority in JxlTaskPriority.values) {
      final execution = JxlExecutionOptions(priority: priority);
      final micros = await _measure(() async {
        for (var index = 0; index < _measuredIterations; index++) {
          await jpegBytesToJxl(
            jpeg,
            execution: execution,
          );
        }
      });
      priorityResults[priority.name] =
          _milliseconds(micros) / _measuredIterations;
    }

    final concurrentPriorityResults = <String, Object>{};
    for (final priority in JxlTaskPriority.values) {
      final execution = JxlExecutionOptions(priority: priority);
      final micros = await _measure(() async {
        final results = await Future.wait(
          List.generate(
            _concurrentJobs,
            (_) => jpegBytesToJxl(
              jpeg,
              execution: execution,
            ),
          ),
        );
        expect(results, everyElement(isNotEmpty));
      });
      concurrentPriorityResults[priority.name] = _milliseconds(micros);
    }

    const schedulerJobCount = 12;
    final schedulerInputs = <JxlPathPair>[
      for (var index = 0; index < schedulerJobCount; index++)
        (
          inputPath: input.path,
          outputPath: '${directory.path}/scheduler_$index.jxl',
        ),
    ];
    final twelveFileBatchMicros = await _measure(
      () => jpegPathsToJxl(schedulerInputs),
    );
    final contentionInput = File('${directory.path}/contention_input.jxl');
    await contentionInput.writeAsBytes(encoded);

    Future<({double normal, double high})> contentionLatencies() async {
      final effectiveMaximum = _configuredMaximum == 0
          ? min(Platform.numberOfProcessors, 256)
          : _configuredMaximum;
      // Leave admission slots for matched normal- and high-priority probes.
      // Measuring them in the same contention window exercises relative
      // priority directly and avoids comparing unrelated thermal conditions.
      final contentionBatchCount = max(
        1,
        min(schedulerInputs.length, effectiveMaximum - 2),
      );
      final batch = jxlPathsToJpeg(
        <JxlPathPair>[
          for (var index = 0; index < contentionBatchCount; index++)
            (
              inputPath: contentionInput.path,
              outputPath: '${directory.path}/contention_$index.jpg',
            ),
        ],
      );
      // Allow the batch reconstructions to reach libjxl's parallel phase so
      // both probes contend for scheduler CPU chunks under the same load.
      await Future<void>.delayed(const Duration(milliseconds: 20));
      Future<double> probe(JxlTaskPriority priority) async {
        final stopwatch = Stopwatch()..start();
        expect(
          await jxlBytesToJpeg(
            encoded,
            execution: JxlExecutionOptions(priority: priority),
          ),
          orderedEquals(jpeg),
        );
        stopwatch.stop();
        return stopwatch.elapsedMicroseconds / 1000;
      }

      final probes = await Future.wait([
        probe(JxlTaskPriority.normal),
        probe(JxlTaskPriority.high),
      ]);
      await batch;
      return (normal: probes[0], high: probes[1]);
    }

    final contention = await contentionLatencies();

    final report = <String, Object>{
      'platform': Platform.operatingSystem,
      'inputBytes': jpeg.length,
      'iterations': _measuredIterations,
      'singleBytesMs': _milliseconds(sequentialMicros) / _measuredIterations,
      'fourConcurrentBytesMs': _milliseconds(concurrentMicros),
      'singleFileMs': _milliseconds(fileMicros) / _measuredIterations,
      'singleInverseMs': _milliseconds(inverseMicros) / _measuredIterations,
      'fourFileBatchMs': _milliseconds(batchMicros),
      'fourSeparateFilesMs': _milliseconds(separateFileMicros),
      'efforts': effortResults,
      'configuredWorkers': _configuredWorkers,
      'configuredMaxActive': _configuredMaximum,
      'priorities': priorityResults,
      'fourConcurrentByPriority': concurrentPriorityResults,
      'schedulerJobs': schedulerJobCount,
      'twelveFileBatchMs': _milliseconds(twelveFileBatchMicros),
      'normalContentionMs': contention.normal,
      'highContentionMs': contention.high,
    };
    // Stable marker used by local profiling and CI benchmark extraction.
    // ignore: avoid_print
    print('JXL_BENCHMARK ${jsonEncode(report)}');
  });
}
