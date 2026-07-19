import Foundation
import Flutter
import JxlCoderCore
import XCTest

@testable import jxl_coder

class RunnerTests: XCTestCase {
  private let plugin = JxlCoderPlugin()

  @discardableResult
  private func invoke(
    _ method: String,
    arguments: Any? = nil,
    using targetPlugin: JxlCoderPlugin? = nil,
    file: StaticString = #filePath,
    line: UInt = #line
  ) -> Any? {
    var callbackCount = 0
    var callbackValue: Any?
    let call = FlutterMethodCall(methodName: method, arguments: arguments)
    let callback = expectation(description: "\(method) result")

    (targetPlugin ?? plugin).handle(call) { value in
      callbackCount += 1
      callbackValue = value
      callback.fulfill()
    }

    wait(for: [callback], timeout: 60)
    XCTAssertEqual(callbackCount, 1, "Result callback count", file: file, line: line)
    return callbackValue
  }

  private func assertFlutterError(
    _ value: Any?,
    code: String,
    expectsDetails: Bool = false,
    file: StaticString = #filePath,
    line: UInt = #line
  ) {
    guard let error = value as? FlutterError else {
      XCTFail("Expected FlutterError, got \(String(describing: value))", file: file, line: line)
      return
    }
    XCTAssertEqual(error.code, code, file: file, line: line)
    XCTAssertFalse(error.message?.isEmpty ?? true, file: file, line: line)
    if expectsDetails {
      XCTAssertFalse((error.details as? String)?.isEmpty ?? true, file: file, line: line)
    } else {
      XCTAssertNil(error.details, file: file, line: line)
    }
  }

  private func fixtureData(_ name: String) throws -> Data {
    let frameworksURL = try XCTUnwrap(Bundle.main.privateFrameworksURL)
    let appBundle = try XCTUnwrap(
      Bundle(url: frameworksURL.appendingPathComponent("App.framework"))
    )
    let resourcesURL = try XCTUnwrap(appBundle.resourceURL)
    return try Data(
      contentsOf: resourcesURL
        .appendingPathComponent("flutter_assets")
        .appendingPathComponent("integration_test")
        .appendingPathComponent(name)
    )
  }

  private func temporaryDirectory() throws -> URL {
    let directory = FileManager.default.temporaryDirectory
      .appendingPathComponent("jxl_coder_xctest_\(UUID().uuidString)")
    try FileManager.default.createDirectory(
      at: directory,
      withIntermediateDirectories: true
    )
    return directory
  }

  private func configuredScheduler(
    maximum: Int
  ) throws -> ConversionScheduler {
    let scheduler = ConversionScheduler()
    try scheduler.configure(workerCount: 0, maxActiveConversions: maximum)
    return scheduler
  }

  func testAdmissionSchedulerConfigurationIsPerAdapterAndImmutable() throws {
    let scheduler = try configuredScheduler(maximum: 1)
    try scheduler.configure(workerCount: 0, maxActiveConversions: 1)

    XCTAssertThrowsError(
      try scheduler.configure(workerCount: 0, maxActiveConversions: 2)
    ) { error in
      let native = error as NSError
      XCTAssertEqual(native.domain, "JxlCoder")
      XCTAssertEqual(native.code, 409)
    }
  }

  func testAdmissionSchedulerConfigurationRaceHasExactlyOneWinner() {
    let scheduler = ConversionScheduler { requested in requested }
    let start = DispatchSemaphore(value: 0)
    let contenders = DispatchGroup()
    let lock = NSLock()
    var outcomes: [Int] = []

    for workerCount in [1, 2] {
      contenders.enter()
      DispatchQueue.global().async {
        start.wait()
        let outcome: Int
        do {
          try scheduler.configure(
            workerCount: workerCount,
            maxActiveConversions: workerCount
          )
          outcome = 0
        } catch {
          outcome = (error as NSError).code
        }
        lock.lock()
        outcomes.append(outcome)
        lock.unlock()
        contenders.leave()
      }
    }

    start.signal()
    start.signal()
    XCTAssertEqual(contenders.wait(timeout: .now() + 5), .success)
    lock.lock()
    let finalOutcomes = outcomes.sorted()
    lock.unlock()
    XCTAssertEqual(finalOutcomes, [0, 409])

    let completed = expectation(description: "work after configuration race")
    scheduler.submit(priority: 1, group: 1, onFailure: { error in
      XCTFail("Winning scheduler failed: \(error)")
      completed.fulfill()
    }) { _ in
      completed.fulfill()
    }
    wait(for: [completed], timeout: 5)
    scheduler.synchronizeForTesting()
  }

  func testAdmissionSchedulerConfigurationAndFirstUseRaceIsFirstWins() {
    let scheduler = ConversionScheduler { requested in
      requested == 0 ? 1 : requested
    }
    let start = DispatchSemaphore(value: 0)
    let contenders = DispatchGroup()
    let lock = NSLock()
    var configurationOutcome = -1
    let completed = expectation(description: "racing conversion completed")

    contenders.enter()
    DispatchQueue.global().async {
      start.wait()
      do {
        try scheduler.configure(workerCount: 2, maxActiveConversions: 3)
        lock.lock()
        configurationOutcome = 0
        lock.unlock()
      } catch {
        lock.lock()
        configurationOutcome = (error as NSError).code
        lock.unlock()
      }
      contenders.leave()
    }

    contenders.enter()
    DispatchQueue.global().async {
      start.wait()
      scheduler.submit(priority: 1, group: 1, onFailure: { error in
        XCTFail("Racing conversion failed: \(error)")
        completed.fulfill()
      }) { _ in
        completed.fulfill()
      }
      contenders.leave()
    }

    start.signal()
    start.signal()
    XCTAssertEqual(contenders.wait(timeout: .now() + 5), .success)
    wait(for: [completed], timeout: 5)
    scheduler.synchronizeForTesting()

    lock.lock()
    let finalOutcome = configurationOutcome
    lock.unlock()
    XCTAssertTrue(finalOutcome == 0 || finalOutcome == 409)
    if finalOutcome == 0 {
      XCTAssertNoThrow(
        try scheduler.configure(workerCount: 2, maxActiveConversions: 3)
      )
      XCTAssertThrowsError(
        try scheduler.configure(workerCount: 0, maxActiveConversions: 0)
      )
    } else {
      XCTAssertNoThrow(
        try scheduler.configure(workerCount: 0, maxActiveConversions: 0)
      )
      XCTAssertThrowsError(
        try scheduler.configure(workerCount: 2, maxActiveConversions: 3)
      )
    }
  }

