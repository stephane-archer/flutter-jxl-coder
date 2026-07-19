#define JXL_CODER_ENABLE_TEST_HOOKS 1

#import <Foundation/Foundation.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#import "../../macos/jxl_coder/Sources/JxlCoderCore/JxlCoderCore.mm"

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
  if (condition) return;
  std::cerr << "FAIL: " << message << '\n';
  ++failures;
}

NSData* ReadData(const char* path) {
  return [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]];
}

bool DataEquals(NSData* lhs, NSData* rhs) {
  return lhs != nil && rhs != nil && [lhs isEqualToData:rhs];
}

void ExpectError(NSError* error, NSInteger code, const std::string& message) {
  Expect(error != nil, message + " must return an NSError");
  if (error == nil) return;
  Expect([error.domain isEqualToString:JxlCoderErrorDomain],
         message + " must use the bridge error domain");
  Expect(error.code == code, message + " must use the expected error code");
  Expect(error.localizedDescription.length > 0,
         message + " must include a useful error message");
}

void TestNativeErrorMapping() {
  struct Mapping {
    jxl_coder::ErrorCode native;
    NSInteger bridge;
  };
  const Mapping mappings[] = {
      {jxl_coder::ErrorCode::kInvalidArguments,
       JxlCoderErrorInvalidArguments},
      {jxl_coder::ErrorCode::kUnsupportedInput,
       JxlCoderErrorUnsupportedInput},
      {jxl_coder::ErrorCode::kIo, JxlCoderErrorIO},
      {jxl_coder::ErrorCode::kTimeout, JxlCoderErrorTimeout},
      {jxl_coder::ErrorCode::kCodec, JxlCoderErrorCodec},
  };
  for (const auto& mapping : mappings) {
    Expect(NativeErrorCode(mapping.native) == mapping.bridge,
           "every shared native error must have a stable Apple error code");
    NSError* error = nil;
    SetError(&error, {mapping.native, "mapped native failure"});
    ExpectError(error, mapping.bridge, "shared native error mapping");
  }
  SetError(nullptr,
           {jxl_coder::ErrorCode::kIo, "ignored error without an output"});
}

void TestConfiguration() {
  NSInteger effective = 0;
  NSError* error = nil;
  Expect([JxlCoderCore configureWorkerCount:2
                       effectiveWorkerCount:&effective
                                      error:&error],
         "the bridge must configure the shared scheduler");
  Expect(error == nil && effective == 2,
         "successful scheduler configuration must report two workers");

  effective = 0;
  Expect([JxlCoderCore configureWorkerCount:2
                       effectiveWorkerCount:&effective
                                      error:&error],
         "identical scheduler configuration must be idempotent");
  Expect(error == nil && effective == 2,
         "identical configuration must preserve its effective worker count");

  effective = 0;
  error = nil;
  Expect(![JxlCoderCore configureWorkerCount:1
                        effectiveWorkerCount:&effective
                                       error:&error],
         "conflicting scheduler configuration must fail");
  Expect(effective == 2,
         "conflicting configuration must return the installed worker count");
  ExpectError(error, JxlCoderErrorSchedulerAlreadyStarted,
              "conflicting scheduler configuration");

  error = nil;
  Expect(![JxlCoderCore configureWorkerCount:257
                        effectiveWorkerCount:nullptr
                                       error:&error],
         "scheduler configuration above the public limit must fail");
  ExpectError(error, JxlCoderErrorInvalidArguments,
              "invalid scheduler configuration");
  effective = -1;
  error = nil;
  Expect(![JxlCoderCore configureWorkerCount:-1
                        effectiveWorkerCount:&effective
                                       error:&error],
         "negative scheduler configuration must fail before native startup");
  Expect(effective == 0,
         "negative scheduler configuration must report no effective workers");
  ExpectError(error, JxlCoderErrorInvalidArguments,
              "negative scheduler configuration");
  Expect(![JxlCoderCore configureWorkerCount:257
                        effectiveWorkerCount:nullptr
                                       error:nullptr],
         "invalid scheduler configuration must allow a null NSError pointer");

  Expect([JxlCoderCore configureWorkerCount:2
                       effectiveWorkerCount:nullptr
                                      error:nullptr],
         "configuration must allow null output and error pointers");
}

NSData* Transcode(NSData* jpeg, NSError** error) {
  return [JxlCoderCore transcode:jpeg
                          effort:7
                   decodingSpeed:0
                        priority:1
                 schedulingGroup:11
             timeoutMilliseconds:0
                           error:error];
}

NSData* Inverse(NSData* jxl, NSError** error) {
  return [JxlCoderCore inverse:jxl
                      priority:1
               schedulingGroup:12
           timeoutMilliseconds:0
                         error:error];
}

void TestRoundTripAndOwnership(NSData* jpeg) {
  Expect(LiveDataOwnersForTesting() == 0,
         "the bridge must start without live output owners");
  @autoreleasepool {
    NSError* error = nil;
    NSData* encoded = Transcode(jpeg, &error);
    Expect(encoded != nil && error == nil,
           "valid JPEG input must transcode through the bridge");
    Expect(LiveDataOwnersForTesting() == 1,
           "the no-copy encoded NSData must retain one native owner");

    NSData* reconstructed = Inverse(encoded, &error);
    Expect(reconstructed != nil && error == nil,
           "valid JXL input must reconstruct through the bridge");
    Expect(DataEquals(reconstructed, jpeg),
           "the bridge round trip must preserve the exact JPEG bytes");
    Expect(LiveDataOwnersForTesting() == 2,
           "both no-copy results must retain their native owners");
  }
  Expect(LiveDataOwnersForTesting() == 0,
         "autorelease must destroy every no-copy native owner exactly once");
}

