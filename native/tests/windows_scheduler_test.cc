#define JXL_CODER_ENABLE_TEST_HOOKS 1
#define JXL_CODER_WINDOWS_SCHEDULER_TEST_ONLY 1

#include "../../windows/jxl_coder_plugin.cpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using jxl_coder::AdmissionConfigurationResult;
using jxl_coder::AdmissionErrorCode;
using jxl_coder::ConversionScheduler;
using jxl_coder::SetAdmissionAfterTimerFailureForTesting;
using jxl_coder::SetAdmissionEnqueueFailureForTesting;
using jxl_coder::SetAdmissionGateFailureForTesting;
using jxl_coder::SetAdmissionThreadStartupFailureCountdownForTesting;
using jxl_coder::TaskPriority;

std::atomic<int> failures{0};

void Expect(bool condition, const std::string& message) {
  if (condition) return;
  std::cerr << "FAIL: " << message << '\n';
  failures.fetch_add(1, std::memory_order_relaxed);
}

class Latch {
 public:
  explicit Latch(int target = 1) : target_(target) {}

  void CountDown() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++count_;
    condition_.notify_all();
  }

  bool Wait(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [&] { return count_ >= target_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  int count_ = 0;
  int target_;
};

class Gate {
 public:
  void EnterAndWait() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [&] { return released_; });
  }

  bool WaitUntilEntered() {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5),
                               [&] { return entered_; });
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool released_ = false;
};

bool Submit(ConversionScheduler& scheduler, TaskPriority priority,
            std::uint64_t group, std::function<void()> on_failure,
            std::function<void()> operation,
            AdmissionConfigurationResult* submission_result = nullptr) {
  return scheduler.Submit(priority, group,
                          std::chrono::steady_clock::time_point::max(), {},
                          std::move(on_failure), std::move(operation),
                          submission_result);
}

void TestConfigurationAndStartupRollback(ConversionScheduler& scheduler) {
  Expect(scheduler.Configure(257, 1) ==
             AdmissionConfigurationResult::kInvalidArguments,
         "worker counts above 256 must be rejected");
  Expect(scheduler.Configure(2, 257) ==
             AdmissionConfigurationResult::kInvalidArguments,
         "active-conversion counts above 256 must be rejected");
  Expect(scheduler.Configure(2, 1) ==
             AdmissionConfigurationResult::kSuccess,
         "a valid Windows admission configuration must succeed");
  Expect(scheduler.Configure(2, 1) ==
             AdmissionConfigurationResult::kSuccess,
         "identical Windows admission configuration must be idempotent");
  Expect(scheduler.Configure(2, 2) ==
             AdmissionConfigurationResult::kAlreadyStarted,
         "a conflicting Windows admission configuration must fail");
  Expect(scheduler.Configure(3, 1) ==
             AdmissionConfigurationResult::kAlreadyStarted,
         "a conflicting Windows worker count must fail");

  AdmissionConfigurationResult submission =
      AdmissionConfigurationResult::kSuccess;
  Expect(!Submit(scheduler, static_cast<TaskPriority>(3), 1, [] {}, [] {},
                 &submission) &&
             submission == AdmissionConfigurationResult::kInvalidArguments,
         "invalid priorities must not index the admission queues");
  Expect(!Submit(scheduler, static_cast<TaskPriority>(3), 1, [] {}, [] {}),
         "invalid priorities must be rejected without a diagnostic output");

  SetAdmissionGateFailureForTesting(true);
  Expect(!Submit(scheduler, TaskPriority::kNormal, 1, [] {}, [] {}),
         "startup failures must be contained without a diagnostic output");

  SetAdmissionGateFailureForTesting(true);
  Expect(!Submit(scheduler, TaskPriority::kNormal, 1, [] {}, [] {},
                 &submission) &&
             submission == AdmissionConfigurationResult::kResourceFailure,
         "failure to allocate the startup gate must reject submission");

  SetAdmissionThreadStartupFailureCountdownForTesting(0);
  Expect(!Submit(scheduler, TaskPriority::kNormal, 1, [] {}, [] {},
                 &submission) &&
             submission == AdmissionConfigurationResult::kResourceFailure,
         "failure to create the first admission worker must reject submission");

  SetAdmissionThreadStartupFailureCountdownForTesting(1);
  Expect(!Submit(scheduler, TaskPriority::kNormal, 1, [] {}, [] {},
                 &submission) &&
             submission == AdmissionConfigurationResult::kResourceFailure,
         "failure to create the expiry timer must roll back the staged worker");

  SetAdmissionThreadStartupFailureCountdownForTesting(-1);
  SetAdmissionAfterTimerFailureForTesting(true);
  Expect(!Submit(scheduler, TaskPriority::kNormal, 1, [] {}, [] {},
                 &submission) &&
             submission == AdmissionConfigurationResult::kResourceFailure,
         "failure after timer creation must join every staged thread");

  SetAdmissionThreadStartupFailureCountdownForTesting(-1);
  Latch completed;
  Expect(Submit(scheduler, TaskPriority::kNormal, 1,
                [&] { Expect(false, "successful startup must not fail"); },
                [&] { completed.CountDown(); }, &submission) &&
             submission == AdmissionConfigurationResult::kSuccess,
         "submission must recover after injected startup failures");
  Expect(completed.Wait(),
         "the recovered admission scheduler must execute submitted work");

  SetAdmissionEnqueueFailureForTesting(true);
  Expect(!Submit(scheduler, TaskPriority::kNormal, 2, [] {}, [] {},
                 &submission) &&
             submission == AdmissionConfigurationResult::kResourceFailure,
         "queue allocation failures must retain their scheduler cause");
}

