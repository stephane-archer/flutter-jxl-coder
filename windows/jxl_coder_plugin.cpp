#if defined(JXL_CODER_WINDOWS_ADAPTER_TEST_ONLY)
#include <flutter/encodable_value.h>
#include <flutter/method_result.h>
#elif !defined(JXL_CODER_WINDOWS_SCHEDULER_TEST_ONLY)
#include "jxl_coder_plugin.h"

#include <windows.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../native/src/deadline_utils.h"
#include "../native/src/jxl_codec.h"

namespace jxl_coder {
namespace {

#if !defined(JXL_CODER_WINDOWS_SCHEDULER_TEST_ONLY)
using Value = flutter::EncodableValue;
using List = flutter::EncodableList;
using Result = flutter::MethodResult<Value>;
#endif
using Clock = std::chrono::steady_clock;
constexpr unsigned kAutomaticJobs = 4;
constexpr std::array<std::size_t, 7> kPriorityCycle = {2, 2, 2, 2, 1, 1, 0};

#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
std::atomic<int> admission_thread_failure_countdown{-1};
std::atomic<bool> admission_enqueue_failure{false};
std::atomic<bool> admission_gate_failure{false};
std::atomic<bool> admission_after_timer_failure{false};

bool FailAdmissionThreadStartupForTesting() {
  int countdown =
      admission_thread_failure_countdown.load(std::memory_order_relaxed);
  while (countdown >= 0) {
    const int next = countdown == 0 ? -1 : countdown - 1;
    if (admission_thread_failure_countdown.compare_exchange_weak(
            countdown, next, std::memory_order_relaxed)) {
      return countdown == 0;
    }
  }
  return false;
}

void SetAdmissionThreadStartupFailureCountdownForTesting(int countdown) {
  admission_thread_failure_countdown.store(countdown,
                                           std::memory_order_relaxed);
}

void SetAdmissionEnqueueFailureForTesting(bool fail) {
  admission_enqueue_failure.store(fail, std::memory_order_relaxed);
}

void SetAdmissionGateFailureForTesting(bool fail) {
  admission_gate_failure.store(fail, std::memory_order_relaxed);
}

void SetAdmissionAfterTimerFailureForTesting(bool fail) {
  admission_after_timer_failure.store(fail, std::memory_order_relaxed);
}
#endif

struct ScheduledJob {
  std::uint64_t id = 0;
  std::uint64_t group = 0;
  Clock::time_point deadline = Clock::time_point::max();
  std::function<void()> on_expired;
  std::function<void()> on_failure;
  std::function<void()> operation;
};

struct StartupGate {
  std::mutex mutex;
  std::condition_variable condition;
  bool released = false;
  bool proceed = false;
};

class PriorityClassQueue {
 public:
  void Enqueue(ScheduledJob job) {
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
    if (admission_enqueue_failure.exchange(false,
                                           std::memory_order_relaxed)) {
      throw std::bad_alloc();
    }
#endif
    auto iterator = jobs_.find(job.group);
    if (iterator == jobs_.end()) {
      groups_.push_back(job.group);
      iterator = jobs_.emplace(job.group, std::deque<ScheduledJob>()).first;
    }
    iterator->second.push_back(std::move(job));
  }

  bool empty() const { return groups_.empty(); }

  ScheduledJob Dequeue() {
    const std::uint64_t group = groups_.front();
    groups_.pop_front();
    auto iterator = jobs_.find(group);
    ScheduledJob job = std::move(iterator->second.front());
    iterator->second.pop_front();
    if (iterator->second.empty()) {
      jobs_.erase(iterator);
    } else {
      groups_.push_back(group);
    }
    return job;
  }

  std::vector<ScheduledJob> RemoveExpired(Clock::time_point now) {
    std::vector<ScheduledJob> expired;
    for (auto iterator = jobs_.begin(); iterator != jobs_.end();) {
      auto& queued = iterator->second;
      for (auto job = queued.begin(); job != queued.end();) {
        if (job->deadline <= now) {
          expired.push_back(std::move(*job));
          job = queued.erase(job);
        } else {
          ++job;
        }
      }
      if (queued.empty()) {
        const std::uint64_t group = iterator->first;
        groups_.erase(std::remove(groups_.begin(), groups_.end(), group),
                      groups_.end());
        iterator = jobs_.erase(iterator);
      } else {
        ++iterator;
      }
    }
    return expired;
  }

 private:
  std::deque<std::uint64_t> groups_;
  std::unordered_map<std::uint64_t, std::deque<ScheduledJob>> jobs_;
};

enum class AdmissionConfigurationResult {
  kSuccess,
  kInvalidArguments,
  kAlreadyStarted,
  kResourceFailure,
};

const char* AdmissionErrorCode(AdmissionConfigurationResult result) {
  if (result == AdmissionConfigurationResult::kInvalidArguments) {
    return "INVALID_ARGUMENTS";
  }
  if (result == AdmissionConfigurationResult::kAlreadyStarted) {
    return "SCHEDULER_ALREADY_STARTED";
  }
  return "SCHEDULER_ERROR";
}

class ConversionScheduler {
 public:
  static ConversionScheduler& Instance() {
    static ConversionScheduler scheduler;
    return scheduler;
  }

