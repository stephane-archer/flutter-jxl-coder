#include "include/jxl_coder/jxl_coder_plugin_c_api.h"

#include <flutter/plugin_registrar_windows.h>

#include "jxl_coder_plugin.h"

void JxlCoderPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  jxl_coder::JxlCoderPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
