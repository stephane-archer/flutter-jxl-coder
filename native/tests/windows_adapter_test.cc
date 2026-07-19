#define JXL_CODER_WINDOWS_ADAPTER_TEST_ONLY 1
#include "../../windows/jxl_coder_plugin.cpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using flutter::EncodableList;
using flutter::EncodableValue;
using jxl_coder::AdmissionConfigurationResult;
using jxl_coder::AdmissionErrorCode;
using jxl_coder::BatchCompletion;
using jxl_coder::Clock;
using jxl_coder::CompleteSingle;
using jxl_coder::ConversionScheduler;
using jxl_coder::Error;
using jxl_coder::ErrorCode;
using jxl_coder::ErrorCodeName;
using jxl_coder::Execute;
using jxl_coder::ExtractSingleScheduling;
using jxl_coder::Failure;
using jxl_coder::GetInteger;
using jxl_coder::GetString;
using jxl_coder::HandleValueMethodCall;
using jxl_coder::Invalid;
using jxl_coder::IoFailure;
using jxl_coder::OperationResult;
using jxl_coder::RemainingTimeout;
using jxl_coder::SafeError;
using jxl_coder::SafeNotImplemented;
using jxl_coder::SafeSuccess;
using jxl_coder::ScheduleBatch;
using jxl_coder::TaskPriority;
using jxl_coder::TimeoutFailure;
using jxl_coder::ValidPerformance;
using jxl_coder::adapter_path_operation_for_testing;

std::atomic<int> failures{0};

void Expect(bool condition, const std::string& message) {
  if (condition) return;
  ++failures;
  std::cerr << "FAIL: " << message << '\n';
}

std::vector<std::uint8_t> ReadFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

std::vector<std::uint8_t> CopyBytes(const jxl_coder::OutputBuffer& output) {
  if (output.size() == 0) return {};
  return {output.data(), output.data() + output.size()};
}

EncodableValue Arguments(EncodableList values) {
  return EncodableValue(std::move(values));
}

EncodableValue I32(std::int32_t value) { return EncodableValue(value); }
EncodableValue I64(std::int64_t value) { return EncodableValue(value); }
EncodableValue Text(std::string value) {
  return EncodableValue(std::move(value));
}
EncodableValue Bytes(std::vector<std::uint8_t> value) {
  return EncodableValue(std::move(value));
}

class RecordingResult final
    : public flutter::MethodResult<EncodableValue> {
 public:
  enum class Kind { kNone, kSuccess, kError, kNotImplemented };

  bool Wait(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [&] { return reply_count_ != 0; });
  }

  Kind kind() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kind_;
  }

  std::size_t reply_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reply_count_;
  }

  std::optional<EncodableValue> value() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return value_;
  }

  std::string code() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return code_;
  }

  std::string message() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return message_;
  }

  void ThrowOnNextSuccess() {
    std::lock_guard<std::mutex> lock(mutex_);
    throw_on_success_ = true;
  }

  void ThrowOnNextError() {
    std::lock_guard<std::mutex> lock(mutex_);
    throw_on_error_ = true;
  }

  void ThrowOnNextNotImplemented() {
    std::lock_guard<std::mutex> lock(mutex_);
    throw_on_not_implemented_ = true;
  }

 protected:
  void SuccessInternal(const EncodableValue* result) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (throw_on_success_) {
      throw_on_success_ = false;
      throw std::bad_alloc();
    }
    ++reply_count_;
    kind_ = Kind::kSuccess;
    value_ = result == nullptr ? std::nullopt
                               : std::optional<EncodableValue>(*result);
    condition_.notify_all();
  }

  void ErrorInternal(const std::string& error_code,
                     const std::string& error_message,
                     const EncodableValue*) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (throw_on_error_) {
      throw_on_error_ = false;
      throw std::runtime_error("injected result error failure");
    }
    ++reply_count_;
    kind_ = Kind::kError;
    code_ = error_code;
    message_ = error_message;
    condition_.notify_all();
  }

  void NotImplementedInternal() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (throw_on_not_implemented_) {
      throw_on_not_implemented_ = false;
      throw std::runtime_error("injected not-implemented failure");
    }
    ++reply_count_;
    kind_ = Kind::kNotImplemented;
    condition_.notify_all();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::size_t reply_count_ = 0;
  Kind kind_ = Kind::kNone;
  std::optional<EncodableValue> value_;
  std::string code_;
  std::string message_;
  bool throw_on_success_ = false;
  bool throw_on_error_ = false;
  bool throw_on_not_implemented_ = false;
};

struct OwnedReplyState {
  using Kind = RecordingResult::Kind;

  bool Wait(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, timeout, [&] { return replies != 0; });
  }

  mutable std::mutex mutex;
  std::condition_variable condition;
  std::size_t replies = 0;
  Kind kind = Kind::kNone;
  std::optional<EncodableValue> value;
  std::string code;
  std::string message;
};

class OwnedRecordingResult final
    : public flutter::MethodResult<EncodableValue> {
 public:
  explicit OwnedRecordingResult(std::shared_ptr<OwnedReplyState> state)
      : state_(std::move(state)) {}

 protected:
  void SuccessInternal(const EncodableValue* result) override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    ++state_->replies;
    state_->kind = OwnedReplyState::Kind::kSuccess;
    state_->value = result == nullptr
                        ? std::nullopt
                        : std::optional<EncodableValue>(*result);
    state_->condition.notify_all();
  }

  void ErrorInternal(const std::string& error_code,
                     const std::string& error_message,
                     const EncodableValue*) override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    ++state_->replies;
    state_->kind = OwnedReplyState::Kind::kError;
    state_->code = error_code;
    state_->message = error_message;
    state_->condition.notify_all();
  }

  void NotImplementedInternal() override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    ++state_->replies;
    state_->kind = OwnedReplyState::Kind::kNotImplemented;
    state_->condition.notify_all();
  }

 private:
  std::shared_ptr<OwnedReplyState> state_;
};