  AdmissionConfigurationResult Configure(std::size_t requested_workers,
                                         std::size_t requested_maximum) {
    if (requested_workers > 256 || requested_maximum > 256) {
      return AdmissionConfigurationResult::kInvalidArguments;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const bool initialized = configured_ || started_;
    if (initialized && requested_workers == requested_workers_ &&
        requested_maximum == requested_maximum_) {
      return AdmissionConfigurationResult::kSuccess;
    }
    const std::size_t available = std::min<std::size_t>(
        256, std::max(1u, std::thread::hardware_concurrency()));
    const std::size_t desired_workers =
        requested_workers == 0 ? available : requested_workers;
    const std::size_t desired_maximum =
        requested_maximum == 0
            ? std::min<std::size_t>(kAutomaticJobs, desired_workers)
            : requested_maximum;
    if (initialized) {
      return desired_workers == effective_workers_ &&
                     desired_maximum == effective_maximum_
                 ? AdmissionConfigurationResult::kSuccess
                 : AdmissionConfigurationResult::kAlreadyStarted;
    }

    std::size_t native_workers = 0;
    const auto native_result =
        ConfigureSharedParallelScheduler(requested_workers, &native_workers);
    if (native_result == SchedulerConfigurationResult::kAlreadyStarted) {
      return AdmissionConfigurationResult::kAlreadyStarted;
    }
    effective_workers_ = native_workers;
    effective_maximum_ =
        requested_maximum == 0
            ? std::min<std::size_t>(kAutomaticJobs, native_workers)
            : requested_maximum;
    requested_workers_ = requested_workers;
    requested_maximum_ = requested_maximum;
    configured_ = true;
    return AdmissionConfigurationResult::kSuccess;
  }

  std::uint64_t MakeGroup() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t group = next_group_++;
    if (next_group_ == 0) next_group_ = 1;
    return group;
  }

#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
  void SetNextGroupForTesting(std::uint64_t next_group) {
    std::lock_guard<std::mutex> lock(mutex_);
    next_group_ = next_group;
  }
#endif

  bool Submit(TaskPriority priority, std::uint64_t group,
              Clock::time_point deadline, std::function<void()> on_expired,
              std::function<void()> on_failure,
              std::function<void()> operation,
              AdmissionConfigurationResult* submission_result = nullptr) {
    try {
      if (!IsValidTaskPriority(priority)) {
        if (submission_result != nullptr) {
          *submission_result =
              AdmissionConfigurationResult::kInvalidArguments;
        }
        return false;
      }
      std::lock_guard<std::mutex> lock(mutex_);
      const AdmissionConfigurationResult startup = StartLocked();
      if (startup != AdmissionConfigurationResult::kSuccess) {
        if (submission_result != nullptr) *submission_result = startup;
        return false;
      }
      queues_[static_cast<std::size_t>(priority)].Enqueue(
          {next_job_++, group, deadline, std::move(on_expired),
           std::move(on_failure), std::move(operation)});
      if (submission_result != nullptr) {
        *submission_result = AdmissionConfigurationResult::kSuccess;
      }
      condition_.notify_all();
      return true;
    } catch (...) {
      if (submission_result != nullptr) {
        *submission_result = AdmissionConfigurationResult::kResourceFailure;
      }
      return false;
    }
  }

 private:
  ConversionScheduler() = default;

  ~ConversionScheduler() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_all();
    for (auto& worker : workers_) worker.join();
    if (timer_.joinable()) timer_.join();
  }

  AdmissionConfigurationResult StartLocked() {
    if (started_) return AdmissionConfigurationResult::kSuccess;
    if (effective_workers_ == 0) {
      std::size_t native_workers = 0;
      const SchedulerConfigurationResult configured =
          ConfigureSharedParallelScheduler(0, &native_workers);
      if (configured == SchedulerConfigurationResult::kAlreadyStarted) {
        return AdmissionConfigurationResult::kAlreadyStarted;
      }
      effective_workers_ = native_workers;
      effective_maximum_ =
          std::min<std::size_t>(kAutomaticJobs, native_workers);
      requested_workers_ = 0;
      requested_maximum_ = 0;
      configured_ = true;
    }

    std::shared_ptr<StartupGate> gate;
    std::vector<std::thread> workers;
    std::thread timer;
    try {
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
      if (admission_gate_failure.exchange(false,
                                          std::memory_order_relaxed)) {
        throw std::bad_alloc();
      }
#endif
      gate = std::make_shared<StartupGate>();
      workers.reserve(effective_maximum_);
      for (std::size_t index = 0; index < effective_maximum_; ++index) {
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
        if (FailAdmissionThreadStartupForTesting()) {
          throw std::runtime_error("injected admission worker failure");
        }
#endif
        workers.emplace_back([this, gate] {
          {
            std::unique_lock<std::mutex> lock(gate->mutex);
            gate->condition.wait(lock, [&] { return gate->released; });
            if (!gate->proceed) return;
          }
          Run();
        });
      }
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
      if (FailAdmissionThreadStartupForTesting()) {
        throw std::runtime_error("injected admission timer failure");
      }
#endif
      timer = std::thread([this, gate] {
        {
          std::unique_lock<std::mutex> lock(gate->mutex);
          gate->condition.wait(lock, [&] { return gate->released; });
          if (!gate->proceed) return;
        }
        ExpireQueued();
      });
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
      if (admission_after_timer_failure.exchange(
              false, std::memory_order_relaxed)) {
        throw std::runtime_error("injected post-timer startup failure");
      }
#endif
    } catch (...) {
      if (gate) {
        {
          std::lock_guard<std::mutex> lock(gate->mutex);
          gate->released = true;
        }
        gate->condition.notify_all();
      }
      for (auto& worker : workers) worker.join();
      if (timer.joinable()) timer.join();
      return AdmissionConfigurationResult::kResourceFailure;
    }

    workers_ = std::move(workers);
    timer_ = std::move(timer);
    started_ = true;
    {
      std::lock_guard<std::mutex> lock(gate->mutex);
      gate->proceed = true;
      gate->released = true;
    }
    gate->condition.notify_all();
    return AdmissionConfigurationResult::kSuccess;
  }

