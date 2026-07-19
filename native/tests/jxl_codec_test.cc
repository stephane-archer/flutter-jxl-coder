#include "../src/jxl_codec.h"

#include <jxl/encode.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../src/deadline_utils.h"
#include "../src/shared_parallel_runner.h"

namespace jxl_coder {

struct OutputBufferTestAccess {
  static bool Allocate(OutputBuffer *buffer, std::size_t capacity,
                       Error *error) {
    return buffer->Allocate(capacity, error);
  }

  static bool Grow(OutputBuffer *buffer, std::size_t used, Error *error) {
    return buffer->Grow(used, error);
  }
};

}  // namespace jxl_coder

namespace {

using jxl_coder::EncodeJpeg;
using jxl_coder::Error;
using jxl_coder::ErrorCode;
using jxl_coder::ClassifyEncoderErrorForTesting;
using jxl_coder::CodecFailurePointForTesting;
using jxl_coder::ConfigureSharedParallelScheduler;
using jxl_coder::Options;
using jxl_coder::OutputBuffer;
using jxl_coder::OutputBufferTestAccess;
using jxl_coder::ReconstructJpeg;
using jxl_coder::SharedParallelScheduler;
using jxl_coder::SharedParallelRunner;
using jxl_coder::SharedRunnerContext;
using jxl_coder::SharedParallelRunnerWorkerCount;
using jxl_coder::SchedulerConfigurationResult;
using jxl_coder::SetInitialOutputCapacityForTesting;
using jxl_coder::SetCodecFailurePointForTesting;
using jxl_coder::SetOutputAllocationFailureCountdownForTesting;
using jxl_coder::SetWorkerStartupFailureCountdownForTesting;
using jxl_coder::SaturatingDeadline;
using jxl_coder::TaskPriority;

int failures = 0;

void Expect(bool condition, const std::string &message) {
  if (condition) return;
  std::cerr << "FAIL: " << message << '\n';
  ++failures;
}

std::vector<std::uint8_t> ReadFile(const char *path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                   std::istreambuf_iterator<char>());
}

bool Equals(const OutputBuffer &output,
            const std::vector<std::uint8_t> &expected) {
  if (output.size() != expected.size()) return false;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (output.data()[index] != expected[index]) return false;
  }
  return true;
}

std::vector<std::uint8_t> DeterministicPayload(std::size_t size) {
  std::uint32_t state = 0x4a584cU;
  std::vector<std::uint8_t> bytes(size);
  for (auto &byte : bytes) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    byte = static_cast<std::uint8_t>(state & 0xff);
  }
  if (!bytes.empty()) bytes[0] = 0;
  return bytes;
}

void UpdateMaximum(std::atomic<int> *maximum, int value) {
  int previous = maximum->load(std::memory_order_relaxed);
  while (previous < value && !maximum->compare_exchange_weak(
                                 previous, value, std::memory_order_relaxed)) {
  }
}

struct CallbackGate {
  std::mutex mutex;
  std::condition_variable condition;
  int entered = 0;
  bool released = false;
};

struct RunnerProbe {
  explicit RunnerProbe(std::size_t values) : counts(values, 0) {
    lane_active.fill(0);
  }

  std::mutex mutex;
  std::thread::id caller_thread;
  std::thread::id init_thread;
  std::vector<int> counts;
  std::array<int, 8> lane_active;
  std::size_t reported_threads = 0;
  int init_calls = 0;
  bool invalid_thread_id = false;
  bool concurrent_lane_use = false;
  std::atomic<int> calls{0};
  std::atomic<int> active{0};
  std::atomic<int> maximum_active{0};
  std::atomic<int> *process_active = nullptr;
  std::atomic<int> *process_maximum = nullptr;
  std::atomic<int> *observe_calls = nullptr;
  std::atomic<int> observed_calls{-1};
  std::atomic<bool> *cancel_after_calls = nullptr;
  int cancellation_threshold = 0;
  CallbackGate *gate = nullptr;
  int marker = -1;
  std::vector<int> *sequence = nullptr;
  std::mutex *sequence_mutex = nullptr;
  std::vector<std::uint32_t> *observed_values = nullptr;
  int delay_milliseconds = 0;
};

JxlParallelRetCode ProbeInit(void *opaque, std::size_t threads) {
  auto *probe = static_cast<RunnerProbe *>(opaque);
  std::lock_guard<std::mutex> lock(probe->mutex);
  ++probe->init_calls;
  probe->reported_threads = threads;
  probe->init_thread = std::this_thread::get_id();
  return JXL_PARALLEL_RET_SUCCESS;
}

void ProbeFunction(void *opaque, std::uint32_t value, std::size_t thread_id) {
  auto *probe = static_cast<RunnerProbe *>(opaque);
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    if (thread_id >= probe->reported_threads ||
        thread_id >= probe->lane_active.size()) {
      probe->invalid_thread_id = true;
    } else {
      if (probe->lane_active[thread_id] != 0) {
        probe->concurrent_lane_use = true;
      }
      ++probe->lane_active[thread_id];
    }
    if (value < probe->counts.size()) ++probe->counts[value];
    if (probe->observed_values != nullptr) {
      probe->observed_values->push_back(value);
    }
  }

  const int call_number =
      probe->calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (call_number == 1 &&
      probe->observe_calls != nullptr) {
    probe->observed_calls.store(
        probe->observe_calls->load(std::memory_order_relaxed),
        std::memory_order_relaxed);
  }
  if (probe->cancel_after_calls != nullptr &&
      call_number >= probe->cancellation_threshold) {
    probe->cancel_after_calls->store(true, std::memory_order_relaxed);
  }
  const int active = probe->active.fetch_add(1, std::memory_order_relaxed) + 1;
  UpdateMaximum(&probe->maximum_active, active);
  if (probe->process_active != nullptr) {
    const int process_active =
        probe->process_active->fetch_add(1, std::memory_order_relaxed) + 1;
    UpdateMaximum(probe->process_maximum, process_active);
  }

  if (probe->gate != nullptr) {
    std::unique_lock<std::mutex> lock(probe->gate->mutex);
    ++probe->gate->entered;
    probe->gate->condition.notify_all();
    probe->gate->condition.wait(lock, [&] { return probe->gate->released; });
  }
  if (probe->delay_milliseconds > 0) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(probe->delay_milliseconds));
  }
  if (probe->sequence != nullptr) {
    std::lock_guard<std::mutex> lock(*probe->sequence_mutex);
    probe->sequence->push_back(probe->marker);
  }

  if (probe->process_active != nullptr) {
    probe->process_active->fetch_sub(1, std::memory_order_relaxed);
  }
  probe->active.fetch_sub(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    if (thread_id < probe->lane_active.size()) {
      --probe->lane_active[thread_id];
    }
  }
}

bool AtomicCancelled(void *opaque) {
  return static_cast<std::atomic<bool> *>(opaque)->load(
      std::memory_order_relaxed);
}

struct ThreadCancellation {
  std::thread::id caller;
};

bool CancelOutsideCallerThread(void *opaque) {
  return std::this_thread::get_id() !=
         static_cast<ThreadCancellation *>(opaque)->caller;
}

bool ThrowingCancellation(void *) {
  throw std::runtime_error("injected cancellation callback failure");
}

bool ThrowingCancellationWhenArmed(void *opaque) {
  if (static_cast<std::atomic<bool> *>(opaque)->load(
          std::memory_order_relaxed)) {
    throw std::runtime_error("injected active cancellation callback failure");
  }
  return false;
}

JxlParallelRetCode FailingInit(void *, std::size_t) { return 42; }

JxlParallelRetCode ThrowingInit(void *, std::size_t) {
  throw std::runtime_error("intentional native init callback failure");
}

void ThrowingFunction(void *, std::uint32_t, std::size_t) {
  throw std::runtime_error("intentional native test callback failure");
}