  func testAdmissionSchedulerRejectsInvalidInternalInputsBeforeStartup() throws {
    var nativeRequests: [Int] = []
    let scheduler = ConversionScheduler { requested in
      nativeRequests.append(requested)
      return requested == 0 ? 2 : requested
    }

    for limits in [(-1, 1), (257, 1), (1, -1), (1, 257)] {
      XCTAssertThrowsError(
        try scheduler.configure(
          workerCount: limits.0,
          maxActiveConversions: limits.1
        )
      ) { error in
        let native = error as NSError
        XCTAssertEqual(native.domain, "JxlCoder")
        XCTAssertEqual(native.code, 400)
      }
    }
    XCTAssertTrue(nativeRequests.isEmpty)

    let rejected = expectation(description: "invalid priorities rejected")
    rejected.expectedFulfillmentCount = 2
    let lock = NSLock()
    var operationRan = false
    for priority in [-1, 3] {
      scheduler.submit(
        priority: priority,
        group: UInt64(priority + 2),
        onFailure: { error in
          let native = error as NSError
          XCTAssertEqual(native.domain, "JxlCoder")
          XCTAssertEqual(native.code, 400)
          rejected.fulfill()
        }
      ) { _ in
        lock.lock()
        operationRan = true
        lock.unlock()
      }
    }

    wait(for: [rejected], timeout: 5)
    scheduler.synchronizeForTesting()
    lock.lock()
    XCTAssertFalse(operationRan)
    lock.unlock()
    XCTAssertTrue(nativeRequests.isEmpty)

    try scheduler.configure(workerCount: 2, maxActiveConversions: 1)
    XCTAssertEqual(nativeRequests, [2])

    let completed = expectation(description: "valid work follows rejection")
    scheduler.submit(priority: 1, group: 10, onFailure: { error in
      XCTFail("Valid work failed: \(error)")
      completed.fulfill()
    }) { _ in
      completed.fulfill()
    }
    wait(for: [completed], timeout: 5)
    scheduler.synchronizeForTesting()
  }

  func testAdmissionSchedulerRejectsInvalidNativeCapacityAndRecovers() throws {
    var nativeCapacities = [0, 257, 2]
    let scheduler = ConversionScheduler { _ in
      nativeCapacities.removeFirst()
    }

    for _ in 0..<2 {
      XCTAssertThrowsError(
        try scheduler.configure(workerCount: 0, maxActiveConversions: 0)
      ) { error in
        let native = error as NSError
        XCTAssertEqual(native.domain, "JxlCoder")
        XCTAssertEqual(native.code, 502)
      }
    }
    try scheduler.configure(workerCount: 0, maxActiveConversions: 0)
    XCTAssertTrue(nativeCapacities.isEmpty)
    try scheduler.configure(workerCount: 0, maxActiveConversions: 0)
    XCTAssertTrue(
      nativeCapacities.isEmpty,
      "identical automatic configuration must use installed capacity"
    )
    XCTAssertNoThrow(
      try scheduler.configure(workerCount: 0, maxActiveConversions: 2),
      "an automatic request with the installed explicit limit is equivalent"
    )
    XCTAssertNoThrow(
      try scheduler.configure(workerCount: 2, maxActiveConversions: 2),
      "an explicitly equivalent configuration must also remain idempotent"
    )

    let completed = expectation(description: "work after capacity recovery")
    scheduler.submit(priority: 1, group: 1, onFailure: { error in
      XCTFail("Recovered scheduler failed: \(error)")
      completed.fulfill()
    }) { _ in
      completed.fulfill()
    }
    wait(for: [completed], timeout: 5)
    scheduler.synchronizeForTesting()
  }

  func testAdmissionSchedulerLazyStartupRecoversFromInvalidNativeCapacity() {
    var attempts = 0
    let scheduler = ConversionScheduler { _ in
      attempts += 1
      return attempts == 1 ? 0 : 2
    }

    let failed = expectation(description: "invalid lazy capacity rejected")
    scheduler.submit(priority: 1, group: 1, onFailure: { error in
      XCTAssertEqual((error as NSError).code, 502)
      failed.fulfill()
    }) { _ in
      XCTFail("Invalid native capacity must not run work")
    }
    wait(for: [failed], timeout: 5)
    scheduler.synchronizeForTesting()

    let completed = expectation(description: "lazy startup recovered")
    scheduler.submit(priority: 1, group: 2, onFailure: { error in
      XCTFail("Recovered lazy scheduler failed: \(error)")
      completed.fulfill()
    }) { _ in
      completed.fulfill()
    }
    wait(for: [completed], timeout: 5)
    scheduler.synchronizeForTesting()
    XCTAssertNoThrow(
      try scheduler.configure(workerCount: 0, maxActiveConversions: 0)
    )
    XCTAssertEqual(attempts, 2)
  }

  func testAdmissionSchedulerEnforcesActiveConversionLimit() throws {
    let scheduler = try configuredScheduler(maximum: 2)
    let firstTwoStarted = expectation(description: "first two jobs admitted")
    firstTwoStarted.expectedFulfillmentCount = 2
    let allFinished = expectation(description: "all jobs finished")
    allFinished.expectedFulfillmentCount = 6
    let release = DispatchSemaphore(value: 0)
    let lock = NSLock()
    var active = 0
    var maximumActive = 0
    var started = 0

    for group in 1...6 {
      scheduler.submit(
        priority: 1,
        group: UInt64(group),
        onFailure: { _ in allFinished.fulfill() }
      ) { _ in
        lock.lock()
        active += 1
        started += 1
        maximumActive = max(maximumActive, active)
        let isFirstTwo = started <= 2
        lock.unlock()
        if isFirstTwo { firstTwoStarted.fulfill() }
        release.wait()
        lock.lock()
        active -= 1
        lock.unlock()
        allFinished.fulfill()
      }
    }
    scheduler.synchronizeForTesting()
    wait(for: [firstTwoStarted], timeout: 5)
    lock.lock()
    XCTAssertEqual(active, 2)
    XCTAssertEqual(maximumActive, 2)
    lock.unlock()
    for _ in 0..<6 { release.signal() }
    wait(for: [allFinished], timeout: 5)
    scheduler.synchronizeForTesting()
    lock.lock()
    XCTAssertEqual(maximumActive, 2)
    lock.unlock()
  }