  bool HasJobsLocked() const {
    return std::any_of(queues_.begin(), queues_.end(),
                       [](const auto& queue) { return !queue.empty(); });
  }

  ScheduledJob DequeueLocked() {
    for (std::size_t attempt = 0; attempt < kPriorityCycle.size(); ++attempt) {
      const std::size_t priority = kPriorityCycle[priority_index_];
      priority_index_ = (priority_index_ + 1) % kPriorityCycle.size();
      if (!queues_[priority].empty()) return queues_[priority].Dequeue();
    }
    for (auto& queue : queues_) {
      if (!queue.empty()) return queue.Dequeue();
    }
    return {};
  }

  void Run() {
    while (true) {
      ScheduledJob job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock,
                        [this] { return stopping_ || HasJobsLocked(); });
        if (stopping_ && !HasJobsLocked()) return;
        job = DequeueLocked();
      }
      try {
        job.operation();
      } catch (...) {
        try {
          if (job.on_failure) job.on_failure();
        } catch (...) {
        }
      }
    }
  }

  void ExpireQueued() {
    while (true) {
      std::vector<ScheduledJob> expired;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait_for(lock, std::chrono::milliseconds(1));
        if (stopping_) return;
        const auto now = Clock::now();
        for (auto& queue : queues_) {
          auto queue_expired = queue.RemoveExpired(now);
          expired.insert(expired.end(),
                         std::make_move_iterator(queue_expired.begin()),
                         std::make_move_iterator(queue_expired.end()));
        }
      }
      for (auto& job : expired) {
        try {
          if (job.on_expired) job.on_expired();
        } catch (...) {
        }
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable condition_;
  std::array<PriorityClassQueue, 3> queues_;
  std::size_t priority_index_ = 0;
  std::size_t effective_workers_ = 0;
  std::size_t effective_maximum_ = 0;
  std::size_t requested_workers_ = 0;
  std::size_t requested_maximum_ = 0;
  std::uint64_t next_group_ = 1;
  std::uint64_t next_job_ = 1;
  std::vector<std::thread> workers_;
  std::thread timer_;
  bool started_ = false;
  bool configured_ = false;
  bool stopping_ = false;
};

#if !defined(JXL_CODER_WINDOWS_SCHEDULER_TEST_ONLY)
bool GetInteger(const List& values, std::size_t index, std::int64_t* output) {
  if (index >= values.size()) return false;
  if (const auto* value = std::get_if<std::int32_t>(&values[index])) {
    *output = *value;
    return true;
  }
  if (const auto* value = std::get_if<std::int64_t>(&values[index])) {
    *output = *value;
    return true;
  }
  return false;
}

bool GetString(const List& values, std::size_t index, std::string* output) {
  if (index >= values.size()) return false;
  const auto* value = std::get_if<std::string>(&values[index]);
  if (value == nullptr) return false;
  *output = *value;
  return true;
}

void SafeSuccess(Result* result) noexcept {
  if (result == nullptr) return;
  try {
    result->Success();
  } catch (...) {
  }
}

void SafeSuccess(Result* result, Value value) noexcept {
  if (result == nullptr) return;
  try {
    result->Success(value);
  } catch (...) {
  }
}

void SafeError(Result* result, const std::string& code,
               const std::string& message) noexcept {
  if (result == nullptr) return;
  try {
    result->Error(code, message);
  } catch (...) {
  }
}

void SafeNotImplemented(Result* result) noexcept {
  if (result == nullptr) return;
  try {
    result->NotImplemented();
  } catch (...) {
  }
}

#if !defined(JXL_CODER_WINDOWS_ADAPTER_TEST_ONLY)
std::wstring Utf8ToLongPath(const std::string& value) {
  if (value.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0);
  if (length <= 0) return {};
  std::wstring path(static_cast<std::size_t>(length), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), path.data(), length) ==
      0) {
    return {};
  }
  if (path.rfind(L"\\\\?\\", 0) == 0) return path;

  const DWORD absolute_length = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (absolute_length == 0) return {};
  std::wstring absolute(static_cast<std::size_t>(absolute_length), L'\0');
  const DWORD written = GetFullPathNameW(path.c_str(), absolute_length,
                                         absolute.data(), nullptr);
  if (written == 0 || written >= absolute_length) return {};
  absolute.resize(written);
  if (absolute.rfind(L"\\\\", 0) == 0) {
    return L"\\\\?\\UNC\\" + absolute.substr(2);
  }
  return L"\\\\?\\" + absolute;
}

