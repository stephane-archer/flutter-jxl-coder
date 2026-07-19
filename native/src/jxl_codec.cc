#include "jxl_codec.h"
#include "deadline_utils.h"
#include "shared_parallel_runner.h"

#include <jxl/decode.h>
#include <jxl/encode.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>

namespace jxl_coder {
namespace {

constexpr std::size_t kInitialOutputBytes = 4096;

#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
std::atomic<int> output_allocation_failure_countdown{-1};
std::atomic<std::size_t> initial_output_capacity_override{0};
std::atomic<std::uint8_t> codec_failure_point{
    static_cast<std::uint8_t>(CodecFailurePointForTesting::kNone)};

bool FailOutputAllocationForTesting() {
  int countdown =
      output_allocation_failure_countdown.load(std::memory_order_relaxed);
  while (countdown >= 0) {
    const int next = countdown == 0 ? -1 : countdown - 1;
    if (output_allocation_failure_countdown.compare_exchange_weak(
            countdown, next, std::memory_order_relaxed)) {
      return countdown == 0;
    }
  }
  return false;
}

bool ConsumeCodecFailureForTesting(CodecFailurePointForTesting point) {
  std::uint8_t expected = static_cast<std::uint8_t>(point);
  return codec_failure_point.compare_exchange_strong(
      expected, static_cast<std::uint8_t>(CodecFailurePointForTesting::kNone),
      std::memory_order_relaxed);
}
#endif

class Deadline {
 public:
  explicit Deadline(std::int64_t timeout_milliseconds)
      : enabled_(timeout_milliseconds > 0), end_(End(timeout_milliseconds)) {}

  bool expired() const {
    return enabled_ && std::chrono::steady_clock::now() >= end_;
  }

 private:
  static std::chrono::steady_clock::time_point End(
      std::int64_t timeout_milliseconds) {
    const auto now = std::chrono::steady_clock::now();
    return SaturatingDeadline<std::chrono::steady_clock>(
        now, timeout_milliseconds);
  }

