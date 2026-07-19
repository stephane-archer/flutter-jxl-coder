#include "shared_parallel_runner.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace jxl_coder {
namespace {

#ifndef JXL_CODER_SHARED_RUNNER_CHUNK_SIZE
#define JXL_CODER_SHARED_RUNNER_CHUNK_SIZE 4
#endif

constexpr std::size_t kMaximumWorkers = 256;
constexpr std::array<std::size_t, 7> kPriorityCycle = {
    2, 2, 2, 2, 1, 1, 0,
};

#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
std::atomic<int> worker_startup_failure_countdown{-1};

bool FailWorkerStartupForTesting() {
  int countdown =
      worker_startup_failure_countdown.load(std::memory_order_relaxed);
  while (countdown >= 0) {
    const int next = countdown == 0 ? -1 : countdown - 1;
    if (worker_startup_failure_countdown.compare_exchange_weak(
            countdown, next, std::memory_order_relaxed)) {
      return countdown == 0;
    }
  }
  return false;
}
#endif

std::size_t AvailableProcessors() {
  return std::min<std::size_t>(
      kMaximumWorkers,
      std::max<std::size_t>(1, std::thread::hardware_concurrency()));
}

bool IsCancelled(const SharedRunnerContext &context) noexcept {
  if (context.cancelled == nullptr) return false;
  try {
    return context.cancelled(context.cancellation_opaque);
  } catch (...) {
    // A cancellation callback belongs to the embedding layer. Treat a broken
    // callback as cancellation so no exception can cross libjxl's C ABI or
    // leave an invocation running after its caller unwinds.
    return true;
  }
}

std::size_t PriorityIndex(TaskPriority priority) {
  return static_cast<std::size_t>(priority);
}

}  // namespace

bool IsValidTaskPriority(TaskPriority priority) {
  return PriorityIndex(priority) <= PriorityIndex(TaskPriority::kHigh);
}

