# 11. Windows

The module builds and runs on Windows 10 (version 2004 or newer) and 11, x86-64. The engine, the connection, DAVE,
video send and receive are the same code as on Linux; what differs is the build, the loading of libraries and the
capture backends. Platform code is in files of its own (`*_win.cc`, the Linux ones next to them) or behind `_WIN32`
where only a few lines differ. Windows 7 / 8 are not possible: current Electron and libwebrtc M156 need Windows 10.

## Build

`powershell -ExecutionPolicy Bypass -File scripts\build.ps1` (Vesktop's `pnpm build` runs it on Windows) builds
everything into `build\out`: `vesktop_media.node`, `index.js`, the FFmpeg DLLs, `vesktop_rnnoise.dll`,
`vesktop_deepfilter.dll`. It needs Visual Studio 2022 or its Build Tools (the C++ desktop workload and a Windows SDK:
headers and libraries), **LLVM 22 for Windows** (clang-cl and lld-link; Visual Studio's own clang is too old for the
libc++ headers of libwebrtc's revision), CMake, Ninja, Git, pnpm and Rust (DeepFilterNet). The script loads the MSVC
environment itself (`vcvars64.bat` of the newest Visual Studio) and puts LLVM ahead of it.

* **libwebrtc**: shiguredo's prebuilt `webrtc.windows_x86_64.zip`, the same M156 build. It is compiled with clang-cl
  against Chromium's libc++ (`std::__Cr`) on the Microsoft C++ ABI and the static CRT (`/MT`), and contains BoringSSL,
  opus, libvpx, dav1d, the Core Audio device module, DirectShow camera capture and the desktop capturers.
* **libc++ on Windows** sits on the MSVC runtime: `cmake/toolchain.cmake` includes Chromium's libc++ headers with `-I`
  ahead of the MSVC headers, without libc++abi, with `_HAS_EXCEPTIONS=0` as Chromium does. Everything is `/MT`, also
  mlspp, whose CMake predates the setting (`CMP0091`).
* **ABI defines**: `WEBRTC_WIN` and `RTC_ENABLE_WIN_WGC` (it changes `DesktopCaptureOptions`' layout; libwebrtc is
  built with it).
* **mlspp / libdave** add GCC's warning flags with `-Werror` for any Clang; clang-cl reads `-Wall` as `-Weverything`.
  `cmake/third_party_win.cmake` (`CMAKE_PROJECT_INCLUDE`) drops their warning flags. nlohmann/json (no system copy on
  Windows) is installed into `build\deps` with its CMake package, which mlspp looks for.
* **Node-API**: there is no "undefined symbols resolved by the host" on Windows. The module links an import library
  for `node.exe` made from node-api-headers' `node_api.def`, delay-loaded, and a delay-load hook
  (`common/delay_load_hook_win.cc`) hands out the executable (Vesktop.exe, which exports Node-API) when the loader asks
  for node.exe, as every node-gyp addon does. The module imports nothing but system DLLs and NODE.EXE (delay-loaded).
* A loaded `.node` / DLL cannot be overwritten, but it can be renamed: `build.ps1` and Vesktop's `copyNative` move the
  old file to `<name>.old` first.

## Libraries next to the module

`common/platform_win.cc`: `LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`, so a DLL next to the module finds
its own dependencies there and never a copy elsewhere on PATH. Paths are UTF-8 in our code and UTF-16 at every Win32
call (`Widen` / `Narrow`, `PathFromUtf8`, `OpenFile`): the user's profile directory, where Vesktop lives, is often not
ASCII. There is no `RTLD_DEEPBIND` and no need for it: a DLL's imports bind to the DLL they name.

* **FFmpeg**: `scripts/build-ffmpeg.ps1` takes a pinned GPL shared build by BtbN (FFmpeg 8.1 with x264, x265 and NVENC,
  checked against its SHA-256) and ships `avcodec-62.dll`, `avutil-60.dll`, `swresample-6.dll`. The module loads
  `avcodec-NN.dll` from its own directory only; Electron's `ffmpeg.dll` (H.264 decoding only) is the fallback for
  decoding. The full build is large (avcodec is ~115 MB); a cut-down build like Linux' needs MSYS2 and is not set up.
  The rest of BtbN's build (with `ffmpeg.exe`) stays in `third_party\ffmpeg-win\tools` for the tests and is not shipped.
* **RNNoise**: its portable C sources compiled with clang-cl into `vesktop_rnnoise.dll` (only `rnnoise_*` exported;
  no x86 run-time dispatch yet). **DeepFilterNet**: the same patched C API as on Linux, a cargo cdylib with the static
  CRT, `vesktop_deepfilter.dll`. Both by `scripts/build-denoise.ps1`; it clones without line-ending conversion, or the
  patch would not apply.

## Audio

libwebrtc's Core Audio module (`AudioLayer` Windows Core Audio). The default device is chosen by role
(`kDefaultDevice`) instead of index 0 as with PulseAudio, so it follows the system setting; the device ids are Core
Audio endpoint ids.

**Soundshare** (`audio/soundshare_win.cc`): WASAPI process loopback (Windows 10 2004 or newer). pid N records that
process **and its child processes** (`PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`: a browser's or a launcher's
audio comes from children); pid 1, "Entire System", records everything except Vesktop (`EXCLUDE_TARGET_PROCESS_TREE`
with Vesktop's main process, whose children include the renderer the module runs in and Chromium's audio service).
The stream ends when the target process exits. Go Live with a device records that Core Audio capture endpoint. Each
source has a capture thread; one mixer makes the 10 ms, 48 kHz stereo frames, as on Linux.

The stream settings' audio choice lists the processes with an audio session on the default output
(`vesktopListAudioApps`, `audio/audio_apps_win.cc`) and preselects the process that owns the streamed window
(`vesktopWindowAudioApp`); on Linux both come from Vesktop's main process (PipeWire).

## Screens and windows

There is no system picker. Discord's Go Live modal asks the engine for its sources when it declares the
`screen_previews` / `window_previews` features: `getScreenPreviews` / `getWindowPreviews(width, height, callback)`
answer `[{id, name, url}]` (Electron's desktopCapturer shape, a PNG thumbnail made with WIC as a data URL), with ids
`screen:<id>:0` / `window:<HWND>:0`. Discord splits the picked id at ":" and calls `setDesktopSource` with type
`screen` / `window` and the id, which `capture/desktop_capture_win.cc` captures with libwebrtc's `DesktopCapturer`:
DXGI desktop duplication for screens (GDI when not available), Windows.Graphics.Capture for windows (GPU-drawn
windows; GDI before Windows 10 1903 or when WGC fails). The thumbnails use GDI (WGC would flash its border). The
capture runs on a thread of its own at up to 60 frames a second; `FrameSource` scales and drops as on Linux. A window
that is closed ends the stream (checked on every frame: WGC can keep handing out the last frame).

Captures need the user's desktop: a process in session 0 (a service, an SSH session) gets no frames.

## Cameras

`capture/camera_win.cc`: libwebrtc's DirectShow `DeviceInfo` lists the video input devices and their registered
modes without starting them; `VideoCaptureFactory::Create` opens them as on Linux. The camera list follows devices
that come and go. Autofocus is not offered (it would need IAMCameraControl).

## Tests

`scripts/test.ps1`: the cases of `test.sh` that are not Linux-only, plus soundshare by pid and of the entire system (a
440 Hz tone played by a process of its own; needs an audio output device), Go Live from the first screen and from a
window that is closed mid-stream (only from a desktop session). The test VM: a Windows 10 22H2 container (dockurr/windows)
with a silent virtual sound card (QEMU `intel-hda` with `audiodev none`).

## Not verified

The Go Live modal and the stream settings in Vesktop's UI on Windows (needs a Discord login); real cameras and real
microphones; NVENC (needs an NVIDIA GPU); several screens; HDR.
