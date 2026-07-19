#ifndef JXL_CODER_NATIVE_DEADLINE_UTILS_H_
#define JXL_CODER_NATIVE_DEADLINE_UTILS_H_

#include <chrono>
#include <cstdint>

namespace jxl_coder {

/// Returns an effectively unbounded deadline when the timeout is disabled or
/// cannot be represented by the clock without overflowing its time point.
template <typename Clock>
typename Clock::time_point SaturatingDeadline(
    typename Clock::time_point start, std::int64_t timeout_milliseconds) {
  const auto maximum = Clock::time_point::max();
  if (timeout_milliseconds <= 0) return maximum;

  const auto remaining_milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(maximum - start)
          .count();
  if (timeout_milliseconds >= remaining_milliseconds) return maximum;

  return start + std::chrono::duration_cast<typename Clock::duration>(
                     std::chrono::milliseconds(timeout_milliseconds));
}

}  // namespace jxl_coder

#endif  // JXL_CODER_NATIVE_DEADLINE_UTILS_H_
