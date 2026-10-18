# Builds the updater's HTTP client and shorebird_http_probe outside the
# engine, for the shorebird_http_client workflow, which runs the probe on
# Linux, macOS and Windows runners and diffs it against dart:io. The engine
# build (BUILD.gn) is the real one; this exists because engine CI doesn't
# run on desktop runners.
#
#   cmake -S . -B out -DBORINGSSL_DIR=<boringssl checkout>
#   cmake --build out --config Release

cmake_minimum_required(VERSION 3.22)
project(shorebird_http_standalone C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BORINGSSL_DIR "" CACHE PATH "A BoringSSL checkout")
add_subdirectory(${BORINGSSL_DIR} boringssl EXCLUDE_FROM_ALL)

# engine/src, for the "flutter/..." includes.
get_filename_component(SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(SHOREBIRD ${SRC_ROOT}/flutter/shell/common/shorebird)
set(FML ${SRC_ROOT}/flutter/fml)

set(TRUST ${SHOREBIRD}/http_trust_linux.cc)
if(APPLE)
  set(TRUST ${SHOREBIRD}/http_trust_apple.cc)
elseif(WIN32)
  set(TRUST ${SHOREBIRD}/http_trust_win.cc)
endif()

add_executable(shorebird_http_probe
  ${SHOREBIRD}/http_probe.cc
  ${SHOREBIRD}/http_client.cc
  ${SHOREBIRD}/http_parser.cc
  ${TRUST}
  ${FML}/logging.cc
  ${FML}/log_settings.cc
  ${FML}/log_settings_state.cc
)
target_include_directories(shorebird_http_probe PRIVATE ${SRC_ROOT})
target_link_libraries(shorebird_http_probe PRIVATE ssl crypto)
if(APPLE)
  target_link_libraries(shorebird_http_probe PRIVATE
    "-framework CoreFoundation" "-framework Security")
elseif(WIN32)
  target_compile_definitions(shorebird_http_probe PRIVATE NOMINMAX
    WIN32_LEAN_AND_MEAN)
  target_link_libraries(shorebird_http_probe PRIVATE ws2_32 crypt32)
endif()