  func testAdmissionSchedulerUsesWeightedPriorityCycle() throws {
    let scheduler = try configuredScheduler(maximum: 1)
    let blockerStarted = expectation(description: "blocker admitted")
    let release = DispatchSemaphore(value: 0)
    scheduler.submit(priority: 0, group: 99, onFailure: { _ in }) { _ in
      blockerStarted.fulfill()
      release.wait()
    }
    wait(for: [blockerStarted], timeout: 5)

    let completed = expectation(description: "weighted jobs completed")
    completed.expectedFulfillmentCount = 7
    let lock = NSLock()
    var sequence: [Int] = []
    for (priority, count) in [(2, 4), (1, 2), (0, 1)] {
      for index in 0..<count {
        scheduler.submit(
          priority: priority,
          group: UInt64(priority * 10 + index),
          onFailure: { _ in completed.fulfill() }
        ) { _ in
          lock.lock()
          sequence.append(priority)
          lock.unlock()
          completed.fulfill()
        }
      }
    }
    scheduler.synchronizeForTesting()
    release.signal()
    wait(for: [completed], timeout: 5)
    scheduler.synchronizeForTesting()
    lock.lock()
    let finalSequence = sequence
    lock.unlock()
    XCTAssertEqual(finalSequence, [2, 2, 2, 2, 1, 1, 0])
  }

  func testAdmissionSchedulerRoundRobinsRequestGroups() throws {
    let scheduler = try configuredScheduler(maximum: 1)
    let blockerStarted = expectation(description: "blocker admitted")
    let release = DispatchSemaphore(value: 0)
    scheduler.submit(priority: 0, group: 99, onFailure: { _ in }) { _ in
      blockerStarted.fulfill()
      release.wait()
    }
    wait(for: [blockerStarted], timeout: 5)

    let completed = expectation(description: "group jobs completed")
    completed.expectedFulfillmentCount = 4
    let lock = NSLock()
    var sequence: [Int] = []
    for marker in [1, 1, 1] {
      scheduler.submit(priority: 1, group: 1, onFailure: { _ in
        completed.fulfill()
      }) { _ in
        lock.lock()
        sequence.append(marker)
        lock.unlock()
        completed.fulfill()
      }
    }
    scheduler.submit(priority: 1, group: 2, onFailure: { _ in
      completed.fulfill()
    }) { _ in
      lock.lock()
      sequence.append(2)
      lock.unlock()
      completed.fulfill()
    }
    scheduler.synchronizeForTesting()
    release.signal()
    wait(for: [completed], timeout: 5)
    scheduler.synchronizeForTesting()
    lock.lock()
    let finalSequence = sequence
    lock.unlock()
    XCTAssertEqual(finalSequence, [1, 2, 1, 1])
  }

  func testAdmissionSchedulerExpiresQueuedSingleWithoutRunningIt() throws {
    let scheduler = try configuredScheduler(maximum: 1)
    let blockerStarted = expectation(description: "blocker admitted")
    let release = DispatchSemaphore(value: 0)
    scheduler.submit(priority: 0, group: 1, onFailure: { _ in }) { _ in
      blockerStarted.fulfill()
      release.wait()
    }
    wait(for: [blockerStarted], timeout: 5)

    let expired = expectation(description: "queued job expired")
    let deadline = DispatchTime.now().uptimeNanoseconds + 20_000_000
    let lock = NSLock()
    var operationRan = false
    scheduler.submit(
      priority: 2,
      group: 2,
      deadline: deadline,
      onFailure: { error in
        let native = error as NSError
        XCTAssertEqual(native.domain, "JxlCoder")
        XCTAssertEqual(native.code, 408)
        expired.fulfill()
      }
    ) { _ in
      lock.lock()
      operationRan = true
      lock.unlock()
    }
    scheduler.synchronizeForTesting()
    wait(for: [expired], timeout: 5)
    release.signal()
    scheduler.synchronizeForTesting()
    lock.lock()
    XCTAssertFalse(operationRan)
    lock.unlock()
  }

  func testAdmissionSchedulerRejectsAlreadyExpiredWork() throws {
    let scheduler = try configuredScheduler(maximum: 1)
    let expired = expectation(description: "expired work rejected")
    let lock = NSLock()
    var operationRan = false

    scheduler.submit(
      priority: 1,
      group: 1,
      deadline: DispatchTime.now().uptimeNanoseconds - 1,
      onFailure: { error in
        let native = error as NSError
        XCTAssertEqual(native.domain, "JxlCoder")
        XCTAssertEqual(native.code, 408)
        expired.fulfill()
      }
    ) { _ in
      lock.lock()
      operationRan = true
      lock.unlock()
    }

    wait(for: [expired], timeout: 5)
    scheduler.synchronizeForTesting()
    lock.lock()
    XCTAssertFalse(operationRan)
    lock.unlock()
  }

  func testAdmissionSchedulerKeepsSiblingWhenQueuedJobExpires() throws {
    let scheduler = try configuredScheduler(maximum: 1)
    let blockerStarted = expectation(description: "blocker admitted")
    let release = DispatchSemaphore(value: 0)
    scheduler.submit(priority: 0, group: 1, onFailure: { _ in }) { _ in
      blockerStarted.fulfill()
      release.wait()
    }
    wait(for: [blockerStarted], timeout: 5)

    let expired = expectation(description: "first group job expired")
    scheduler.submit(
      priority: 1,
      group: 2,
      deadline: DispatchTime.now().uptimeNanoseconds + 20_000_000,
      onFailure: { error in
        XCTAssertEqual((error as NSError).code, 408)
        expired.fulfill()
      }
    ) { _ in
      XCTFail("expired work must not run")
    }

    let siblingRan = expectation(description: "second group job admitted")
    scheduler.submit(priority: 1, group: 2, onFailure: { error in
      XCTFail("sibling failed: \(error)")
      siblingRan.fulfill()
    }) { _ in
      siblingRan.fulfill()
    }

    scheduler.synchronizeForTesting()
    wait(for: [expired], timeout: 5)
    release.signal()
    wait(for: [siblingRan], timeout: 5)
    scheduler.synchronizeForTesting()
  }