std::shared_ptr<OwnedReplyState> Invoke(std::string method,
                                        EncodableValue arguments) {
  auto state = std::make_shared<OwnedReplyState>();
  HandleValueMethodCall(
      method, std::move(arguments),
      std::make_unique<OwnedRecordingResult>(state));
  return state;
}

class BlockingPathGate {
 public:
  void EnterAndWait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++entered_;
    condition_.notify_all();
    condition_.wait(lock, [&] { return released_; });
  }

  bool WaitUntilEntered(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5),
                               [&] { return entered_ >= count; });
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::size_t entered_ = 0;
  bool released_ = false;
};

void ExpectOwnedSuccess(const std::shared_ptr<OwnedReplyState>& state,
                        const std::string& message) {
  Expect(state->Wait(), message + " must complete");
  std::lock_guard<std::mutex> lock(state->mutex);
  Expect(state->kind == OwnedReplyState::Kind::kSuccess &&
             state->replies == 1,
         message);
}

void ExpectOwnedError(const std::shared_ptr<OwnedReplyState>& state,
                      const std::string& code, const std::string& message) {
  Expect(state->Wait(), message + " must complete");
  std::lock_guard<std::mutex> lock(state->mutex);
  Expect(state->kind == OwnedReplyState::Kind::kError &&
             state->code == code && state->replies == 1,
         message);
}

void ExpectError(const std::shared_ptr<RecordingResult>& result,
                 const std::string& code, const std::string& message) {
  Expect(result->Wait(), message + " must complete");
  Expect(result->kind() == RecordingResult::Kind::kError &&
             result->code() == code && result->reply_count() == 1,
         message);
}

void ExpectSuccess(const std::shared_ptr<RecordingResult>& result,
                   const std::string& message) {
  Expect(result->Wait(), message + " must complete");
  Expect(result->kind() == RecordingResult::Kind::kSuccess &&
             result->reply_count() == 1,
         message);
}

void TestPrimitiveExtraction() {
  const EncodableList values{
      I32(-7), I64(std::numeric_limits<std::int64_t>::max()), Text("path"),
      EncodableValue(true)};
  std::int64_t integer = 99;
  std::string text = "unchanged";
  Expect(GetInteger(values, 0, &integer) && integer == -7,
         "32-bit channel integers must be accepted");
  Expect(GetInteger(values, 1, &integer) &&
             integer == std::numeric_limits<std::int64_t>::max(),
         "64-bit channel integers must be accepted");
  Expect(!GetInteger(values, 2, &integer) && !GetInteger(values, 20, &integer),
         "non-integers and missing integer fields must be rejected");
  Expect(GetString(values, 2, &text) && text == "path",
         "channel strings must be extracted exactly");
  Expect(!GetString(values, 0, &text) && !GetString(values, 20, &text),
         "non-strings and missing string fields must be rejected");
}

void TestErrorMappingAndTimeouts() {
  Expect(std::string(AdmissionErrorCode(
             AdmissionConfigurationResult::kInvalidArguments)) ==
                 "INVALID_ARGUMENTS" &&
             std::string(AdmissionErrorCode(
                 AdmissionConfigurationResult::kAlreadyStarted)) ==
                 "SCHEDULER_ALREADY_STARTED" &&
             std::string(AdmissionErrorCode(
                 AdmissionConfigurationResult::kResourceFailure)) ==
                 "SCHEDULER_ERROR",
         "every admission failure must retain its public scheduler code");
  Expect(ErrorCodeName(ErrorCode::kInvalidArguments, "fallback") ==
             "INVALID_ARGUMENTS" &&
             ErrorCodeName(ErrorCode::kUnsupportedInput, "fallback") ==
                 "UNSUPPORTED_INPUT" &&
             ErrorCodeName(ErrorCode::kIo, "fallback") == "IO_ERROR" &&
             ErrorCodeName(ErrorCode::kTimeout, "fallback") == "TIMEOUT" &&
             ErrorCodeName(ErrorCode::kCodec, "fallback") == "fallback",
         "every shared codec error must retain the Windows public code");

  const Error error{ErrorCode::kCodec, "codec message"};
  const auto failure = Failure(error, "INVERSE_ERROR");
  const auto invalid = Invalid("invalid message");
  const auto io = IoFailure("io message");
  const auto timeout = TimeoutFailure("timeout message");
  Expect(!failure.success && failure.code == "INVERSE_ERROR" &&
             failure.message == "codec message" &&
             invalid.code == "INVALID_ARGUMENTS" && io.code == "IO_ERROR" &&
             timeout.code == "TIMEOUT",
         "Windows operation failure constructors must preserve their cause");

  const auto now = Clock::now();
  const auto remaining = RemainingTimeout(100, now);
  Expect(RemainingTimeout(0, now) == 0 && remaining > 0 && remaining <= 100 &&
             RemainingTimeout(1, now - std::chrono::milliseconds(10)) == 0,
         "remaining time must handle disabled, active, and expired deadlines");
  Expect(ValidPerformance(0, 0) && ValidPerformance(2, 1) &&
             !ValidPerformance(-1, 0) && !ValidPerformance(3, 0) &&
             !ValidPerformance(1, -1),
         "priority and timeout boundaries must be exact");

  Expect(ConversionScheduler::Instance().Configure(257, 0) ==
             AdmissionConfigurationResult::kInvalidArguments &&
             ConversionScheduler::Instance().Configure(0, 257) ==
                 AdmissionConfigurationResult::kInvalidArguments,
         "the native scheduler must reject both limits above 256");
  AdmissionConfigurationResult submission =
      AdmissionConfigurationResult::kSuccess;
  Expect(!ConversionScheduler::Instance().Submit(
             static_cast<TaskPriority>(3), 1, Clock::time_point::max(), {},
             {}, {}, &submission) &&
             submission == AdmissionConfigurationResult::kInvalidArguments &&
             !ConversionScheduler::Instance().Submit(
                 static_cast<TaskPriority>(3), 1,
                 Clock::time_point::max(), {}, {}, {}),
         "invalid priorities must fail before scheduler startup");
}

