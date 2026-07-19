#ifndef JXL_CODER_NATIVE_JXL_CODEC_H_
#define JXL_CODER_NATIVE_JXL_CODEC_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "shared_parallel_runner.h"

namespace jxl_coder {

enum class ErrorCode {
  kInvalidArguments,
  kUnsupportedInput,
  kIo,
  kTimeout,
  kCodec,
};

struct Error {
  ErrorCode code = ErrorCode::kCodec;
  std::string message;
};

struct Options {
  int effort = 7;
  int decoding_speed = 0;
  TaskPriority priority = TaskPriority::kNormal;
  /// Conversions from one batch share a group so they do not multiply that
  /// batch's share within a priority class. Zero is valid for direct callers.
  std::uint64_t scheduling_group = 0;
  std::int64_t timeout_milliseconds = 0;
};

/// Move-only codec output whose capacity is allocated without zero-filling.
class OutputBuffer {
 public:
  static constexpr std::size_t kSafetyLimit = std::size_t{1} << 30;

  explicit OutputBuffer(std::size_t maximum_capacity = kSafetyLimit);
  ~OutputBuffer() = default;
  OutputBuffer(OutputBuffer&&) noexcept = default;
  OutputBuffer& operator=(OutputBuffer&&) noexcept = default;

  OutputBuffer(const OutputBuffer&) = delete;
  OutputBuffer& operator=(const OutputBuffer&) = delete;

  std::uint8_t* data() { return bytes_.get(); }
  const std::uint8_t* data() const { return bytes_.get(); }
  std::size_t size() const { return size_; }
  std::size_t capacity() const { return capacity_; }

 private:
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
  friend struct OutputBufferTestAccess;
#endif
  friend bool EncodeJpeg(const std::uint8_t*, std::size_t, const Options&,
                         OutputBuffer*, Error*);
  friend bool ReconstructJpeg(const std::uint8_t*, std::size_t,
                              const Options&, OutputBuffer*, Error*);

  bool Allocate(std::size_t capacity, Error* error);
  bool Grow(std::size_t used, Error* error);

  std::unique_ptr<std::uint8_t[]> bytes_;
  std::size_t size_ = 0;
  std::size_t capacity_ = 0;
  std::size_t maximum_capacity_ = kSafetyLimit;
};

bool EncodeJpeg(const std::uint8_t* input, std::size_t input_size,
                const Options& options, OutputBuffer* output, Error* error);

bool ReconstructJpeg(const std::uint8_t* input, std::size_t input_size,
                     const Options& options, OutputBuffer* output,
                     Error* error);

#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
enum class CodecFailurePointForTesting : std::uint8_t {
  kNone,
  kEncoderCreate,
  kEncoderConfigure,
  kEncoderSettings,
  kEncoderProcess,
  kEncoderBeforeStartTimeout,
  kEncoderAfterProcessTimeout,
  kDecoderCreate,
  kDecoderConfigure,
  kDecoderAfterConfigureTimeout,
  kDecoderAfterHeaderTimeout,
  kDecoderBuffer,
  kDecoderGrowBuffer,
  kDecoderBeforeOutputProcessTimeout,
};

/// Fails one output allocation after the requested number of successful
/// allocation attempts. A negative value disables the one-shot failure.
void SetOutputAllocationFailureCountdownForTesting(int countdown);
/// Overrides the next encoder's initial output capacity. Zero disables the
/// one-shot override.
void SetInitialOutputCapacityForTesting(std::size_t capacity);
/// Injects one libjxl failure at the selected wrapper boundary.
void SetCodecFailurePointForTesting(CodecFailurePointForTesting point);
/// Exposes the libjxl encoder-error classification for complete mapping tests.
ErrorCode ClassifyEncoderErrorForTesting(int encoder_error);
#endif

}  // namespace jxl_coder

#endif  // JXL_CODER_NATIVE_JXL_CODEC_H_
