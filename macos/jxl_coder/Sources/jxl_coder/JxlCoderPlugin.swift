import Foundation
import Dispatch
#if os(macOS)
import FlutterMacOS
#else
import Flutter
#endif
#if canImport(JxlCoderCore)
import JxlCoderCore
#endif

private final class AdmissionJob {
  let id: UInt64
  let group: UInt64
  let deadline: UInt64?
  let onFailure: (Error) -> Void
  let operation: (UInt64) -> Void

  init(
    id: UInt64,
    group: UInt64,
    deadline: UInt64?,
    onFailure: @escaping (Error) -> Void,
    operation: @escaping (UInt64) -> Void
  ) {
    self.id = id
    self.group = group
    self.deadline = deadline
    self.onFailure = onFailure
    self.operation = operation
  }
}

private let nanosecondsPerMillisecond: UInt64 = 1_000_000

private func monotonicNow() -> UInt64 {
  DispatchTime.now().uptimeNanoseconds
}

private func monotonicDeadline(
  since started: UInt64,
  timeoutMilliseconds: Int
) -> UInt64? {
  guard timeoutMilliseconds > 0 else { return nil }
  let (duration, durationOverflow) = UInt64(timeoutMilliseconds)
    .multipliedReportingOverflow(by: nanosecondsPerMillisecond)
  if durationOverflow { return UInt64.max }
  let (deadline, deadlineOverflow) = started.addingReportingOverflow(duration)
  return deadlineOverflow ? UInt64.max : deadline
}

private final class PriorityClassQueue {
  private var groups: [UInt64] = []
  private var jobs: [UInt64: [AdmissionJob]] = [:]

  func enqueue(_ job: AdmissionJob) {
    if jobs[job.group] == nil {
      groups.append(job.group)
      jobs[job.group] = []
    }
    jobs[job.group]!.append(job)
  }

  func dequeue() -> AdmissionJob? {
    guard !groups.isEmpty else { return nil }
    let group = groups.removeFirst()
    guard var queued = jobs[group], !queued.isEmpty else {
      jobs.removeValue(forKey: group)
      return dequeue()
    }
    let job = queued.removeFirst()
    if queued.isEmpty {
      jobs.removeValue(forKey: group)
    } else {
      jobs[group] = queued
      groups.append(group)
    }
    return job
  }

  func remove(id: UInt64) -> AdmissionJob? {
    for group in groups {
      guard var queued = jobs[group],
            let index = queued.firstIndex(where: { $0.id == id }) else {
        continue
      }
      let job = queued.remove(at: index)
      if queued.isEmpty {
        jobs.removeValue(forKey: group)
        groups.removeAll(where: { $0 == group })
      } else {
        jobs[group] = queued
      }
      return job
    }
    return nil
  }

  #if DEBUG
  func injectMissingGroupForTesting(_ group: UInt64) {
    groups.append(group)
    jobs.removeValue(forKey: group)
  }
  #endif
}

final class ConversionScheduler {
  static let shared = ConversionScheduler()

  private let stateQueue = DispatchQueue(
    label: "com.stephanearcher.jxl-coder.scheduler-state"
  )
  private let executionQueue = DispatchQueue(
    label: "com.stephanearcher.jxl-coder.scheduler-workers",
    qos: .userInitiated,
    attributes: .concurrent
  )
  private let configureNativeWorkers: (Int) throws -> Int
  private let priorityCycle = [2, 2, 2, 2, 1, 1, 0]
  private var priorityIndex = 0
  private var queues = [
    PriorityClassQueue(),
    PriorityClassQueue(),
    PriorityClassQueue(),
  ]
  private var active = 0
  private var effectiveWorkers = 0
  private var effectiveMaximum = 0
  private var configured = false
  private var started = false
  private var requestedWorkers = 0
  private var requestedMaximum = 0
  private var nextGroup: UInt64 = 1
  private var nextJob: UInt64 = 1

  init(
    configureNativeWorkers: @escaping (Int) throws -> Int = { requested in
      var effective = 0
      try JxlCoderCore.configureWorkerCount(
        requested,
        effectiveWorkerCount: &effective
      )
      return effective
    }
  ) {
    self.configureNativeWorkers = configureNativeWorkers
  }

  #if DEBUG
  func synchronizeForTesting() {
    stateQueue.sync {}
  }