  bool enabled_;
  std::chrono::steady_clock::time_point end_;
};

void SetError(Error* error, ErrorCode code, std::string message) {
  if (error != nullptr) {
    error->code = code;
    error->message = std::move(message);
  }
}

bool CheckCommonArguments(const std::uint8_t* input, std::size_t input_size,
                          const Options& options, Error* error) {
  if (input == nullptr || input_size == 0 || options.effort < 1 ||
      options.effort > 9 || options.decoding_speed < 0 ||
      options.decoding_speed > 4 || !IsValidTaskPriority(options.priority) ||
      options.timeout_milliseconds < 0) {
    SetError(error, ErrorCode::kInvalidArguments,
             "Invalid codec input or options");
    return false;
  }
  return true;
}

struct EncoderDeleter {
  void operator()(JxlEncoder* encoder) const { JxlEncoderDestroy(encoder); }
};

struct DecoderDeleter {
  void operator()(JxlDecoder* decoder) const { JxlDecoderDestroy(decoder); }
};

using Encoder = std::unique_ptr<JxlEncoder, EncoderDeleter>;
using Decoder = std::unique_ptr<JxlDecoder, DecoderDeleter>;

struct DeadlineRunner {
  const Deadline* deadline;
  std::atomic<bool> expired{false};
};

bool DeadlineCancelled(void* opaque) {
  auto* runner = static_cast<DeadlineRunner*>(opaque);
  if (runner->expired.load(std::memory_order_relaxed) ||
      runner->deadline->expired()) {
    runner->expired.store(true, std::memory_order_relaxed);
    return true;
  }
  return false;
}

ErrorCode EncodeErrorCode(JxlEncoderError code) {
  switch (code) {
    case JXL_ENC_ERR_JBRD:
    case JXL_ENC_ERR_BAD_INPUT:
    case JXL_ENC_ERR_NOT_SUPPORTED:
      return ErrorCode::kUnsupportedInput;
    default:
      return ErrorCode::kCodec;
  }
}

JxlEncoder* CreateEncoder() {
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
  if (ConsumeCodecFailureForTesting(
          CodecFailurePointForTesting::kEncoderCreate)) {
    return nullptr;
  }
#endif
  return JxlEncoderCreate(nullptr);
}

JxlDecoder* CreateDecoder() {
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
  if (ConsumeCodecFailureForTesting(
          CodecFailurePointForTesting::kDecoderCreate)) {
    return nullptr;
  }
#endif
  return JxlDecoderCreate(nullptr);
}

}  // namespace

#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
void SetOutputAllocationFailureCountdownForTesting(int countdown) {
  output_allocation_failure_countdown.store(countdown,
                                            std::memory_order_relaxed);
}

void SetInitialOutputCapacityForTesting(std::size_t capacity) {
  initial_output_capacity_override.store(capacity, std::memory_order_relaxed);
}

void SetCodecFailurePointForTesting(CodecFailurePointForTesting point) {
  codec_failure_point.store(static_cast<std::uint8_t>(point),
                            std::memory_order_relaxed);
}

ErrorCode ClassifyEncoderErrorForTesting(int encoder_error) {
  return EncodeErrorCode(static_cast<JxlEncoderError>(encoder_error));
}
#endif

OutputBuffer::OutputBuffer(std::size_t maximum_capacity)
    : maximum_capacity_(std::min(maximum_capacity, kSafetyLimit)) {}

bool OutputBuffer::Allocate(std::size_t capacity, Error* error) {
  if (capacity == 0 || capacity > maximum_capacity_) {
    SetError(error, ErrorCode::kCodec,
             "Codec output exceeds the 1 GiB safety limit");
    return false;
  }
  std::unique_ptr<std::uint8_t[]> bytes;
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
  if (!FailOutputAllocationForTesting())
#endif
  {
    bytes.reset(new (std::nothrow) std::uint8_t[capacity]);
  }
  if (!bytes) {
    SetError(error, ErrorCode::kCodec,
             "Not enough memory for the codec output");
    return false;
  }
  bytes_ = std::move(bytes);
  capacity_ = capacity;
  size_ = 0;
  return true;
}

bool OutputBuffer::Grow(std::size_t used, Error* error) {
  if (used > capacity_ || capacity_ >= maximum_capacity_) {
    SetError(error, ErrorCode::kCodec,
             "Codec output exceeds the 1 GiB safety limit");
    return false;
  }
  const std::size_t next_capacity =
      std::min(maximum_capacity_, capacity_ * 2);
  std::unique_ptr<std::uint8_t[]> bytes;
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
  if (!FailOutputAllocationForTesting())
#endif
  {
    bytes.reset(new (std::nothrow) std::uint8_t[next_capacity]);
  }
  if (!bytes) {
    SetError(error, ErrorCode::kCodec,
             "Not enough memory to grow the codec output");
    return false;
  }
  std::memcpy(bytes.get(), bytes_.get(), used);
  bytes_ = std::move(bytes);
  capacity_ = next_capacity;
  return true;
}

bool EncodeJpeg(const std::uint8_t* input, std::size_t input_size,
                const Options& options, OutputBuffer* output, Error* error) {
  if (output == nullptr) {
    SetError(error, ErrorCode::kInvalidArguments,
             "A codec output buffer is required");
    return false;
  }
  if (!CheckCommonArguments(input, input_size, options, error)) {
    return false;
  }
  Deadline deadline(options.timeout_milliseconds);
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
  const bool force_before_start_timeout = ConsumeCodecFailureForTesting(
      CodecFailurePointForTesting::kEncoderBeforeStartTimeout);
#else
  constexpr bool force_before_start_timeout = false;
#endif
  if (force_before_start_timeout || deadline.expired()) {
    SetError(error, ErrorCode::kTimeout, "JPEG XL encoding timed out");
    return false;
  }

  Encoder encoder(CreateEncoder());
  if (!encoder) {
    SetError(error, ErrorCode::kCodec,
             "Failed to create the JPEG XL encoder");
    return false;
  }
  DeadlineRunner deadline_runner{&deadline};
  SharedRunnerContext runner_context{options.priority,
                                     options.scheduling_group,
                                     DeadlineCancelled, &deadline_runner};
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
  const bool fail_encoder_configuration = ConsumeCodecFailureForTesting(
      CodecFailurePointForTesting::kEncoderConfigure);
#else
  constexpr bool fail_encoder_configuration = false;
#endif
  if (fail_encoder_configuration ||
      JxlEncoderSetParallelRunner(encoder.get(), SharedParallelRunner,
                                  &runner_context) != JXL_ENC_SUCCESS ||
      JxlEncoderStoreJPEGMetadata(encoder.get(), JXL_TRUE) != JXL_ENC_SUCCESS) {
    SetError(error, ErrorCode::kCodec,
             "Failed to configure the JPEG XL encoder");
    return false;
  }

#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
  const bool fail_encoder_settings = ConsumeCodecFailureForTesting(
      CodecFailurePointForTesting::kEncoderSettings);
#else
  constexpr bool fail_encoder_settings = false;
#endif
  JxlEncoderFrameSettings* settings = fail_encoder_settings
                                          ? nullptr
                                          : JxlEncoderFrameSettingsCreate(
                                                encoder.get(), nullptr);
  if (settings == nullptr ||
      JxlEncoderSetFrameLossless(settings, JXL_TRUE) != JXL_ENC_SUCCESS ||
      JxlEncoderFrameSettingsSetOption(
          settings, JXL_ENC_FRAME_SETTING_EFFORT, options.effort) !=
          JXL_ENC_SUCCESS ||
      JxlEncoderFrameSettingsSetOption(
          settings, JXL_ENC_FRAME_SETTING_DECODING_SPEED,
          options.decoding_speed) != JXL_ENC_SUCCESS) {
    SetError(error, ErrorCode::kCodec,
             "Failed to configure JPEG XL encoder settings");
    return false;
  }
  if (JxlEncoderAddJPEGFrame(settings, input, input_size) != JXL_ENC_SUCCESS) {
    const JxlEncoderError encoder_error = JxlEncoderGetError(encoder.get());
    SetError(error, EncodeErrorCode(encoder_error),
             "JPEG cannot be losslessly transcoded (libjxl error " +
                 std::to_string(static_cast<int>(encoder_error)) + ")");
    return false;
  }
  JxlEncoderCloseInput(encoder.get());

  std::size_t initial_capacity = std::max(input_size, kInitialOutputBytes);
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
  const std::size_t overridden_capacity =
      initial_output_capacity_override.exchange(0, std::memory_order_relaxed);
  if (overridden_capacity > 0) initial_capacity = overridden_capacity;
#endif
  if (!output->Allocate(initial_capacity, error)) {
    return false;
  }
  std::uint8_t* next_output = output->data();
  std::size_t available_output = output->capacity();
  JxlEncoderStatus status = JXL_ENC_NEED_MORE_OUTPUT;
  while (status == JXL_ENC_NEED_MORE_OUTPUT) {
    if (deadline.expired()) {
      SetError(error, ErrorCode::kTimeout, "JPEG XL encoding timed out");
      return false;
    }
    status =
        JxlEncoderProcessOutput(encoder.get(), &next_output, &available_output);
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
    if (ConsumeCodecFailureForTesting(
            CodecFailurePointForTesting::kEncoderProcess)) {
      status = JXL_ENC_ERROR;
    }
#endif
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
    const bool force_after_process_timeout = ConsumeCodecFailureForTesting(
        CodecFailurePointForTesting::kEncoderAfterProcessTimeout);
#else
    constexpr bool force_after_process_timeout = false;
#endif
    if (force_after_process_timeout || deadline.expired() ||
        deadline_runner.expired.load(std::memory_order_relaxed)) {
      SetError(error, ErrorCode::kTimeout, "JPEG XL encoding timed out");
      return false;
    }
    if (status == JXL_ENC_NEED_MORE_OUTPUT) {
      const std::size_t used =
          static_cast<std::size_t>(next_output - output->data());
      if (!output->Grow(used, error)) return false;
      next_output = output->data() + used;
      available_output = output->capacity() - used;
    }
  }
  if (status != JXL_ENC_SUCCESS) {
    const JxlEncoderError encoder_error = JxlEncoderGetError(encoder.get());
    SetError(error, EncodeErrorCode(encoder_error),
             "JPEG XL encoding failed (libjxl error " +
                 std::to_string(static_cast<int>(encoder_error)) + ")");
    return false;
  }
  output->size_ = static_cast<std::size_t>(next_output - output->data());
  return true;
}

bool ReconstructJpeg(const std::uint8_t* input, std::size_t input_size,
                     const Options& options, OutputBuffer* output,
                     Error* error) {
  if (output == nullptr) {
    SetError(error, ErrorCode::kInvalidArguments,
             "A codec output buffer is required");
    return false;
  }
  if (!CheckCommonArguments(input, input_size, options, error)) {
    return false;
  }
  Deadline deadline(options.timeout_milliseconds);
  Decoder decoder(CreateDecoder());
  if (!decoder) {
    SetError(error, ErrorCode::kCodec,
             "Failed to create the JPEG XL decoder");
    return false;
  }
  DeadlineRunner deadline_runner{&deadline};
  SharedRunnerContext runner_context{options.priority,
                                     options.scheduling_group,
                                     DeadlineCancelled, &deadline_runner};
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
  const bool fail_decoder_configuration = ConsumeCodecFailureForTesting(
      CodecFailurePointForTesting::kDecoderConfigure);
#else
  constexpr bool fail_decoder_configuration = false;
#endif
  if (fail_decoder_configuration ||
      JxlDecoderSetParallelRunner(decoder.get(), SharedParallelRunner,
                                  &runner_context) != JXL_DEC_SUCCESS ||
      JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_JPEG_RECONSTRUCTION |
                                                   JXL_DEC_FULL_IMAGE) !=
          JXL_DEC_SUCCESS ||
      JxlDecoderSetInput(decoder.get(), input, input_size) != JXL_DEC_SUCCESS) {
    SetError(error, ErrorCode::kCodec,
             "Failed to configure JPEG reconstruction");
    return false;
  }
  JxlDecoderCloseInput(decoder.get());
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
  const bool force_after_configure_timeout = ConsumeCodecFailureForTesting(
      CodecFailurePointForTesting::kDecoderAfterConfigureTimeout);
#else
  constexpr bool force_after_configure_timeout = false;
#endif
  if (force_after_configure_timeout || deadline.expired()) {
    SetError(error, ErrorCode::kTimeout, "JPEG reconstruction timed out");
    return false;
  }

