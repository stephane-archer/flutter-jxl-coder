cmake_minimum_required(VERSION 3.15)

project(jxl_coder_native_tests LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "The standalone native test target requires MSVC on Windows")
endif()

set(REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(WINDOWS_ROOT "${REPOSITORY_ROOT}/windows")
set(NATIVE_LIBRARY_DIRECTORY "${WINDOWS_ROOT}/third_party/lib/x64")
set(FLUTTER_CPP_HEADERS "" CACHE PATH
  "Flutter C++ client-wrapper include directory")

if(NOT EXISTS "${FLUTTER_CPP_HEADERS}/flutter/encodable_value.h" OR
   NOT EXISTS "${FLUTTER_CPP_HEADERS}/flutter/method_result.h")
  message(FATAL_ERROR
    "FLUTTER_CPP_HEADERS must contain Flutter's portable C++ wrapper headers")
endif()

add_executable(jxl_coder_native_test
  "${CMAKE_CURRENT_LIST_DIR}/jxl_codec_test.cc"
  "${REPOSITORY_ROOT}/native/src/jxl_codec.cc"
  "${REPOSITORY_ROOT}/native/src/shared_parallel_runner.cc"
)

add_executable(jxl_coder_windows_scheduler_test
  "${CMAKE_CURRENT_LIST_DIR}/windows_scheduler_test.cc"
  "${REPOSITORY_ROOT}/native/src/shared_parallel_runner.cc"
)

add_executable(jxl_coder_windows_adapter_test
  "${CMAKE_CURRENT_LIST_DIR}/windows_adapter_test.cc"
  "${REPOSITORY_ROOT}/native/src/jxl_codec.cc"
  "${REPOSITORY_ROOT}/native/src/shared_parallel_runner.cc"
)

set_target_properties(jxl_coder_native_test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES
  MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
set_target_properties(jxl_coder_windows_scheduler_test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES
  MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
set_target_properties(jxl_coder_windows_adapter_test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES
  MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
target_compile_options(jxl_coder_native_test PRIVATE /W4 /EHsc)
target_compile_options(jxl_coder_windows_scheduler_test PRIVATE /W4 /EHsc)
target_compile_options(jxl_coder_windows_adapter_test PRIVATE /W4 /EHsc)
target_compile_definitions(jxl_coder_native_test PRIVATE
  JXL_CODER_ENABLE_TEST_HOOKS=1
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  _ALLOW_MSC_VER_MISMATCH
  _HAS_EXCEPTIONS=1
  _ITERATOR_DEBUG_LEVEL=0
)
target_compile_definitions(jxl_coder_windows_scheduler_test PRIVATE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  _ALLOW_MSC_VER_MISMATCH
  _HAS_EXCEPTIONS=1
  _ITERATOR_DEBUG_LEVEL=0
)
target_compile_definitions(jxl_coder_windows_adapter_test PRIVATE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  _ALLOW_MSC_VER_MISMATCH
  _HAS_EXCEPTIONS=1
  _ITERATOR_DEBUG_LEVEL=0
)
target_include_directories(jxl_coder_native_test PRIVATE
  "${WINDOWS_ROOT}/third_party/include"
  "${REPOSITORY_ROOT}/native/src"
)
target_include_directories(jxl_coder_windows_scheduler_test PRIVATE
  "${WINDOWS_ROOT}/third_party/include"
  "${REPOSITORY_ROOT}/native/src"
)
target_include_directories(jxl_coder_windows_adapter_test PRIVATE
  "${FLUTTER_CPP_HEADERS}"
  "${WINDOWS_ROOT}/third_party/include"
  "${REPOSITORY_ROOT}/native/src"
)
target_link_libraries(jxl_coder_native_test PRIVATE
  "${NATIVE_LIBRARY_DIRECTORY}/jxl.lib"
  "${NATIVE_LIBRARY_DIRECTORY}/jxl_cms.lib"
  "${NATIVE_LIBRARY_DIRECTORY}/hwy.lib"
  "${NATIVE_LIBRARY_DIRECTORY}/brotlienc.lib"
  "${NATIVE_LIBRARY_DIRECTORY}/brotlidec.lib"
  "${NATIVE_LIBRARY_DIRECTORY}/brotlicommon.lib"
)
target_link_libraries(jxl_coder_windows_adapter_test PRIVATE
  "${NATIVE_LIBRARY_DIRECTORY}/jxl.lib"
  "${NATIVE_LIBRARY_DIRECTORY}/jxl_cms.lib"
  "${NATIVE_LIBRARY_DIRECTORY}/hwy.lib"
  "${NATIVE_LIBRARY_DIRECTORY}/brotlienc.lib"
  "${NATIVE_LIBRARY_DIRECTORY}/brotlidec.lib"
  "${NATIVE_LIBRARY_DIRECTORY}/brotlicommon.lib"
)

enable_testing()
add_test(
  NAME jxl_coder_native_test
  COMMAND jxl_coder_native_test
    "${REPOSITORY_ROOT}/example/integration_test/2.jpg"
    "${REPOSITORY_ROOT}/example/integration_test/1.jxl"
)
add_test(
  NAME jxl_coder_native_lazy_configuration_test
  COMMAND jxl_coder_native_test --lazy-configuration
)
add_test(
  NAME jxl_coder_native_configuration_race_test
  COMMAND jxl_coder_native_test --configuration-race
)
add_test(
  NAME jxl_coder_native_configuration_first_use_race_test
  COMMAND jxl_coder_native_test --configuration-first-use-race
)
add_test(
  NAME jxl_coder_windows_scheduler_test
  COMMAND jxl_coder_windows_scheduler_test
)
add_test(
  NAME jxl_coder_windows_adapter_test
  COMMAND jxl_coder_windows_adapter_test
    "${REPOSITORY_ROOT}/example/integration_test/2.jpg"
    "${REPOSITORY_ROOT}/example/integration_test/1.jxl"
)
add_test(
  NAME jxl_coder_windows_scheduler_lazy_test
  COMMAND jxl_coder_windows_scheduler_test --lazy
)
add_test(
  NAME jxl_coder_windows_scheduler_automatic_test
  COMMAND jxl_coder_windows_scheduler_test --automatic
)
add_test(
  NAME jxl_coder_windows_scheduler_configuration_race_test
  COMMAND jxl_coder_windows_scheduler_test --configuration-race
)
add_test(
  NAME jxl_coder_windows_scheduler_configuration_first_use_race_test
  COMMAND jxl_coder_windows_scheduler_test --configuration-first-use-race
)
add_test(
  NAME jxl_coder_windows_scheduler_configuration_conflict_test
  COMMAND jxl_coder_windows_scheduler_test --configure-conflict
)
add_test(
  NAME jxl_coder_windows_scheduler_lazy_conflict_test
  COMMAND jxl_coder_windows_scheduler_test --lazy-conflict
)
