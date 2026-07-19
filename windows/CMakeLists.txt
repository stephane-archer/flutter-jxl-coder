cmake_minimum_required(VERSION 3.14)

set(PROJECT_NAME "jxl_coder")
project(${PROJECT_NAME} LANGUAGES CXX)
cmake_policy(VERSION 3.14...3.25)

set(PLUGIN_NAME "jxl_coder_plugin")
set(NATIVE_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lib/x64")
set(FLUTTER_WRAPPER_ROOT
  "${FLUTTER_MANAGED_DIR}/ephemeral/cpp_client_wrapper")

if(CMAKE_GENERATOR_PLATFORM MATCHES "ARM64|Win32")
  message(FATAL_ERROR "jxl_coder 1.0.0 ships Windows x64 artifacts only")
endif()

add_library(${PLUGIN_NAME} SHARED
  "include/jxl_coder/jxl_coder_plugin_c_api.h"
  "jxl_coder_plugin_c_api.cpp"
  "jxl_coder_plugin.cpp"
  "jxl_coder_plugin.h"
  "${FLUTTER_WRAPPER_ROOT}/core_implementations.cc"
  "${FLUTTER_WRAPPER_ROOT}/standard_codec.cc"
  "${FLUTTER_WRAPPER_ROOT}/plugin_registrar.cc"
  "../native/src/jxl_codec.cc"
  "../native/src/jxl_codec.h"
  "../native/src/shared_parallel_runner.cc"
  "../native/src/shared_parallel_runner.h"
)
add_dependencies(${PLUGIN_NAME} flutter_assemble)

# The official static release uses ClangCL's VS2015-compatible STL ABI and the
# static multithreaded CRT. Compile the small Flutter C++ wrapper into this DLL
# with the same runtime instead of linking Flutter's shared debug-CRT wrapper.
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES
  MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
target_compile_features(${PLUGIN_NAME} PUBLIC cxx_std_17)
target_compile_options(${PLUGIN_NAME} PRIVATE /W4 /wd"4100" /EHsc)
target_compile_definitions(${PLUGIN_NAME} PRIVATE
  FLUTTER_PLUGIN_IMPL
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  _ALLOW_MSC_VER_MISMATCH
  _HAS_EXCEPTIONS=1
  _ITERATOR_DEBUG_LEVEL=0
  "$<$<CONFIG:Debug>:_DEBUG>"
)
target_include_directories(${PLUGIN_NAME}
  INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/src"
    "${FLUTTER_WRAPPER_ROOT}/include"
)
target_link_libraries(${PLUGIN_NAME} PRIVATE
  flutter
  "${NATIVE_LIB_DIR}/jxl.lib"
  "${NATIVE_LIB_DIR}/jxl_cms.lib"
  "${NATIVE_LIB_DIR}/hwy.lib"
  "${NATIVE_LIB_DIR}/brotlienc.lib"
  "${NATIVE_LIB_DIR}/brotlidec.lib"
  "${NATIVE_LIB_DIR}/brotlicommon.lib"
)

set(jxl_coder_bundled_libraries "" PARENT_SCOPE)