  JxlDecoderStatus status = JxlDecoderProcessInput(decoder.get());
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
  const bool force_after_header_timeout = ConsumeCodecFailureForTesting(
      CodecFailurePointForTesting::kDecoderAfterHeaderTimeout);
#else
  constexpr bool force_after_header_timeout = false;
#endif
  if (force_after_header_timeout || deadline.expired() ||
      deadline_runner.expired.load(std::memory_order_relaxed)) {
    SetError(error, ErrorCode::kTimeout, "JPEG reconstruction timed out");
    return false;
  }
  if (status != JXL_DEC_JPEG_RECONSTRUCTION) {
    SetError(error, ErrorCode::kUnsupportedInput,
             "The JXL stream has no reconstructable JPEG payload");
    return false;
  }
  const std::size_t initial_capacity = std::max(
      kInitialOutputBytes,
      input_size > output->maximum_capacity_ / 2
          ? output->maximum_capacity_
          : input_size * 2);
  if (!output->Allocate(initial_capacity, error)) {
    return false;
  }
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
  const bool fail_decoder_buffer = ConsumeCodecFailureForTesting(
      CodecFailurePointForTesting::kDecoderBuffer);
#else
  constexpr bool fail_decoder_buffer = false;
#endif
  if (fail_decoder_buffer ||
      JxlDecoderSetJPEGBuffer(decoder.get(), output->data(),
                              output->capacity()) != JXL_DEC_SUCCESS) {
    SetError(error, ErrorCode::kCodec,
             "Failed to configure the JPEG reconstruction buffer");
    return false;
  }