void TestSafeResultDelivery() {
  SafeSuccess(nullptr);
  SafeSuccess(nullptr, EncodableValue());
  SafeError(nullptr, "ERROR", "message");
  SafeNotImplemented(nullptr);

  auto success = std::make_shared<RecordingResult>();
  SafeSuccess(success.get(), EncodableValue(std::int32_t{7}));
  ExpectSuccess(success, "safe valued success must reply exactly once");
  const auto success_value = success->value();
  Expect(success_value && std::get<std::int32_t>(*success_value) == 7,
         "safe valued success must preserve its payload");

  auto empty = std::make_shared<RecordingResult>();
  SafeSuccess(empty.get());
  ExpectSuccess(empty, "safe empty success must reply exactly once");

  auto error = std::make_shared<RecordingResult>();
  SafeError(error.get(), "ERROR", "message");
  ExpectError(error, "ERROR", "safe errors must reply exactly once");

  auto not_implemented = std::make_shared<RecordingResult>();
  SafeNotImplemented(not_implemented.get());
  Expect(not_implemented->Wait() &&
             not_implemented->kind() ==
                 RecordingResult::Kind::kNotImplemented &&
             not_implemented->reply_count() == 1,
         "safe not-implemented delivery must reply exactly once");

  auto throwing_success = std::make_shared<RecordingResult>();
  throwing_success->ThrowOnNextSuccess();
  SafeSuccess(throwing_success.get());
  Expect(throwing_success->reply_count() == 0,
         "result success exceptions must not escape native callbacks");
  auto throwing_valued_success = std::make_shared<RecordingResult>();
  throwing_valued_success->ThrowOnNextSuccess();
  SafeSuccess(throwing_valued_success.get(), EncodableValue(std::int32_t{8}));
  Expect(throwing_valued_success->reply_count() == 0,
         "valued result exceptions must not escape native callbacks");
  auto throwing_error = std::make_shared<RecordingResult>();
  throwing_error->ThrowOnNextError();
  SafeError(throwing_error.get(), "ERROR", "message");
  Expect(throwing_error->reply_count() == 0,
         "result error exceptions must not escape native callbacks");
  auto throwing_not_implemented = std::make_shared<RecordingResult>();
  throwing_not_implemented->ThrowOnNextNotImplemented();
  SafeNotImplemented(throwing_not_implemented.get());
  Expect(throwing_not_implemented->reply_count() == 0,
         "not-implemented exceptions must not escape the Flutter boundary");
}

void TestMethodDispatch(const std::vector<std::uint8_t>& jpeg) {
  const std::vector<EncodableValue> malformed_configurations{
      EncodableValue(true),
      Arguments({I32(0)}),
      Arguments({Text("workers"), I32(0)}),
      Arguments({I32(-1), I32(0)}),
      Arguments({I32(257), I32(0)}),
      Arguments({I32(0), I32(-1)}),
      Arguments({I32(0), I32(257)}),
  };
  for (const auto& arguments : malformed_configurations) {
    ExpectOwnedError(Invoke("configureJxlScheduler", arguments),
                     "INVALID_ARGUMENTS",
                     "malformed configuration must fail before first use");
  }

  ExpectOwnedSuccess(
      Invoke("configureJxlScheduler", Arguments({I32(0), I64(2)})),
      "valid automatic scheduler configuration must succeed");
  ExpectOwnedSuccess(
      Invoke("configureJxlScheduler", Arguments({I64(0), I32(2)})),
      "identical scheduler configuration must be idempotent");

  auto unknown = Invoke("unknownMethod", EncodableValue());
  Expect(unknown->Wait(), "unknown dispatch must complete");
  {
    std::lock_guard<std::mutex> lock(unknown->mutex);
    Expect(unknown->kind == OwnedReplyState::Kind::kNotImplemented &&
               unknown->replies == 1,
           "unknown dispatch must return not-implemented exactly once");
  }

  ExpectOwnedError(
      Invoke("jpegBytesToJxl",
             Arguments({Bytes(jpeg), I32(10), I32(0), I32(1), I32(0)})),
      "INVALID_ARGUMENTS",
      "malformed conversion dispatch must fail before scheduler admission");

  auto encoded_state = Invoke(
      "jpegBytesToJxl",
      Arguments({Bytes(jpeg), I32(7), I32(0), I32(2), I32(0)}));
  ExpectOwnedSuccess(encoded_state,
                     "valid byte dispatch must complete asynchronously");
  std::vector<std::uint8_t> encoded;
  {
    std::lock_guard<std::mutex> lock(encoded_state->mutex);
    const auto* bytes = encoded_state->value
                            ? std::get_if<std::vector<std::uint8_t>>(
                                  &*encoded_state->value)
                            : nullptr;
    Expect(bytes != nullptr && !bytes->empty(),
           "byte dispatch must return a typed byte result");
    if (bytes != nullptr) encoded = *bytes;
  }

  auto restored_state = Invoke(
      "jxlBytesToJpeg", Arguments({Bytes(encoded), I32(0), I32(0)}));
  ExpectOwnedSuccess(restored_state,
                     "valid inverse dispatch must complete asynchronously");
  {
    std::lock_guard<std::mutex> lock(restored_state->mutex);
    const auto* restored = restored_state->value
                               ? std::get_if<std::vector<std::uint8_t>>(
                                     &*restored_state->value)
                               : nullptr;
    Expect(restored != nullptr && *restored == jpeg,
           "method dispatch must preserve a byte-exact round trip");
  }

  adapter_path_operation_for_testing =
      [](bool encode, const std::string& input, const std::string& output) {
        Expect(encode && input == "input.jpg" && output == "output.jxl",
               "path dispatch must preserve method direction and values");
        return OperationResult{true, {}, {}, jxl_coder::OutputBuffer()};
      };
  ExpectOwnedSuccess(
      Invoke("jpegPathToJxl",
             Arguments({Text("input.jpg"), Text("output.jxl"), I32(7),
                        I32(0), I32(1), I32(0)})),
      "valid path dispatch must complete asynchronously");
  adapter_path_operation_for_testing = {};

  BlockingPathGate gate;
  adapter_path_operation_for_testing =
      [&](bool, const std::string& input, const std::string&) {
        if (input == "block-one" || input == "block-two") {
          gate.EnterAndWait();
        }
        return OperationResult{true, {}, {}, jxl_coder::OutputBuffer()};
      };
  auto first_blocker = Invoke(
      "jpegPathToJxl",
      Arguments({Text("block-one"), Text("one.jxl"), I32(7), I32(0),
                 I32(1), I32(0)}));
  auto second_blocker = Invoke(
      "jpegPathToJxl",
      Arguments({Text("block-two"), Text("two.jxl"), I32(7), I32(0),
                 I32(1), I32(0)}));
  Expect(gate.WaitUntilEntered(2),
         "both active-conversion slots must enter the blocking path hook");
  ExpectOwnedError(
      Invoke("jpegBytesToJxl",
             Arguments({Bytes(jpeg), I32(7), I32(0), I32(2), I32(1)})),
      "TIMEOUT", "queued method dispatch must honor its admission deadline");
  gate.Release();
  ExpectOwnedSuccess(first_blocker, "the first blocking path must finish");
  ExpectOwnedSuccess(second_blocker, "the second blocking path must finish");

  adapter_path_operation_for_testing =
      [](bool, const std::string&, const std::string&) -> OperationResult {
    throw std::runtime_error("injected single path operation failure");
  };
  ExpectOwnedError(
      Invoke("jpegPathToJxl",
             Arguments({Text("throw"), Text("out"), I32(7), I32(0), I32(1),
                        I32(0)})),
      "TRANSCODE_ERROR",
      "single encode operation exceptions must reach the failure callback");
  ExpectOwnedError(
      Invoke("jxlPathToJpeg",
             Arguments({Text("throw"), Text("out"), I32(1), I32(0)})),
      "INVERSE_ERROR",
      "single inverse operation exceptions must reach the failure callback");
  adapter_path_operation_for_testing = {};

  ExpectOwnedSuccess(
      Invoke("jxlPathsToJpeg", Arguments({I32(1), I32(0)})),
      "empty batch dispatch must complete immediately");
  ExpectOwnedSuccess(
      Invoke("jpegPathsToJxl",
             Arguments({I32(7), I32(0), I32(1), I32(0)})),
      "empty encode batch dispatch must complete immediately");
  ExpectOwnedError(
      Invoke("configureJxlScheduler", Arguments({I32(0), I32(3)})),
      "SCHEDULER_ALREADY_STARTED",
      "conversion dispatch must make conflicting scheduler changes immutable");
}

