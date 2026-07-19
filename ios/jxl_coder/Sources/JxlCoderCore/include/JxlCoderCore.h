#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

FOUNDATION_EXPORT NSString *const JxlCoderErrorDomain;

typedef NS_ENUM(NSInteger, JxlCoderError) {
  JxlCoderErrorInvalidArguments = 400,
  JxlCoderErrorUnsupportedInput = 415,
  JxlCoderErrorTimeout = 408,
  JxlCoderErrorIO = 500,
  JxlCoderErrorCodec = 501,
  JxlCoderErrorSchedulerAlreadyStarted = 409,
  JxlCoderErrorSchedulerUnavailable = 502,
};

@interface JxlCoderCore : NSObject

+ (BOOL)configureWorkerCount:(NSInteger)workerCount
        effectiveWorkerCount:(NSInteger *_Nullable)effectiveWorkerCount
                       error:(NSError *_Nullable *_Nullable)error;

+ (nullable NSData *)transcode:(NSData *)data
                        effort:(NSInteger)effort
                 decodingSpeed:(NSInteger)decodingSpeed
                      priority:(NSInteger)priority
               schedulingGroup:(uint64_t)schedulingGroup
           timeoutMilliseconds:(NSInteger)timeoutMilliseconds
                         error:(NSError *_Nullable *_Nullable)error;

+ (nullable NSData *)inverse:(NSData *)data
                      priority:(NSInteger)priority
               schedulingGroup:(uint64_t)schedulingGroup
           timeoutMilliseconds:(NSInteger)timeoutMilliseconds
                         error:(NSError *_Nullable *_Nullable)error;

@end

NS_ASSUME_NONNULL_END