class SharedParallelScheduler::Impl {
 public:
  Impl(std::size_t worker_count, std::size_t chunk_size)
      : worker_count_(std::max<std::size_t>(1, worker_count)),
        chunk_size_(std::max<std::size_t>(1, chunk_size)) {
    workers_.reserve(worker_count_);
    std::exception_ptr startup_error;
    for (std::size_t index = 0; index < worker_count_; ++index) {
      try {
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
        if (FailWorkerStartupForTesting()) {
          throw std::runtime_error("injected worker startup failure");
        }
#endif
        workers_.emplace_back([this] { WorkerLoop(); });
      } catch (...) {
        startup_error = std::current_exception();
        break;
      }
    }
    if (startup_error) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
      }
      ready_condition_.notify_all();
      for (auto &worker : workers_) worker.join();
      std::rethrow_exception(startup_error);
    }
    worker_count_ = workers_.size();
  }

  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    ready_condition_.notify_all();
    for (auto &worker : workers_) worker.join();
  }

  std::size_t worker_count() const { return worker_count_; }

  JxlParallelRetCode Run(const SharedRunnerContext &context,
                         void *jpegxl_opaque, JxlParallelRunInit init,
                         JxlParallelRunFunction function,
                         std::uint32_t start_range, std::uint32_t end_range) {
    if (init == nullptr || function == nullptr ||
        !IsValidTaskPriority(context.priority) || IsCancelled(context)) {
      return JXL_PARALLEL_RET_RUNNER_ERROR;
    }

    const std::size_t work_items =
        end_range > start_range
            ? static_cast<std::size_t>(end_range - start_range)
            : 0;
    const std::size_t lane_count = std::max<std::size_t>(
        1, std::min(worker_count_, std::max<std::size_t>(1, work_items)));

    const JxlParallelRetCode init_result = init(jpegxl_opaque, lane_count);
    if (init_result != JXL_PARALLEL_RET_SUCCESS) return init_result;
    if (work_items == 0) return JXL_PARALLEL_RET_SUCCESS;

    auto invocation = std::make_shared<Invocation>(
        context, jpegxl_opaque, function, start_range, end_range, lane_count);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) return JXL_PARALLEL_RET_RUNNER_ERROR;
      QueueLocked(invocation);
    }
    ready_condition_.notify_one();

    std::unique_lock<std::mutex> lock(mutex_);
    while (!invocation->completed) {
      if (IsCancelled(invocation->context)) CancelLocked(invocation);
      if (!invocation->completed) {
        invocation->done_condition.wait_for(lock, std::chrono::milliseconds(1));
      }
    }
    return invocation->cancelled || invocation->failed
               ? JXL_PARALLEL_RET_RUNNER_ERROR
               : JXL_PARALLEL_RET_SUCCESS;
  }

 private:
  struct Invocation {
    Invocation(const SharedRunnerContext &runner_context, void *function_opaque,
               JxlParallelRunFunction run_function, std::uint32_t start_range,
               std::uint32_t end_range, std::size_t lanes)
        : context(runner_context),
          jpegxl_opaque(function_opaque),
          function(run_function),
          priority(runner_context.priority),
          scheduling_group(runner_context.scheduling_group),
          next_value(start_range),
          end_value(end_range),
          lane_busy(lanes, false) {}

    SharedRunnerContext context;
    void *jpegxl_opaque;
    JxlParallelRunFunction function;
    TaskPriority priority;
    std::uint64_t scheduling_group;
    std::uint32_t next_value;
    const std::uint32_t end_value;
    std::vector<bool> lane_busy;
    std::size_t active_chunks = 0;
    bool queued = false;
    bool cancelled = false;
    bool failed = false;
    bool completed = false;
    std::condition_variable done_condition;
  };

  struct WorkChunk {
    std::shared_ptr<Invocation> invocation;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::size_t lane = 0;
  };

  void QueueLocked(const std::shared_ptr<Invocation> &invocation) {
    if (invocation->queued || invocation->completed || invocation->cancelled ||
        invocation->failed || invocation->next_value >= invocation->end_value ||
        invocation->active_chunks >= invocation->lane_busy.size()) {
      return;
    }
    invocation->queued = true;
    ready_[PriorityIndex(invocation->priority)].push_back(invocation);
  }

  void RemoveReadyLocked(const std::shared_ptr<Invocation> &invocation) {
    auto &queue = ready_[PriorityIndex(invocation->priority)];
    queue.erase(std::remove(queue.begin(), queue.end(), invocation),
                queue.end());
    invocation->queued = false;
  }

  void FinishIfReadyLocked(const std::shared_ptr<Invocation> &invocation) {
    if (invocation->active_chunks != 0) return;
    if (!invocation->cancelled && !invocation->failed &&
        invocation->next_value < invocation->end_value) {
      return;
    }
    invocation->completed = true;
    RemoveReadyLocked(invocation);
    invocation->done_condition.notify_one();
  }

  void CancelLocked(const std::shared_ptr<Invocation> &invocation) {
    invocation->cancelled = true;
    invocation->next_value = invocation->end_value;
    RemoveReadyLocked(invocation);
    FinishIfReadyLocked(invocation);
  }

  bool TakeWorkLocked(WorkChunk *chunk) {
    while (HasReadyLocked()) {
      const std::size_t priority = SelectPriorityLocked();
      auto &queue = ready_[priority];
      auto iterator = std::find_if(
          queue.begin(), queue.end(), [&](const auto &candidate) {
            return candidate->scheduling_group != last_group_[priority];
          });
      if (iterator == queue.end()) iterator = queue.begin();
      auto invocation = std::move(*iterator);
      queue.erase(iterator);
      invocation->queued = false;
      last_group_[priority] = invocation->scheduling_group;

      if (invocation->completed || invocation->cancelled ||
          invocation->failed) {
        FinishIfReadyLocked(invocation);
        continue;
      }
      if (IsCancelled(invocation->context)) {
        CancelLocked(invocation);
        continue;
      }
      if (invocation->next_value >= invocation->end_value) {
        FinishIfReadyLocked(invocation);
        continue;
      }

      const auto lane_iterator = std::find(invocation->lane_busy.begin(),
                                           invocation->lane_busy.end(), false);
      if (lane_iterator == invocation->lane_busy.end()) continue;
      const std::size_t lane = static_cast<std::size_t>(
          lane_iterator - invocation->lane_busy.begin());
      invocation->lane_busy[lane] = true;
      ++invocation->active_chunks;

      chunk->invocation = invocation;
      chunk->begin = invocation->next_value;
      const std::uint64_t proposed_end =
          static_cast<std::uint64_t>(invocation->next_value) + chunk_size_;
      chunk->end = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(invocation->end_value, proposed_end));
      chunk->lane = lane;
      invocation->next_value = chunk->end;
      QueueLocked(invocation);
      ready_condition_.notify_one();
      return true;
    }
    return false;
  }

  bool HasReadyLocked() const {
    return std::any_of(ready_.begin(), ready_.end(),
                       [](const auto &queue) { return !queue.empty(); });
  }

  std::size_t SelectPriorityLocked() {
    for (std::size_t attempt = 0; attempt < kPriorityCycle.size(); ++attempt) {
      const std::size_t priority = kPriorityCycle[priority_cycle_index_];
      priority_cycle_index_ =
          (priority_cycle_index_ + 1) % kPriorityCycle.size();
      if (!ready_[priority].empty()) return priority;
    }
    for (std::size_t priority = 0; priority < ready_.size(); ++priority) {
      if (!ready_[priority].empty()) return priority;
    }
    return PriorityIndex(TaskPriority::kNormal);
  }

  void CompleteChunk(WorkChunk chunk, bool cancelled, bool failed) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &invocation = chunk.invocation;
    invocation->lane_busy[chunk.lane] = false;
    --invocation->active_chunks;
    if (cancelled) invocation->cancelled = true;
    if (failed) invocation->failed = true;
    if (invocation->cancelled || invocation->failed) {
      invocation->next_value = invocation->end_value;
      RemoveReadyLocked(invocation);
    } else {
      QueueLocked(invocation);
    }
    FinishIfReadyLocked(invocation);
    ready_condition_.notify_all();
  }

  void WorkerLoop() {
    while (true) {
      WorkChunk chunk;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_condition_.wait(lock,
                              [this] { return stopping_ || HasReadyLocked(); });
        if (stopping_ && !HasReadyLocked()) return;
        if (!TakeWorkLocked(&chunk)) continue;
      }

      bool cancelled = false;
      bool failed = false;
      try {
        for (std::uint32_t value = chunk.begin; value < chunk.end; ++value) {
          if (IsCancelled(chunk.invocation->context)) {
            cancelled = true;
            break;
          }
          chunk.invocation->function(chunk.invocation->jpegxl_opaque, value,
                                     chunk.lane);
        }
      } catch (...) {
        failed = true;
      }
      CompleteChunk(std::move(chunk), cancelled, failed);
    }
  }

  std::size_t worker_count_;
  const std::size_t chunk_size_;
  std::mutex mutex_;
  std::condition_variable ready_condition_;
  std::array<std::deque<std::shared_ptr<Invocation>>, 3> ready_;
  std::array<std::uint64_t, 3> last_group_{};
  std::size_t priority_cycle_index_ = 0;
  std::vector<std::thread> workers_;
  bool stopping_ = false;
};