class MappedFile {
 public:
  explicit MappedFile(const std::string& utf8_path) {
    const std::wstring path = Utf8ToLongPath(utf8_path);
    if (path.empty()) return;
    file_ = CreateFileW(path.c_str(), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER length = {};
    if (!GetFileSizeEx(file_, &length) || length.QuadPart < 0 ||
        static_cast<unsigned long long>(length.QuadPart) > SIZE_MAX) {
      return;
    }
    size_ = static_cast<std::size_t>(length.QuadPart);
    if (size_ == 0) {
      valid_ = true;
      return;
    }
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping_ == nullptr) return;
    data_ = static_cast<const std::uint8_t*>(
        MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
    valid_ = data_ != nullptr;
  }

  ~MappedFile() {
    if (data_ != nullptr) UnmapViewOfFile(data_);
    if (mapping_ != nullptr) CloseHandle(mapping_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
  }

  bool valid() const { return valid_; }
  const std::uint8_t* data() const { return data_; }
  std::size_t size() const { return size_; }

 private:
  HANDLE file_ = INVALID_HANDLE_VALUE;
  HANDLE mapping_ = nullptr;
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  bool valid_ = false;
};

std::atomic<unsigned long long> g_temporary_file_id{0};

bool WriteFileBytes(const std::string& utf8_path, const std::uint8_t* bytes,
                    std::size_t size) {
  const std::wstring path = Utf8ToLongPath(utf8_path);
  if (path.empty()) return false;
  std::wstring temporary_path;
  HANDLE file = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 16 && file == INVALID_HANDLE_VALUE;
       ++attempt) {
    temporary_path =
        path + L".jxl_coder_tmp_" + std::to_wstring(GetCurrentProcessId()) +
        L"_" + std::to_wstring(g_temporary_file_id.fetch_add(1));
    file = CreateFileW(temporary_path.c_str(), GENERIC_WRITE, 0, nullptr,
                       CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) {
      return false;
    }
  }
  if (file == INVALID_HANDLE_VALUE) return false;
  std::size_t offset = 0;
  bool success = true;
  while (offset < size) {
    const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
        size - offset, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(file, bytes + offset, requested, &written, nullptr) ||
        written == 0) {
      success = false;
      break;
    }
    offset += written;
  }
  if (success) success = FlushFileBuffers(file) != FALSE;
  CloseHandle(file);
  if (success) {
    success = MoveFileExW(temporary_path.c_str(), path.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
              FALSE;
  }
  if (!success) DeleteFileW(temporary_path.c_str());
  return success;
}
#endif

struct OperationResult {
  bool success = false;
  std::string code;
  std::string message;
  OutputBuffer bytes;
};

std::string ErrorCodeName(ErrorCode code, const std::string& fallback) {
  switch (code) {
    case ErrorCode::kInvalidArguments:
      return "INVALID_ARGUMENTS";
    case ErrorCode::kUnsupportedInput:
      return "UNSUPPORTED_INPUT";
    case ErrorCode::kIo:
      return "IO_ERROR";
    case ErrorCode::kTimeout:
      return "TIMEOUT";
    case ErrorCode::kCodec:
      return fallback;
  }
}

OperationResult Failure(const Error& error, const std::string& fallback) {
  return {false, ErrorCodeName(error.code, fallback), error.message,
          OutputBuffer()};
}

OperationResult Invalid(std::string message) {
  return {false, "INVALID_ARGUMENTS", std::move(message), OutputBuffer()};
}

OperationResult IoFailure(std::string message) {
  return {false, "IO_ERROR", std::move(message), OutputBuffer()};
}

OperationResult TimeoutFailure(std::string message) {
  return {false, "TIMEOUT", std::move(message), OutputBuffer()};
}

std::int64_t RemainingTimeout(std::int64_t timeout,
                              Clock::time_point started) {
  if (timeout == 0) return 0;
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           Clock::now() - started)
                           .count();
  return std::max<std::int64_t>(0, timeout - elapsed);
}

#if defined(JXL_CODER_WINDOWS_ADAPTER_TEST_ONLY)
using AdapterPathOperation = std::function<OperationResult(
    bool, const std::string&, const std::string&)>;
AdapterPathOperation adapter_path_operation_for_testing;