void TestExecuteBytes(const std::vector<std::uint8_t>& jpeg,
                      const std::vector<std::uint8_t>& legacy_jxl) {
  const auto started = Clock::now();
  auto encoded = Execute(
      "jpegBytesToJxl",
      Arguments({Bytes(jpeg), I64(7), I32(0), I64(2), I32(0)}), started,
      42);
  Expect(encoded.success && encoded.bytes.size() > 0,
         "the Windows byte adapter must invoke the shared encoder");
  const auto encoded_bytes = CopyBytes(encoded.bytes);

  auto restored = Execute(
      "jxlBytesToJpeg",
      Arguments({Bytes(encoded_bytes), I32(0), I64(0)}), started, 43);
  Expect(restored.success && CopyBytes(restored.bytes) == jpeg,
         "the Windows byte adapter must reconstruct its input byte-exactly");

  auto legacy = Execute(
      "jxlBytesToJpeg", Arguments({Bytes(legacy_jxl), I32(1), I32(0)}),
      started, 44);
  const auto legacy_bytes = CopyBytes(legacy.bytes);
  Expect(legacy.success && legacy_bytes.size() > 2 &&
             legacy_bytes[0] == 0xff && legacy_bytes[1] == 0xd8,
         "the Windows byte adapter must reconstruct the legacy JXL fixture");

  const std::vector<EncodableValue> invalid_arguments{
      EncodableValue(true),
      Arguments({Bytes(jpeg), I32(7), I32(0), I32(1)}),
      Arguments({Bytes(jpeg), Text("7"), I32(0), I32(1), I32(0)}),
      Arguments({Bytes(jpeg), I32(0), I32(0), I32(1), I32(0)}),
      Arguments({Bytes(jpeg), I32(10), I32(0), I32(1), I32(0)}),
      Arguments({Bytes(jpeg), I32(7), I32(-1), I32(1), I32(0)}),
      Arguments({Bytes(jpeg), I32(7), I32(5), I32(1), I32(0)}),
      Arguments({Bytes(jpeg), I32(7), Text("speed"), I32(1), I32(0)}),
      Arguments({Bytes(jpeg), I32(7), I32(0), I32(-1), I32(0)}),
      Arguments({Bytes(jpeg), I32(7), I32(0), I32(3), I32(0)}),
      Arguments({Bytes(jpeg), I32(7), I32(0), Text("priority"), I32(0)}),
      Arguments({Bytes(jpeg), I32(7), I32(0), I32(1), I32(-1)}),
      Arguments({Bytes(jpeg), I32(7), I32(0), I32(1), Text("timeout")}),
      Arguments({Text("jpeg"), I32(7), I32(0), I32(1), I32(0)}),
  };
  for (const auto& arguments : invalid_arguments) {
    const auto operation =
        Execute("jpegBytesToJxl", arguments, started, 45);
    Expect(!operation.success && operation.code == "INVALID_ARGUMENTS",
           "every malformed JPEG byte shape must fail before codec admission");
  }

  const auto invalid_inverse = Execute(
      "jxlBytesToJpeg", Arguments({Text("jxl"), I32(1), I32(0)}), started,
      46);
  Expect(!invalid_inverse.success &&
             invalid_inverse.code == "INVALID_ARGUMENTS",
         "the inverse byte adapter must reject a non-byte payload");
  const std::vector<EncodableValue> invalid_inverse_arguments{
      Arguments({Bytes(legacy_jxl), I32(1)}),
      Arguments({Bytes(legacy_jxl), Text("priority"), I32(0)}),
      Arguments({Bytes(legacy_jxl), I32(3), I32(0)}),
      Arguments({Bytes(legacy_jxl), I32(1), I32(-1)}),
      Arguments({Bytes(legacy_jxl), I32(1), Text("timeout")}),
  };
  for (const auto& arguments : invalid_inverse_arguments) {
    const auto operation =
        Execute("jxlBytesToJpeg", arguments, started, 46);
    Expect(!operation.success && operation.code == "INVALID_ARGUMENTS",
           "every malformed inverse byte shape must fail pre-admission");
  }
  const auto expired_encode = Execute(
      "jpegBytesToJxl",
      Arguments({Bytes(jpeg), I32(7), I32(0), I32(1), I32(1)}),
      started - std::chrono::seconds(1), 47);
  const auto expired_inverse = Execute(
      "jxlBytesToJpeg", Arguments({Bytes(legacy_jxl), I32(1), I32(1)}),
      started - std::chrono::seconds(1), 48);
  Expect(expired_encode.code == "TIMEOUT" &&
             expired_inverse.code == "TIMEOUT",
         "expired byte calls must stop before entering libjxl");

  const auto malformed = Execute(
      "jpegBytesToJxl",
      Arguments({Bytes({0, 1, 2, 3}), I32(7), I32(0), I32(1), I32(0)}),
      started, 49);
  Expect(!malformed.success && !malformed.code.empty() &&
             !malformed.message.empty(),
         "codec failures must preserve a stable public error and message");
  const auto malformed_inverse = Execute(
      "jxlBytesToJpeg", Arguments({Bytes({0, 1, 2, 3}), I32(1), I32(0)}),
      started, 49);
  Expect(!malformed_inverse.success && !malformed_inverse.code.empty() &&
             !malformed_inverse.message.empty(),
         "inverse codec failures must preserve a public error and message");
  const auto unknown = Execute("unknown", Arguments({}), started, 50);
  Expect(!unknown.success && unknown.code == "NOT_IMPLEMENTED",
         "unknown execution methods must remain distinguishable");
}