void TestPriorityAndGroupFairness(ConversionScheduler& scheduler) {
  Gate blocker;
  Latch blocker_done;
  Expect(Submit(scheduler, TaskPriority::kLow, 99, [] {}, [&] {
           blocker.EnterAndWait();
           blocker_done.CountDown();
         }),
         "the priority test blocker must submit");
  Expect(blocker.WaitUntilEntered(), "the priority test blocker must start");

  std::mutex sequence_mutex;
  std::vector<int> sequence;
  Latch weighted_done(7);
  for (const auto [priority, count] :
       std::vector<std::pair<TaskPriority, int>>{
           {TaskPriority::kHigh, 4},
           {TaskPriority::kNormal, 2},
           {TaskPriority::kLow, 1},
       }) {
    for (int index = 0; index < count; ++index) {
      const int marker = static_cast<int>(priority);
      Expect(Submit(scheduler, priority,
                    static_cast<std::uint64_t>(marker * 10 + index + 1),
                    [&] {
                      Expect(false, "weighted admission work must not fail");
                      weighted_done.CountDown();
                    },
                    [&, marker] {
                      {
                        std::lock_guard<std::mutex> lock(sequence_mutex);
                        sequence.push_back(marker);
                      }
                      weighted_done.CountDown();
                    }),
             "weighted admission work must submit");
    }
  }
  blocker.Release();
  Expect(blocker_done.Wait(), "the priority blocker must finish");
  Expect(weighted_done.Wait(), "all weighted admission work must finish");
  {
    std::lock_guard<std::mutex> lock(sequence_mutex);
    Expect(sequence == std::vector<int>({2, 2, 2, 2, 1, 1, 0}),
           "Windows admission must preserve the 1:2:4 priority cycle");
  }

  Gate group_blocker;
  Latch group_blocker_done;
  Expect(Submit(scheduler, TaskPriority::kLow, 199, [] {}, [&] {
           group_blocker.EnterAndWait();
           group_blocker_done.CountDown();
         }),
         "the group-fairness blocker must submit");
  Expect(group_blocker.WaitUntilEntered(),
         "the group-fairness blocker must start");

  sequence.clear();
  Latch groups_done(4);
  const auto submit_marker = [&](std::uint64_t group, int marker) {
    Expect(Submit(scheduler, TaskPriority::kNormal, group,
                  [&] {
                    Expect(false, "group-fair admission work must not fail");
                    groups_done.CountDown();
                  },
                  [&, marker] {
                    {
                      std::lock_guard<std::mutex> lock(sequence_mutex);
                      sequence.push_back(marker);
                    }
                    groups_done.CountDown();
                  }),
           "group-fair admission work must submit");
  };
  submit_marker(10, 1);
  submit_marker(10, 1);
  submit_marker(10, 1);
  submit_marker(20, 2);
  group_blocker.Release();
  Expect(group_blocker_done.Wait(), "the group-fairness blocker must finish");
  Expect(groups_done.Wait(), "all group-fair admission work must finish");
  {
    std::lock_guard<std::mutex> lock(sequence_mutex);
    Expect(sequence == std::vector<int>({1, 2, 1, 1}),
           "same-priority request groups must advance round-robin");
  }
}