OperationResult EncodePath(const std::string& input_path,
                           const std::string& output_path, int, int,
                           TaskPriority, std::uint64_t, std::int64_t,
                           Clock::time_point) {
  if (adapter_path_operation_for_testing) {
    return adapter_path_operation_for_testing(true, input_path, output_path);
  }
  return {true, {}, {}, OutputBuffer()};
}

OperationResult InversePath(const std::string& input_path,
                            const std::string& output_path, TaskPriority,
                            std::uint64_t, std::int64_t,
                            Clock::time_point) {
  if (adapter_path_operation_for_testing) {
    return adapter_path_operation_for_testing(false, input_path, output_path);
  }
  return {true, {}, {}, OutputBuffer()};
}
#else
OperationResult EncodePath(const std::string& input_path,
                           const std::string& output_path, int effort,
                           int decoding_speed, TaskPriority priority,
                           std::uint64_t scheduling_group,
                           std::int64_t timeout, Clock::time_point started) {
  OutputBuffer output;
  Error error;
  {
    if (timeout > 0 && RemainingTimeout(timeout, started) == 0) {
      return TimeoutFailure("JPEG XL encoding timed out");
    }
    MappedFile input(input_path);
    if (!input.valid()) return IoFailure("Unable to open the JPEG input file");
    const std::int64_t remaining = RemainingTimeout(timeout, started);
    if (timeout > 0 && remaining == 0) {
      return TimeoutFailure("JPEG XL encoding timed out");
    }
    Options options{effort, decoding_speed, priority, scheduling_group,
                    remaining};
    if (!EncodeJpeg(input.data(), input.size(), options, &output, &error)) {
      return Failure(error, "TRANSCODE_ERROR");
    }
  }
  if (timeout > 0 && RemainingTimeout(timeout, started) == 0) {
    return TimeoutFailure("JPEG XL encoding timed out");
  }
  if (!WriteFileBytes(output_path, output.data(), output.size())) {
    return IoFailure("Unable to write the JPEG XL output file");
  }
  return {true, {}, {}, OutputBuffer()};
}

OperationResult InversePath(const std::string& input_path,
                            const std::string& output_path,
                            TaskPriority priority,
                            std::uint64_t scheduling_group,
                            std::int64_t timeout,
                            Clock::time_point started) {
  OutputBuffer output;
  Error error;
  {
    if (timeout > 0 && RemainingTimeout(timeout, started) == 0) {
      return TimeoutFailure("JPEG reconstruction timed out");
    }
    MappedFile input(input_path);
    if (!input.valid()) {
      return IoFailure("Unable to open the JPEG XL input file");
    }
    const std::int64_t remaining = RemainingTimeout(timeout, started);
    if (timeout > 0 && remaining == 0) {
      return TimeoutFailure("JPEG reconstruction timed out");
    }
    Options options{7, 0, priority, scheduling_group, remaining};
    if (!ReconstructJpeg(input.data(), input.size(), options, &output, &error)) {
      return Failure(error, "INVERSE_ERROR");
    }
  }
  if (timeout > 0 && RemainingTimeout(timeout, started) == 0) {
    return TimeoutFailure("JPEG reconstruction timed out");
  }
  if (!WriteFileBytes(output_path, output.data(), output.size())) {
    return IoFailure("Unable to write the JPEG output file");
  }
  return {true, {}, {}, OutputBuffer()};
}
#endif

bool ValidPerformance(std::int64_t priority, std::int64_t timeout) {
  return priority >= 0 && priority <= 2 && timeout >= 0;
}