void TestSchedulingExtraction(const std::vector<std::uint8_t>& bytes) {
  struct Case {
    std::string method;
    EncodableValue arguments;
    TaskPriority priority;
    std::int64_t timeout;
  };
  std::vector<Case> cases;
  cases.push_back({"jpegBytesToJxl",
                   Arguments({Bytes(bytes), I32(1), I32(4), I32(0), I32(1)}),
                   TaskPriority::kLow, 1});
  cases.push_back({"jxlBytesToJpeg",
                   Arguments({Bytes(bytes), I64(2), I64(2)}),
                   TaskPriority::kHigh, 2});
  cases.push_back({"jpegPathToJxl",
                   Arguments({Text("in.jpg"), Text("out.jxl"), I32(9),
                              I32(0), I32(1), I64(3)}),
                   TaskPriority::kNormal, 3});
  cases.push_back({"jxlPathToJpeg",
                   Arguments({Text("in.jxl"), Text("out.jpg"), I32(2),
                              I32(4)}),
                   TaskPriority::kHigh, 4});
  for (const auto& item : cases) {
    TaskPriority priority = TaskPriority::kNormal;
    std::int64_t timeout = -1;
    Expect(ExtractSingleScheduling(item.method, item.arguments, &priority,
                                   &timeout) &&
               priority == item.priority && timeout == item.timeout,
           "every single-call shape must expose scheduling before admission");
  }

  TaskPriority priority = TaskPriority::kNormal;
  std::int64_t timeout = 0;
  const std::vector<std::pair<std::string, EncodableValue>> invalid{
      {"unknown", Arguments({})},
      {"jpegBytesToJxl", EncodableValue(true)},
      {"jpegBytesToJxl",
       Arguments({Text("jpeg"), I32(7), I32(0), I32(1), I32(0)})},
      {"jpegBytesToJxl",
       Arguments({Bytes(bytes), Text("effort"), I32(0), I32(1), I32(0)})},
      {"jpegBytesToJxl",
       Arguments({Bytes(bytes), I32(7), Text("speed"), I32(1), I32(0)})},
      {"jpegBytesToJxl",
       Arguments({Bytes(bytes), I32(7), I32(0), Text("priority"), I32(0)})},
      {"jpegBytesToJxl",
       Arguments({Bytes(bytes), I32(7), I32(0), I32(1), Text("timeout")})},
      {"jxlBytesToJpeg", Arguments({Bytes(bytes), I32(3), I32(0)})},
      {"jxlBytesToJpeg", Arguments({Bytes(bytes), I32(1), Text("timeout")})},
      {"jpegPathToJxl",
       Arguments({Text("in"), Text("out"), I32(7), I32(5), I32(1),
                  I32(0)})},
      {"jpegPathToJxl",
       Arguments({Text("in"), Text("out"), Text("effort"), I32(0),
                  I32(1), I32(0)})},
      {"jpegPathToJxl",
       Arguments({Text("in"), Text("out"), I32(7), Text("speed"),
                  I32(1), I32(0)})},
      {"jpegPathToJxl",
       Arguments({Text("in"), Text("out"), I32(7), I32(0),
                  Text("priority"), I32(0)})},
      {"jpegPathToJxl",
       Arguments({Text("in"), Text("out"), I32(7), I32(0), I32(1),
                  Text("timeout")})},
      {"jxlPathToJpeg",
       Arguments({Text("in"), EncodableValue(true), I32(1), I32(0)})},
      {"jxlPathToJpeg",
       Arguments({Text("in"), Text("out"), Text("priority"), I32(0)})},
      {"jxlPathToJpeg",
       Arguments({Text("in"), Text("out"), I32(1), Text("timeout")})},
  };
  for (const auto& item : invalid) {
    Expect(!ExtractSingleScheduling(item.first, item.second, &priority,
                                    &timeout),
           "malformed single-call scheduling must be rejected pre-admission");
  }
}

