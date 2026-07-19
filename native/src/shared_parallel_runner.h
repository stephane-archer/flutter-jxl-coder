#ifndef JXL_CODER_NATIVE_SHARED_PARALLEL_RUNNER_H_
#define JXL_CODER_NATIVE_SHARED_PARALLEL_RUNNER_H_

#include <jxl/parallel_runner.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace jxl_coder {

using RunnerCancellationCheck = bool (*)(void *opaque);

enum class TaskPriority : std::uint8_t {
  kLow = 0,
  kNormal = 1,
  kHigh = 2,
};

bool IsValidTaskPriority(TaskPriority priority);

enum class SchedulerConfigurationResult {
  kSuccess,
  kInvalidArguments,
  kAlreadyStarted,
};

/// Per-codec state supplied to the process-wide parallel runner.
struct SharedRunnerContext {
  TaskPriority priority = TaskPriority::kNormal;
  std::uint64_t scheduling_group = 0;
  RunnerCancellationCheck cancelled = nullptr;
  void *cancellation_opaque = nullptr;
};

/// A fair, re-entrant implementation of libjxl's synchronous parallel runner.
///
/// Production calls use SharedParallelRunner below. This class remains
/// constructible so native tests can exercise deterministic small pools.
class SharedParallelScheduler {
 public:
  explicit SharedParallelScheduler(std::size_t worker_count,
                                   std::size_t chunk_size = 4);
  ~SharedParallelScheduler();

  SharedParallelScheduler(const SharedParallelScheduler &) = delete;
  SharedParallelScheduler &operator=(const SharedParallelScheduler &) = delete;

  std::size_t worker_count() const;

  JxlParallelRetCode Run(const SharedRunnerContext &context,
                         void *jpegxl_opaque, JxlParallelRunInit init,
                         JxlParallelRunFunction function,
                         std::uint32_t start_range, std::uint32_t end_range);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

JxlParallelRetCode SharedParallelRunner(void *runner_opaque,
                                        void *jpegxl_opaque,
                                        JxlParallelRunInit init,
                                        JxlParallelRunFunction function,
                                        std::uint32_t start_range,
                                        std::uint32_t end_range);

SchedulerConfigurationResult ConfigureSharedParallelScheduler(
    std::size_t requested_workers, std::size_t *effective_workers);

std::size_t SharedParallelRunnerWorkerCount();

#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
/// Fails one worker launch after the requested number of successful launch
/// attempts. A negative value disables the one-shot failure.
void SetWorkerStartupFailureCountdownForTesting(int countdown);
#endif

}  // namespace jxl_coder

#endif  // JXL_CODER_NATIVE_SHARED_PARALLEL_RUNNER_H_
