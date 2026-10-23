# 2. Build and toolchain

`sh scripts/build.sh` builds everything into `build/out/` (`index.js`, `vesktop_media.node`,
`libvesktop_ffmpeg.so`). Vesktop's `pnpm build` runs it (`buildVesktopMedia` in `scripts/build/build.mts`) and copies
the three files to `dist/vesktop_media`.

## libwebrtc and Chromium's libc++

The module is built on a prebuilt libwebrtc (shiguredo webrtc-build, M156, Linux x86-64). That build is compiled
with clang against **Chromium's own libc++**, whose symbols live in the inline namespace `std::__Cr`, and the static
library already contains that libc++, libc++abi, BoringSSL and opus.

Everything that passes `std::` types into libwebrtc must agree with it on the exact layout of those types, so all
of our C++ and the C++ dependencies (mlspp, libdave) are compiled with the same headers and configuration:

* `scripts/build.sh` fetches the libc++ and libc++abi headers at the commits Chromium M156 uses, and
  `__config_site` / `__assertion_handler` from Chromium's buildtools;
* `cmake/toolchain.cmake` compiles with `-nostdinc++` plus those include paths and Chromium's defines
  (`_LIBCPP_HARDENING_MODE_EXTENSIVE`…), and links with `-nostdlib++`: the libc++ in `libwebrtc.a` is used.

Never build against the system's libstdc++: its `std::` types are different types (another namespace and layout),
so nothing that crosses into libwebrtc would match.

BoringSSL from libwebrtc stands in for OpenSSL: mlspp and libdave support it natively (`REQUIRE_BORINGSSL=ON`),
and the toolchain points `OPENSSL_*` at `libwebrtc.a`.

## Linking

`CMakeLists.txt` links libdave, mlspp and `libwebrtc.a` statically, with:

* `-fvisibility=hidden` and `-Wl,--exclude-libs,ALL`: nothing of ours, of libwebrtc or of its libc++ is exported.
  Electron also contains (another version of) abseil, BoringSSL and FFmpeg; exported duplicates would bind to the
  wrong copy;
* `-Wl,-z,undefs`: the Node-API symbols (`napi_*`) are left undefined and resolved from the host process (Electron),
  as for every Node addon;
* `--gc-sections`, `-static-libgcc`.

`scripts/test.sh` checks the result: the only undefined symbols allowed are Node-API, glibc and three Xlib functions
libwebrtc references. Anything else would fail only when that code path runs.

Optional libraries are **not linked** but opened with `dlopen` when needed (`common/platform.h`, `OpenLibrary`):
libsodium (legacy encryption modes), librnnoise, libpulse, GIO / GLib, PipeWire, FFmpeg. Each is looked up next to
`vesktop_media.node` first, then on the system search path. A system without PulseAudio or PipeWire still loads the
module; only the feature that needs it is missing. GIO and PipeWire headers come from the system (`pkg-config`) for
the declarations only.

## FFmpeg

H.264 and H.265 need FFmpeg (decoding and encoding). Electron's own libffmpeg has an H.264 decoder but no HEVC and
no encoders, and system FFmpeg versions vary, so `scripts/build-ffmpeg.sh` builds our own, cut down:

* FFmpeg 7.1.1 with `--disable-everything` and only the h264 / hevc decoders and parsers, the encoders
  `libx264`, `libx265`, `h264_nvenc`, `hevc_nvenc`;
* x264 (stable branch, 8 bit, 4:2:0), x265 4.1, nv-codec-headers 12.1 (NVENC SDK 12.1, NVIDIA drivers 530+);
* everything static, then linked into **one** shared library `libvesktop_ffmpeg.so` with `-Bsymbolic` and a version
  script that exports only `av_*`, `avcodec_*`, `avutil_*`.

Why it is built this way (each was a crash):

* **One library, `-Bsymbolic`, loaded without `RTLD_DEEPBIND`.** FFmpeg's internal calls must bind to our copy, not
  to Electron's libffmpeg, which exports the same names. Loading separate libraries with `RTLD_DEEPBIND` did that,
  but also made the library's `malloc` / `free` resolve to different allocators (libc's `strdup` through
  Electron's allocator, the matching `free` through libc's), which crashed. With `-Bsymbolic` FFmpeg's own symbols
  bind inside the library while `malloc` / `free` stay the process' for everything.
* **x264 without `memalign`.** x264 asks `memalign` for 2 MB (huge page) alignment; Chromium's allocator, which is
  Electron's `malloc`, traps on that. The script sets `HAVE_MALLOC_H 0` in x264's `config.h`, so x264 aligns plain
  `malloc` blocks itself.
* **x265 with CMake 4.** x265's `CMakeLists.txt` sets two policies to `OLD`, which CMake 4 refuses; the script comments
  them out (they only matter for Apple Clang and old `if()` quoting) and passes `CMAKE_POLICY_VERSION_MINIMUM=3.5`.
* The NVENC encoders load the driver's `libnvidia-encode` at runtime; without an NVIDIA GPU they simply fail to open
  and are not offered (see [05](05-video-send.md)).

x264 and x265 are GPL, so this FFmpeg is GPL, as Vesktop is. If `libvesktop_ffmpeg.so` is missing the module falls
back to a system FFmpeg (libavcodec 58 to 63), loaded with `RTLD_DEEPBIND`, and for decoding also to Electron's
libffmpeg ([06](06-video-receive.md)).

FFmpeg is used through `dlsym` and a few known structure offsets (the leading fields of `AVPacket` and `AVFrame`,
`pict_type` moved in libavutil 60), not through its headers: the encoder accepts libavutil 56 to 61 and refuses
anything else rather than guess a layout.

## Relocatable builds

The scripts do not depend on where the project lives or on the user:

* every path is relative to the package directory;
* a CMake build directory remembers the absolute source path; `fresh()` in `build.sh` (and the same checks in
  `build-ffmpeg.sh`) starts a build directory over when it was configured elsewhere;
* `-ffile-prefix-map=<root>/=` (toolchain) removes the build machine's paths from `__FILE__` in log messages;
* FFmpeg keeps its configure line and source paths (in assertions) inside the library. It is configured by a
  relative path, with `--prefix=/prefix`, installed with `DESTDIR`, and compiled through a wrapper named `vm-cc` that
  adds the prefix map, so the library names no directory of the machine it was built on.

## Atomic replacement

`build.sh` and Vesktop's `copyNative` write each native file to a temporary name and `rename` it over the old one.
Overwriting a `.node` or `.so` in place changes pages a running Vesktop has mapped, which crashed it (SIGBUS /
SIGSEGV) every time a build ran while Vesktop was open. A rename leaves the old inode to the running process.

## Portability

The binaries need the glibc of the machine that built them (glibc 2.44 on the current build machine): they run on
distributions with a glibc at least that new. To make a build for most systems, build in a container with an older
glibc (Ubuntu 22.04, glibc 2.35). This has not been set up yet.

## Packaging

Vesktop's `package.json` lists `dist/vesktop_media` in `asarUnpack`: native files cannot be loaded from inside the
asar archive. `getVesktopMediaPath()` in `src/main/discordVoice.ts` rewrites `app.asar` to `app.asar.unpacked`.
`VESKTOP_SKIP_MEDIA=1` builds Vesktop without the module; without the build tools Vesktop is built without it too.