void TestExecutePathParsing() {
  std::mutex calls_mutex;
  std::vector<std::tuple<bool, std::string, std::string>> calls;
  adapter_path_operation_for_testing =
      [&](bool encode, const std::string& input, const std::string& output) {
        std::lock_guard<std::mutex> lock(calls_mutex);
        calls.emplace_back(encode, input, output);
        return OperationResult{true, {}, {}, jxl_coder::OutputBuffer()};
      };
  const auto started = Clock::now();
  const auto encode = Execute(
      "jpegPathToJxl",
      Arguments({Text("entrée.jpg"), Text("résultat.jxl"), I64(1), I32(4),
                 I32(2), I64(0)}),
      started, 51);
  const auto inverse = Execute(
      "jxlPathToJpeg",
      Arguments({Text("résultat.jxl"), Text("restored.jpg"), I32(0), I64(0)}),
      started, 52);
  Expect(encode.success && inverse.success && calls.size() == 2 &&
             calls[0] == std::make_tuple(true, std::string("entrée.jpg"),
                                         std::string("résultat.jxl")) &&
             calls[1] == std::make_tuple(false, std::string("résultat.jxl"),
                                         std::string("restored.jpg")),
         "valid path calls must preserve direction and UTF-8 paths");

  const std::vector<std::pair<std::string, EncodableValue>> invalid{
      {"jpegPathToJxl", Arguments({Text("in"), Text("out")})},
      {"jpegPathToJxl",
       Arguments({EncodableValue(true), Text("out"), I32(7), I32(0), I32(1),
                  I32(0)})},
      {"jpegPathToJxl",
       Arguments({Text("in"), EncodableValue(true), I32(7), I32(0), I32(1),
                  I32(0)})},
      {"jpegPathToJxl",
       Arguments({Text("in"), Text("out"), Text("effort"), I32(0), I32(1),
                  I32(0)})},
      {"jpegPathToJxl",
       Arguments({Text("in"), Text("out"), I32(7), Text("speed"), I32(1),
                  I32(0)})},
      {"jpegPathToJxl",
       Arguments({Text("in"), Text("out"), I32(7), I32(0),
                  Text("priority"), I32(0)})},
      {"jpegPathToJxl",
       Arguments({Text("in"), Text("out"), I32(7), I32(0), I32(1),
                  Text("timeout")})},
      {"jpegPathToJxl",
       Arguments({Text("in"), Text("out"), I32(0), I32(0), I32(1), I32(0)})},
      {"jpegPathToJxl",
       Arguments({Text("in"), Text("out"), I32(10), I32(0), I32(1),
                  I32(0)})},
      {"jpegPathToJxl",
       Arguments({Text("in"), Text("out"), I32(7), I32(-1), I32(1),
                  I32(0)})},
      {"jpegPathToJxl",
       Arguments({Text("in"), Text("out"), I32(7), I32(5), I32(1), I32(0)})},
      {"jpegPathToJxl",
       Arguments({Text("in"), Text("out"), I32(7), I32(0), I32(-1),
                  I32(0)})},
      {"jpegPathToJxl",
       Arguments({Text("in"), Text("out"), I32(7), I32(0), I32(3), I32(0)})},
      {"jpegPathToJxl",
       Arguments({Text("in"), Text("out"), I32(7), I32(0), I32(1),
                  I32(-1)})},
      {"jxlPathToJpeg", Arguments({Text("in"), Text("out"), I32(1)})},
      {"jxlPathToJpeg",
       Arguments({EncodableValue(true), Text("out"), I32(1), I32(0)})},
      {"jxlPathToJpeg",
       Arguments({Text("in"), EncodableValue(true), I32(1), I32(0)})},
      {"jxlPathToJpeg",
       Arguments({Text("in"), Text("out"), Text("priority"), I32(0)})},
      {"jxlPathToJpeg",
       Arguments({Text("in"), Text("out"), I32(1), Text("timeout")})},
      {"jxlPathToJpeg",
       Arguments({Text("in"), Text("out"), I32(-1), I32(0)})},
      {"jxlPathToJpeg",
       Arguments({Text("in"), Text("out"), I32(3), I32(0)})},
      {"jxlPathToJpeg",
       Arguments({Text("in"), Text("out"), I32(1), I32(-1)})},
  };
  for (const auto& item : invalid) {
    const auto operation = Execute(item.first, item.second, started, 53);
    Expect(!operation.success && operation.code == "INVALID_ARGUMENTS",
           "malformed path calls must fail before native file access");
  }

  adapter_path_operation_for_testing = {};
  const auto default_stub = Execute(
      "jpegPathToJxl",
      Arguments({Text("in"), Text("out"), I32(7), I32(0), I32(1), I32(0)}),
      started, 54);
  const auto default_inverse_stub = Execute(
      "jxlPathToJpeg",
      Arguments({Text("in"), Text("out"), I32(1), I32(0)}), started, 55);
  Expect(default_stub.success && default_inverse_stub.success,
         "the portable adapter's default path boundaries must be inert");
}