void WaitForCalls(const std::atomic<int> &calls, int minimum) {
  for (int attempt = 0; attempt < 1000; ++attempt) {
    if (calls.load(std::memory_order_relaxed) >= minimum) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

struct TestClock {
  using duration = std::chrono::milliseconds;
  using rep = duration::rep;
  using period = duration::period;
  using time_point = std::chrono::time_point<TestClock, duration>;
};

void TestSaturatingDeadline() {
  const TestClock::time_point start(TestClock::duration(100));
  Expect(SaturatingDeadline<TestClock>(start, 50) ==
             TestClock::time_point(TestClock::duration(150)),
         "representable deadlines must retain their exact duration");
  Expect(SaturatingDeadline<TestClock>(start, 0) ==
             TestClock::time_point::max(),
         "a zero timeout must create an unbounded deadline");
  Expect(SaturatingDeadline<TestClock>(start, -1) ==
             TestClock::time_point::max(),
         "a negative timeout must not overflow deadline arithmetic");

  const auto near_maximum =
      TestClock::time_point::max() - TestClock::duration(5);
  Expect(SaturatingDeadline<TestClock>(near_maximum, 4) ==
             near_maximum + TestClock::duration(4),
         "a near-maximum representable deadline must remain exact");
  Expect(SaturatingDeadline<TestClock>(near_maximum, 5) ==
             TestClock::time_point::max(),
         "a deadline equal to the remaining clock range must saturate");
  Expect(SaturatingDeadline<TestClock>(
             near_maximum, std::numeric_limits<std::int64_t>::max()) ==
             TestClock::time_point::max(),
         "an extreme timeout must saturate instead of wrapping");
}

void TestOutputBufferBoundaries() {
  Error error;
  OutputBuffer zero_limit(0);
  Expect(!OutputBufferTestAccess::Allocate(&zero_limit, 0, nullptr),
         "zero-capacity output allocation must fail with a null error sink");

  OutputBuffer limited(8);
  Expect(!OutputBufferTestAccess::Allocate(&limited, 9, &error) &&
             error.code == ErrorCode::kCodec,
         "output allocation above its limit must fail safely");
  Expect(OutputBufferTestAccess::Allocate(&limited, 4, &error),
         "a bounded output allocation must succeed within its limit");
  for (std::size_t index = 0; index < 4; ++index) {
    limited.data()[index] = static_cast<std::uint8_t>(index + 1);
  }
  Expect(!OutputBufferTestAccess::Grow(&limited, 5, &error) &&
             limited.capacity() == 4,
         "growth must reject a used length larger than current capacity");
  Expect(OutputBufferTestAccess::Grow(&limited, 4, &error) &&
             limited.capacity() == 8,
         "bounded output growth must reach its exact maximum");
  Expect(limited.data()[0] == 1 && limited.data()[1] == 2 &&
             limited.data()[2] == 3 && limited.data()[3] == 4,
         "successful output growth must preserve every used byte");
  Expect(!OutputBufferTestAccess::Grow(&limited, 8, &error) &&
             limited.capacity() == 8,
         "output growth must stop at the configured safety limit");
}

void TestSharedRunnerBasicBehavior() {
  SharedParallelScheduler scheduler(3, 1);
  RunnerProbe automatic(64);
  automatic.delay_milliseconds = 1;
  automatic.caller_thread = std::this_thread::get_id();
  const SharedRunnerContext automatic_context{};
  Expect(scheduler.Run(automatic_context, &automatic, ProbeInit, ProbeFunction,
                       0, 64) == JXL_PARALLEL_RET_SUCCESS,
         "automatic shared-runner work must succeed");
  Expect(automatic.init_calls == 1,
         "parallel initialization must run exactly once");
  Expect(automatic.init_thread == automatic.caller_thread,
         "parallel initialization must run on the requesting thread");
  Expect(automatic.reported_threads == 3,
         "automatic work must be eligible for the complete shared pool");
  Expect(automatic.maximum_active.load() > 1 &&
             automatic.maximum_active.load() <= 3,
         "automatic work must use, but not exceed, the shared pool");
  Expect(!automatic.invalid_thread_id && !automatic.concurrent_lane_use,
         "logical thread IDs must be valid and exclusively held");
  Expect(std::all_of(automatic.counts.begin(), automatic.counts.end(),
                     [](int count) { return count == 1; }),
         "every parallel range value must execute exactly once");

  RunnerProbe low(16);
  low.delay_milliseconds = 1;
  const SharedRunnerContext low_context{TaskPriority::kLow, 1, nullptr,
                                        nullptr};
  Expect(scheduler.Run(low_context, &low, ProbeInit, ProbeFunction, 0, 16) ==
             JXL_PARALLEL_RET_SUCCESS,
         "low-priority work must succeed when uncontended");
  Expect(low.reported_threads == 3 && low.maximum_active.load() > 1,
         "a lone low-priority invocation may use the complete pool");
}

void TestSharedRunnerBoundariesAndShutdown() {
  {
    SharedParallelScheduler scheduler(2, 4);
    RunnerProbe empty(0);
    empty.caller_thread = std::this_thread::get_id();
    const SharedRunnerContext context{};
    Expect(scheduler.Run(context, &empty, ProbeInit, ProbeFunction, 7, 7) ==
               JXL_PARALLEL_RET_SUCCESS,
           "an empty range must initialize and succeed");
    Expect(empty.init_calls == 1 && empty.calls.load() == 0,
           "an empty range must initialize once without callbacks");
    Expect(empty.reported_threads == 1 &&
               empty.init_thread == empty.caller_thread,
           "an empty range must initialize one lane on the caller thread");

    RunnerProbe invalid(1);
    const SharedRunnerContext invalid_context{
        static_cast<TaskPriority>(3), 0, nullptr, nullptr};
    Expect(scheduler.Run(invalid_context, &invalid, ProbeInit, ProbeFunction, 0,
                         1) == JXL_PARALLEL_RET_RUNNER_ERROR,
           "an invalid priority must be rejected");
    Expect(invalid.init_calls == 0 && invalid.calls.load() == 0,
           "invalid work must not initialize or dispatch callbacks");

    RunnerProbe cancelled(1);
    std::atomic<bool> cancellation{true};
    const SharedRunnerContext cancelled_context{
        TaskPriority::kNormal, 0, AtomicCancelled, &cancellation};
    Expect(scheduler.Run(cancelled_context, &cancelled, ProbeInit,
                         ProbeFunction, 0, 1) ==
               JXL_PARALLEL_RET_RUNNER_ERROR,
           "work cancelled before submission must be rejected");
    Expect(cancelled.init_calls == 0 && cancelled.calls.load() == 0,
           "pre-cancelled work must not initialize or dispatch callbacks");

    RunnerProbe failed_init(1);
    Expect(scheduler.Run(context, &failed_init, FailingInit, ProbeFunction, 0,
                         1) == 42,
           "initialization callback errors must propagate unchanged");
    Expect(failed_init.calls.load() == 0,
           "work must not run after initialization failure");

    RunnerProbe throwing(4);
    Expect(scheduler.Run(context, &throwing, ProbeInit, ThrowingFunction, 0,
                         4) == JXL_PARALLEL_RET_RUNNER_ERROR,
           "callback exceptions must become runner errors");

    Expect(scheduler.Run(context, &empty, nullptr, ProbeFunction, 0, 1) ==
               JXL_PARALLEL_RET_RUNNER_ERROR,
           "a null initialization callback must be rejected");
    Expect(scheduler.Run(context, &empty, ProbeInit, nullptr, 0, 1) ==
               JXL_PARALLEL_RET_RUNNER_ERROR,
           "a null work callback must be rejected");
  }

  {
    SharedParallelScheduler clamped(0, 0);
    Expect(clamped.worker_count() == 1,
           "zero worker and chunk settings must clamp to one");
    RunnerProbe one(1);
    const SharedRunnerContext context{};
    Expect(clamped.Run(context, &one, ProbeInit, ProbeFunction, 0, 1) ==
               JXL_PARALLEL_RET_SUCCESS &&
               one.reported_threads == 1 && one.calls.load() == 1,
           "a clamped scheduler must execute single-item work");
  }

  {
    SharedParallelScheduler boundary_scheduler(1, 4);
    RunnerProbe boundary(0);
    std::vector<std::uint32_t> observed;
    boundary.observed_values = &observed;
    const SharedRunnerContext context{};
    const std::uint32_t maximum =
        std::numeric_limits<std::uint32_t>::max();
    Expect(boundary_scheduler.Run(context, &boundary, ProbeInit, ProbeFunction,
                                  maximum - 2, maximum) ==
               JXL_PARALLEL_RET_SUCCESS,
           "range chunking must not overflow near uint32 maximum");
    Expect(observed ==
               std::vector<std::uint32_t>({maximum - 2, maximum - 1}),
           "near-maximum range values must each execute exactly once");

    RunnerProbe reversed(0);
    Expect(boundary_scheduler.Run(context, &reversed, ProbeInit,
                                  ProbeFunction, maximum, maximum - 1) ==
               JXL_PARALLEL_RET_SUCCESS &&
               reversed.init_calls == 1 && reversed.calls.load() == 0,
           "a reversed range must initialize once and dispatch no work");
  }

  for (int iteration = 0; iteration < 8; ++iteration) {
    SharedParallelScheduler scheduler(2, 1);
    RunnerProbe probe(8);
    const SharedRunnerContext context{};
    Expect(scheduler.Run(context, &probe, ProbeInit, ProbeFunction, 0, 8) ==
               JXL_PARALLEL_RET_SUCCESS,
           "scheduler work must complete before clean shutdown");
    Expect(probe.calls.load() == 8,
           "clean shutdown must not lose completed callbacks");
  }
}

void TestInjectedSchedulerResourceFailures() {
  SetWorkerStartupFailureCountdownForTesting(1);
  bool partial_startup_failed = false;
  try {
    SharedParallelScheduler scheduler(2, 1);
  } catch (const std::runtime_error &) {
    partial_startup_failed = true;
  }
  Expect(partial_startup_failed,
         "partial worker startup must fail instead of shrinking the pool");

  SetWorkerStartupFailureCountdownForTesting(0);
  Expect(SharedParallelRunnerWorkerCount() == 1,
         "worker-count discovery must use its safe fallback on startup "
         "failure");

  RunnerProbe sequential(6);
  const SharedRunnerContext context{};
  SetWorkerStartupFailureCountdownForTesting(0);
  Expect(SharedParallelRunner(const_cast<SharedRunnerContext *>(&context),
                              &sequential, ProbeInit, ProbeFunction, 0, 6) ==
             JXL_PARALLEL_RET_SUCCESS,
         "the public runner must fall back to safe sequential execution");
  Expect(sequential.init_calls == 1 && sequential.reported_threads == 1 &&
             sequential.calls.load() == 6 &&
             std::all_of(sequential.counts.begin(), sequential.counts.end(),
                         [](int count) { return count == 1; }),
         "the sequential fallback must initialize once and execute every "
         "value exactly once");

  RunnerProbe cancelled_after_one(4);
  std::atomic<bool> cancel_fallback{false};
  cancelled_after_one.cancel_after_calls = &cancel_fallback;
  cancelled_after_one.cancellation_threshold = 1;
  const SharedRunnerContext fallback_cancel_context{
      TaskPriority::kNormal, 0, AtomicCancelled, &cancel_fallback};
  SetWorkerStartupFailureCountdownForTesting(0);
  Expect(SharedParallelRunner(
             const_cast<SharedRunnerContext *>(&fallback_cancel_context),
             &cancelled_after_one, ProbeInit, ProbeFunction, 0, 4) ==
             JXL_PARALLEL_RET_RUNNER_ERROR,
         "the sequential fallback must observe cancellation between values");
  Expect(cancelled_after_one.calls.load() == 1,
         "fallback cancellation must not dispatch later values");

  RunnerProbe failed_init(1);
  SetWorkerStartupFailureCountdownForTesting(0);
  Expect(SharedParallelRunner(const_cast<SharedRunnerContext *>(&context),
                              &failed_init, FailingInit, ProbeFunction, 0, 1) ==
             42,
         "the sequential fallback must preserve initialization errors");

  std::atomic<bool> cancelled{true};
  const SharedRunnerContext cancelled_context{
      TaskPriority::kNormal, 0, AtomicCancelled, &cancelled};
  RunnerProbe cancelled_probe(1);
  SetWorkerStartupFailureCountdownForTesting(0);
  Expect(SharedParallelRunner(
             const_cast<SharedRunnerContext *>(&cancelled_context),
             &cancelled_probe, ProbeInit, ProbeFunction, 0, 1) ==
             JXL_PARALLEL_RET_RUNNER_ERROR &&
             cancelled_probe.init_calls == 0,
         "the sequential fallback must reject pre-cancelled work");

  RunnerProbe throwing(1);
  SetWorkerStartupFailureCountdownForTesting(0);
  Expect(SharedParallelRunner(const_cast<SharedRunnerContext *>(&context),
                              &throwing, ProbeInit, ThrowingFunction, 0, 1) ==
             JXL_PARALLEL_RET_RUNNER_ERROR,
         "fallback callback exceptions must not cross the C ABI boundary");

  RunnerProbe throwing_init(1);
  SetWorkerStartupFailureCountdownForTesting(0);
  Expect(SharedParallelRunner(const_cast<SharedRunnerContext *>(&context),
                              &throwing_init, ThrowingInit, ProbeFunction, 0,
                              1) == JXL_PARALLEL_RET_RUNNER_ERROR &&
             throwing_init.calls.load() == 0,
         "fallback initialization exceptions must not cross the C ABI");

  RunnerProbe broken_cancellation(2);
  const SharedRunnerContext broken_cancellation_context{
      TaskPriority::kNormal, 0, ThrowingCancellation, nullptr};
  SetWorkerStartupFailureCountdownForTesting(0);
  Expect(SharedParallelRunner(
             const_cast<SharedRunnerContext *>(&broken_cancellation_context),
             &broken_cancellation, ProbeInit, ProbeFunction, 0, 2) ==
             JXL_PARALLEL_RET_RUNNER_ERROR &&
             broken_cancellation.init_calls == 0 &&
             broken_cancellation.calls.load() == 0,
         "fallback cancellation exceptions must reject work safely");

  RunnerProbe active_broken_cancellation(3);
  std::atomic<bool> throw_armed{false};
  active_broken_cancellation.cancel_after_calls = &throw_armed;
  active_broken_cancellation.cancellation_threshold = 1;
  const SharedRunnerContext active_broken_context{
      TaskPriority::kNormal, 0, ThrowingCancellationWhenArmed, &throw_armed};
  SetWorkerStartupFailureCountdownForTesting(0);
  Expect(SharedParallelRunner(
             const_cast<SharedRunnerContext *>(&active_broken_context),
             &active_broken_cancellation, ProbeInit, ProbeFunction, 0, 3) ==
             JXL_PARALLEL_RET_RUNNER_ERROR &&
             active_broken_cancellation.calls.load() == 1,
         "fallback cancellation exceptions must stop later callbacks");

  SetWorkerStartupFailureCountdownForTesting(0);
  Expect(SharedParallelRunner(const_cast<SharedRunnerContext *>(&context),
                              &sequential, nullptr, ProbeFunction, 0, 1) ==
             JXL_PARALLEL_RET_RUNNER_ERROR,
         "the sequential fallback must reject a null initializer");
  SetWorkerStartupFailureCountdownForTesting(0);
  Expect(SharedParallelRunner(const_cast<SharedRunnerContext *>(&context),
                              &sequential, ProbeInit, nullptr, 0, 1) ==
             JXL_PARALLEL_RET_RUNNER_ERROR,
         "the sequential fallback must reject a null work callback");
  SetWorkerStartupFailureCountdownForTesting(-1);
}

void TestSharedRunnerFairnessAndPoolLimit() {
  SharedParallelScheduler scheduler(2, 4);
  std::atomic<int> process_active{0};
  std::atomic<int> process_maximum{0};
  RunnerProbe first(100);
  first.delay_milliseconds = 1;
  first.process_active = &process_active;
  first.process_maximum = &process_maximum;
  RunnerProbe second(8);
  second.delay_milliseconds = 1;
  second.process_active = &process_active;
  second.process_maximum = &process_maximum;
  second.observe_calls = &first.calls;
  const SharedRunnerContext batch_context{TaskPriority::kNormal, 1, nullptr,
                                          nullptr};
  const SharedRunnerContext single_context{TaskPriority::kNormal, 2, nullptr,
                                           nullptr};

  std::thread first_thread([&] {
    scheduler.Run(batch_context, &first, ProbeInit, ProbeFunction, 0, 100);
  });
  WaitForCalls(first.calls, 2);
  std::thread second_thread([&] {
    scheduler.Run(single_context, &second, ProbeInit, ProbeFunction, 0, 8);
  });
  first_thread.join();
  second_thread.join();

  Expect(
      second.observed_calls.load() >= 0 && second.observed_calls.load() < 100,
      "a later single request group must progress before a batch finishes");
  Expect(process_maximum.load() <= 2,
         "concurrent invocations must not exceed the process-wide pool");
}

void TestSharedRunnerContentionStress() {
  constexpr int kInvocations = 18;
  constexpr int kValues = 97;
  SharedParallelScheduler scheduler(4, 4);
  std::atomic<int> process_active{0};
  std::atomic<int> process_maximum{0};
  std::vector<std::unique_ptr<RunnerProbe>> probes;
  probes.reserve(kInvocations);
  for (int index = 0; index < kInvocations; ++index) {
    auto probe = std::make_unique<RunnerProbe>(kValues);
    probe->process_active = &process_active;
    probe->process_maximum = &process_maximum;
    probe->delay_milliseconds = 1;
    probes.push_back(std::move(probe));
  }

  std::mutex start_mutex;
  std::condition_variable start_condition;
  int ready = 0;
  bool start = false;
  std::array<JxlParallelRetCode, kInvocations> results{};
  std::vector<std::thread> callers;
  callers.reserve(kInvocations);
  for (int index = 0; index < kInvocations; ++index) {
    callers.emplace_back([&, index] {
      {
        std::unique_lock<std::mutex> lock(start_mutex);
        ++ready;
        start_condition.notify_all();
        start_condition.wait(lock, [&] { return start; });
      }
      const SharedRunnerContext context{
          static_cast<TaskPriority>(index % 3),
          static_cast<std::uint64_t>(index % 5 + 1), nullptr, nullptr};
      results[static_cast<std::size_t>(index)] = scheduler.Run(
          context, probes[static_cast<std::size_t>(index)].get(), ProbeInit,
          ProbeFunction, 0, static_cast<std::uint32_t>(kValues));
    });
  }
  {
    std::unique_lock<std::mutex> lock(start_mutex);
    start_condition.wait(lock, [&] { return ready == kInvocations; });
    start = true;
  }
  start_condition.notify_all();
  for (auto &caller : callers) caller.join();

  Expect(process_maximum.load() > 1 && process_maximum.load() <= 4,
         "contention stress must use without exceeding the shared pool");
  for (int index = 0; index < kInvocations; ++index) {
    const auto &probe = *probes[static_cast<std::size_t>(index)];
    Expect(results[static_cast<std::size_t>(index)] ==
                   JXL_PARALLEL_RET_SUCCESS &&
               probe.init_calls == 1 && probe.calls.load() == kValues &&
               !probe.invalid_thread_id && !probe.concurrent_lane_use &&
               std::all_of(probe.counts.begin(), probe.counts.end(),
                           [](int count) { return count == 1; }),
           "every contended invocation must execute exactly once and safely");
  }
}

void TestSharedRunnerPriorityWeights() {
  SharedParallelScheduler scheduler(1, 1);
  CallbackGate blocker_gate;
  RunnerProbe blocker(1);
  blocker.gate = &blocker_gate;
  const SharedRunnerContext blocker_context{TaskPriority::kNormal, 99,
                                            nullptr, nullptr};
  std::thread blocker_thread([&] {
    scheduler.Run(blocker_context, &blocker, ProbeInit, ProbeFunction, 0, 1);
  });
  {
    std::unique_lock<std::mutex> lock(blocker_gate.mutex);
    blocker_gate.condition.wait(lock,
                                [&] { return blocker_gate.entered == 1; });
  }

  std::vector<int> sequence;
  std::mutex sequence_mutex;
  RunnerProbe low(80);
  low.marker = 0;
  low.sequence = &sequence;
  low.sequence_mutex = &sequence_mutex;
  RunnerProbe normal(80);
  normal.marker = 1;
  normal.sequence = &sequence;
  normal.sequence_mutex = &sequence_mutex;
  RunnerProbe normal_batch_peer(80);
  normal_batch_peer.marker = 1;
  normal_batch_peer.sequence = &sequence;
  normal_batch_peer.sequence_mutex = &sequence_mutex;
  RunnerProbe high(80);
  high.marker = 2;
  high.sequence = &sequence;
  high.sequence_mutex = &sequence_mutex;
  const SharedRunnerContext low_context{TaskPriority::kLow, 1, nullptr,
                                        nullptr};
  const SharedRunnerContext normal_context{TaskPriority::kNormal, 2, nullptr,
                                           nullptr};
  const SharedRunnerContext high_context{TaskPriority::kHigh, 3, nullptr,
                                         nullptr};
  std::thread low_thread([&] {
    scheduler.Run(low_context, &low, ProbeInit, ProbeFunction, 0, 80);
  });
  std::thread normal_thread([&] {
    scheduler.Run(normal_context, &normal, ProbeInit, ProbeFunction, 0, 80);
  });
  std::thread normal_batch_peer_thread([&] {
    scheduler.Run(normal_context, &normal_batch_peer, ProbeInit, ProbeFunction,
                  0, 80);
  });
  std::thread high_thread([&] {
    scheduler.Run(high_context, &high, ProbeInit, ProbeFunction, 0, 80);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  {
    std::lock_guard<std::mutex> lock(blocker_gate.mutex);
    blocker_gate.released = true;
  }
  blocker_gate.condition.notify_all();
  blocker_thread.join();
  low_thread.join();
  normal_thread.join();
  normal_batch_peer_thread.join();
  high_thread.join();

  std::array<int, 3> early_counts{};
  for (std::size_t index = 0; index < std::min<std::size_t>(70, sequence.size());
       ++index) {
    ++early_counts[sequence[index]];
  }
  Expect(early_counts[2] >= early_counts[1] * 3 / 2 &&
             early_counts[1] >= early_counts[0] * 3 / 2,
         "priority classes must receive weighted 1:2:4 progress");
  Expect(low.calls.load() == 80 && normal.calls.load() == 80 &&
             normal_batch_peer.calls.load() == 80 && high.calls.load() == 80,
         "weighted scheduling must not starve any priority class");
  Expect(early_counts[1] < early_counts[2],
         "more invocations in one batch group must not multiply its class "
         "share");
}

void TestGlobalSchedulerConfiguration() {
  std::size_t effective = 0;
  Expect(ConfigureSharedParallelScheduler(2, &effective) ==
                 SchedulerConfigurationResult::kSuccess &&
             effective == 2,
         "a worker count must be configurable before first use");
  Expect(ConfigureSharedParallelScheduler(2, &effective) ==
             SchedulerConfigurationResult::kSuccess,
         "identical scheduler configuration must be idempotent");
  Expect(ConfigureSharedParallelScheduler(1, &effective) ==
             SchedulerConfigurationResult::kAlreadyStarted,
         "the first explicit scheduler configuration must win");
  Expect(SharedParallelRunnerWorkerCount() == 2,
         "the global runner must use the configured worker count");
  Expect(ConfigureSharedParallelScheduler(1, &effective) ==
             SchedulerConfigurationResult::kAlreadyStarted,
         "a conflicting configuration must fail after first use");
  Expect(ConfigureSharedParallelScheduler(257, nullptr) ==
             SchedulerConfigurationResult::kInvalidArguments,
         "worker counts above the hard limit must be rejected");
  Expect(ConfigureSharedParallelScheduler(2, nullptr) ==
             SchedulerConfigurationResult::kSuccess,
         "identical configuration must allow an omitted result pointer");
  Expect(ConfigureSharedParallelScheduler(0, nullptr) ==
             SchedulerConfigurationResult::kAlreadyStarted,
         "automatic configuration must not replace an explicit pool");

  RunnerProbe probe(1);
  const SharedRunnerContext context{};
  Expect(SharedParallelRunner(nullptr, &probe, ProbeInit, ProbeFunction, 0,
                              1) == JXL_PARALLEL_RET_RUNNER_ERROR,
         "the public runner must reject a null context");
  Expect(SharedParallelRunner(const_cast<SharedRunnerContext *>(&context),
                              &probe, nullptr, ProbeFunction, 0, 1) ==
             JXL_PARALLEL_RET_RUNNER_ERROR,
         "the public runner must reject a null initialization callback");
}

void TestSharedRunnerCancellation() {
  SharedParallelScheduler scheduler(2, 1);
  CallbackGate gate;
  RunnerProbe running(16);
  running.gate = &gate;
  std::atomic<bool> cancelled{false};
  const SharedRunnerContext context{TaskPriority::kNormal, 1, AtomicCancelled,
                                    &cancelled};
  std::atomic<bool> returned{false};
  JxlParallelRetCode result = JXL_PARALLEL_RET_SUCCESS;
  std::thread call([&] {
    result = scheduler.Run(context, &running, ProbeInit, ProbeFunction, 0, 16);
    returned.store(true, std::memory_order_relaxed);
  });
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    gate.condition.wait(lock, [&] { return gate.entered == 2; });
  }
  cancelled.store(true, std::memory_order_relaxed);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  Expect(!returned.load(std::memory_order_relaxed),
         "cancellation must wait for in-flight callbacks to finish");
  {
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.released = true;
  }
  gate.condition.notify_all();
  call.join();
  Expect(result == JXL_PARALLEL_RET_RUNNER_ERROR,
         "cancelled work must report a runner error");
  Expect(running.calls.load() == 2,
         "cancelled work must discard callbacks that were not dispatched");

  SharedParallelScheduler queued_scheduler(1, 1);
  CallbackGate blocking_gate;
  RunnerProbe blocker(4);
  blocker.gate = &blocking_gate;
  const SharedRunnerContext automatic{};
  std::thread blocker_thread([&] {
    queued_scheduler.Run(automatic, &blocker, ProbeInit, ProbeFunction, 0, 4);
  });
  {
    std::unique_lock<std::mutex> lock(blocking_gate.mutex);
    blocking_gate.condition.wait(lock,
                                 [&] { return blocking_gate.entered == 1; });
  }
  RunnerProbe queued(4);
  std::atomic<bool> queued_cancelled{false};
  const SharedRunnerContext queued_context{TaskPriority::kNormal, 2,
                                           AtomicCancelled,
                                           &queued_cancelled};
  JxlParallelRetCode queued_result = JXL_PARALLEL_RET_SUCCESS;
  std::thread queued_thread([&] {
    queued_result = queued_scheduler.Run(queued_context, &queued, ProbeInit,
                                         ProbeFunction, 0, 4);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  queued_cancelled.store(true, std::memory_order_relaxed);
  queued_thread.join();
  Expect(queued_result == JXL_PARALLEL_RET_RUNNER_ERROR &&
             queued.calls.load() == 0,
         "queued cancellation must discard all undispatched callbacks");
  {
    std::lock_guard<std::mutex> lock(blocking_gate.mutex);
    blocking_gate.released = true;
  }
  blocking_gate.condition.notify_all();
  blocker_thread.join();

  SharedParallelScheduler chunk_scheduler(1, 4);
  RunnerProbe between_callbacks(8);
  std::atomic<bool> chunk_cancelled{false};
  between_callbacks.cancel_after_calls = &chunk_cancelled;
  between_callbacks.cancellation_threshold = 1;
  const SharedRunnerContext chunk_context{TaskPriority::kNormal, 3,
                                          AtomicCancelled,
                                          &chunk_cancelled};
  Expect(chunk_scheduler.Run(chunk_context, &between_callbacks, ProbeInit,
                             ProbeFunction, 0, 8) ==
             JXL_PARALLEL_RET_RUNNER_ERROR,
         "cancellation between callbacks must stop the active chunk");
  Expect(between_callbacks.calls.load() == 1,
         "no later callback may start after cancellation is observed");

  SharedParallelScheduler worker_cancel_scheduler(1, 1);
  RunnerProbe worker_cancelled(4);
  ThreadCancellation worker_cancellation{std::this_thread::get_id()};
  const SharedRunnerContext worker_cancel_context{
      TaskPriority::kNormal, 4, CancelOutsideCallerThread,
      &worker_cancellation};
  Expect(worker_cancel_scheduler.Run(worker_cancel_context, &worker_cancelled,
                                     ProbeInit, ProbeFunction, 0, 4) ==
             JXL_PARALLEL_RET_RUNNER_ERROR,
         "a worker-observed queued cancellation must report a runner error");
  Expect(worker_cancelled.calls.load() == 0,
         "worker-observed cancellation must not dispatch a callback");

  SharedParallelScheduler throwing_cancel_scheduler(1, 1);
  RunnerProbe throwing_cancelled(1);
  const SharedRunnerContext throwing_cancel_context{
      TaskPriority::kNormal, 5, ThrowingCancellation, nullptr};
  Expect(throwing_cancel_scheduler.Run(
             throwing_cancel_context, &throwing_cancelled, ProbeInit,
             ProbeFunction, 0, 1) == JXL_PARALLEL_RET_RUNNER_ERROR,
         "throwing cancellation callbacks must become safe cancellation");
  Expect(throwing_cancelled.init_calls == 0 &&
             throwing_cancelled.calls.load() == 0,
         "a broken cancellation callback must not initialize or dispatch");

  SharedParallelScheduler active_throw_scheduler(1, 1);
  CallbackGate active_throw_gate;
  RunnerProbe active_throw_probe(4);
  active_throw_probe.gate = &active_throw_gate;
  std::atomic<bool> throw_armed{false};
  active_throw_probe.cancel_after_calls = &throw_armed;
  active_throw_probe.cancellation_threshold = 1;
  const SharedRunnerContext active_throw_context{
      TaskPriority::kNormal, 6, ThrowingCancellationWhenArmed, &throw_armed};
  std::atomic<bool> active_throw_returned{false};
  JxlParallelRetCode active_throw_result = JXL_PARALLEL_RET_SUCCESS;
  std::thread active_throw_call([&] {
    active_throw_result = active_throw_scheduler.Run(
        active_throw_context, &active_throw_probe, ProbeInit, ProbeFunction, 0,
        4);
    active_throw_returned.store(true, std::memory_order_relaxed);
  });
  {
    std::unique_lock<std::mutex> lock(active_throw_gate.mutex);
    active_throw_gate.condition.wait(
        lock, [&] { return active_throw_gate.entered == 1; });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  Expect(!active_throw_returned.load(std::memory_order_relaxed),
         "broken cancellation must still wait for in-flight callbacks");
  {
    std::lock_guard<std::mutex> lock(active_throw_gate.mutex);
    active_throw_gate.released = true;
  }
  active_throw_gate.condition.notify_all();
  active_throw_call.join();
  Expect(active_throw_result == JXL_PARALLEL_RET_RUNNER_ERROR &&
             active_throw_probe.calls.load() == 1,
         "active cancellation exceptions must stop undispatched work safely");
}

void ExpectInvalidCodecArguments(const std::vector<std::uint8_t> &jpeg) {
  const Options valid{7, 0, TaskPriority::kNormal, 0, 0};
  Error error;
  Expect(!EncodeJpeg(jpeg.data(), jpeg.size(), valid, nullptr, &error) &&
             error.code == ErrorCode::kInvalidArguments,
         "encoding must classify a null output as invalid arguments");
  Expect(!ReconstructJpeg(jpeg.data(), jpeg.size(), valid, nullptr, &error) &&
             error.code == ErrorCode::kInvalidArguments,
         "reconstruction must classify a null output as invalid arguments");

  auto expect_invalid = [&](const std::uint8_t *input, std::size_t size,
                            Options options, const std::string &label) {
    OutputBuffer encoded;
    Error encode_error;
    Expect(!EncodeJpeg(input, size, options, &encoded, &encode_error),
           "encoding: " + label);
    Expect(encode_error.code == ErrorCode::kInvalidArguments,
           "encoding: " + label + " must report invalid arguments");

    OutputBuffer restored;
    Error decode_error;
    Expect(!ReconstructJpeg(input, size, options, &restored, &decode_error),
           "reconstruction: " + label);
    Expect(decode_error.code == ErrorCode::kInvalidArguments,
           "reconstruction: " + label + " must report invalid arguments");
  };

  expect_invalid(nullptr, jpeg.size(), valid, "a null input must fail");
  expect_invalid(jpeg.data(), 0, valid, "an empty input must fail");
  Options options = valid;
  options.effort = 0;
  expect_invalid(jpeg.data(), jpeg.size(), options, "effort zero must fail");
  options = valid;
  options.effort = 10;
  expect_invalid(jpeg.data(), jpeg.size(), options, "effort ten must fail");
  options = valid;
  options.decoding_speed = -1;
  expect_invalid(jpeg.data(), jpeg.size(), options,
                 "negative decoding speed must fail");
  options = valid;
  options.decoding_speed = 5;
  expect_invalid(jpeg.data(), jpeg.size(), options,
                 "decoding speed five must fail");
  options = valid;
  options.priority = static_cast<TaskPriority>(3);
  expect_invalid(jpeg.data(), jpeg.size(), options,
                 "an invalid priority must fail");
  options = valid;
  options.timeout_milliseconds = -1;
  expect_invalid(jpeg.data(), jpeg.size(), options,
                 "a negative timeout must fail");

  OutputBuffer ignored;
  Expect(!EncodeJpeg(nullptr, 0, valid, &ignored, nullptr),
         "invalid arguments must be safe without an error destination");
}

void TestInjectedCodecFailures(const std::vector<std::uint8_t> &jpeg) {
  const Options options{7, 0, TaskPriority::kNormal, 0, 0};
  const auto expect_encode_failure = [&](CodecFailurePointForTesting point,
                                         const std::string &label) {
    SetCodecFailurePointForTesting(point);
    OutputBuffer output;
    Error error;
    Expect(!EncodeJpeg(jpeg.data(), jpeg.size(), options, &output, &error),
           label);
    Expect(error.code == ErrorCode::kCodec && !error.message.empty(),
           label + " must report a stable codec error");
  };
  expect_encode_failure(CodecFailurePointForTesting::kEncoderCreate,
                        "injected encoder creation failure must fail");
  expect_encode_failure(CodecFailurePointForTesting::kEncoderConfigure,
                        "injected encoder configuration failure must fail");
  expect_encode_failure(CodecFailurePointForTesting::kEncoderSettings,
                        "injected encoder settings failure must fail");
  expect_encode_failure(CodecFailurePointForTesting::kEncoderProcess,
                        "injected encoder processing failure must fail");

  OutputBuffer encoded;
  Error encode_error;
  Expect(EncodeJpeg(jpeg.data(), jpeg.size(), options, &encoded, &encode_error),
         "decoder failure fixture must encode");

  SetOutputAllocationFailureCountdownForTesting(0);
  OutputBuffer allocation_failure;
  Error allocation_error;
  Expect(!ReconstructJpeg(encoded.data(), encoded.size(), options,
                          &allocation_failure, &allocation_error),
         "an injected initial reconstruction allocation failure must fail");
  Expect(allocation_error.code == ErrorCode::kCodec &&
             allocation_error.message ==
                 "Not enough memory for the codec output",
         "initial reconstruction allocation failure must retain its stable "
         "error");

  const auto expect_decode_failure = [&](CodecFailurePointForTesting point,
                                         const std::string &label) {
    SetCodecFailurePointForTesting(point);
    OutputBuffer output;
    Error error;
    Expect(!ReconstructJpeg(encoded.data(), encoded.size(), options, &output,
                            &error),
           label);
    Expect(error.code == ErrorCode::kCodec && !error.message.empty(),
           label + " must report a stable codec error");
  };
  expect_decode_failure(CodecFailurePointForTesting::kDecoderCreate,
                        "injected decoder creation failure must fail");
  expect_decode_failure(CodecFailurePointForTesting::kDecoderConfigure,
                        "injected decoder configuration failure must fail");
  expect_decode_failure(CodecFailurePointForTesting::kDecoderBuffer,
                        "injected decoder buffer failure must fail");
  SetCodecFailurePointForTesting(CodecFailurePointForTesting::kNone);
}

void TestEncoderErrorClassification() {
  Expect(ClassifyEncoderErrorForTesting(JXL_ENC_ERR_JBRD) ==
             ErrorCode::kUnsupportedInput,
         "invalid JPEG reconstruction metadata must be unsupported input");
  Expect(ClassifyEncoderErrorForTesting(JXL_ENC_ERR_BAD_INPUT) ==
             ErrorCode::kUnsupportedInput,
         "libjxl bad input must be unsupported input");
  Expect(ClassifyEncoderErrorForTesting(JXL_ENC_ERR_NOT_SUPPORTED) ==
             ErrorCode::kUnsupportedInput,
         "libjxl unsupported input must retain its public classification");
  Expect(ClassifyEncoderErrorForTesting(JXL_ENC_ERR_GENERIC) ==
             ErrorCode::kCodec,
         "internal encoder failures must remain codec errors");
}

void TestInjectedTimeoutCheckpoints(const std::vector<std::uint8_t> &jpeg) {
  const Options options{7, 0, TaskPriority::kNormal, 0, 60000};
  const auto expect_encode_timeout = [&](CodecFailurePointForTesting point,
                                         const std::string &label) {
    SetCodecFailurePointForTesting(point);
    OutputBuffer output;
    Error error;
    Expect(!EncodeJpeg(jpeg.data(), jpeg.size(), options, &output, &error),
           label);
    Expect(error.code == ErrorCode::kTimeout && !error.message.empty(),
           label + " must report the stable timeout classification");
  };
  expect_encode_timeout(
      CodecFailurePointForTesting::kEncoderBeforeStartTimeout,
      "timeout before encoder allocation must fail");
  expect_encode_timeout(
      CodecFailurePointForTesting::kEncoderAfterProcessTimeout,
      "timeout after encoder processing must fail");

  OutputBuffer encoded;
  Error encode_error;
  const Options unbounded{7, 0, TaskPriority::kNormal, 0, 0};
  Expect(EncodeJpeg(jpeg.data(), jpeg.size(), unbounded, &encoded,
                    &encode_error),
         "timeout checkpoint reconstruction fixture must encode");

  const auto expect_decode_timeout = [&](CodecFailurePointForTesting point,
                                         const std::string &label) {
    SetCodecFailurePointForTesting(point);
    OutputBuffer output;
    Error error;
    Expect(!ReconstructJpeg(encoded.data(), encoded.size(), options, &output,
                            &error),
           label);
    Expect(error.code == ErrorCode::kTimeout && !error.message.empty(),
           label + " must report the stable timeout classification");
  };
  expect_decode_timeout(
      CodecFailurePointForTesting::kDecoderAfterConfigureTimeout,
      "timeout after decoder configuration must fail");
  expect_decode_timeout(CodecFailurePointForTesting::kDecoderAfterHeaderTimeout,
                        "timeout after decoder header processing must fail");
  expect_decode_timeout(
      CodecFailurePointForTesting::kDecoderBeforeOutputProcessTimeout,
      "timeout before decoder output processing must fail");
  SetCodecFailurePointForTesting(CodecFailurePointForTesting::kNone);
}

std::vector<std::uint8_t> MetadataHeavyJpeg(
    const std::vector<std::uint8_t> &jpeg) {
  std::vector<std::uint8_t> result;
  if (jpeg.size() < 2) return result;
  constexpr std::size_t kPayloadBytes = 60 * 1024;
  constexpr int kSegments = 16;
  result.reserve(jpeg.size() + kSegments * (kPayloadBytes + 4));
  result.insert(result.end(), jpeg.begin(), jpeg.begin() + 2);
  for (int segment = 0; segment < kSegments; ++segment) {
    result.push_back(0xff);
    result.push_back(0xfe);
    const std::uint16_t length =
        static_cast<std::uint16_t>(kPayloadBytes + 2);
    result.push_back(static_cast<std::uint8_t>(length >> 8));
    result.push_back(static_cast<std::uint8_t>(length & 0xff));
    for (std::size_t index = 0; index < kPayloadBytes; ++index) {
      result.push_back(static_cast<std::uint8_t>((index + segment) & 0x0f));
    }
  }
  result.insert(result.end(), jpeg.begin() + 2, jpeg.end());
  return result;
}

void TestOutputGrowthAndLimits(const std::vector<std::uint8_t> &jpeg) {
  const Options options{7, 0, TaskPriority::kNormal, 0, 0};
  SetOutputAllocationFailureCountdownForTesting(0);
  OutputBuffer allocation_failure;
  Error allocation_error;
  Expect(!EncodeJpeg(jpeg.data(), jpeg.size(), options, &allocation_failure,
                     &allocation_error),
         "an injected initial output allocation failure must be reported");
  Expect(allocation_error.code == ErrorCode::kCodec &&
             allocation_error.message ==
                 "Not enough memory for the codec output",
         "initial output allocation failure must retain its stable error");

  OutputBuffer encoded;
  Error error;
  Expect(EncodeJpeg(jpeg.data(), jpeg.size(), options, &encoded, &error),
         "growth-limit fixture must encode");

  OutputBuffer constrained(4096);
  Error constrained_error;
  Expect(!ReconstructJpeg(encoded.data(), encoded.size(), options,
                          &constrained, &constrained_error),
         "reconstruction must fail when growth reaches its limit");
  Expect(constrained_error.code == ErrorCode::kCodec &&
             constrained.capacity() == 4096,
         "growth-limit failure must preserve the enforced capacity");

  const auto metadata_heavy = MetadataHeavyJpeg(jpeg);
  OutputBuffer compressed_metadata;
  Error metadata_error;
  Expect(EncodeJpeg(metadata_heavy.data(), metadata_heavy.size(), options,
                    &compressed_metadata, &metadata_error),
         "metadata-heavy JPEG must encode");
  OutputBuffer restored;
  Expect(ReconstructJpeg(compressed_metadata.data(),
                         compressed_metadata.size(), options, &restored,
                         &metadata_error),
         "metadata-heavy JPEG must reconstruct");
  Expect(Equals(restored, metadata_heavy),
         "metadata-heavy reconstruction must remain byte-exact");
  Expect(restored.capacity() >
             std::max<std::size_t>(4096, compressed_metadata.size() * 2),
         "metadata-heavy reconstruction must exercise successful growth");

  SetOutputAllocationFailureCountdownForTesting(1);
  OutputBuffer growth_failure;
  Error growth_error;
  Expect(!ReconstructJpeg(compressed_metadata.data(),
                          compressed_metadata.size(), options,
                          &growth_failure, &growth_error),
         "an injected reconstruction growth allocation failure must be "
         "reported");
  Expect(growth_error.code == ErrorCode::kCodec &&
             growth_error.message ==
                 "Not enough memory to grow the codec output",
         "growth allocation failure must retain its stable error");
  SetOutputAllocationFailureCountdownForTesting(-1);

  SetCodecFailurePointForTesting(
      CodecFailurePointForTesting::kDecoderGrowBuffer);
  OutputBuffer buffer_configuration_failure;
  Error buffer_configuration_error;
  Expect(!ReconstructJpeg(compressed_metadata.data(),
                          compressed_metadata.size(), options,
                          &buffer_configuration_failure,
                          &buffer_configuration_error),
         "an injected grown decoder-buffer failure must be reported");
  Expect(buffer_configuration_error.code == ErrorCode::kCodec &&
             buffer_configuration_error.message ==
                 "Failed to grow the JPEG reconstruction buffer",
         "a grown decoder-buffer failure must retain its stable error");
  SetCodecFailurePointForTesting(CodecFailurePointForTesting::kNone);

  SetInitialOutputCapacityForTesting(32);
  OutputBuffer expanded_encoding;
  Error expanded_error;
  Expect(EncodeJpeg(jpeg.data(), jpeg.size(), options, &expanded_encoding,
                    &expanded_error),
         "a constrained initial encoder buffer must grow successfully");
  Expect(expanded_encoding.capacity() > 32,
         "a constrained initial encoder buffer must exercise output growth");
  OutputBuffer expanded_restoration;
  Expect(ReconstructJpeg(expanded_encoding.data(), expanded_encoding.size(),
                         options, &expanded_restoration, &expanded_error) &&
             Equals(expanded_restoration, jpeg),
         "encoder output growth must preserve byte-exact reconstruction");
}

void TestVeryLargeDeadline(const std::vector<std::uint8_t> &jpeg) {
  const Options options{7, 0, TaskPriority::kNormal, 0,
                        std::numeric_limits<std::int64_t>::max()};
  OutputBuffer encoded;
  Error error;
  Expect(EncodeJpeg(jpeg.data(), jpeg.size(), options, &encoded, &error),
         "a very large deadline must not overflow into an immediate timeout");
}

void TestExactRoundTrip(const std::vector<std::uint8_t> &jpeg) {
  const Options options{7, 0, TaskPriority::kNormal, 0, 0};
  OutputBuffer encoded;
  Error error;
  Expect(EncodeJpeg(jpeg.data(), jpeg.size(), options, &encoded, &error),
         "valid JPEG must encode");
  if (encoded.size() == 0) return;

  OutputBuffer restored;
  Expect(ReconstructJpeg(encoded.data(), encoded.size(), options, &restored,
                         &error),
         "encoded JPEG must reconstruct");
  Expect(Equals(restored, jpeg), "reconstruction must be byte-for-byte exact");
}

void TestTuningRoundTrips(const std::vector<std::uint8_t> &jpeg) {
  const std::array<Options, 4> options = {{
      {1, 0, TaskPriority::kLow, 11, 0},
      {3, 4, TaskPriority::kNormal, 12, 0},
      {7, 2, TaskPriority::kHigh, 13, 0},
      {9, 4, TaskPriority::kLow, 14, 0},
  }};
  for (const auto &option : options) {
    OutputBuffer encoded;
    Error error;
    Expect(EncodeJpeg(jpeg.data(), jpeg.size(), option, &encoded, &error),
           "every valid tuning and priority combination must encode");
    if (encoded.size() == 0) continue;
    OutputBuffer restored;
    Expect(ReconstructJpeg(encoded.data(), encoded.size(), option, &restored,
                           &error) &&
               Equals(restored, jpeg),
           "every valid tuning and priority combination must remain "
           "byte-exact");
  }
}

void TestMutationStress(const std::vector<std::uint8_t> &jpeg,
                        const std::vector<std::uint8_t> &jxl) {
  std::uint32_t state = 0x9e3779b9U;
  auto next = [&] {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };

  for (int iteration = 0; iteration < 128; ++iteration) {
    auto mutated_jpeg = jpeg;
    auto mutated_jxl = jxl;
    const int mutations = 1 + iteration % 8;
    for (int mutation = 0; mutation < mutations; ++mutation) {
      const std::size_t jpeg_index = next() % mutated_jpeg.size();
      mutated_jpeg[jpeg_index] ^=
          static_cast<std::uint8_t>(1u << (next() % 8));
      const std::size_t jxl_index = next() % mutated_jxl.size();
      mutated_jxl[jxl_index] ^=
          static_cast<std::uint8_t>(1u << (next() % 8));
    }
    if (iteration % 4 == 0) {
      mutated_jpeg.resize(1 + next() % mutated_jpeg.size());
      mutated_jxl.resize(1 + next() % mutated_jxl.size());
    }
    if (iteration % 5 == 0) {
      mutated_jpeg.insert(mutated_jpeg.begin() + next() % mutated_jpeg.size(),
                          static_cast<std::uint8_t>(next()));
      mutated_jxl.insert(mutated_jxl.begin() + next() % mutated_jxl.size(),
                         static_cast<std::uint8_t>(next()));
    }
    if (iteration % 7 == 0) {
      const std::size_t jpeg_start = next() % mutated_jpeg.size();
      const std::size_t jxl_start = next() % mutated_jxl.size();
      std::fill(mutated_jpeg.begin() + jpeg_start,
                mutated_jpeg.begin() +
                    std::min(mutated_jpeg.size(), jpeg_start + 16),
                0);
      std::fill(mutated_jxl.begin() + jxl_start,
                mutated_jxl.begin() +
                    std::min(mutated_jxl.size(), jxl_start + 16),
                0);
    }

    const Options options{
        1 + iteration % 9,
        iteration % 5,
        static_cast<TaskPriority>(iteration % 3),
        static_cast<std::uint64_t>(100 + iteration),
        0,
    };
    OutputBuffer encoded;
    Error encode_error;
    if (EncodeJpeg(mutated_jpeg.data(), mutated_jpeg.size(), options, &encoded,
                   &encode_error)) {
      OutputBuffer restored;
      Error restore_error;
      Expect(ReconstructJpeg(encoded.data(), encoded.size(), options,
                             &restored, &restore_error) &&
                 Equals(restored, mutated_jpeg),
             "every accepted mutated JPEG must reconstruct byte-exactly");
    } else {
      Expect(!encode_error.message.empty() &&
                 (encode_error.code == ErrorCode::kUnsupportedInput ||
                  encode_error.code == ErrorCode::kCodec),
             "every rejected mutated JPEG must return a stable codec error");
    }

    OutputBuffer restored;
    Error restore_error;
    if (ReconstructJpeg(mutated_jxl.data(), mutated_jxl.size(), options,
                        &restored, &restore_error)) {
      Expect(restored.size() >= 2 && restored.data()[0] == 0xff &&
                 restored.data()[1] == 0xd8,
             "every accepted mutated JXL must reconstruct a JPEG stream");
    } else {
      Expect(!restore_error.message.empty() &&
                 (restore_error.code == ErrorCode::kUnsupportedInput ||
                  restore_error.code == ErrorCode::kCodec),
             "every rejected mutated JXL must return a stable codec error");
    }
  }
}

void TestMalformedInputs() {
  const Options options{7, 0, TaskPriority::kNormal, 0, 0};
  for (const std::size_t size :
       {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4},
        std::size_t{7}, std::size_t{8}, std::size_t{15}, std::size_t{16},
        std::size_t{31}, std::size_t{32}, std::size_t{63}, std::size_t{64},
        std::size_t{65}, std::size_t{127}, std::size_t{128},
        std::size_t{255}, std::size_t{256}, std::size_t{1023},
        std::size_t{1024}, std::size_t{4095}, std::size_t{4096},
        std::size_t{4097}, std::size_t{65535}}) {
    const auto payload = DeterministicPayload(size);
    Error encode_error;
    OutputBuffer encoded;
    Expect(!EncodeJpeg(payload.data(), payload.size(), options, &encoded,
                       &encode_error),
           "random payload must not encode as JPEG");

    Error decode_error;
    OutputBuffer restored;
    Expect(!ReconstructJpeg(payload.data(), payload.size(), options, &restored,
                            &decode_error),
           "random payload must not reconstruct as JXL");
  }
}

void TestTruncatedInputs(const std::vector<std::uint8_t> &jpeg) {
  const Options options{7, 0, TaskPriority::kNormal, 0, 0};
  OutputBuffer complete;
  Error error;
  Expect(EncodeJpeg(jpeg.data(), jpeg.size(), options, &complete, &error),
         "truncation test fixture must encode");

  for (const std::size_t size :
       {std::size_t{2}, jpeg.size() / 4, jpeg.size() - 1}) {
    OutputBuffer encoded;
    Error truncated_error;
    Expect(!EncodeJpeg(jpeg.data(), size, options, &encoded, &truncated_error),
           "truncated JPEG must not encode");
  }
  for (const std::size_t size :
       {std::size_t{2}, complete.size() / 4, complete.size() - 1}) {
    OutputBuffer restored;
    Error truncated_error;
    Expect(!ReconstructJpeg(complete.data(), size, options, &restored,
                            &truncated_error),
           "truncated JXL must not reconstruct");
  }
}

void TestInitialAllocationLimit(const std::vector<std::uint8_t> &jpeg) {
  const Options options{7, 0, TaskPriority::kNormal, 0, 0};
  OutputBuffer limited(1024);
  Error error;
  Expect(!EncodeJpeg(jpeg.data(), jpeg.size(), options, &limited, &error),
         "initial output allocation must obey its safety limit");
  Expect(error.code == ErrorCode::kCodec,
         "allocation-limit failures must be codec errors");
  Expect(limited.capacity() == 0,
         "rejected initial allocations must not reserve memory");
}

void TestTimeout(const std::vector<std::uint8_t> &jpeg) {
  const Options options{9, 0, TaskPriority::kNormal, 0, 1};
  OutputBuffer output;
  Error error;
  Expect(!EncodeJpeg(jpeg.data(), jpeg.size(), options, &output, &error),
         "one-millisecond effort-9 encode must time out");
  Expect(error.code == ErrorCode::kTimeout,
         "expired encode must report the timeout code");
}

void TestReconstructionTimeout(const std::vector<std::uint8_t> &jxl) {
  const Options options{7, 0, TaskPriority::kNormal, 0, 1};
  OutputBuffer output;
  Error error;
  Expect(!ReconstructJpeg(jxl.data(), jxl.size(), options, &output, &error),
         "one-millisecond reconstruction must time out");
  Expect(error.code == ErrorCode::kTimeout,
         "expired reconstruction must report the timeout code");
}

void TestConcurrentStress(const std::vector<std::uint8_t> &jpeg) {
  const Options options{7, 0, TaskPriority::kNormal, 0, 0};
  OutputBuffer reference;
  Error reference_error;
  Expect(EncodeJpeg(jpeg.data(), jpeg.size(), options, &reference,
                    &reference_error),
         "concurrency test fixture must encode");
  const std::vector<std::uint8_t> jxl(reference.data(),
                                      reference.data() + reference.size());

  std::atomic<int> stress_failures{0};
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&, worker] {
      for (int iteration = 0; iteration < 8; ++iteration) {
        if ((worker + iteration) % 3 == 0) {
          OutputBuffer cancelled;
          Error timeout_error;
          const Options timeout_options{9, 0, TaskPriority::kNormal, 0, 1};
          if (EncodeJpeg(jpeg.data(), jpeg.size(), timeout_options, &cancelled,
                         &timeout_error) ||
              timeout_error.code != ErrorCode::kTimeout) {
            ++stress_failures;
          }
          continue;
        }

        OutputBuffer encoded;
        Error encode_error;
        if (!EncodeJpeg(jpeg.data(), jpeg.size(), options, &encoded,
                        &encode_error)) {
          ++stress_failures;
          continue;
        }
        OutputBuffer restored;
        Error decode_error;
        if (!ReconstructJpeg(jxl.data(), jxl.size(), options, &restored,
                             &decode_error) ||
            !Equals(restored, jpeg)) {
          ++stress_failures;
        }
      }
    });
  }
  for (auto &worker : workers) worker.join();
  Expect(stress_failures.load() == 0,
         "mixed encode, decode, and cancellation stress must pass");
}

int RunSharedConfigurationRaceTest() {
  std::mutex start_mutex;
  std::condition_variable start_condition;
  int ready = 0;
  bool start = false;
  std::array<SchedulerConfigurationResult, 2> results{};
  std::array<std::size_t, 2> effective{};
  std::array<std::thread, 2> contenders;
  for (std::size_t index = 0; index < contenders.size(); ++index) {
    contenders[index] = std::thread([&, index] {
      {
        std::unique_lock<std::mutex> lock(start_mutex);
        ++ready;
        start_condition.notify_all();
        start_condition.wait(lock, [&] { return start; });
      }
      results[index] =
          ConfigureSharedParallelScheduler(index + 1, &effective[index]);
    });
  }
  {
    std::unique_lock<std::mutex> lock(start_mutex);
    start_condition.wait(lock, [&] { return ready == 2; });
    start = true;
  }
  start_condition.notify_all();
  for (auto &contender : contenders) contender.join();

  const int successes = static_cast<int>(std::count(
      results.begin(), results.end(), SchedulerConfigurationResult::kSuccess));
  const int conflicts = static_cast<int>(std::count(
      results.begin(), results.end(),
      SchedulerConfigurationResult::kAlreadyStarted));
  Expect(successes == 1 && conflicts == 1,
         "a shared-pool configuration race must have exactly one winner");
  const std::size_t installed = SharedParallelRunnerWorkerCount();
  Expect(installed >= 1 && installed <= 2 && effective[0] == installed &&
             effective[1] == installed,
         "every configuration contender must observe the installed pool");
  return failures == 0 ? 0 : 1;
}

int RunSharedConfigurationAndFirstUseRaceTest() {
  std::mutex start_mutex;
  std::condition_variable start_condition;
  int ready = 0;
  bool start = false;
  SchedulerConfigurationResult configuration =
      SchedulerConfigurationResult::kInvalidArguments;
  std::size_t configured_effective = 0;
  RunnerProbe probe(256);
  const SharedRunnerContext context{};
  JxlParallelRetCode runner_result = JXL_PARALLEL_RET_RUNNER_ERROR;

  const auto await_start = [&] {
    std::unique_lock<std::mutex> lock(start_mutex);
    ++ready;
    start_condition.notify_all();
    start_condition.wait(lock, [&] { return start; });
  };
  std::thread configure([&] {
    await_start();
    configuration = ConfigureSharedParallelScheduler(2, &configured_effective);
  });
  std::thread first_use([&] {
    await_start();
    runner_result = SharedParallelRunner(
        const_cast<SharedRunnerContext *>(&context), &probe, ProbeInit,
        ProbeFunction, 0, 256);
  });
  {
    std::unique_lock<std::mutex> lock(start_mutex);
    start_condition.wait(lock, [&] { return ready == 2; });
    start = true;
  }
  start_condition.notify_all();
  configure.join();
  first_use.join();

  const std::size_t installed = SharedParallelRunnerWorkerCount();
  Expect(runner_result == JXL_PARALLEL_RET_SUCCESS &&
             probe.init_calls == 1 && probe.calls.load() == 256 &&
             std::all_of(probe.counts.begin(), probe.counts.end(),
                         [](int count) { return count == 1; }),
         "first use racing configuration must execute every callback once");
  Expect((configuration == SchedulerConfigurationResult::kSuccess ||
          configuration == SchedulerConfigurationResult::kAlreadyStarted) &&
             configured_effective == installed &&
             probe.reported_threads == installed,
         "configuration and first use must agree on the first installed pool");
  return failures == 0 ? 0 : 1;
}

int RunSharedLazyConfigurationTest() {
  RunnerProbe probe(1);
  const SharedRunnerContext context{};
  Expect(SharedParallelRunner(const_cast<SharedRunnerContext *>(&context),
                              &probe, ProbeInit, ProbeFunction, 0, 1) ==
                 JXL_PARALLEL_RET_SUCCESS &&
             probe.init_calls == 1 && probe.calls.load() == 1,
         "first use must lazily install and execute the automatic pool");

  const std::size_t installed = SharedParallelRunnerWorkerCount();
  std::size_t effective = 0;
  Expect(ConfigureSharedParallelScheduler(0, &effective) ==
                 SchedulerConfigurationResult::kSuccess &&
             effective == installed,
         "automatic configuration must be idempotent after lazy first use");
  const std::size_t conflicting = installed == 1 ? 2 : 1;
  Expect(ConfigureSharedParallelScheduler(conflicting, &effective) ==
                 SchedulerConfigurationResult::kAlreadyStarted &&
             effective == installed,
         "explicit configuration must not replace the lazy automatic pool");
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--lazy-configuration") {
    return RunSharedLazyConfigurationTest();
  }
  if (argc == 2 && std::string(argv[1]) == "--configuration-race") {
    return RunSharedConfigurationRaceTest();
  }
  if (argc == 2 &&
      std::string(argv[1]) == "--configuration-first-use-race") {
    return RunSharedConfigurationAndFirstUseRaceTest();
  }
  if (argc != 3) {
    std::cerr << "usage: jxl_codec_test JPEG_FIXTURE JXL_FIXTURE\n"
                 "       jxl_codec_test "
                 "[--lazy-configuration|--configuration-race|"
                 "--configuration-first-use-race]\n";
    return 2;
  }
  const auto jpeg = ReadFile(argv[1]);
  const auto jxl = ReadFile(argv[2]);
  if (jpeg.empty() || jxl.empty()) {
    std::cerr << "fixtures could not be read\n";
    return 2;
  }

  TestSharedRunnerBasicBehavior();
  TestSaturatingDeadline();
  TestOutputBufferBoundaries();
  TestSharedRunnerBoundariesAndShutdown();
  TestInjectedSchedulerResourceFailures();
  TestSharedRunnerFairnessAndPoolLimit();
  TestSharedRunnerContentionStress();
  TestSharedRunnerPriorityWeights();
  TestSharedRunnerCancellation();
  TestGlobalSchedulerConfiguration();
  ExpectInvalidCodecArguments(jpeg);
  TestInjectedCodecFailures(jpeg);
  TestEncoderErrorClassification();
  TestInjectedTimeoutCheckpoints(jpeg);
  TestExactRoundTrip(jpeg);
  TestTuningRoundTrips(jpeg);
  TestMalformedInputs();
  TestTruncatedInputs(jpeg);
  TestInitialAllocationLimit(jpeg);
  TestOutputGrowthAndLimits(jpeg);
  TestVeryLargeDeadline(jpeg);
  TestTimeout(jpeg);
  TestReconstructionTimeout(jxl);
  TestConcurrentStress(jpeg);
  TestMutationStress(jpeg, jxl);

  const Options options{7, 0, TaskPriority::kNormal, 0, 0};
  OutputBuffer restored;
  Error error;
  Expect(ReconstructJpeg(jxl.data(), jxl.size(), options, &restored, &error),
         "existing JXL fixture must remain reconstructable");
  Expect(restored.size() > 2 && restored.data()[0] == 0xff &&
             restored.data()[1] == 0xd8,
         "existing JXL fixture must reconstruct a JPEG");

  if (failures != 0) return 1;
  std::cout << "native codec sanitizer tests passed\n";
  return 0;
}
