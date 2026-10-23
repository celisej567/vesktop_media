cmake_minimum_required(VERSION 3.20)

# Use: cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake (scripts/build.sh does all of it)
project(vesktop_media VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(VV_DEPS_DIR "${CMAKE_BINARY_DIR}/deps" CACHE PATH "mlspp + libdave install prefix (scripts/build.sh)")
set(VV_NODE_MODULES "${CMAKE_SOURCE_DIR}/node_modules")
if(NOT EXISTS "${VV_NODE_MODULES}/node-addon-api")
    set(VV_NODE_MODULES "${CMAKE_SOURCE_DIR}/../../node_modules") # Vesktop's (hoisted)
endif()

add_library(vesktop_media MODULE
    src/addon.cc
    src/common/js.cc
    src/common/log.cc
    src/common/platform.cc
    src/audio/adm.cc
    src/audio/deepfilter.cc
    src/audio/mixer.cc
    src/audio/rnnoise.cc
    src/audio/soundshare.cc
    src/audio/volume_analyzer.cc
    src/engine/engine.cc
    src/connection/connection.cc
    src/connection/connection_stats.cc
    src/connection/video_send.cc
    src/capture/camera.cc
    src/capture/gio_api.cc
    src/capture/pipewire_capturer.cc
    src/capture/portal.cc
    src/dave/dave_session.cc
    src/transport/crypto.cc
    src/transport/region_tester.cc
    src/transport/udp_socket.cc
    src/video/decoder_factory.cc
    src/video/ffmpeg.cc
    src/video/ffmpeg_encoder.cc
    src/video/video_sinks.cc
    src/video/frame_source.cc
)

set_target_properties(vesktop_media PROPERTIES
    PREFIX ""
    OUTPUT_NAME vesktop_media   # Vesktop loads <dir>/vesktop_media.node next to index.js
    SUFFIX ".node"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")

target_compile_definitions(vesktop_media PRIVATE
    NAPI_VERSION=8
    NAPI_CPP_EXCEPTIONS
    NODE_ADDON_API_DISABLE_DEPRECATED
    VESKTOP_MEDIA_VERSION="${PROJECT_VERSION}"
    WEBRTC_POSIX WEBRTC_LINUX
    WITH_BORINGSSL)
target_compile_options(vesktop_media PRIVATE -fexceptions -Wall -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers)

target_include_directories(vesktop_media PRIVATE
    src
    "${VV_NODE_MODULES}/node-addon-api"
    "${VV_NODE_MODULES}/node-api-headers/include"
    "${VV_WEBRTC_DIR}/include"
    "${VV_WEBRTC_DIR}/include/third_party/abseil-cpp"
    "${VV_WEBRTC_DIR}/include/third_party/boringssl/src/include"
    "${VV_WEBRTC_DIR}/include/third_party/libyuv/include"
    "${VV_DEPS_DIR}/include")
# nlohmann/json: header only, from the system
find_path(NLOHMANN_INCLUDE nlohmann/json.hpp REQUIRED)
# GLib / GIO and PipeWire: headers only, the libraries are loaded at runtime (src/capture)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED gio-unix-2.0)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
target_include_directories(vesktop_media SYSTEM PRIVATE ${GIO_INCLUDE_DIRS} ${PIPEWIRE_INCLUDE_DIRS})
target_include_directories(vesktop_media SYSTEM PRIVATE "${NLOHMANN_INCLUDE}")

target_link_libraries(vesktop_media PRIVATE
    "${VV_DEPS_DIR}/lib/libdave.a"
    "${VV_DEPS_DIR}/lib/libmlspp.a"
    "${VV_DEPS_DIR}/lib/libmls_ds.a"
    "${VV_DEPS_DIR}/lib/libhpke.a"
    "${VV_DEPS_DIR}/lib/libtls_syntax.a"
    "${VV_DEPS_DIR}/lib/libbytes.a"
    "${VV_WEBRTC_DIR}/lib/libwebrtc.a"
    pthread dl m)
# Node-API symbols come from the host process; keep our own (and libwebrtc's libc++) private
target_link_options(vesktop_media PRIVATE -Wl,--exclude-libs,ALL -Wl,-z,undefs -Wl,--gc-sections -static-libgcc)

add_custom_command(TARGET vesktop_media POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/index.js" "${CMAKE_BINARY_DIR}/out/index.js")