  func testAdmissionSchedulerExpiresOnlyTargetRequestGroup() throws {
    let scheduler = try configuredScheduler(maximum: 1)
    let blockerStarted = expectation(description: "blocker admitted")
    let release = DispatchSemaphore(value: 0)
    scheduler.submit(priority: 0, group: 1, onFailure: { _ in }) { _ in
      blockerStarted.fulfill()
      release.wait()
    }
    wait(for: [blockerStarted], timeout: 5)

    let persistentRan = expectation(description: "persistent group admitted")
    scheduler.submit(priority: 1, group: 2, onFailure: { error in
      XCTFail("persistent group failed: \(error)")
      persistentRan.fulfill()
    }) { _ in
      persistentRan.fulfill()
    }

    let expired = expectation(description: "target group expired")
    scheduler.submit(
      priority: 1,
      group: 3,
      deadline: DispatchTime.now().uptimeNanoseconds + 20_000_000,
      onFailure: { error in
        XCTAssertEqual((error as NSError).code, 408)
        expired.fulfill()
      }
    ) { _ in
      XCTFail("expired target group must not run")
    }

    scheduler.synchronizeForTesting()
    wait(for: [expired], timeout: 5)
    release.signal()
    wait(for: [persistentRan], timeout: 5)
    scheduler.synchronizeForTesting()
  }

  func testAdmissionSchedulerReportsNativeStartupFailure() throws {
    let scheduler = ConversionScheduler { _ in
      throw NSError(
        domain: "JxlCoder",
        code: 409,
        userInfo: [NSLocalizedDescriptionKey: "injected startup failure"]
      )
    }

    let failed = expectation(description: "native startup failure reported")
    let lock = NSLock()
    var operationRan = false
    scheduler.submit(priority: 1, group: 1, onFailure: { error in
      let native = error as NSError
      XCTAssertEqual(native.domain, "JxlCoder")
      XCTAssertEqual(native.code, 409)
      failed.fulfill()
    }) { _ in
      lock.lock()
      operationRan = true
      lock.unlock()
    }

    wait(for: [failed], timeout: 5)
    scheduler.synchronizeForTesting()
    lock.lock()
    XCTAssertFalse(operationRan)
    lock.unlock()
  }

  func testAdmissionQueueRecoversFromMissingGroupState() throws {
    let scheduler = try configuredScheduler(maximum: 1)
    scheduler.injectMissingQueueGroupForTesting(priority: 1, group: 99)
    let completed = expectation(description: "valid job survives stale group")
    scheduler.submit(priority: 1, group: 1, onFailure: { error in
      XCTFail("Unexpected scheduler failure: \(error)")
      completed.fulfill()
    }) { _ in
      completed.fulfill()
    }
    wait(for: [completed], timeout: 5)
    scheduler.synchronizeForTesting()
  }

  func testSingleCallReportsAdmissionStartupFailure() {
    let scheduler = ConversionScheduler { _ in
      throw NSError(
        domain: "JxlCoder",
        code: 409,
        userInfo: [NSLocalizedDescriptionKey: "injected startup failure"]
      )
    }
    let failingPlugin = JxlCoderPlugin(scheduler: scheduler)

    assertFlutterError(
      invoke(
        "jpegBytesToJxl",
        arguments: [
          FlutterStandardTypedData(bytes: Data([0xff])), 7, 0, 1, 0,
        ],
        using: failingPlugin
      ),
      code: "SCHEDULER_ALREADY_STARTED"
    )
  }

  func testBatchReportsAdmissionStartupFailure() {
    let scheduler = ConversionScheduler { _ in
      throw NSError(
        domain: "JxlCoder",
        code: 409,
        userInfo: [NSLocalizedDescriptionKey: "injected startup failure"]
      )
    }
    let failingPlugin = JxlCoderPlugin(scheduler: scheduler)

    assertFlutterError(
      invoke(
        "jpegPathsToJxl",
        arguments: [
          7, 0, 1, 0,
          "first.jpg", "first.jxl",
          "second.jpg", "second.jxl",
        ],
        using: failingPlugin
      ),
      code: "SCHEDULER_ALREADY_STARTED"
    )
  }

  func testConfigurationReportsInvalidNativeCapacity() {
    let scheduler = ConversionScheduler { _ in 0 }
    let failingPlugin = JxlCoderPlugin(scheduler: scheduler)

    assertFlutterError(
      invoke(
        "configureJxlScheduler",
        arguments: [0, 0],
        using: failingPlugin
      ),
      code: "SCHEDULER_ERROR"
    )
  }

  func testEveryConversionReportsInvalidNativeCapacityAsSchedulerError() {
    let scheduler = ConversionScheduler { _ in 0 }
    let failingPlugin = JxlCoderPlugin(scheduler: scheduler)
    let calls: [(String, Any)] = [
      (
        "jpegBytesToJxl",
        [FlutterStandardTypedData(bytes: Data([0xff])), 7, 0, 1, 0]
      ),
      (
        "jxlBytesToJpeg",
        [FlutterStandardTypedData(bytes: Data([0xff])), 1, 0]
      ),
      ("jpegPathToJxl", ["input.jpg", "output.jxl", 7, 0, 1, 0]),
      ("jxlPathToJpeg", ["input.jxl", "output.jpg", 1, 0]),
      (
        "jpegPathsToJxl",
        [7, 0, 1, 0, "input.jpg", "output.jxl"]
      ),
      (
        "jxlPathsToJpeg",
        [1, 0, "input.jxl", "output.jpg"]
      ),
    ]

    for (method, arguments) in calls {
      assertFlutterError(
        invoke(method, arguments: arguments, using: failingPlugin),
        code: "SCHEDULER_ERROR"
      )
    }
  }