void TestExistingJxl(NSData* jxl) {
  @autoreleasepool {
    NSError* error = nil;
    NSData* jpeg = Inverse(jxl, &error);
    Expect(jpeg != nil && jpeg.length > 0 && error == nil,
           "the bridge must reconstruct the packaged JXL fixture");
  }
  Expect(LiveDataOwnersForTesting() == 0,
         "existing JXL reconstruction must release its native owner");
}

void TestBridgeFailures(NSData* jpeg, NSData* jxl) {
  NSError* error = nil;
  SetBridgeOwnerAllocationFailureForTesting(true);
  Expect(Transcode(jpeg, &error) == nil,
         "native owner allocation failure must return nil");
  ExpectError(error, JxlCoderErrorCodec, "native owner allocation failure");
  Expect(LiveDataOwnersForTesting() == 0,
         "owner allocation failure must not leak an owner");

  error = nil;
  SetBridgeExceptionForTesting(true);
  Expect(Transcode(jpeg, &error) == nil,
         "an unexpected C++ exception must be contained by the bridge");
  ExpectError(error, JxlCoderErrorCodec, "unexpected bridge exception");
  Expect(LiveDataOwnersForTesting() == 0,
         "unexpected bridge exceptions must release the allocated owner");

  error = nil;
  SetBridgeOwnerAllocationFailureForTesting(true);
  Expect(Inverse(jxl, &error) == nil,
         "inverse owner allocation failure must return nil");
  ExpectError(error, JxlCoderErrorCodec,
              "inverse native owner allocation failure");
  Expect(LiveDataOwnersForTesting() == 0,
         "inverse owner allocation failure must not leak an owner");

  error = nil;
  SetBridgeExceptionForTesting(true);
  Expect(Inverse(jxl, &error) == nil,
         "an inverse C++ exception must be contained by the bridge");
  ExpectError(error, JxlCoderErrorCodec, "unexpected inverse exception");
  Expect(LiveDataOwnersForTesting() == 0,
         "inverse bridge exceptions must release the allocated owner");

  error = nil;
  jxl_coder::SetOutputAllocationFailureCountdownForTesting(0);
  Expect(Transcode(jpeg, &error) == nil,
         "codec output allocation failure must cross the bridge as nil");
  ExpectError(error, JxlCoderErrorCodec, "codec allocation failure");
  Expect(LiveDataOwnersForTesting() == 0,
         "codec allocation failure must release the bridge owner");

  error = nil;
  jxl_coder::SetCodecFailurePointForTesting(
      jxl_coder::CodecFailurePointForTesting::kEncoderSettings);
  Expect(Transcode(jpeg, &error) == nil,
         "encoder settings failure must cross the bridge as nil");
  ExpectError(error, JxlCoderErrorCodec, "encoder settings failure");
  Expect(LiveDataOwnersForTesting() == 0,
         "encoder settings failure must release the bridge owner");

  SetBridgeOwnerAllocationFailureForTesting(true);
  Expect(Transcode(jpeg, nullptr) == nil,
         "bridge failures must be safe when the caller omits NSError");
  Expect(LiveDataOwnersForTesting() == 0,
         "a null NSError pointer must not change ownership cleanup");
}

void TestInvalidInput() {
  NSData* empty = [NSData data];
  NSError* error = nil;
  Expect(Transcode(empty, &error) == nil,
         "empty JPEG input must be rejected by the bridge");
  ExpectError(error, JxlCoderErrorInvalidArguments, "empty JPEG input");

  error = nil;
  Expect(Inverse(empty, &error) == nil,
         "empty JXL input must be rejected by the bridge");
  ExpectError(error, JxlCoderErrorInvalidArguments, "empty JXL input");

  const std::uint8_t malformed_bytes[] = {0, 1, 2, 3, 4, 5};
  NSData* malformed = [NSData dataWithBytes:malformed_bytes
                                     length:sizeof(malformed_bytes)];
  error = nil;
  Expect(Transcode(malformed, &error) == nil,
         "malformed JPEG input must be rejected by the bridge");
  ExpectError(error, JxlCoderErrorUnsupportedInput, "malformed JPEG input");
  Expect(Transcode(malformed, nullptr) == nil,
         "native codec errors must be safe when the caller omits NSError");
  Expect(LiveDataOwnersForTesting() == 0,
         "invalid input failures must release every bridge owner");
}

}  // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    if (argc != 3) {
      std::cerr << "usage: jxl_coder_core_test <jpeg> <jxl>\n";
      return 2;
    }
    NSData* jpeg = ReadData(argv[1]);
    NSData* jxl = ReadData(argv[2]);
    if (jpeg == nil || jpeg.length == 0 || jxl == nil || jxl.length == 0) {
      std::cerr << "fixtures could not be read\n";
      return 2;
    }

    TestConfiguration();
    TestNativeErrorMapping();
    TestRoundTripAndOwnership(jpeg);
    TestExistingJxl(jxl);
    TestBridgeFailures(jpeg, jxl);
    TestInvalidInput();
  }

  if (failures != 0) {
    std::cerr << failures << " Apple bridge test(s) failed\n";
    return 1;
  }
  std::cout << "Apple Objective-C++ bridge tests passed\n";
  return 0;
}