  func injectMissingQueueGroupForTesting(priority: Int, group: UInt64) {
    stateQueue.sync {
      queues[priority].injectMissingGroupForTesting(group)
    }
  }
  #endif

  func configure(workerCount: Int, maxActiveConversions: Int) throws {
    try stateQueue.sync {
      guard (0...256).contains(workerCount),
            (0...256).contains(maxActiveConversions) else {
        throw schedulerInvalidArgumentsError()
      }
      let initialized = configured || started
      if initialized,
         workerCount == requestedWorkers,
         maxActiveConversions == requestedMaximum {
        return
      }
      let processors = min(
        256,
        max(1, ProcessInfo.processInfo.activeProcessorCount)
      )
      let desiredWorkers = workerCount == 0
        ? (initialized ? effectiveWorkers : processors)
        : workerCount
      let desiredMaximum = maxActiveConversions == 0
        ? desiredWorkers
        : maxActiveConversions
      if initialized {
        guard desiredWorkers == effectiveWorkers,
              desiredMaximum == effectiveMaximum else {
          throw schedulerStartedError()
        }
        return
      }

      let nativeWorkers = try configureNativeWorkers(workerCount)
      guard (1...256).contains(nativeWorkers) else {
        throw schedulerUnavailableError()
      }
      effectiveWorkers = nativeWorkers
      effectiveMaximum = maxActiveConversions == 0
        ? nativeWorkers
        : maxActiveConversions
      requestedWorkers = workerCount
      requestedMaximum = maxActiveConversions
      configured = true
    }
  }

  func makeGroup() -> UInt64 {
    stateQueue.sync {
      let group = nextGroup
      nextGroup &+= 1
      if nextGroup == 0 { nextGroup = 1 }
      return group
    }
  }

  func submit(
    priority: Int,
    group: UInt64,
    deadline: UInt64? = nil,
    onFailure: @escaping (Error) -> Void,
    operation: @escaping (UInt64) -> Void
  ) {
    stateQueue.async {
      do {
        guard self.queues.indices.contains(priority) else {
          throw schedulerInvalidArgumentsError()
        }
        try self.startIfNeeded()
        if let deadline, deadline <= monotonicNow() {
          self.executionQueue.async {
            onFailure(codecTimeoutError("Timed out waiting for codec capacity"))
          }
          return
        }
        let id = self.nextJob
        self.nextJob &+= 1
        if self.nextJob == 0 { self.nextJob = 1 }
        let job = AdmissionJob(
          id: id,
          group: group,
          deadline: deadline,
          onFailure: onFailure,
          operation: operation
        )
        self.queues[priority].enqueue(job)
        if let deadline {
          self.stateQueue.asyncAfter(
            deadline: DispatchTime(uptimeNanoseconds: deadline)
          ) {
            guard let expired = self.queues[priority].remove(id: id) else {
              return
            }
            self.executionQueue.async {
              expired.onFailure(
                codecTimeoutError("Timed out waiting for codec capacity")
              )
            }
          }
        }
        self.drain()
      } catch {
        self.executionQueue.async { onFailure(error) }
      }
    }
  }

  private func startIfNeeded() throws {
    guard !started else { return }
    if effectiveWorkers == 0 {
      let nativeWorkers = try configureNativeWorkers(0)
      guard (1...256).contains(nativeWorkers) else {
        throw schedulerUnavailableError()
      }
      effectiveWorkers = nativeWorkers
      effectiveMaximum = nativeWorkers
      requestedWorkers = 0
      requestedMaximum = 0
      configured = true
    }
    started = true
  }

  private func dequeue() -> AdmissionJob? {
    for _ in 0..<priorityCycle.count {
      let priority = priorityCycle[priorityIndex]
      priorityIndex = (priorityIndex + 1) % priorityCycle.count
      if let job = queues[priority].dequeue() { return job }
    }
    for queue in queues {
      if let job = queue.dequeue() { return job }
    }
    return nil
  }

  private func drain() {
    while active < effectiveMaximum, let job = dequeue() {
      let admitted = monotonicNow()
      active += 1
      executionQueue.async {
        job.operation(admitted)
        self.stateQueue.async {
          self.active -= 1
          self.drain()
        }
      }
    }
  }
}