  func testAdmissionTimestampIsRecordedAfterQueueWaiting() throws {
    let scheduler = try configuredScheduler(maximum: 1)
    let blockerStarted = expectation(description: "blocker admitted")
    let release = DispatchSemaphore(value: 0)
    scheduler.submit(priority: 0, group: 1, onFailure: { _ in }) { _ in
      blockerStarted.fulfill()
      release.wait()
    }
    wait(for: [blockerStarted], timeout: 5)

    let admitted = expectation(description: "queued job admitted")
    let lock = NSLock()
    var releaseTime: UInt64 = 0
    var admissionTime: UInt64 = 0
    scheduler.submit(priority: 1, group: 2, onFailure: { _ in
      admitted.fulfill()
    }) { started in
      lock.lock()
      admissionTime = started
      lock.unlock()
      admitted.fulfill()
    }
    scheduler.synchronizeForTesting()
    releaseTime = DispatchTime.now().uptimeNanoseconds
    release.signal()
    wait(for: [admitted], timeout: 5)
    scheduler.synchronizeForTesting()
    lock.lock()
    XCTAssertGreaterThanOrEqual(admissionTime, releaseTime)
    lock.unlock()
  }

  func testSchedulerConfigurationIsIdempotentAndImmutable() throws {
    XCTAssertNil(invoke("configureJxlScheduler", arguments: [0, 0]))
    XCTAssertNil(invoke("configureJxlScheduler", arguments: [0, 0]))
    assertFlutterError(
      invoke("configureJxlScheduler", arguments: [0, 256]),
      code: "SCHEDULER_ALREADY_STARTED"
    )

    let jpegData = try fixtureData("2.jpg")
    XCTAssertNotNil(invoke(
      "jpegBytesToJxl",
      arguments: [FlutterStandardTypedData(bytes: jpegData), 3, 0, 1, 0]
    ))
    assertFlutterError(
      invoke("configureJxlScheduler", arguments: [0, 256]),
      code: "SCHEDULER_ALREADY_STARTED"
    )
  }

  func testByteRoundTripIsLossless() throws {
    let fixtureNames = [
      "2.jpg",
      "baseline_rgb.jpg",
      "progressive_rgb.jpg",
      "grayscale.jpg",
    ]

    for fixtureName in fixtureNames {
      let jpegData = try fixtureData(fixtureName)
      let encoded = try XCTUnwrap(
        invoke(
          "jpegBytesToJxl",
          arguments: [FlutterStandardTypedData(bytes: jpegData), 3, 0, 1, 0]
        ) as? FlutterStandardTypedData
      ).data
      XCTAssertFalse(encoded.isEmpty, fixtureName)

      let restored = try XCTUnwrap(
        invoke(
          "jxlBytesToJpeg",
          arguments: [FlutterStandardTypedData(bytes: encoded), 1, 0]
        ) as? FlutterStandardTypedData
      ).data

      XCTAssertEqual(restored, jpegData, fixtureName)
    }
  }

  func testCMYKJpegReturnsTranscodeError() throws {
    assertFlutterError(
      invoke(
        "jpegBytesToJxl",
        arguments: [
          FlutterStandardTypedData(bytes: try fixtureData("cmyk.jpg")), 3, 0, 1, 0,
        ]
      ),
      code: "UNSUPPORTED_INPUT"
    )
  }

  func testFileRoundTripIsLossless() throws {
    let directory = try temporaryDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }
    let inputURL = directory.appendingPathComponent("input.jpg")
    let jxlURL = directory.appendingPathComponent("output.jxl")
    let restoredURL = directory.appendingPathComponent("restored.jpg")
    let jpegData = try fixtureData("2.jpg")
    try jpegData.write(to: inputURL)

    XCTAssertNil(invoke(
      "jpegPathToJxl",
      arguments: [inputURL.path, jxlURL.path, 3, 0, 1, 0]
    ))
    XCTAssertGreaterThan(try Data(contentsOf: jxlURL).count, 0)
    XCTAssertNil(invoke(
      "jxlPathToJpeg",
      arguments: [jxlURL.path, restoredURL.path, 1, 0]
    ))