OperationResult Execute(const std::string& method, const Value& arguments,
                        Clock::time_point started,
                        std::uint64_t scheduling_group) {
  const auto* values = std::get_if<List>(&arguments);
  if (values == nullptr) return Invalid("A positional argument list is required");

  if (method == "jpegBytesToJxl") {
    std::int64_t effort, decoding_speed, priority, timeout;
    if (values->size() != 5 ||
        !GetInteger(*values, 1, &effort) ||
        !GetInteger(*values, 2, &decoding_speed) ||
        !GetInteger(*values, 3, &priority) ||
        !GetInteger(*values, 4, &timeout) || effort < 1 || effort > 9 ||
        decoding_speed < 0 || decoding_speed > 4 ||
        !ValidPerformance(priority, timeout)) {
      return Invalid("JPEG data and valid options are required");
    }
    const auto* input =
        std::get_if<std::vector<std::uint8_t>>(&(*values)[0]);
    if (input == nullptr) return Invalid("JPEG data is required");
    const std::int64_t remaining = RemainingTimeout(timeout, started);
    if (timeout > 0 && remaining == 0) {
      return TimeoutFailure("JPEG XL encoding timed out");
    }
    Options options{static_cast<int>(effort), static_cast<int>(decoding_speed),
                    static_cast<TaskPriority>(priority), scheduling_group,
                    remaining};
    OperationResult result;
    Error error;
    if (!EncodeJpeg(input->data(), input->size(), options, &result.bytes,
                    &error)) {
      return Failure(error, "TRANSCODE_ERROR");
    }
    result.success = true;
    return result;
  }

  if (method == "jxlBytesToJpeg") {
    std::int64_t priority, timeout;
    if (values->size() != 3 || !GetInteger(*values, 1, &priority) ||
        !GetInteger(*values, 2, &timeout) ||
        !ValidPerformance(priority, timeout)) {
      return Invalid("JXL data and valid options are required");
    }
    const auto* input =
        std::get_if<std::vector<std::uint8_t>>(&(*values)[0]);
    if (input == nullptr) return Invalid("JXL data is required");
    const std::int64_t remaining = RemainingTimeout(timeout, started);
    if (timeout > 0 && remaining == 0) {
      return TimeoutFailure("JPEG reconstruction timed out");
    }
    Options options{7, 0, static_cast<TaskPriority>(priority),
                    scheduling_group, remaining};
    OperationResult result;
    Error error;
    if (!ReconstructJpeg(input->data(), input->size(), options, &result.bytes,
                         &error)) {
      return Failure(error, "INVERSE_ERROR");
    }
    result.success = true;
    return result;
  }

  if (method == "jpegPathToJxl") {
    std::string input, output;
    std::int64_t effort, decoding_speed, priority, timeout;
    if (values->size() != 6 || !GetString(*values, 0, &input) ||
        !GetString(*values, 1, &output) || !GetInteger(*values, 2, &effort) ||
        !GetInteger(*values, 3, &decoding_speed) ||
        !GetInteger(*values, 4, &priority) ||
        !GetInteger(*values, 5, &timeout) || effort < 1 || effort > 9 ||
        decoding_speed < 0 || decoding_speed > 4 ||
        !ValidPerformance(priority, timeout)) {
      return Invalid("Input path, output path, and valid options are required");
    }
    return EncodePath(input, output, static_cast<int>(effort),
                      static_cast<int>(decoding_speed),
                      static_cast<TaskPriority>(priority), scheduling_group,
                      timeout, started);
  }

  if (method == "jxlPathToJpeg") {
    std::string input, output;
    std::int64_t priority, timeout;
    if (values->size() != 4 || !GetString(*values, 0, &input) ||
        !GetString(*values, 1, &output) || !GetInteger(*values, 2, &priority) ||
        !GetInteger(*values, 3, &timeout) ||
        !ValidPerformance(priority, timeout)) {
      return Invalid("Input path, output path, and valid options are required");
    }
    return InversePath(input, output, static_cast<TaskPriority>(priority),
                       scheduling_group, timeout, started);
  }

  return {false, "NOT_IMPLEMENTED", {}, OutputBuffer()};
}

void CompleteSingle(const std::string& method,
                    const std::shared_ptr<Result>& result,
                    OperationResult operation) {
  if (!operation.success) {
    SafeError(result.get(), operation.code, operation.message);
    return;
  }
  if (method != "jpegBytesToJxl" && method != "jxlBytesToJpeg") {
    SafeSuccess(result.get());
    return;
  }
  try {
    std::vector<std::uint8_t> response;
    if (operation.bytes.size() > 0) {
      response.assign(operation.bytes.data(),
                      operation.bytes.data() + operation.bytes.size());
    }
    SafeSuccess(result.get(), Value(std::move(response)));
  } catch (const std::bad_alloc&) {
    SafeError(result.get(),
              method == "jpegBytesToJxl" ? "TRANSCODE_ERROR"
                                          : "INVERSE_ERROR",
              "Not enough memory to copy the codec output");
  }
}

class BatchCompletion {
 public:
  BatchCompletion(std::size_t count, std::shared_ptr<Result> result)
      : remaining_(count),
        results_(count),
        finished_(count, false),
        result_(std::move(result)) {}

  void Finish(std::size_t index, OperationResult operation) {
    std::string code;
    std::string message;
    bool complete = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (replied_ || index >= results_.size() || finished_[index]) return;
      finished_[index] = true;
      results_[index] = std::move(operation);
      --remaining_;
      if (remaining_ == 0) {
        replied_ = true;
        complete = true;
        for (const auto& item : results_) {
          if (!item.success) {
            code = item.code;
            message = item.message;
            break;
          }
        }
      }
    }
    if (!complete) return;
    if (!code.empty()) {
      SafeError(result_.get(), code, message);
    } else {
      SafeSuccess(result_.get());
    }
  }

 private:
  std::mutex mutex_;
  std::size_t remaining_;
  std::vector<OperationResult> results_;
  std::vector<bool> finished_;
  std::shared_ptr<Result> result_;
  bool replied_ = false;
};

