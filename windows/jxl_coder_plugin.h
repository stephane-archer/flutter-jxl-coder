#ifndef FLUTTER_PLUGIN_JXL_CODER_PLUGIN_H_
#define FLUTTER_PLUGIN_JXL_CODER_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <memory>

namespace jxl_coder {

class JxlCoderPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(
      flutter::PluginRegistrarWindows* registrar);

  JxlCoderPlugin() = default;
  ~JxlCoderPlugin() override = default;

  JxlCoderPlugin(const JxlCoderPlugin&) = delete;
  JxlCoderPlugin& operator=(const JxlCoderPlugin&) = delete;

 private:
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
};

}  // namespace jxl_coder

#endif  // FLUTTER_PLUGIN_JXL_CODER_PLUGIN_H_