private final class BatchCompletion {
  private let lock = NSLock()
  private var remaining: Int
  private var errors: [Error?]
  private let completion: ([Error?]) -> Void

  init(count: Int, completion: @escaping ([Error?]) -> Void) {
    remaining = count
    errors = Array(repeating: nil, count: count)
    self.completion = completion
  }

  func finish(index: Int, error: Error?) {
    lock.lock()
    errors[index] = error
    remaining -= 1
    let finished = remaining == 0
    let finalErrors = errors
    lock.unlock()
    if finished { completion(finalErrors) }
  }
}

private func codecTimeoutError(_ message: String) -> NSError {
  NSError(
    domain: JxlCoderErrorDomain,
    code: JxlCoderError.timeout.rawValue,
    userInfo: [NSLocalizedDescriptionKey: message]
  )
}

private func schedulerStartedError() -> NSError {
  NSError(
    domain: JxlCoderErrorDomain,
    code: JxlCoderError.schedulerAlreadyStarted.rawValue,
    userInfo: [
      NSLocalizedDescriptionKey: "The JPEG XL scheduler has already started",
    ]
  )
}

private func schedulerInvalidArgumentsError() -> NSError {
  NSError(
    domain: JxlCoderErrorDomain,
    code: JxlCoderError.invalidArguments.rawValue,
    userInfo: [
      NSLocalizedDescriptionKey: "Invalid JPEG XL scheduler configuration",
    ]
  )
}

private func schedulerUnavailableError() -> NSError {
  NSError(
    domain: JxlCoderErrorDomain,
    code: JxlCoderError.schedulerUnavailable.rawValue,
    userInfo: [
      NSLocalizedDescriptionKey: "The JPEG XL scheduler returned an invalid capacity",
    ]
  )
}

struct JxlFileAccess {
  let fileURL: URL
  let securityScopedURL: URL?
}

private func invalidPathBookmarkError(_ message: String) -> NSError {
  NSError(
    domain: JxlCoderErrorDomain,
    code: JxlCoderError.invalidArguments.rawValue,
    userInfo: [NSLocalizedDescriptionKey: message]
  )
}

private func deniedPathBookmarkError(_ url: URL) -> NSError {
  NSError(
    domain: NSCocoaErrorDomain,
    code: NSFileReadNoPermissionError,
    userInfo: [
      NSFilePathErrorKey: url.path,
      NSLocalizedDescriptionKey: "Could not access the security-scoped resource",
    ]
  )
}

#if os(macOS)
private func fileAccess(
  path: String,
  securityScopedBookmark: Data?
) throws -> JxlFileAccess {
  let requestedURL = URL(fileURLWithPath: path).standardizedFileURL
  guard let securityScopedBookmark else {
    return JxlFileAccess(fileURL: requestedURL, securityScopedURL: nil)
  }

  var isStale = false
  let scopedURL = try URL(
    resolvingBookmarkData: securityScopedBookmark,
    options: .withSecurityScope,
    relativeTo: nil,
    bookmarkDataIsStale: &isStale
  ).standardizedFileURL
  let scopedPath = scopedURL.path
  let requestedPath = requestedURL.path
  return JxlFileAccess(
    fileURL: requestedPath == scopedPath ? scopedURL : requestedURL,
    securityScopedURL: scopedURL
  )
}
#else
private func fileAccess(
  path: String,
  securityScopedBookmark: Data?
) throws -> JxlFileAccess {
  guard securityScopedBookmark == nil else {
    throw invalidPathBookmarkError(
      "Security-scoped bookmarks are supported only on macOS"
    )
  }
  return JxlFileAccess(
    fileURL: URL(fileURLWithPath: path).standardizedFileURL,
    securityScopedURL: nil
  )
}
#endif