bool ExtractSingleScheduling(const std::string& method,
                             const Value& arguments, TaskPriority* priority,
                             std::int64_t* timeout) {
  const auto* values = std::get_if<List>(&arguments);
  if (values == nullptr) return false;

  std::int64_t effort = 7;
  std::int64_t decoding_speed = 0;
  std::int64_t priority_value = 0;
  if (method == "jpegBytesToJxl") {
    if (values->size() != 5 ||
        std::get_if<std::vector<std::uint8_t>>(&(*values)[0]) == nullptr ||
        !GetInteger(*values, 1, &effort) ||
        !GetInteger(*values, 2, &decoding_speed) ||
        !GetInteger(*values, 3, &priority_value) ||
        !GetInteger(*values, 4, timeout) || effort < 1 || effort > 9 ||
        decoding_speed < 0 || decoding_speed > 4 ||
        !ValidPerformance(priority_value, *timeout)) {
      return false;
    }
  } else if (method == "jxlBytesToJpeg") {
    if (values->size() != 3 ||
        std::get_if<std::vector<std::uint8_t>>(&(*values)[0]) == nullptr ||
        !GetInteger(*values, 1, &priority_value) ||
        !GetInteger(*values, 2, timeout) ||
        !ValidPerformance(priority_value, *timeout)) {
      return false;
    }
  } else if (method == "jpegPathToJxl") {
    std::string input;
    std::string output;
    if (values->size() != 6 || !GetString(*values, 0, &input) ||
        !GetString(*values, 1, &output) ||
        !GetInteger(*values, 2, &effort) ||
        !GetInteger(*values, 3, &decoding_speed) ||
        !GetInteger(*values, 4, &priority_value) ||
        !GetInteger(*values, 5, timeout) || effort < 1 || effort > 9 ||
        decoding_speed < 0 || decoding_speed > 4 ||
        !ValidPerformance(priority_value, *timeout)) {
      return false;
    }
  } else if (method == "jxlPathToJpeg") {
    std::string input;
    std::string output;
    if (values->size() != 4 || !GetString(*values, 0, &input) ||
        !GetString(*values, 1, &output) ||
        !GetInteger(*values, 2, &priority_value) ||
        !GetInteger(*values, 3, timeout) ||
        !ValidPerformance(priority_value, *timeout)) {
      return false;
    }
  } else {
    return false;
  }

  *priority = static_cast<TaskPriority>(priority_value);
  return true;
}

void ScheduleBatch(const std::string& method, const Value& arguments,
                   const std::shared_ptr<Result>& result) {
  const auto* values = std::get_if<List>(&arguments);
  const bool encode = method == "jpegPathsToJxl";
  const std::size_t prefix = encode ? 4 : 2;
  if (values == nullptr || values->size() < prefix ||
      (values->size() - prefix) % 2 != 0) {
    SafeError(result.get(), "INVALID_ARGUMENTS",
              "Batch options and path pairs are required");
    return;
  }

  std::int64_t effort = 7;
  std::int64_t decoding_speed = 0;
  std::int64_t priority_value = 0;
  std::int64_t timeout = 0;
  const bool valid_options =
      encode
          ? GetInteger(*values, 0, &effort) &&
                GetInteger(*values, 1, &decoding_speed) &&
                GetInteger(*values, 2, &priority_value) &&
                GetInteger(*values, 3, &timeout) && effort >= 1 &&
                effort <= 9 && decoding_speed >= 0 && decoding_speed <= 4
          : GetInteger(*values, 0, &priority_value) &&
                GetInteger(*values, 1, &timeout);
  if (!valid_options || !ValidPerformance(priority_value, timeout)) {
    SafeError(result.get(), "INVALID_ARGUMENTS",
              "Batch options and path pairs are required");
    return;
  }

  std::vector<std::pair<std::string, std::string>> paths;
  paths.reserve((values->size() - prefix) / 2);
  for (std::size_t index = prefix; index < values->size(); index += 2) {
    std::string input;
    std::string output;
    if (!GetString(*values, index, &input) ||
        !GetString(*values, index + 1, &output)) {
      SafeError(result.get(), "INVALID_ARGUMENTS",
                "Batch path pairs must be strings");
      return;
    }
    paths.emplace_back(std::move(input), std::move(output));
  }
  if (paths.empty()) {
    SafeSuccess(result.get());
    return;
  }

  const TaskPriority priority = static_cast<TaskPriority>(priority_value);
  const std::uint64_t group = ConversionScheduler::Instance().MakeGroup();
  auto completion =
      std::make_shared<BatchCompletion>(paths.size(), result);
  for (std::size_t index = 0; index < paths.size(); ++index) {
    try {
      const auto input = paths[index].first;
      const auto output = paths[index].second;
      AdmissionConfigurationResult submission =
          AdmissionConfigurationResult::kResourceFailure;
      const bool submitted = ConversionScheduler::Instance().Submit(
          priority, group, Clock::time_point::max(), {},
          [completion, index, encode] {
            completion->Finish(
                index,
                {false, encode ? "TRANSCODE_ERROR" : "INVERSE_ERROR",
                 "Unexpected native JPEG XL conversion failure",
                 OutputBuffer()});
          },
          [encode, input, output, effort, decoding_speed, priority, group,
           timeout, completion, index] {
            const Clock::time_point started = Clock::now();
            OperationResult operation =
                encode
                    ? EncodePath(input, output, static_cast<int>(effort),
                                 static_cast<int>(decoding_speed), priority,
                                 group, timeout, started)
                    : InversePath(input, output, priority, group, timeout,
                                  started);
            completion->Finish(index, std::move(operation));
          },
          &submission);
      if (submitted) continue;
      completion->Finish(
          index,
          {false, AdmissionErrorCode(submission),
           "Unable to start the JPEG XL conversion scheduler",
           OutputBuffer()});
    } catch (...) {
      completion->Finish(
          index,
          {false, "SCHEDULER_ERROR",
           "Unexpected native JPEG XL scheduling failure", OutputBuffer()});
    }
  }
}