void TestQueuedExpiryAndExceptionRecovery(ConversionScheduler& scheduler) {
  Gate blocker;
  Latch blocker_done;
  Expect(Submit(scheduler, TaskPriority::kLow, 300, [] {}, [&] {
           blocker.EnterAndWait();
           blocker_done.CountDown();
         }),
         "the timeout blocker must submit");
  Expect(blocker.WaitUntilEntered(), "the timeout blocker must start");

  Latch expired;
  std::atomic<bool> expired_operation_ran{false};
  Expect(scheduler.Submit(
             TaskPriority::kHigh, 301,
             std::chrono::steady_clock::now() + std::chrono::milliseconds(20),
             [&] { expired.CountDown(); },
             [&] {
               Expect(false, "queued timeout must not use failure callback");
             },
             [&] { expired_operation_ran.store(true); }),
         "expiring queued work must submit");
  Expect(expired.Wait(), "queued Windows admission work must expire");
  Expect(!expired_operation_ran.load(),
         "expired queued work must never execute its operation");

  Latch throwing_expiry_called;
  Expect(scheduler.Submit(
             TaskPriority::kNormal, 305,
             std::chrono::steady_clock::now() + std::chrono::milliseconds(20),
             [&] {
               throwing_expiry_called.CountDown();
               throw std::runtime_error("injected expiry callback failure");
             },
             [&] {
               Expect(false, "queued expiry must not use failure callback");
             },
             [&] {
               Expect(false, "throwing expiry callback work must not run");
             }),
         "work with a throwing expiry callback must submit");
  Expect(throwing_expiry_called.Wait(),
         "the throwing expiry callback must still be invoked once");

  Latch sibling_expired;
  Latch sibling_survived;
  Expect(scheduler.Submit(
             TaskPriority::kNormal, 306,
             std::chrono::steady_clock::now() + std::chrono::milliseconds(20),
             [&] { sibling_expired.CountDown(); },
             [&] {
               Expect(false, "queued sibling expiry must not use failure");
             },
             [&] {
               Expect(false, "expired sibling work must never execute");
             }),
         "an expiring same-group sibling must submit");
  Expect(Submit(scheduler, TaskPriority::kNormal, 306,
                [&] {
                  Expect(false, "persistent same-group sibling must not fail");
                  sibling_survived.CountDown();
                },
                [&] { sibling_survived.CountDown(); }),
         "a persistent same-group sibling must submit");
  Expect(sibling_expired.Wait(), "the targeted same-group job must expire");

  std::atomic<bool> empty_expiry_operation_ran{false};
  Expect(scheduler.Submit(
             TaskPriority::kNormal, 307,
             std::chrono::steady_clock::now() + std::chrono::milliseconds(20),
             {},
             [&] {
               Expect(false, "empty expiry callback must not use failure");
             },
             [&] { empty_expiry_operation_ran.store(true); }),
         "work with an empty expiry callback must submit");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  blocker.Release();
  Expect(blocker_done.Wait(), "the timeout blocker must finish");
  Expect(sibling_survived.Wait(),
         "expiry must preserve another job in the same request group");

  Latch expiry_fence;
  Expect(Submit(scheduler, TaskPriority::kLow, 308,
                [&] {
                  Expect(false, "post-expiry fence must not fail");
                  expiry_fence.CountDown();
                },
                [&] { expiry_fence.CountDown(); }),
         "post-expiry fence work must submit");
  Expect(expiry_fence.Wait(), "post-expiry fence work must execute");
  Expect(!empty_expiry_operation_ran.load(),
         "expired work with no callback must still be removed");

  Latch failed;
  Expect(Submit(scheduler, TaskPriority::kNormal, 302,
                [&] { failed.CountDown(); },
                [] { throw std::runtime_error("injected operation failure"); }),
         "throwing admission work must submit");
  Expect(failed.Wait(),
         "unexpected operation exceptions must reach the failure callback");

  Latch throwing_failure_called;
  Expect(Submit(
             scheduler, TaskPriority::kNormal, 303,
             [&] {
               throwing_failure_called.CountDown();
               throw std::runtime_error("injected failure callback failure");
             },
             [] { throw std::runtime_error("second operation failure"); }),
         "work with a throwing failure callback must submit");
  Expect(throwing_failure_called.Wait(),
         "the throwing failure callback must still be invoked once");

  Expect(Submit(scheduler, TaskPriority::kNormal, 309, {}, [] {
           throw std::runtime_error("operation failure without callback");
         }),
         "throwing work without a failure callback must submit");

  Latch survivor;
  Expect(Submit(scheduler, TaskPriority::kNormal, 304,
                [&] { Expect(false, "survivor work must not fail"); },
                [&] { survivor.CountDown(); }),
         "work after callback exceptions must submit");
  Expect(survivor.Wait(),
         "the admission worker must survive operation and callback exceptions");
}