func withJxlSecurityScopedAccess<T>(
  _ files: [JxlFileAccess],
  startAccessing: (URL) -> Bool = {
    $0.startAccessingSecurityScopedResource()
  },
  stopAccessing: (URL) -> Void = {
    $0.stopAccessingSecurityScopedResource()
  },
  operation: () throws -> T
) throws -> T {
  var uniqueURLs: [URL] = []
  for file in files {
    if let url = file.securityScopedURL, !uniqueURLs.contains(url) {
      uniqueURLs.append(url)
    }
  }
  var accessedURLs: [URL] = []
  defer {
    for url in accessedURLs.reversed() {
      stopAccessing(url)
    }
  }
  for url in uniqueURLs {
    guard startAccessing(url) else {
      throw deniedPathBookmarkError(url)
    }
    accessedURLs.append(url)
  }
  for file in files {
    guard let scopedURL = file.securityScopedURL else { continue }
    let scopedPath = scopedURL.path
    let requestedPath = file.fileURL.path
    if requestedPath == scopedPath { continue }
    var isDirectory: ObjCBool = false
    let exists = FileManager.default.fileExists(
      atPath: scopedPath,
      isDirectory: &isDirectory
    )
    let scopedPrefix = scopedPath == "/" ? "/" : scopedPath + "/"
    guard exists,
          isDirectory.boolValue,
          requestedPath.hasPrefix(scopedPrefix) else {
      throw invalidPathBookmarkError(
        "The security-scoped bookmark does not contain the requested path"
      )
    }
  }
  return try operation()
}

private func securityScopedBookmarks(
  from arguments: [Any],
  unscopedCount: Int,
  endpointCount: Int
) -> ([Any], [Data?])? {
  if arguments.count == unscopedCount {
    return (arguments, Array(repeating: nil, count: endpointCount))
  }
  guard arguments.count == unscopedCount + 1,
        let configuration = arguments.last as? [String: Any],
        configuration.count == 1,
        let values = configuration["securityScopedBookmarks"] as? [Any],
        values.count == endpointCount else {
    return nil
  }
  var bookmarks: [Data?] = []
  for value in values {
    if value is NSNull {
      bookmarks.append(nil)
    } else if let data = value as? FlutterStandardTypedData {
      bookmarks.append(data.data)
    } else {
      return nil
    }
  }
  return (Array(arguments.dropLast()), bookmarks)
}

func jxlCoderFlutterErrorCode(_ error: NSError, fallback: String) -> String {
  if error.domain == NSCocoaErrorDomain || error.domain == NSPOSIXErrorDomain {
    return "IO_ERROR"
  }
  guard error.domain == JxlCoderErrorDomain else { return fallback }
  switch error.code {
  case JxlCoderError.invalidArguments.rawValue:
    return "INVALID_ARGUMENTS"
  case JxlCoderError.unsupportedInput.rawValue:
    return "UNSUPPORTED_INPUT"
  case JxlCoderError.timeout.rawValue:
    return "TIMEOUT"
  case JxlCoderError.schedulerAlreadyStarted.rawValue:
    return "SCHEDULER_ALREADY_STARTED"
  case JxlCoderError.schedulerUnavailable.rawValue:
    return "SCHEDULER_ERROR"
  case 500:
    return "IO_ERROR"
  default:
    return fallback
  }
}

public class JxlCoderPlugin: NSObject, FlutterPlugin {
  private let scheduler: ConversionScheduler

  public override init() {
    scheduler = .shared
    super.init()
  }

  init(scheduler: ConversionScheduler) {
    self.scheduler = scheduler
    super.init()
  }

  public static func register(with registrar: FlutterPluginRegistrar) {
#if os(macOS)
    let messenger = registrar.messenger
#else
    let messenger = registrar.messenger()
#endif
    let channel = FlutterMethodChannel(
      name: "jxl_coder",
      binaryMessenger: messenger
    )
    registrar.addMethodCallDelegate(JxlCoderPlugin(), channel: channel)
  }

