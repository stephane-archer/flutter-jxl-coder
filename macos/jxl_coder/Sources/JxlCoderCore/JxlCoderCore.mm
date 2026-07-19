#import "JxlCoderCore.h"

#include "../../../../native/src/shared_parallel_runner.cc"
#include "../../../../native/src/jxl_codec.cc"

#include <atomic>
#include <memory>
#include <new>
#include <stdexcept>

namespace {

NSInteger NativeErrorCode(jxl_coder::ErrorCode code) {
  switch (code) {
    case jxl_coder::ErrorCode::kInvalidArguments:
      return JxlCoderErrorInvalidArguments;
    case jxl_coder::ErrorCode::kUnsupportedInput:
      return JxlCoderErrorUnsupportedInput;
    case jxl_coder::ErrorCode::kIo:
      return JxlCoderErrorIO;
    case jxl_coder::ErrorCode::kTimeout:
      return JxlCoderErrorTimeout;
    case jxl_coder::ErrorCode::kCodec:
      return JxlCoderErrorCodec;
  }
}

void SetError(NSError* _Nullable* _Nullable error,
              const jxl_coder::Error& native_error) {
  if (error == nullptr) return;
  *error = [NSError
      errorWithDomain:JxlCoderErrorDomain
                 code:NativeErrorCode(native_error.code)
             userInfo:@{
               NSLocalizedDescriptionKey :
                   [NSString stringWithUTF8String:native_error.message.c_str()]
             }];
}

void SetBridgeError(NSError* _Nullable* _Nullable error,
                    NSString* _Nonnull message) {
  if (error == nullptr) return;
  *error = [NSError errorWithDomain:JxlCoderErrorDomain
                               code:JxlCoderErrorCodec
                           userInfo:@{NSLocalizedDescriptionKey : message}];
}

#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
std::atomic<bool> fail_data_owner_allocation{false};
std::atomic<bool> throw_from_bridge{false};
std::atomic<int> live_data_owners{0};

void SetBridgeOwnerAllocationFailureForTesting(bool fail) {
  fail_data_owner_allocation.store(fail, std::memory_order_relaxed);
}

void SetBridgeExceptionForTesting(bool should_throw) {
  throw_from_bridge.store(should_throw, std::memory_order_relaxed);
}

int LiveDataOwnersForTesting() {
  return live_data_owners.load(std::memory_order_relaxed);
}
#endif

class DataOwner {
 public:
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
  DataOwner() { live_data_owners.fetch_add(1, std::memory_order_relaxed); }
  ~DataOwner() { live_data_owners.fetch_sub(1, std::memory_order_relaxed); }
#endif
  jxl_coder::OutputBuffer bytes;
};

DataOwner* _Nullable CreateDataOwner() {
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
  if (fail_data_owner_allocation.exchange(false, std::memory_order_relaxed)) {
    return nullptr;
  }
#endif
  return new (std::nothrow) DataOwner();
}

void MaybeThrowFromBridgeForTesting() {
#if defined(JXL_CODER_ENABLE_TEST_HOOKS)
  if (throw_from_bridge.exchange(false, std::memory_order_relaxed)) {
    throw std::runtime_error("injected Objective-C++ bridge failure");
  }
#endif
}

NSData* _Nonnull WrapOutput(DataOwner* _Nonnull owner) {
  return [[NSData alloc]
      initWithBytesNoCopy:owner->bytes.data()
                  length:owner->bytes.size()
             deallocator:^(void*, NSUInteger) {
               delete owner;
             }];
}

}  // namespace

NSString* _Nonnull const JxlCoderErrorDomain = @"JxlCoder";

@implementation JxlCoderCore

+ (BOOL)configureWorkerCount:(NSInteger)workerCount
        effectiveWorkerCount:(NSInteger* _Nullable)effectiveWorkerCount
                       error:(NSError* _Nullable* _Nullable)error {
  std::size_t effective = 0;
  const auto result = jxl_coder::ConfigureSharedParallelScheduler(
      static_cast<std::size_t>(workerCount), &effective);
  if (effectiveWorkerCount != nullptr) {
    *effectiveWorkerCount = static_cast<NSInteger>(effective);
  }
  if (result == jxl_coder::SchedulerConfigurationResult::kSuccess) return YES;
  if (error != nullptr) {
    const NSInteger code =
        result == jxl_coder::SchedulerConfigurationResult::kAlreadyStarted
            ? JxlCoderErrorSchedulerAlreadyStarted
            : JxlCoderErrorInvalidArguments;
    *error = [NSError
        errorWithDomain:JxlCoderErrorDomain
                   code:code
               userInfo:@{
                 NSLocalizedDescriptionKey :
                     code == JxlCoderErrorSchedulerAlreadyStarted
                         ? @"The JPEG XL scheduler has already started"
                         : @"Invalid JPEG XL scheduler configuration"
               }];
  }
  return NO;
}

+ (nullable NSData*)transcode:(NSData* _Nonnull)data
                       effort:(NSInteger)effort
                decodingSpeed:(NSInteger)decodingSpeed
                     priority:(NSInteger)priority
              schedulingGroup:(uint64_t)schedulingGroup
          timeoutMilliseconds:(NSInteger)timeoutMilliseconds
                        error:(NSError* _Nullable* _Nullable)error {
  try {
    std::unique_ptr<DataOwner> owner(CreateDataOwner());
    if (!owner) {
      SetBridgeError(error, @"Not enough memory for the codec result");
      return nil;
    }
    MaybeThrowFromBridgeForTesting();
    const jxl_coder::Options options{
        static_cast<int>(effort), static_cast<int>(decodingSpeed),
        static_cast<jxl_coder::TaskPriority>(priority), schedulingGroup,
        static_cast<std::int64_t>(timeoutMilliseconds)};
    jxl_coder::Error native_error;
    if (!jxl_coder::EncodeJpeg(
            static_cast<const std::uint8_t*>(data.bytes), data.length, options,
            &owner->bytes, &native_error)) {
      SetError(error, native_error);
      return nil;
    }
    return WrapOutput(owner.release());
  } catch (...) {
    SetBridgeError(error, @"Unexpected native JPEG XL encoding failure");
    return nil;
  }
}

+ (nullable NSData*)inverse:(NSData* _Nonnull)data
                    priority:(NSInteger)priority
             schedulingGroup:(uint64_t)schedulingGroup
         timeoutMilliseconds:(NSInteger)timeoutMilliseconds
                       error:(NSError* _Nullable* _Nullable)error {
  try {
    std::unique_ptr<DataOwner> owner(CreateDataOwner());
    if (!owner) {
      SetBridgeError(error, @"Not enough memory for the codec result");
      return nil;
    }
    MaybeThrowFromBridgeForTesting();
    const jxl_coder::Options options{
        7, 0, static_cast<jxl_coder::TaskPriority>(priority), schedulingGroup,
        static_cast<std::int64_t>(timeoutMilliseconds)};
    jxl_coder::Error native_error;
    if (!jxl_coder::ReconstructJpeg(
            static_cast<const std::uint8_t*>(data.bytes), data.length, options,
            &owner->bytes, &native_error)) {
      SetError(error, native_error);
      return nil;
    }
    return WrapOutput(owner.release());
  } catch (...) {
    SetBridgeError(error, @"Unexpected native JPEG reconstruction failure");
    return nil;
  }
}

@end