void TestGroupIdentifiers(ConversionScheduler& scheduler) {
  const std::uint64_t first = scheduler.MakeGroup();
  const std::uint64_t second = scheduler.MakeGroup();
  Expect(first != 0 && second != 0 && second != first,
         "request-group identifiers must be nonzero and distinct");

  scheduler.SetNextGroupForTesting(
      std::numeric_limits<std::uint64_t>::max());
  const std::uint64_t maximum = scheduler.MakeGroup();
  const std::uint64_t wrapped = scheduler.MakeGroup();
  const std::uint64_t after_wrapped = scheduler.MakeGroup();
  Expect(maximum == std::numeric_limits<std::uint64_t>::max() &&
             wrapped == 1 && after_wrapped == 2,
         "request-group identifiers must skip zero after integer wrap");
}

void TestAdmissionErrorCodes() {
  Expect(std::string(AdmissionErrorCode(
             AdmissionConfigurationResult::kInvalidArguments)) ==
             "INVALID_ARGUMENTS",
         "invalid admission failures must retain their public code");
  Expect(std::string(AdmissionErrorCode(
             AdmissionConfigurationResult::kAlreadyStarted)) ==
             "SCHEDULER_ALREADY_STARTED",
         "native-pool conflicts must retain their public scheduler code");
  Expect(std::string(AdmissionErrorCode(
             AdmissionConfigurationResult::kResourceFailure)) ==
             "SCHEDULER_ERROR" &&
             std::string(AdmissionErrorCode(
                 AdmissionConfigurationResult::kSuccess)) ==
                 "SCHEDULER_ERROR",
         "resource and defensive admission failures must be scheduler errors");
}

}  // namespace

int RunLazyInitializationTest() {
  auto& scheduler = ConversionScheduler::Instance();
  Latch completed;
  Expect(Submit(scheduler, TaskPriority::kNormal, 1,
                [&] { Expect(false, "lazy scheduler startup must not fail"); },
                [&] { completed.CountDown(); }),
         "the first conversion must lazily start automatic admission defaults");
  Expect(completed.Wait(), "lazy automatic admission work must execute");
  Expect(scheduler.Configure(0, 0) ==
             AdmissionConfigurationResult::kSuccess,
         "automatic configuration after lazy startup must be idempotent");
  Expect(scheduler.Configure(0, 256) ==
             AdmissionConfigurationResult::kAlreadyStarted,
         "a conversion must make conflicting scheduler changes immutable");
  return failures.load(std::memory_order_relaxed) == 0 ? 0 : 1;
}