void HandleValueMethodCall(const std::string& method, Value arguments,
                           std::unique_ptr<Result> result) {
  if (method == "configureJxlScheduler") {
    try {
      const auto* values = std::get_if<List>(&arguments);
      std::int64_t workers = 0;
      std::int64_t maximum = 0;
      if (values == nullptr || values->size() != 2 ||
          !GetInteger(*values, 0, &workers) ||
          !GetInteger(*values, 1, &maximum) || workers < 0 || workers > 256 ||
          maximum < 0 || maximum > 256) {
        SafeError(result.get(), "INVALID_ARGUMENTS",
                  "Valid scheduler limits are required");
        return;
      }
      const auto configured = ConversionScheduler::Instance().Configure(
          static_cast<std::size_t>(workers),
          static_cast<std::size_t>(maximum));
      if (configured == AdmissionConfigurationResult::kSuccess) {
        SafeSuccess(result.get());
      } else if (configured ==
                 AdmissionConfigurationResult::kAlreadyStarted) {
        SafeError(result.get(), "SCHEDULER_ALREADY_STARTED",
                  "The JPEG XL scheduler has already started");
      } else if (configured ==
                 AdmissionConfigurationResult::kInvalidArguments) {
        SafeError(result.get(), "INVALID_ARGUMENTS",
                  "Valid scheduler limits are required");
      } else {
        SafeError(result.get(), "SCHEDULER_ERROR",
                  "Unable to configure the JPEG XL scheduler");
      }
    } catch (...) {
      SafeError(result.get(), "SCHEDULER_ERROR",
                "Unexpected native JPEG XL scheduler failure");
    }
    return;
  }
  if (method != "jpegBytesToJxl" && method != "jxlBytesToJpeg" &&
      method != "jpegPathToJxl" && method != "jxlPathToJpeg" &&
      method != "jpegPathsToJxl" && method != "jxlPathsToJpeg") {
    SafeNotImplemented(result.get());
    return;
  }

  std::shared_ptr<Result> shared_result;
  try {
    shared_result = std::shared_ptr<Result>(std::move(result));
    if (method == "jpegPathsToJxl" || method == "jxlPathsToJpeg") {
      ScheduleBatch(method, arguments, shared_result);
      return;
    }

    TaskPriority priority = TaskPriority::kNormal;
    std::int64_t timeout = 0;
    if (!ExtractSingleScheduling(method, arguments, &priority, &timeout)) {
      SafeError(shared_result.get(), "INVALID_ARGUMENTS",
                "Complete and valid conversion arguments are required");
      return;
    }
    const Clock::time_point started = Clock::now();
    const Clock::time_point deadline =
        SaturatingDeadline<Clock>(started, timeout);
    const std::uint64_t group = ConversionScheduler::Instance().MakeGroup();
    const bool encode = method == "jpegBytesToJxl" ||
                        method == "jpegPathToJxl";
    AdmissionConfigurationResult submission =
        AdmissionConfigurationResult::kResourceFailure;
    if (!ConversionScheduler::Instance().Submit(
            priority, group, deadline,
            [shared_result] {
              SafeError(shared_result.get(), "TIMEOUT",
                        "Timed out waiting for codec capacity");
            },
            [shared_result, encode] {
              SafeError(shared_result.get(),
                        encode ? "TRANSCODE_ERROR" : "INVERSE_ERROR",
                        "Unexpected native JPEG XL conversion failure");
            },
            [method, arguments = std::move(arguments), shared_result, started,
             group]() mutable {
              CompleteSingle(method, shared_result,
                             Execute(method, arguments, started, group));
            },
            &submission)) {
      SafeError(shared_result.get(), AdmissionErrorCode(submission),
                "Unable to start the JPEG XL conversion scheduler");
    }
  } catch (...) {
    SafeError(shared_result ? shared_result.get() : result.get(),
              "SCHEDULER_ERROR",
              "Unexpected native JPEG XL scheduling failure");
  }
}

}  // namespace

#if !defined(JXL_CODER_WINDOWS_ADAPTER_TEST_ONLY)
void JxlCoderPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  auto channel = std::make_unique<flutter::MethodChannel<Value>>(
      registrar->messenger(), "jxl_coder",
      &flutter::StandardMethodCodec::GetInstance());
  auto plugin = std::make_unique<JxlCoderPlugin>();
  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get()](const auto& call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });
  registrar->AddPlugin(std::move(plugin));
}

void JxlCoderPlugin::HandleMethodCall(
    const flutter::MethodCall<Value>& method_call,
    std::unique_ptr<Result> result) {
  HandleValueMethodCall(method_call.method_name(),
                        method_call.arguments() == nullptr
                            ? Value()
                            : *method_call.arguments(),
                        std::move(result));
}
#endif

#else
}  // namespace
#endif
}  // namespace jxl_coder