  std::size_t used = 0;
  while (true) {
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
    const bool force_before_output_timeout = ConsumeCodecFailureForTesting(
        CodecFailurePointForTesting::kDecoderBeforeOutputProcessTimeout);
#else
    constexpr bool force_before_output_timeout = false;
#endif
    if (force_before_output_timeout || deadline.expired()) {
      SetError(error, ErrorCode::kTimeout, "JPEG reconstruction timed out");
      return false;
    }
    status = JxlDecoderProcessInput(decoder.get());
    if (deadline.expired() ||
        deadline_runner.expired.load(std::memory_order_relaxed)) {
      SetError(error, ErrorCode::kTimeout, "JPEG reconstruction timed out");
      return false;
    }
    if (status != JXL_DEC_JPEG_NEED_MORE_OUTPUT) break;
    used = output->capacity() - JxlDecoderReleaseJPEGBuffer(decoder.get());
    if (!output->Grow(used, error)) return false;
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
    const bool fail_decoder_growth = ConsumeCodecFailureForTesting(
        CodecFailurePointForTesting::kDecoderGrowBuffer);
#else
    constexpr bool fail_decoder_growth = false;
#endif
    if (fail_decoder_growth ||
        JxlDecoderSetJPEGBuffer(decoder.get(), output->data() + used,
                                output->capacity() - used) != JXL_DEC_SUCCESS) {
      SetError(error, ErrorCode::kCodec,
               "Failed to grow the JPEG reconstruction buffer");
      return false;
    }
  }
  if (status != JXL_DEC_FULL_IMAGE && status != JXL_DEC_SUCCESS) {
    SetError(error, ErrorCode::kUnsupportedInput,
             "The JXL stream is malformed or truncated");
    return false;
  }
  used = output->capacity() - JxlDecoderReleaseJPEGBuffer(decoder.get());
  output->size_ = used;
  return true;
}

}  // namespace jxl_coder
