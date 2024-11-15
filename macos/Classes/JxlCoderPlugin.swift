import Cocoa
import FlutterMacOS
import JxlCoder

public class JxlCoderPlugin: NSObject, FlutterPlugin {
    public static func register(with registrar: FlutterPluginRegistrar) {
        let channel = FlutterMethodChannel(name: "jxl_coder", binaryMessenger: registrar.messenger)
        let instance = JxlCoderPlugin()
        registrar.addMethodCallDelegate(instance, channel: channel)
    }
    
    public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        switch call.method {
        case "jpegToJxl":
            if let args = call.arguments as? [String: Any],
               let jpegData = args["jpegData"] as? FlutterStandardTypedData {
                do {
                    let transcoded = try JXLCoder.transcode(jpegData: jpegData.data)
                    result(FlutterStandardTypedData(bytes: transcoded))
                } catch {
                    result(FlutterError(code: "TRANSCODE_ERROR", message: "Failed to transcode JPEG to JXL", details: error.localizedDescription))
                }
            } else {
                result(FlutterError(code: "INVALID_ARGUMENTS", message: "jpegData is required", details: nil))
            }
        case "jxlToJpeg":
            if let args = call.arguments as? [String: Any],
               let jxlData = args["jxlData"] as? FlutterStandardTypedData {
                do {
                    let jpegData = try JXLCoder.inverse(jxlData: jxlData.data)
                    result(FlutterStandardTypedData(bytes: jpegData))
                } catch {
                    result(FlutterError(code: "INVERSE_ERROR", message: "Failed to convert JXL to JPEG", details: error.localizedDescription))
                }
            } else {
                result(FlutterError(code: "INVALID_ARGUMENTS", message: "jxlData is required", details: nil))
            }
        case "getPlatformVersion":
            result("macOS " + ProcessInfo.processInfo.operatingSystemVersionString)
        default:
            result(FlutterMethodNotImplemented)
        }
    }
}
