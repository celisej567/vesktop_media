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

set(VV_SOURCES
    src/addon.cc
    src/common/js.cc
    src/common/log.cc
    src/audio/adm.cc
    src/audio/deepfilter.cc
    src/audio/mixer.cc
    src/audio/rnnoise.cc
    src/audio/volume_analyzer.cc
    src/engine/engine.cc
    src/connection/connection.cc
    src/connection/connection_stats.cc
    src/connection/video_send.cc
    src/capture/camera.cc
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
if(WIN32)
    list(APPEND VV_SOURCES
        src/common/platform_win.cc
        src/common/delay_load_hook_win.cc
        src/common/watchdog_win.cc
        src/audio/audio_apps_win.cc
        src/audio/soundshare_win.cc
        src/capture/camera_win.cc
        src/capture/desktop_capture_win.cc
        src/capture/portal_win.cc)
else()
    list(APPEND VV_SOURCES
        src/common/platform.cc
        src/audio/soundshare.cc
        src/capture/camera_v4l2.cc
        src/capture/gio_api.cc
        src/capture/pipewire_capturer.cc
        src/capture/portal.cc)
endif()

add_library(vesktop_media MODULE ${VV_SOURCES})

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
    WITH_BORINGSSL)
if(WIN32)
    # the Windows headers as libwebrtc is compiled against them (Chromium's build/config/win)
    target_compile_definitions(vesktop_media PRIVATE WEBRTC_WIN RTC_ENABLE_WIN_WGC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
        _WIN32_WINNT=0x0A00 WINVER=0x0A00 _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
    # clang-cl's -Wall is -Weverything; /W3 is clang's -Wall
    # line tables in a PDB next to the build (not shipped): the watchdog's stacks (src/common/watchdog_win.cc) are
    # module offsets, symbolized with it
    target_compile_options(vesktop_media PRIVATE /EHsc /W3 -Wno-unused-parameter -Wno-unused-function
        -Wno-missing-field-initializers -gline-tables-only -gcodeview)
else()
    target_compile_definitions(vesktop_media PRIVATE WEBRTC_POSIX WEBRTC_LINUX)
    target_compile_options(vesktop_media PRIVATE -fexceptions -Wall -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers)
endif()

target_include_directories(vesktop_media PRIVATE
    src
    "${VV_NODE_MODULES}/node-addon-api"
    "${VV_NODE_MODULES}/node-api-headers/include"
    "${VV_WEBRTC_DIR}/include"
    "${VV_WEBRTC_DIR}/include/third_party/abseil-cpp"
    "${VV_WEBRTC_DIR}/include/third_party/boringssl/src/include"
    "${VV_WEBRTC_DIR}/include/third_party/libyuv/include"
    "${VV_DEPS_DIR}/include")
# nlohmann/json: header only, from the system (Linux) or installed into the deps by scripts/build.ps1 (Windows)
find_path(NLOHMANN_INCLUDE nlohmann/json.hpp HINTS "${VV_DEPS_DIR}/include" REQUIRED)
target_include_directories(vesktop_media SYSTEM PRIVATE "${NLOHMANN_INCLUDE}")

if(WIN32)
    # Node-API comes from the host executable: an import library for "node.exe" made from node-api-headers' def file,
    # delay-loaded and redirected to the executable (src/common/delay_load_hook_win.cc)
    set(VV_NODE_LIB "${CMAKE_BINARY_DIR}/node.lib")
    add_custom_command(OUTPUT "${VV_NODE_LIB}"
        COMMAND "${CMAKE_AR}" /machine:x64 "/def:${VV_NODE_MODULES}/node-api-headers/def/node_api.def" "/out:${VV_NODE_LIB}"
        DEPENDS "${VV_NODE_MODULES}/node-api-headers/def/node_api.def")
    add_custom_target(vesktop_media_node_lib DEPENDS "${VV_NODE_LIB}")
    add_dependencies(vesktop_media vesktop_media_node_lib)
    target_link_libraries(vesktop_media PRIVATE
        "${VV_DEPS_DIR}/lib/libdave.lib"
        "${VV_DEPS_DIR}/lib/mlspp.lib"
        "${VV_DEPS_DIR}/lib/mls_ds.lib"
        "${VV_DEPS_DIR}/lib/hpke.lib"
        "${VV_DEPS_DIR}/lib/tls_syntax.lib"
        "${VV_DEPS_DIR}/lib/bytes.lib"
        "${VV_WEBRTC_DIR}/lib/webrtc.lib"
        "${VV_NODE_LIB}"
        delayimp
        # what libwebrtc's audio, camera, screen capture and network code use
        ws2_32 iphlpapi secur32 crypt32 winmm ole32 oleaut32 uuid strmiids dmoguids wmcodecdspuuid msdmo amstrmid
        d3d11 dxgi dwmapi shcore user32 gdi32 advapi32 shell32 runtimeobject
        # libdave's persisted keys; the Go Live thumbnails (WIC); soundshare (process loopback)
        ncrypt windowscodecs mmdevapi
        # the Rust standard library inside libwebrtc
        ntdll userenv)
    target_link_options(vesktop_media PRIVATE /DELAYLOAD:node.exe /OPT:REF /OPT:ICF /DEBUG /PDBALTPATH:%_PDB%)
else()
    # GLib / GIO and PipeWire: headers only, the libraries are loaded at runtime (src/capture)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GIO REQUIRED gio-unix-2.0)
    pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
    target_include_directories(vesktop_media SYSTEM PRIVATE ${GIO_INCLUDE_DIRS} ${PIPEWIRE_INCLUDE_DIRS})

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
endif()

add_custom_command(TARGET vesktop_media POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/index.js" "${CMAKE_BINARY_DIR}/out/index.js")