SharedParallelScheduler::SharedParallelScheduler(std::size_t worker_count,
                                                 std::size_t chunk_size)
    : impl_(std::make_unique<Impl>(worker_count, chunk_size)) {}

SharedParallelScheduler::~SharedParallelScheduler() = default;

std::size_t SharedParallelScheduler::worker_count() const {
  return impl_->worker_count();
}

JxlParallelRetCode SharedParallelScheduler::Run(
    const SharedRunnerContext &context, void *jpegxl_opaque,
    JxlParallelRunInit init, JxlParallelRunFunction function,
    std::uint32_t start_range, std::uint32_t end_range) {
  return impl_->Run(context, jpegxl_opaque, init, function, start_range,
                    end_range);
}

namespace {

struct GlobalSchedulerState {
  std::mutex mutex;
  std::size_t configured_workers = AvailableProcessors();
  bool configured = false;
  std::unique_ptr<SharedParallelScheduler> scheduler;
};

GlobalSchedulerState &SchedulerState() {
  static GlobalSchedulerState state;
  return state;
}

SharedParallelScheduler &GlobalScheduler() {
  auto &state = SchedulerState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.scheduler) {
    state.scheduler = std::make_unique<SharedParallelScheduler>(
        state.configured_workers, JXL_CODER_SHARED_RUNNER_CHUNK_SIZE);
    state.configured = true;
  }
  return *state.scheduler;
}

}  // namespace

JxlParallelRetCode SharedParallelRunner(void *runner_opaque,
                                        void *jpegxl_opaque,
                                        JxlParallelRunInit init,
                                        JxlParallelRunFunction function,
                                        std::uint32_t start_range,
                                        std::uint32_t end_range) {
  if (runner_opaque == nullptr) return JXL_PARALLEL_RET_RUNNER_ERROR;
  const auto &context = *static_cast<SharedRunnerContext *>(runner_opaque);
  try {
    return GlobalScheduler().Run(context, jpegxl_opaque, init, function,
                                 start_range, end_range);
  } catch (...) {
    try {
      if (init == nullptr || function == nullptr || IsCancelled(context)) {
        return JXL_PARALLEL_RET_RUNNER_ERROR;
      }
      const JxlParallelRetCode init_result = init(jpegxl_opaque, 1);
      if (init_result != JXL_PARALLEL_RET_SUCCESS) return init_result;
      for (std::uint32_t value = start_range; value < end_range; ++value) {
        if (IsCancelled(context)) return JXL_PARALLEL_RET_RUNNER_ERROR;
        function(jpegxl_opaque, value, 0);
      }
      return JXL_PARALLEL_RET_SUCCESS;
    } catch (...) {
      return JXL_PARALLEL_RET_RUNNER_ERROR;
    }
  }
}

#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
void SetWorkerStartupFailureCountdownForTesting(int countdown) {
  worker_startup_failure_countdown.store(countdown,
                                         std::memory_order_relaxed);
}
#endif

SchedulerConfigurationResult ConfigureSharedParallelScheduler(
    std::size_t requested_workers, std::size_t *effective_workers) {
  if (requested_workers > kMaximumWorkers) {
    return SchedulerConfigurationResult::kInvalidArguments;
  }
  const std::size_t resolved =
      requested_workers == 0 ? AvailableProcessors() : requested_workers;
  auto &state = SchedulerState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.configured && state.configured_workers != resolved) {
    if (effective_workers != nullptr) {
      *effective_workers = state.configured_workers;
    }
    return SchedulerConfigurationResult::kAlreadyStarted;
  }
  state.configured_workers = resolved;
  state.configured = true;
  if (effective_workers != nullptr) *effective_workers = resolved;
  return SchedulerConfigurationResult::kSuccess;
}

std::size_t SharedParallelRunnerWorkerCount() {
  try {
    return GlobalScheduler().worker_count();
  } catch (...) {
    return 1;
  }
}

}  // namespace jxl_coder