int RunAutomaticConfigurationTest() {
  auto& scheduler = ConversionScheduler::Instance();
  Expect(scheduler.Configure(0, 0) ==
             AdmissionConfigurationResult::kSuccess,
         "automatic Windows admission configuration must succeed");
  Expect(scheduler.Configure(0, 0) ==
             AdmissionConfigurationResult::kSuccess,
         "automatic Windows admission configuration must be idempotent");
  Expect(scheduler.Configure(0, 256) ==
             AdmissionConfigurationResult::kAlreadyStarted,
         "automatic admission configuration must remain immutable");
  Latch completed;
  Expect(Submit(scheduler, TaskPriority::kLow, 1,
                [&] { Expect(false, "automatic work must not fail"); },
                [&] { completed.CountDown(); }),
         "automatic admission work must submit");
  Expect(completed.Wait(), "automatic admission work must execute");
  return failures.load(std::memory_order_relaxed) == 0 ? 0 : 1;
}

int RunConfigurationRaceTest() {
  auto& scheduler = ConversionScheduler::Instance();
  std::mutex start_mutex;
  std::condition_variable start_condition;
  int ready = 0;
  bool start = false;
  std::array<AdmissionConfigurationResult, 2> results{};
  std::array<std::thread, 2> contenders;
  for (std::size_t index = 0; index < contenders.size(); ++index) {
    contenders[index] = std::thread([&, index] {
      {
        std::unique_lock<std::mutex> lock(start_mutex);
        ++ready;
        start_condition.notify_all();
        start_condition.wait(lock, [&] { return start; });
      }
      const std::size_t limit = index + 1;
      results[index] = scheduler.Configure(limit, limit);
    });
  }
  {
    std::unique_lock<std::mutex> lock(start_mutex);
    start_condition.wait(lock, [&] { return ready == 2; });
    start = true;
  }
  start_condition.notify_all();
  for (auto& contender : contenders) contender.join();

  const auto successes = static_cast<int>(std::count(
      results.begin(), results.end(), AdmissionConfigurationResult::kSuccess));
  const auto conflicts = static_cast<int>(std::count(
      results.begin(), results.end(),
      AdmissionConfigurationResult::kAlreadyStarted));
  Expect(successes == 1 && conflicts == 1,
         "a Windows configuration race must have exactly one winner");

  Latch completed;
  Expect(Submit(scheduler, TaskPriority::kNormal, 1,
                [&] { Expect(false, "post-race work must not fail"); },
                [&] { completed.CountDown(); }),
         "the winning Windows scheduler must accept work");
  Expect(completed.Wait(), "work after the configuration race must execute");
  return failures.load(std::memory_order_relaxed) == 0 ? 0 : 1;
}