void TestSingleCompletion(const std::vector<std::uint8_t>& jpeg) {
  auto encoded = Execute(
      "jpegBytesToJxl",
      Arguments({Bytes(jpeg), I32(7), I32(0), I32(1), I32(0)}),
      Clock::now(), 60);
  const auto expected = CopyBytes(encoded.bytes);
  auto byte_result = std::make_shared<RecordingResult>();
  CompleteSingle("jpegBytesToJxl", byte_result, std::move(encoded));
  ExpectSuccess(byte_result, "byte completion must return success once");
  const auto value = byte_result->value();
  const auto* returned =
      value ? std::get_if<std::vector<std::uint8_t>>(&*value) : nullptr;
  Expect(returned != nullptr && *returned == expected,
         "byte completion must make one exact-size channel copy");

  auto empty_result = std::make_shared<RecordingResult>();
  CompleteSingle("jxlBytesToJpeg", empty_result,
                 {true, {}, {}, jxl_coder::OutputBuffer()});
  ExpectSuccess(empty_result, "an empty successful byte output must complete");
  const auto empty_value = empty_result->value();
  const auto* empty_bytes =
      empty_value ? std::get_if<std::vector<std::uint8_t>>(&*empty_value)
                  : nullptr;
  Expect(empty_bytes != nullptr && empty_bytes->empty(),
         "an empty native output must remain an empty typed byte value");

  auto path_result = std::make_shared<RecordingResult>();
  CompleteSingle("jpegPathToJxl", path_result,
                 {true, {}, {}, jxl_coder::OutputBuffer()});
  ExpectSuccess(path_result, "path completion must return a null success");
  Expect(!path_result->value().has_value(),
         "path success must not synthesize a byte value");

  auto failure_result = std::make_shared<RecordingResult>();
  CompleteSingle("jxlBytesToJpeg", failure_result,
                 {false, "INVERSE_ERROR", "failed",
                  jxl_coder::OutputBuffer()});
  ExpectError(failure_result, "INVERSE_ERROR",
              "failed single calls must preserve their native code");

  auto allocation_result = std::make_shared<RecordingResult>();
  allocation_result->ThrowOnNextSuccess();
  CompleteSingle("jpegBytesToJxl", allocation_result,
                 {true, {}, {}, jxl_coder::OutputBuffer()});
  Expect(allocation_result->reply_count() == 0,
         "throwing result delivery must not escape single completion");
}

void TestBatchCompletionOrdering() {
  auto ordered_result = std::make_shared<RecordingResult>();
  auto ordered = std::make_shared<BatchCompletion>(3, ordered_result);
  ordered->Finish(
      2, {false, "TIMEOUT", "third", jxl_coder::OutputBuffer()});
  ordered->Finish(1, {true, {}, {}, jxl_coder::OutputBuffer()});
  Expect(ordered_result->reply_count() == 0,
         "a batch must wait for every entry before replying");
  ordered->Finish(
      0, {false, "IO_ERROR", "first", jxl_coder::OutputBuffer()});
  ExpectError(ordered_result, "IO_ERROR",
              "batch error selection must follow input order");
  Expect(ordered_result->message() == "first",
         "batch error selection must retain the first input message");

  auto success_result = std::make_shared<RecordingResult>();
  auto success = std::make_shared<BatchCompletion>(64, success_result);
  std::vector<std::thread> finishers;
  finishers.reserve(64);
  for (std::size_t index = 0; index < 64; ++index) {
    finishers.emplace_back(
        [success, index] {
          success->Finish(
              index, {true, {}, {}, jxl_coder::OutputBuffer()});
        });
  }
  for (auto& finisher : finishers) finisher.join();
  ExpectSuccess(success_result,
                "concurrent batch completion must send exactly one reply");

  auto guarded_result = std::make_shared<RecordingResult>();
  auto guarded = std::make_shared<BatchCompletion>(2, guarded_result);
  guarded->Finish(0, {true, {}, {}, jxl_coder::OutputBuffer()});
  guarded->Finish(0, {false, "IO_ERROR", "duplicate",
                      jxl_coder::OutputBuffer()});
  guarded->Finish(9, {false, "IO_ERROR", "out of range",
                      jxl_coder::OutputBuffer()});
  Expect(guarded_result->reply_count() == 0,
         "duplicate and out-of-range completion must not finish a batch");
  guarded->Finish(1, {true, {}, {}, jxl_coder::OutputBuffer()});
  guarded->Finish(1, {false, "IO_ERROR", "late duplicate",
                      jxl_coder::OutputBuffer()});
  ExpectSuccess(guarded_result,
                "each batch index must contribute to completion exactly once");

  auto throwing_result = std::make_shared<RecordingResult>();
  throwing_result->ThrowOnNextError();
  auto throwing = std::make_shared<BatchCompletion>(1, throwing_result);
  throwing->Finish(0, {false, "IO_ERROR", "failure",
                       jxl_coder::OutputBuffer()});
  Expect(throwing_result->reply_count() == 0,
         "throwing batch result delivery must stay inside the native boundary");
}