  public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    switch call.method {
    case "configureJxlScheduler":
      guard let arguments = call.arguments as? [Any],
            arguments.count == 2,
            let workerCount = arguments[0] as? Int,
            let maxActiveConversions = arguments[1] as? Int,
            (0...256).contains(workerCount),
            (0...256).contains(maxActiveConversions) else {
        invalidArguments("Valid scheduler limits are required", result)
        return
      }
      do {
        try scheduler.configure(
          workerCount: workerCount,
          maxActiveConversions: maxActiveConversions
        )
        result(nil)
      } catch {
        finish(error: error, fallbackCode: "SCHEDULER_ERROR", result)
      }

    case "jpegBytesToJxl":
      guard let arguments = call.arguments as? [Any],
            arguments.count == 5,
            let data = arguments[0] as? FlutterStandardTypedData,
            let effort = arguments[1] as? Int,
            let decodingSpeed = arguments[2] as? Int,
            let priority = arguments[3] as? Int,
            let timeout = arguments[4] as? Int,
            validEncode(effort, decodingSpeed, priority, timeout) else {
        invalidArguments("JPEG data and valid options are required", result)
        return
      }
      run(
        priority: priority,
        timeout: timeout,
        result: result,
        fallbackCode: "TRANSCODE_ERROR"
      ) {
        started, group in
        let remaining = try self.remainingTimeout(
          timeout,
          since: started,
          message: "JPEG XL encoding timed out"
        )
        return FlutterStandardTypedData(bytes: try JxlCoderCore.transcode(
          data.data,
          effort: effort,
          decodingSpeed: decodingSpeed,
          priority: priority,
          schedulingGroup: group,
          timeoutMilliseconds: remaining
        ))
      }

    case "jxlBytesToJpeg":
      guard let arguments = call.arguments as? [Any],
            arguments.count == 3,
            let data = arguments[0] as? FlutterStandardTypedData,
            let priority = arguments[1] as? Int,
            let timeout = arguments[2] as? Int,
            validPerformance(priority, timeout) else {
        invalidArguments("JXL data and valid options are required", result)
        return
      }
      run(
        priority: priority,
        timeout: timeout,
        result: result,
        fallbackCode: "INVERSE_ERROR"
      ) {
        started, group in
        let remaining = try self.remainingTimeout(
          timeout,
          since: started,
          message: "JPEG reconstruction timed out"
        )
        return FlutterStandardTypedData(bytes: try JxlCoderCore.inverse(
          data.data,
          priority: priority,
          schedulingGroup: group,
          timeoutMilliseconds: remaining
        ))
      }

    case "jpegPathToJxl":
      guard let arguments = call.arguments as? [Any],
            let (values, bookmarks) = securityScopedBookmarks(
              from: arguments,
              unscopedCount: 6,
              endpointCount: 2
            ),
            let inputPath = values[0] as? String,
            let outputPath = values[1] as? String,
            let effort = values[2] as? Int,
            let decodingSpeed = values[3] as? Int,
            let priority = values[4] as? Int,
            let timeout = values[5] as? Int,
            validEncode(effort, decodingSpeed, priority, timeout) else {
        invalidArguments("Input path, output path, and valid options are required", result)
        return
      }
      run(
        priority: priority,
        timeout: timeout,
        result: result,
        fallbackCode: "TRANSCODE_ERROR"
      ) {
        started, group in
        let inputFile = try fileAccess(
          path: inputPath,
          securityScopedBookmark: bookmarks[0]
        )
        let outputFile = try fileAccess(
          path: outputPath,
          securityScopedBookmark: bookmarks[1]
        )
        return try withJxlSecurityScopedAccess([inputFile, outputFile]) {
          _ = try self.remainingTimeout(
            timeout,
            since: started,
            message: "JPEG XL encoding timed out"
          )
          let input = try Data(
            contentsOf: inputFile.fileURL,
            options: .mappedIfSafe
          )
          let remaining = try self.remainingTimeout(
            timeout,
            since: started,
            message: "JPEG XL encoding timed out"
          )
          let output = try JxlCoderCore.transcode(
            input,
            effort: effort,
            decodingSpeed: decodingSpeed,
            priority: priority,
            schedulingGroup: group,
            timeoutMilliseconds: remaining
          )
          _ = try self.remainingTimeout(
            timeout,
            since: started,
            message: "JPEG XL encoding timed out"
          )
          try output.write(to: outputFile.fileURL, options: .atomic)
          return nil
        }
      }

    case "jxlPathToJpeg":
      guard let arguments = call.arguments as? [Any],
            let (values, bookmarks) = securityScopedBookmarks(
              from: arguments,
              unscopedCount: 4,
              endpointCount: 2
            ),
            let inputPath = values[0] as? String,
            let outputPath = values[1] as? String,
            let priority = values[2] as? Int,
            let timeout = values[3] as? Int,
            validPerformance(priority, timeout) else {
        invalidArguments("Input path, output path, and valid options are required", result)
        return
      }
      run(
        priority: priority,
        timeout: timeout,
        result: result,
        fallbackCode: "INVERSE_ERROR"
      ) {
        started, group in
        let inputFile = try fileAccess(
          path: inputPath,
          securityScopedBookmark: bookmarks[0]
        )
        let outputFile = try fileAccess(
          path: outputPath,
          securityScopedBookmark: bookmarks[1]
        )
        return try withJxlSecurityScopedAccess([inputFile, outputFile]) {
          _ = try self.remainingTimeout(
            timeout,
            since: started,
            message: "JPEG reconstruction timed out"
          )
          let input = try Data(
            contentsOf: inputFile.fileURL,
            options: .mappedIfSafe
          )
          let remaining = try self.remainingTimeout(
            timeout,
            since: started,
            message: "JPEG reconstruction timed out"
          )
          let output = try JxlCoderCore.inverse(
            input,
            priority: priority,
            schedulingGroup: group,
            timeoutMilliseconds: remaining
          )
          _ = try self.remainingTimeout(
            timeout,
            since: started,
            message: "JPEG reconstruction timed out"
          )
          try output.write(to: outputFile.fileURL, options: .atomic)
          return nil
        }
      }

    case "jpegPathsToJxl":
      guard let arguments = call.arguments as? [Any] else {
        invalidArguments("Batch options and path pairs are required", result)
        return
      }
      let configurationCount = arguments.last is [String: Any] ? 1 : 0
      guard arguments.count >= 4 + configurationCount,
            (arguments.count - 4 - configurationCount).isMultiple(of: 2),
            let (values, bookmarks) = securityScopedBookmarks(
              from: arguments,
              unscopedCount: arguments.count - configurationCount,
              endpointCount: arguments.count - 4 - configurationCount
            ),
            let effort = values[0] as? Int,
            let decodingSpeed = values[1] as? Int,
            let priority = values[2] as? Int,
            let timeout = values[3] as? Int,
            let paths = Array(values.dropFirst(4)) as? [String],
            validEncode(effort, decodingSpeed, priority, timeout) else {
        invalidArguments("Batch options and path pairs are required", result)
        return
      }
      runBatch(
        paths: paths,
        bookmarks: bookmarks,
        priority: priority,
        result: result,
        fallbackCode: "TRANSCODE_ERROR"
      ) { inputPath, outputPath, group, started in
          try withJxlSecurityScopedAccess([inputPath, outputPath]) {
            let input = try Data(
              contentsOf: inputPath.fileURL,
              options: .mappedIfSafe
            )
            let remaining = try self.remainingTimeout(
              timeout,
              since: started,
              message: "JPEG XL encoding timed out"
            )
            let output = try JxlCoderCore.transcode(
              input,
              effort: effort,
              decodingSpeed: decodingSpeed,
              priority: priority,
              schedulingGroup: group,
              timeoutMilliseconds: remaining
            )
            _ = try self.remainingTimeout(
              timeout,
              since: started,
              message: "JPEG XL encoding timed out"
            )
            try output.write(to: outputPath.fileURL, options: .atomic)
          }
      }

    case "jxlPathsToJpeg":
      guard let arguments = call.arguments as? [Any] else {
        invalidArguments("Batch options and path pairs are required", result)
        return
      }
      let configurationCount = arguments.last is [String: Any] ? 1 : 0
      guard arguments.count >= 2 + configurationCount,
            (arguments.count - 2 - configurationCount).isMultiple(of: 2),
            let (values, bookmarks) = securityScopedBookmarks(
              from: arguments,
              unscopedCount: arguments.count - configurationCount,
              endpointCount: arguments.count - 2 - configurationCount
            ),
            let priority = values[0] as? Int,
            let timeout = values[1] as? Int,
            let paths = Array(values.dropFirst(2)) as? [String],
            validPerformance(priority, timeout) else {
        invalidArguments("Batch options and path pairs are required", result)
        return
      }
      runBatch(
        paths: paths,
        bookmarks: bookmarks,
        priority: priority,
        result: result,
        fallbackCode: "INVERSE_ERROR"
      ) { inputPath, outputPath, group, started in
          try withJxlSecurityScopedAccess([inputPath, outputPath]) {
            let input = try Data(
              contentsOf: inputPath.fileURL,
              options: .mappedIfSafe
            )
            let remaining = try self.remainingTimeout(
              timeout,
              since: started,
              message: "JPEG reconstruction timed out"
            )
            let output = try JxlCoderCore.inverse(
              input,
              priority: priority,
              schedulingGroup: group,
              timeoutMilliseconds: remaining
            )
            _ = try self.remainingTimeout(
              timeout,
              since: started,
              message: "JPEG reconstruction timed out"
            )
            try output.write(to: outputPath.fileURL, options: .atomic)
          }
      }

    default:
      result(FlutterMethodNotImplemented)
    }
  }

  private func validEncode(
    _ effort: Int,
    _ decodingSpeed: Int,
    _ priority: Int,
    _ timeout: Int
  ) -> Bool {
    (1...9).contains(effort) &&
      (0...4).contains(decodingSpeed) &&
      validPerformance(priority, timeout)
  }

  private func validPerformance(_ priority: Int, _ timeout: Int) -> Bool {
    (0...2).contains(priority) && timeout >= 0
  }

  private func remainingTimeout(
    _ timeout: Int,
    since started: UInt64,
    message: String
  ) throws -> Int {
    guard timeout > 0 else { return 0 }
    guard let deadline = monotonicDeadline(
      since: started,
      timeoutMilliseconds: timeout
    ) else { return 0 }
    let now = monotonicNow()
    guard now < deadline else { throw codecTimeoutError(message) }
    let remainingNanoseconds = deadline - now
    let remainingMilliseconds =
      remainingNanoseconds / nanosecondsPerMillisecond +
      (remainingNanoseconds % nanosecondsPerMillisecond == 0 ? 0 : 1)
    return remainingMilliseconds > UInt64(Int.max)
      ? Int.max
      : Int(remainingMilliseconds)
  }

  private func run(
    priority: Int,
    timeout: Int,
    result: @escaping FlutterResult,
    fallbackCode: String,
    operation: @escaping (UInt64, UInt64) throws -> Any?
  ) {
    let submitted = monotonicNow()
    let group = scheduler.makeGroup()
    scheduler.submit(
      priority: priority,
      group: group,
      deadline: monotonicDeadline(
        since: submitted,
        timeoutMilliseconds: timeout
      ),
      onFailure: {
        self.finish(error: $0, fallbackCode: fallbackCode, result)
      }
    ) { _ in
      autoreleasepool {
        do {
          let value = try operation(submitted, group)
          DispatchQueue.main.async { result(value) }
        } catch {
          self.finish(error: error, fallbackCode: fallbackCode, result)
        }
      }
    }
  }

  private func invalidArguments(_ message: String, _ result: FlutterResult) {
    result(FlutterError(code: "INVALID_ARGUMENTS", message: message, details: nil))
  }

  private func finish(
    error: Error,
    fallbackCode: String,
    _ result: @escaping FlutterResult
  ) {
    let native = error as NSError
    let flutterError = FlutterError(
      code: jxlCoderFlutterErrorCode(native, fallback: fallbackCode),
      message: native.localizedDescription,
      details: nil
    )
    DispatchQueue.main.async { result(flutterError) }
  }

  private func runBatch(
    paths: [String],
    bookmarks: [Data?],
    priority: Int,
    result: @escaping FlutterResult,
    fallbackCode: String,
    operation: @escaping (JxlFileAccess, JxlFileAccess, UInt64, UInt64) throws -> Void
  ) {
    let pairCount = paths.count / 2
    guard pairCount > 0 else {
      result(nil)
      return
    }
    let group = scheduler.makeGroup()
    let completion = BatchCompletion(count: pairCount) { errors in
      if let firstError = errors.compactMap({ $0 }).first {
        self.finish(
          error: firstError,
          fallbackCode: fallbackCode,
          result
        )
      } else {
        DispatchQueue.main.async { result(nil) }
      }
    }

    for index in 0..<pairCount {
      scheduler.submit(
        priority: priority,
        group: group,
        onFailure: { completion.finish(index: index, error: $0) }
      ) { started in
        autoreleasepool {
          do {
            let inputFile = try fileAccess(
              path: paths[index * 2],
              securityScopedBookmark: bookmarks[index * 2]
            )
            let outputFile = try fileAccess(
              path: paths[index * 2 + 1],
              securityScopedBookmark: bookmarks[index * 2 + 1]
            )
            try operation(
              inputFile,
              outputFile,
              group,
              started
            )
            completion.finish(index: index, error: nil)
          } catch {
            completion.finish(index: index, error: error)
          }
        }
      }
    }
  }
}