int RunConfigurationAndFirstUseRaceTest() {
  auto& scheduler = ConversionScheduler::Instance();
  std::mutex start_mutex;
  std::condition_variable start_condition;
  int ready = 0;
  bool start = false;
  AdmissionConfigurationResult configuration =
      AdmissionConfigurationResult::kResourceFailure;
  bool submitted = false;
  Latch completed;

  const auto await_start = [&] {
    std::unique_lock<std::mutex> lock(start_mutex);
    ++ready;
    start_condition.notify_all();
    start_condition.wait(lock, [&] { return start; });
  };
  std::thread configure([&] {
    await_start();
    configuration = scheduler.Configure(2, 3);
  });
  std::thread convert([&] {
    await_start();
    submitted = Submit(
        scheduler, TaskPriority::kNormal, 1,
        [&] {
          Expect(false, "conversion racing configuration must not fail");
          completed.CountDown();
        },
        [&] { completed.CountDown(); });
  });
  {
    std::unique_lock<std::mutex> lock(start_mutex);
    start_condition.wait(lock, [&] { return ready == 2; });
    start = true;
  }
  start_condition.notify_all();
  configure.join();
  convert.join();

  Expect(submitted, "the conversion side of the startup race must submit");
  Expect(completed.Wait(), "the racing conversion must execute");
  Expect(configuration == AdmissionConfigurationResult::kSuccess ||
             configuration == AdmissionConfigurationResult::kAlreadyStarted,
         "configuration and first use must resolve by first-wins semantics");
  if (configuration == AdmissionConfigurationResult::kSuccess) {
    Expect(scheduler.Configure(2, 3) ==
               AdmissionConfigurationResult::kSuccess,
           "the explicit configuration winner must remain installed");
    Expect(scheduler.Configure(0, 0) ==
               AdmissionConfigurationResult::kAlreadyStarted,
           "automatic defaults must not replace the explicit winner");
  } else {
    Expect(scheduler.Configure(0, 0) ==
               AdmissionConfigurationResult::kSuccess,
           "the lazy automatic winner must remain installed");
    Expect(scheduler.Configure(2, 3) ==
               AdmissionConfigurationResult::kAlreadyStarted,
           "explicit limits must not replace the lazy winner");
  }
  return failures.load(std::memory_order_relaxed) == 0 ? 0 : 1;
}

int RunConfigurationConflictTest(bool lazy) {
  std::size_t effective = 0;
  Expect(jxl_coder::ConfigureSharedParallelScheduler(1, &effective) ==
             jxl_coder::SchedulerConfigurationResult::kSuccess &&
             effective == 1,
         "the conflict fixture must preconfigure one shared worker");
  auto& scheduler = ConversionScheduler::Instance();
  if (lazy) {
    AdmissionConfigurationResult submission =
        AdmissionConfigurationResult::kSuccess;
    Expect(!Submit(scheduler, TaskPriority::kNormal, 1, [] {}, [] {},
                   &submission) &&
               submission == AdmissionConfigurationResult::kAlreadyStarted,
           "lazy admission startup must preserve a native pool conflict");
  } else {
    Expect(scheduler.Configure(2, 1) ==
               AdmissionConfigurationResult::kAlreadyStarted,
           "explicit admission configuration must preserve a native conflict");
  }
  return failures.load(std::memory_order_relaxed) == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--lazy") {
    return RunLazyInitializationTest();
  }
  if (argc == 2 && std::string(argv[1]) == "--automatic") {
    return RunAutomaticConfigurationTest();
  }
  if (argc == 2 && std::string(argv[1]) == "--configuration-race") {
    return RunConfigurationRaceTest();
  }
  if (argc == 2 &&
      std::string(argv[1]) == "--configuration-first-use-race") {
    return RunConfigurationAndFirstUseRaceTest();
  }
  if (argc == 2 && std::string(argv[1]) == "--configure-conflict") {
    return RunConfigurationConflictTest(false);
  }
  if (argc == 2 && std::string(argv[1]) == "--lazy-conflict") {
    return RunConfigurationConflictTest(true);
  }
  if (argc != 1) {
    std::cerr << "usage: windows_scheduler_test "
                 "[--lazy|--automatic|--configuration-race|"
                 "--configuration-first-use-race|--configure-conflict|"
                 "--lazy-conflict]\n";
    return 2;
  }

  auto& scheduler = ConversionScheduler::Instance();
  TestConfigurationAndStartupRollback(scheduler);
  TestPriorityAndGroupFairness(scheduler);
  TestQueuedExpiryAndExceptionRecovery(scheduler);
  TestGroupIdentifiers(scheduler);
  TestAdmissionErrorCodes();
  SetAdmissionThreadStartupFailureCountdownForTesting(-1);

  const int failure_count = failures.load(std::memory_order_relaxed);
  if (failure_count != 0) {
    std::cerr << failure_count << " Windows admission scheduler test(s) failed\n";
    return 1;
  }
  std::cout << "Windows admission scheduler tests passed\n";
  return 0;
}