void TestBatchScheduling() {
  Expect(ConversionScheduler::Instance().Configure(0, 2) ==
             AdmissionConfigurationResult::kSuccess,
         "adapter batches must accept an explicit two-pipeline test policy");
  const auto expect_invalid = [](const std::string& method,
                                 EncodableValue arguments,
                                 const std::string& label) {
    auto result = std::make_shared<RecordingResult>();
    ScheduleBatch(method, arguments, result);
    ExpectError(result, "INVALID_ARGUMENTS", label);
  };
  expect_invalid("jpegPathsToJxl", EncodableValue(true),
                 "batch arguments must be a positional list");
  expect_invalid("jpegPathsToJxl",
                 Arguments({I32(7), I32(0), I32(1)}),
                 "encode batches must contain their complete prefix");
  expect_invalid("jpegPathsToJxl",
                 Arguments({I32(7), I32(0), I32(1), I32(0), Text("in")}),
                 "batch paths must form complete pairs");
  expect_invalid("jpegPathsToJxl",
                 Arguments({I32(0), I32(0), I32(1), I32(0)}),
                 "batch effort must respect the lower boundary");
  expect_invalid("jpegPathsToJxl",
                 Arguments({Text("effort"), I32(0), I32(1), I32(0)}),
                 "batch effort must be an integer");
  expect_invalid("jpegPathsToJxl",
                 Arguments({I32(10), I32(0), I32(1), I32(0)}),
                 "batch effort must respect the upper boundary");
  expect_invalid("jpegPathsToJxl",
                 Arguments({I32(7), Text("speed"), I32(1), I32(0)}),
                 "batch decoding speed must be an integer");
  expect_invalid("jpegPathsToJxl",
                 Arguments({I32(7), I32(-1), I32(1), I32(0)}),
                 "batch decoding speed must respect the lower boundary");
  expect_invalid("jpegPathsToJxl",
                 Arguments({I32(7), I32(5), I32(1), I32(0)}),
                 "batch decoding speed must respect the upper boundary");
  expect_invalid("jpegPathsToJxl",
                 Arguments({I32(7), I32(0), Text("priority"), I32(0)}),
                 "encode batch priority must be an integer");
  expect_invalid("jpegPathsToJxl",
                 Arguments({I32(7), I32(0), I32(3), I32(0)}),
                 "encode batch priority must be valid");
  expect_invalid("jpegPathsToJxl",
                 Arguments({I32(7), I32(0), I32(1), Text("timeout")}),
                 "encode batch timeout must be an integer");
  expect_invalid("jpegPathsToJxl",
                 Arguments({I32(7), I32(0), I32(1), I32(-1)}),
                 "encode batch timeout must be nonnegative");
  expect_invalid("jxlPathsToJpeg",
                 Arguments({Text("priority"), I32(0)}),
                 "inverse batch priority must be an integer");
  expect_invalid("jxlPathsToJpeg", Arguments({I32(3), I32(0)}),
                 "inverse batch priority must be valid");
  expect_invalid("jxlPathsToJpeg",
                 Arguments({I32(1), Text("timeout")}),
                 "inverse batch timeout must be an integer");
  expect_invalid("jxlPathsToJpeg",
                 Arguments({I32(1), I32(-1)}),
                 "inverse batch timeouts must be nonnegative");
  expect_invalid("jxlPathsToJpeg",
                 Arguments({I32(1), I32(0), EncodableValue(true),
                            Text("out")}),
                 "batch input paths must be strings");
  expect_invalid("jxlPathsToJpeg",
                 Arguments({I32(1), I32(0), Text("in"),
                            EncodableValue(true)}),
                 "batch path values must be strings");

  auto empty_encode = std::make_shared<RecordingResult>();
  ScheduleBatch("jpegPathsToJxl",
                Arguments({I64(9), I32(4), I32(2), I64(0)}), empty_encode);
  ExpectSuccess(empty_encode,
                "an empty encode batch must complete immediately");
  auto empty_inverse = std::make_shared<RecordingResult>();
  ScheduleBatch("jxlPathsToJpeg", Arguments({I32(0), I64(0)}),
                empty_inverse);
  ExpectSuccess(empty_inverse,
                "an empty inverse batch must complete immediately");

  adapter_path_operation_for_testing =
      [](bool encode, const std::string& input, const std::string& output) {
        Expect(encode && input == "in.jpg" && output == "out.jxl",
               "valid batch paths must reach the adapter unchanged");
        return OperationResult{true, {}, {}, jxl_coder::OutputBuffer()};
      };
  auto success = std::make_shared<RecordingResult>();
  ScheduleBatch("jpegPathsToJxl",
                Arguments({I32(7), I32(0), I32(1), I32(0), Text("in.jpg"),
                           Text("out.jxl")}),
                success);
  ExpectSuccess(success, "a valid native path batch must complete once");

  adapter_path_operation_for_testing =
      [](bool, const std::string& input, const std::string&) {
        if (input == "first") {
          std::this_thread::sleep_for(std::chrono::milliseconds(30));
          return OperationResult{false, "IO_ERROR", "first error",
                                 jxl_coder::OutputBuffer()};
        }
        return OperationResult{false, "TIMEOUT", "second error",
                               jxl_coder::OutputBuffer()};
      };
  auto ordered = std::make_shared<RecordingResult>();
  ScheduleBatch(
      "jxlPathsToJpeg",
      Arguments({I32(1), I32(0), Text("first"), Text("one"), Text("second"),
                 Text("two")}),
      ordered);
  ExpectError(ordered, "IO_ERROR",
              "out-of-order native batch failures must use input order");
  Expect(ordered->message() == "first error",
         "the selected out-of-order batch error must preserve its message");

  adapter_path_operation_for_testing =
      [](bool, const std::string&, const std::string&) -> OperationResult {
    throw std::runtime_error("injected path adapter failure");
  };
  auto throwing = std::make_shared<RecordingResult>();
  ScheduleBatch("jpegPathsToJxl",
                Arguments({I32(7), I32(0), I32(1), I32(0), Text("throw"),
                           Text("out")}),
                throwing);
  ExpectError(throwing, "TRANSCODE_ERROR",
              "unexpected batch operation exceptions must be contained");
  adapter_path_operation_for_testing = {};
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: windows_adapter_test JPEG_FIXTURE JXL_FIXTURE\n";
    return 2;
  }
  const auto jpeg = ReadFile(argv[1]);
  const auto jxl = ReadFile(argv[2]);
  if (jpeg.empty() || jxl.empty()) {
    std::cerr << "fixtures could not be read\n";
    return 2;
  }

  TestPrimitiveExtraction();
  TestErrorMappingAndTimeouts();
  TestSafeResultDelivery();
  TestMethodDispatch(jpeg);
  TestExecuteBytes(jpeg, jxl);
  TestSchedulingExtraction(jpeg);
  TestExecutePathParsing();
  TestSingleCompletion(jpeg);
  TestBatchCompletionOrdering();
  TestBatchScheduling();

  const int failure_count = failures.load();
  if (failure_count != 0) {
    std::cerr << failure_count << " Windows adapter test(s) failed\n";
    return 1;
  }
  std::cout << "Windows adapter tests passed\n";
  return 0;
}