    XCTAssertEqual(try Data(contentsOf: restoredURL), jpegData)
  }

  func testMalformedDataReturnsCodecErrors() {
    assertFlutterError(
      invoke(
        "jpegBytesToJxl",
        arguments: [
          FlutterStandardTypedData(bytes: Data([0xff, 0xd8])), 3, 0, 1, 0,
        ]
      ),
      code: "UNSUPPORTED_INPUT"
    )
    assertFlutterError(
      invoke(
        "jxlBytesToJpeg",
        arguments: [FlutterStandardTypedData(bytes: Data([0x00, 0x01])), 1, 0]
      ),
      code: "UNSUPPORTED_INPUT"
    )
  }

  func testMissingFilesReturnErrors() throws {
    let directory = try temporaryDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }

    assertFlutterError(
      invoke(
        "jpegPathToJxl",
        arguments: [
          directory.appendingPathComponent("missing.jpg").path,
          directory.appendingPathComponent("out.jxl").path,
          3,
          0,
          1,
          0,
        ]
      ),
      code: "IO_ERROR"
    )
    assertFlutterError(
      invoke(
        "jxlPathToJpeg",
        arguments: [
          directory.appendingPathComponent("missing.jxl").path,
          directory.appendingPathComponent("out.jpg").path,
          1,
          0,
        ]
      ),
      code: "IO_ERROR"
    )
  }

  func testNativePathErrorMappingKeepsCodecAndIoFailuresDistinct() {
    let ioError = NSError(
      domain: NSCocoaErrorDomain,
      code: NSFileReadNoSuchFileError
    )
    let codecError = NSError(
      domain: "JxlCoder",
      code: 501
    )

    XCTAssertEqual(
      jxlCoderFlutterErrorCode(ioError, fallback: "TRANSCODE_ERROR"),
      "IO_ERROR"
    )
    XCTAssertEqual(
      jxlCoderFlutterErrorCode(codecError, fallback: "TRANSCODE_ERROR"),
      "TRANSCODE_ERROR"
    )
    XCTAssertEqual(
      jxlCoderFlutterErrorCode(codecError, fallback: "INVERSE_ERROR"),
      "INVERSE_ERROR"
    )
  }

  func testEveryNativeErrorCodeMapsToThePublicContract() {
    let expected = [
      400: "INVALID_ARGUMENTS",
      408: "TIMEOUT",
      409: "SCHEDULER_ALREADY_STARTED",
      415: "UNSUPPORTED_INPUT",
      500: "IO_ERROR",
      501: "TRANSCODE_ERROR",
      502: "SCHEDULER_ERROR",
    ]
    for (code, publicCode) in expected {
      XCTAssertEqual(
        jxlCoderFlutterErrorCode(
          NSError(domain: "JxlCoder", code: code),
          fallback: "TRANSCODE_ERROR"
        ),
        publicCode
      )
    }
    XCTAssertEqual(
      jxlCoderFlutterErrorCode(
        NSError(domain: "UnexpectedDomain", code: 1),
        fallback: "INVERSE_ERROR"
      ),
      "INVERSE_ERROR"
    )
    XCTAssertEqual(
      jxlCoderFlutterErrorCode(
        NSError(domain: NSPOSIXErrorDomain, code: 13),
        fallback: "INVERSE_ERROR"
      ),
      "IO_ERROR"
    )
  }

  func testCoreBridgeRejectsInvalidArguments() throws {
    var effectiveWorkers = 0
    XCTAssertThrowsError(
      try JxlCoderCore.configureWorkerCount(
        257,
        effectiveWorkerCount: &effectiveWorkers
      )
    ) { error in
      XCTAssertEqual((error as NSError).code, 400)
    }

    let validJPEG = try fixtureData("2.jpg")
    let invalidCalls: [() throws -> Data] = [
      {
        try JxlCoderCore.transcode(
          Data(), effort: 7, decodingSpeed: 0, priority: 1,
          schedulingGroup: 0, timeoutMilliseconds: 0
        )
      },
      {
        try JxlCoderCore.transcode(
          validJPEG, effort: 0, decodingSpeed: 0, priority: 1,
          schedulingGroup: 0, timeoutMilliseconds: 0
        )
      },
      {
        try JxlCoderCore.transcode(
          validJPEG, effort: 7, decodingSpeed: 5, priority: 1,
          schedulingGroup: 0, timeoutMilliseconds: 0
        )
      },
      {
        try JxlCoderCore.transcode(
          validJPEG, effort: 7, decodingSpeed: 0, priority: 3,
          schedulingGroup: 0, timeoutMilliseconds: 0
        )
      },
      {
        try JxlCoderCore.transcode(
          validJPEG, effort: 7, decodingSpeed: 0, priority: 1,
          schedulingGroup: 0, timeoutMilliseconds: -1
        )
      },
      {
        try JxlCoderCore.inverse(
          Data(), priority: 1, schedulingGroup: 0,
          timeoutMilliseconds: 0
        )
      },
    ]
    for invalidCall in invalidCalls {
      XCTAssertThrowsError(try invalidCall()) { error in
        XCTAssertEqual((error as NSError).domain, "JxlCoder")
        XCTAssertEqual((error as NSError).code, 400)
      }
    }
  }

  func testWriteFailuresReturnErrors() throws {
    let directory = try temporaryDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }
    let jpegURL = directory.appendingPathComponent("input.jpg")
    let jxlURL = directory.appendingPathComponent("input.jxl")
    try fixtureData("2.jpg").write(to: jpegURL)
    try fixtureData("1.jxl").write(to: jxlURL)

    assertFlutterError(
      invoke(
        "jpegPathToJxl",
        arguments: [jpegURL.path, directory.path, 3, 0, 1, 0]
      ),
      code: "IO_ERROR"
    )
    assertFlutterError(
      invoke(
        "jxlPathToJpeg",
        arguments: [jxlURL.path, directory.path, 1, 0]
      ),
      code: "IO_ERROR"
    )
  }

  func testInvalidArgumentsReturnErrors() {
    let calls: [(String, Any?)] = [
      ("jpegBytesToJxl", nil),
      ("jxlBytesToJpeg", ["jxlData": "wrong"]),
      ("jpegPathToJxl", ["inputPath": "input.jpg"]),
      ("jxlPathToJpeg", ["inputPath": 42, "outputPath": "output.jpg"]),
      ("jpegPathsToJxl", [3, 0, 1, 0, "unpaired.jpg"]),
      ("jxlPathsToJpeg", [1, 0, "unpaired.jxl"]),
      ("jpegBytesToJxl", [FlutterStandardTypedData(bytes: Data()), 10, 0, 0, 0]),
      ("jxlBytesToJpeg", [FlutterStandardTypedData(bytes: Data()), -1, 0]),
      ("jpegPathsToJxl", [3, 0, 3, 0]),
    ]

    for (method, arguments) in calls {
      assertFlutterError(
        invoke(method, arguments: arguments),
        code: "INVALID_ARGUMENTS",
        expectsDetails: false
      )
    }
  }

  func testBatchRoundTripIsLossless() throws {
    let directory = try temporaryDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }
    let jpegData = try fixtureData("2.jpg")
    var encodeArguments: [Any] = [3, 0, 1, 0]
    var decodeArguments: [Any] = [1, 0]

    for index in 0..<4 {
      let input = directory.appendingPathComponent("input_\(index).jpg")
      let encoded = directory.appendingPathComponent("output_\(index).jxl")
      let restored = directory.appendingPathComponent("restored_\(index).jpg")
      try jpegData.write(to: input)
      encodeArguments.append(contentsOf: [input.path, encoded.path])
      decodeArguments.append(contentsOf: [encoded.path, restored.path])
    }

    XCTAssertNil(invoke("jpegPathsToJxl", arguments: encodeArguments))
    XCTAssertNil(invoke("jxlPathsToJpeg", arguments: decodeArguments))

    for index in 0..<4 {
      let restored = directory.appendingPathComponent("restored_\(index).jpg")
      XCTAssertEqual(try Data(contentsOf: restored), jpegData)
    }
  }

  func testBatchFailureReturnsCodecError() throws {
    let directory = try temporaryDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }

    assertFlutterError(
      invoke(
        "jpegPathsToJxl",
        arguments: [
          3,
          0,
          1,
          0,
          directory.appendingPathComponent("missing.jpg").path,
          directory.appendingPathComponent("output.jxl").path,
        ]
      ),
      code: "IO_ERROR"
    )
  }

  func testEmptyBatchesSucceed() {
    XCTAssertNil(invoke("jpegPathsToJxl", arguments: [7, 0, 1, 0]))
    XCTAssertNil(invoke("jxlPathsToJpeg", arguments: [1, 0]))
  }

  func testTuningBoundariesRemainLossless() throws {
    let jpegData = try fixtureData("2.jpg")
    let settings = [
      (effort: 1, decodingSpeed: 0, priority: 0),
      (effort: 9, decodingSpeed: 4, priority: 2),
    ]

    for setting in settings {
      let encoded = try XCTUnwrap(
        invoke(
          "jpegBytesToJxl",
          arguments: [
            FlutterStandardTypedData(bytes: jpegData),
            setting.effort,
            setting.decodingSpeed,
            setting.priority,
            0,
          ]
        ) as? FlutterStandardTypedData
      ).data
      let restored = try XCTUnwrap(
        invoke(
          "jxlBytesToJpeg",
          arguments: [
            FlutterStandardTypedData(bytes: encoded), setting.priority, 0,
          ]
        ) as? FlutterStandardTypedData
      ).data

      XCTAssertEqual(restored, jpegData)
    }
  }

  func testCodecFailuresPreserveExistingOutputFiles() throws {
    let directory = try temporaryDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }
    let badJPEG = directory.appendingPathComponent("bad.jpg")
    let badJXL = directory.appendingPathComponent("bad.jxl")
    let jpegOutput = directory.appendingPathComponent("existing.jxl")
    let jxlOutput = directory.appendingPathComponent("existing.jpg")
    let sentinel = Data("keep this output".utf8)
    try Data([0xff, 0xd8]).write(to: badJPEG)
    try Data([0x00, 0x01]).write(to: badJXL)
    try sentinel.write(to: jpegOutput)
    try sentinel.write(to: jxlOutput)

    assertFlutterError(
      invoke(
        "jpegPathToJxl",
        arguments: [badJPEG.path, jpegOutput.path, 7, 0, 1, 0]
      ),
      code: "UNSUPPORTED_INPUT"
    )
    assertFlutterError(
      invoke(
        "jxlPathToJpeg",
        arguments: [badJXL.path, jxlOutput.path, 1, 0]
      ),
      code: "UNSUPPORTED_INPUT"
    )

    XCTAssertEqual(try Data(contentsOf: jpegOutput), sentinel)
    XCTAssertEqual(try Data(contentsOf: jxlOutput), sentinel)
  }

  func testDeadlinePreservesExistingOutputFile() throws {
    let directory = try temporaryDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }
    let input = directory.appendingPathComponent("input.jpg")
    let output = directory.appendingPathComponent("existing.jxl")
    let sentinel = Data("keep this output".utf8)
    try fixtureData("2.jpg").write(to: input)
    try sentinel.write(to: output)

    assertFlutterError(
      invoke(
        "jpegPathToJxl",
        arguments: [input.path, output.path, 9, 0, 1, 1]
      ),
      code: "TIMEOUT"
    )
    XCTAssertEqual(try Data(contentsOf: output), sentinel)
  }

  func testInverseDeadlinesPreserveExistingOutputFiles() throws {
    let directory = try temporaryDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }
    let jxlData = try fixtureData("1.jxl")

    assertFlutterError(
      invoke(
        "jxlBytesToJpeg",
        arguments: [FlutterStandardTypedData(bytes: jxlData), 1, 1]
      ),
      code: "TIMEOUT"
    )

    let input = directory.appendingPathComponent("input.jxl")
    let output = directory.appendingPathComponent("existing.jpg")
    let sentinel = Data("keep inverse output".utf8)
    try jxlData.write(to: input)
    try sentinel.write(to: output)
    assertFlutterError(
      invoke(
        "jxlPathToJpeg",
        arguments: [input.path, output.path, 1, 1]
      ),
      code: "TIMEOUT"
    )
    XCTAssertEqual(try Data(contentsOf: output), sentinel)
  }

  func testBatchDeadlinesPreserveEveryExistingOutput() throws {
    let directory = try temporaryDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }
    let jpeg = try fixtureData("2.jpg")
    let sentinel = Data("keep batch output".utf8)
    var arguments: [Any] = [9, 0, 1, 1]
    var outputs: [URL] = []
    for index in 0..<4 {
      let input = directory.appendingPathComponent("input_\(index).jpg")
      let output = directory.appendingPathComponent("existing_\(index).jxl")
      try jpeg.write(to: input)
      try sentinel.write(to: output)
      arguments.append(contentsOf: [input.path, output.path])
      outputs.append(output)
    }

    assertFlutterError(
      invoke("jpegPathsToJxl", arguments: arguments),
      code: "TIMEOUT"
    )
    for output in outputs {
      XCTAssertEqual(try Data(contentsOf: output), sentinel)
    }
  }

  func testBatchFinishesValidJobsBeforeReportingAnotherFailure() throws {
    let directory = try temporaryDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }
    let jpegData = try fixtureData("2.jpg")
    let validJPEG = directory.appendingPathComponent("valid.jpg")
    let validJXL = directory.appendingPathComponent("valid.jxl")
    let validRestored = directory.appendingPathComponent("valid-restored.jpg")
    let missingJPEG = directory.appendingPathComponent("missing.jpg")
    let missingJXL = directory.appendingPathComponent("missing.jxl")
    try jpegData.write(to: validJPEG)

    assertFlutterError(
      invoke(
        "jpegPathsToJxl",
        arguments: [
          7,
          0,
          1,
          0,
          validJPEG.path,
          validJXL.path,
          missingJPEG.path,
          directory.appendingPathComponent("missing-output.jxl").path,
        ]
      ),
      code: "IO_ERROR"
    )
    XCTAssertFalse(try Data(contentsOf: validJXL).isEmpty)

    assertFlutterError(
      invoke(
        "jxlPathsToJpeg",
        arguments: [
          1,
          0,
          validJXL.path,
          validRestored.path,
          missingJXL.path,
          directory.appendingPathComponent("missing-output.jpg").path,
        ]
      ),
      code: "IO_ERROR"
    )
    XCTAssertEqual(try Data(contentsOf: validRestored), jpegData)
  }

  func testBatchSelectsFailureByInputOrderNotCompletionOrder() throws {
    let directory = try temporaryDirectory()
    defer { try? FileManager.default.removeItem(at: directory) }
    let valid = directory.appendingPathComponent("large-valid.jpg")
    let malformed = directory.appendingPathComponent("malformed.jpg")
    try fixtureData("2.jpg").write(to: valid)
    try Data([0xff, 0xd8]).write(to: malformed)

    assertFlutterError(
      invoke(
        "jpegPathsToJxl",
        arguments: [
          9,
          0,
          1,
          0,
          valid.path,
          directory.path,
          malformed.path,
          directory.appendingPathComponent("unused.jxl").path,
        ]
      ),
      code: "IO_ERROR"
    )
  }

  func testEveryInvalidArgumentBoundaryReturnsInvalidArguments() {
    let bytes = FlutterStandardTypedData(bytes: Data())
    let calls: [(String, Any?)] = [
      ("configureJxlScheduler", nil),
      ("configureJxlScheduler", [0]),
      ("configureJxlScheduler", [-1, 0]),
      ("configureJxlScheduler", [257, 0]),
      ("configureJxlScheduler", [0, -1]),
      ("configureJxlScheduler", [0, 257]),
      ("jpegBytesToJxl", []),
      ("jpegBytesToJxl", [bytes, 7, 0, 1]),
      ("jpegBytesToJxl", [bytes, 7, 0, 1, 0, "extra"]),
      ("jpegBytesToJxl", [Data(), 7, 0, 1, 0]),
      ("jpegBytesToJxl", [bytes, "7", 0, 1, 0]),
      ("jpegBytesToJxl", [bytes, 0, 0, 1, 0]),
      ("jpegBytesToJxl", [bytes, 10, 0, 1, 0]),
      ("jpegBytesToJxl", [bytes, 7, -1, 1, 0]),
      ("jpegBytesToJxl", [bytes, 7, 5, 1, 0]),
      ("jpegBytesToJxl", [bytes, 7, 0, -1, 0]),
      ("jpegBytesToJxl", [bytes, 7, 0, 3, 0]),
      ("jpegBytesToJxl", [bytes, 7, 0, 1, -1]),
      ("jxlBytesToJpeg", []),
      ("jxlBytesToJpeg", [bytes, 1]),
      ("jxlBytesToJpeg", [bytes, 1, 0, "extra"]),
      ("jxlBytesToJpeg", [Data(), 1, 0]),
      ("jxlBytesToJpeg", [bytes, -1, 0]),
      ("jxlBytesToJpeg", [bytes, 3, 0]),
      ("jxlBytesToJpeg", [bytes, 1, -1]),
      ("jpegPathToJxl", []),
      ("jpegPathToJxl", ["in.jpg", "out.jxl", 7, 0, 1]),
      ("jpegPathToJxl", ["in.jpg", "out.jxl", 7, 0, 1, 0, "extra"]),
      ("jpegPathToJxl", [1, "out.jxl", 7, 0, 1, 0]),
      ("jpegPathToJxl", ["in.jpg", 2, 7, 0, 1, 0]),
      ("jpegPathToJxl", ["in.jpg", "out.jxl", 0, 0, 1, 0]),
      ("jpegPathToJxl", ["in.jpg", "out.jxl", 7, 5, 1, 0]),
      ("jpegPathToJxl", ["in.jpg", "out.jxl", 7, 0, 3, 0]),
      ("jpegPathToJxl", ["in.jpg", "out.jxl", 7, 0, 1, -1]),
      ("jxlPathToJpeg", []),
      ("jxlPathToJpeg", ["in.jxl", "out.jpg", 1]),
      ("jxlPathToJpeg", ["in.jxl", "out.jpg", 1, 0, "extra"]),
      ("jxlPathToJpeg", [1, "out.jpg", 1, 0]),
      ("jxlPathToJpeg", ["in.jxl", 2, 1, 0]),
      ("jxlPathToJpeg", ["in.jxl", "out.jpg", 3, 0]),
      ("jxlPathToJpeg", ["in.jxl", "out.jpg", 1, -1]),
      ("jpegPathsToJxl", []),
      ("jpegPathsToJxl", [7, 0, 1]),
      ("jpegPathsToJxl", [0, 0, 1, 0]),
      ("jpegPathsToJxl", [7, 5, 1, 0]),
      ("jpegPathsToJxl", [7, 0, -1, 0]),
      ("jpegPathsToJxl", [7, 0, 3, 0]),
      ("jpegPathsToJxl", [7, 0, 1, -1]),
      ("jpegPathsToJxl", [7, 0, 1, 0, "unpaired"]),
      ("jpegPathsToJxl", [7, 0, 1, 0, 1, "out.jxl"]),
      ("jxlPathsToJpeg", []),
      ("jxlPathsToJpeg", [1]),
      ("jxlPathsToJpeg", [-1, 0]),
      ("jxlPathsToJpeg", [3, 0]),
      ("jxlPathsToJpeg", [1, -1]),
      ("jxlPathsToJpeg", [1, 0, "unpaired"]),
      ("jxlPathsToJpeg", [1, 0, "in.jxl", 2]),
    ]

    for (method, arguments) in calls {
      assertFlutterError(
        invoke(method, arguments: arguments),
        code: "INVALID_ARGUMENTS",
        expectsDetails: false,
        line: #line
      )
    }
  }

  func testUnknownMethodIsNotImplemented() throws {
    let result = try XCTUnwrap(invoke("unknownMethod"))

    XCTAssertTrue((result as AnyObject) === FlutterMethodNotImplemented)
  }
}
