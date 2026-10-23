# vesktop_media

Open reimplementation of Discord's native media engine (`discord_voice.node`) for Linux. It keeps the original's
API, so Discord's web client running in Vesktop can use it as `MediaEngineNative`. It is built as
`vesktop_media.node` and ships with Vesktop (see [Using it with Vesktop](#using-it-with-vesktop)).

Status: the engine is complete for Linux.
* **Voice:** transport (all 7 encryption modes), DAVE end-to-end encryption, Opus, the processing chain, VAD, the
  mic test, soundboard / local samples, stats.
* **Video receive:** VP8, VP9, AV1 (libwebrtc), H.264 and H.265 (FFmpeg), with DAVE.
* **Video send:** cameras (V4L2), VP8 (libvpx) and H.264 / H.265 (FFmpeg: NVIDIA NVENC or the CPU), simulcast
  layers, bitrate and degradation handling.
* **Go Live:** screens and windows (xdg-desktop-portal ScreenCast + PipeWire), a camera as the source, stream audio
  (soundshare, PulseAudio per application or a microphone).
* **Not done:** Windows and macOS backends, AV1 / VP9 encoding, AMD / Intel hardware encoders.

How it works and why it is built this way: [docs/](docs/README.md) (architecture, threads, audio, transport,
video, capture, the Vesktop integration, diagnostics, open problems).

## Building

```bash
sh scripts/build.sh   # -> build/out/{index.js,vesktop_media.node,libvesktop_ffmpeg.so,libvesktop_rnnoise.so,libvesktop_deepfilter.so}
# or from the Vesktop root: pnpm build (builds this too and copies it to dist/vesktop_media)
sh scripts/test.sh    # fake SFU acceptance tests against build/out
```

The scripts do not depend on where the project lives or on the user: every path is relative to this directory,
CMake build directories made elsewhere are started over, and the binaries carry no source paths of the build
machine (`-ffile-prefix-map`).

**Needs** (from the system), Linux x86-64 only (the prebuilt libwebrtc is x86-64): clang, cmake, ninja, make, nasm, git,
curl, pkg-config and pnpm; autoconf, automake and libtool (RNNoise); cargo, Rust 1.70 or newer (DeepFilterNet); the
development headers of GLib / GIO, PipeWire (`libpipewire-0.3`), PulseAudio (`libpulse`, soundshare), nlohmann-json,
and the kernel's `linux/videodev2.h` (cameras). Tests also need Node and Electron (see Tests). `build.sh` stops at the
first missing tool: without cargo or autotools the denoise libraries fail, and so does the whole module build.
About 2 GB of disk end up in `third_party/` and `build/` after a full build.

**Downloads on the first run** (into `third_party/` and `build/`, both git-ignored):
* the prebuilt libwebrtc M156 (shiguredo webrtc-build, Linux x64, ~110 MB);
* the matching Chromium libc++ / libc++abi headers and `__config_site`;
* libdave and mlspp;
* for FFmpeg ([scripts/build-ffmpeg.sh](scripts/build-ffmpeg.sh)): FFmpeg 7.1.1, x264, x265 4.1 and the NVENC
  headers (nv-codec-headers 12.1);
* for the noise suppression libraries ([scripts/build-denoise.sh](scripts/build-denoise.sh), into `third_party/denoise`):
  RNNoise v0.2 with its model, DeepFilterNet v0.5.6 with the Rust crates it needs.

libwebrtc is compiled against Chromium's own libc++ (`std::__Cr`), so all our code, mlspp and libdave are
built with the same headers and configuration (`cmake/toolchain.cmake`). BoringSSL and opus come from
libwebrtc. Never build it against the system's libstdc++.

**FFmpeg** is our own cut-down build, one library next to the module (`libvesktop_ffmpeg.so`, ~11 MB): the h264 /
hevc decoders, the encoders `libx264`, `libx265`, `h264_nvenc` and `hevc_nvenc`. Static libavcodec + libavutil +
x264 + x265 linked with `-Bsymbolic` (its `av_*` names do not clash with Electron's libffmpeg), loaded without
isolation so malloc / free are the process' (loading it isolated crashed: allocations went through Electron's
allocator, frees through libc's). x264 and x265 are GPL, so this FFmpeg is GPL, as Vesktop. The NVENC encoders
use the driver's `libnvidia-encode` at runtime (driver 530 or newer).

**RNNoise and DeepFilterNet** are built from those sources into two libraries next to the module,
`libvesktop_rnnoise.so` (RNNoise, BSD) and `libvesktop_deepfilter.so` (DeepFilterNet, MIT / Apache-2.0, its C API).
Both are loaded **only from the module's own directory**, never from the system. Both run on the microphone when
Discord's noise cancellation is on, whichever `setNoiseModel` selects; DeepFilterNet carries its model (DeepFilterNet3)
inside the library.

**Loaded at runtime when used** (nothing else is linked besides libc / libm):
* libsodium, for the legacy `xsalsa20_*` modes;
* libpulse, for soundshare (stream audio);
* GIO / GLib, for the screen share picker (xdg-desktop-portal);
* PipeWire, for screen capture;
* FFmpeg: ours first, a system libavcodec 58–63 and Electron's libffmpeg as fallbacks.

Each one is looked up next to `vesktop_media.node` first.

**Portability.** The binaries need the glibc of the machine that built them (2.44 on the current build machine),
so a build runs on distributions with a glibc at least that new. Building in a container with an older glibc
(Ubuntu 22.04, glibc 2.35) is the way to make a build for most systems.

## Encoders

| Codec | Encoder | Notes |
|---|---|---|
| VP8 | libvpx (in libwebrtc) | always available, the fallback Discord negotiates when nothing else fits |
| H.264 | `h264_nvenc` (NVIDIA), else `libx264` (CPU) | |
| H.265 | `hevc_nvenc` (NVIDIA), else `libx265` (CPU) | |

Every encoder is opened once on a small test picture at start-up and only the ones that really work are offered
to Discord (the `encode` flags of `getCodecCapabilities`): NVENC can be compiled in without a GPU or driver.
The preference is "auto" (hardware first), "hardware" or "software": `setVideoEncoderPreference`, or the
environment variable `VESKTOP_MEDIA_ENCODER=hardware|software` for tests.

## Source layout

| Path | Content |
|---|---|
| `index.js` | JS wrapper (role of discord_voice's `index.js`), declares only the features this build implements |
| `src/addon.cc` | N-API export table: the 99 exports of the API Discord's JS uses, `VoiceConnection` (66 methods), `VoiceReplayConnection`, and our 11 (the extensions, `NoiseModel`, `vesktopVoiceInfo`) |
| `src/engine/` | the engine: WebRTC environment, "Voice" worker thread, ADM, APM, mixer, level meter / VAD, mic test, recording |
| `src/connection/` | `VoiceConnection` over `webrtc::Call`: UDP transport, discovery, pings, send gate, DAVE frame crypto, video send (`video_send.cc`), stats |
| `src/transport/` | UDP socket, transport encryption (all 7 modes), region latency test |
| `src/audio/` | ADM wrapper (input gain + RNNoise / DeepFilterNet), fake ADM, playout mixer (and the mic test loopback), level meter, RNNoise and DeepFilterNet loaders, soundshare (PulseAudio) |
| `src/dave/` | DAVE session over libdave |
| `src/capture/` | cameras (V4L2 / fake, several at once), xdg-desktop-portal ScreenCast picker (GIO), PipeWire screen capture |
| `src/video/` | decoder factory (libwebrtc VP8 / VP9 / AV1 + FFmpeg H.264 / H.265), FFmpeg encoders, frame sources, RGB → I420, JS frame sinks (RGBA, one frame in flight) |
| `src/common/` | JS interop (thread-safe callbacks, JSON), logging, runtime library loading |
| `scripts/` | `build.sh` (module), `build-ffmpeg.sh` (FFmpeg), `build-denoise.sh` (RNNoise, DeepFilterNet; `patches/` has our change of DeepFilterNet's C API), `test.sh` |
| `tools/` | `fake-sfu.js`, the fake SFU used by the tests; `noise-models.js`, the check of the noise model extension |

## Logs

The module writes `vesktop_media.log` into the log directory Vesktop gives it (`~/.config/vesktop/logs/`).
Besides the engine's events it reports, every 5 to 10 seconds, what a stream or call is doing: frames encoded,
encoder resolution and bitrate (`video: send: …`), audio packets sent and received, UDP packets that arrived, the
keepalive pings, the SFU's bandwidth limit (`SFU REMB`), the processing that runs (`processing applied`) and the cost
of the noise canceller. `VESKTOP_MEDIA_LOG_STDERR=1` also prints it to stderr.

## Tests

`scripts/test.sh` runs `tools/fake-sfu.js`, which hosts a `discord_voice`-compatible module against a local fake
SFU. It needs no Discord servers, uses fake capture and sends traffic only to 127.0.0.1:

```bash
ELECTRON_RUN_AS_NODE=1 ../../node_modules/electron/dist/electron tools/fake-sfu.js build/out/vesktop_media.node --recv-video H264
```

The module needs Electron's symbols, so Electron's binary runs it. `scripts/test.sh` finds it in Vesktop's `node_modules`
(`../../node_modules/electron/dist/electron`); somewhere else set `ELECTRON=/path/to/electron`.

It exits with 0 when every packet decrypts and the reflected remote audio is received. The cases `test.sh` runs:
the noise model extension (`tools/noise-models.js`), speaking and silent calls (with the remote user reported as
speaking and the ping callback), a DAVE reset, receiving VP8 / H.264 / H.265 clips (needs the `ffmpeg` CLI), a late join
(key frame on request), sending a camera (several layers, viewers asking for pixels late), Go Live with a camera
(`--golive-camera`), Go Live from a PipeWire node (needs `gst-launch-1.0` with `pipewiresink` and `pw-dump`), and a
source that goes away mid-stream (`--expect-source-end`: the stream must end, not freeze).

Useful options: `--video VP8|H264|H265` (what to send), `--layers 2`, `--wants-late`, `--js-inactive`,
`--video-bitrate <bps>`, `--real-audio` (the real microphone), `--trace` (every audio packet). Environment:
`VESKTOP_MEDIA_PIPEWIRE_NODE` (capture this PipeWire node instead of a portal pick), `VESKTOP_MEDIA_ENCODER`,
`VESKTOP_MEDIA_FAKE_MIC=speech` (the fake microphone makes a noise floor with voiced bursts instead of a tone) or
`VESKTOP_MEDIA_FAKE_MIC=file:<48 kHz mono 16-bit WAV>` (plays that file, looped), to compare the noise suppression modes.

The stream audio test (`--soundshare <pid>`) plays a tone into a temporary PulseAudio sink, which changes the
audio graph of the machine (the default output or Easy Effects can follow it and lose the real device), so it
runs only on request: `VV_TEST_AUDIO=1 sh scripts/test.sh`.

## Using it with Vesktop

This directory is `Vesktop/packages/vesktop_media`. Vesktop's `pnpm build` runs `scripts/build.sh` and copies
`index.js`, `vesktop_media.node`, `libvesktop_ffmpeg.so`, `libvesktop_rnnoise.so` and `libvesktop_deepfilter.so` to `dist/vesktop_media` (packaged outside the asar
archive, `asarUnpack`). Native files are replaced atomically (temporary file + rename): overwriting one in place
crashes a running Vesktop that has it mapped. Without the build tools Vesktop is built without the module;
`VESKTOP_SKIP_MEDIA=1` skips it.

In Vesktop's settings, Voice & Video → engine: WebRTC, Discord integration (the official `discord_voice`, offered
only when an installed client is found at start-up) or Vesktop Media (this). See `Vesktop/src/main/discordVoice.ts`.

A package for someone else to try: `npx electron-builder --linux tar.gz --x64 --publish never` in the Vesktop root
(`dist/vesktop-<version>.tar.gz`, or `AppImage` instead of `tar.gz`). It runs on systems with a glibc at least as new
as the one that built it (see Portability above).

### Extensions

What the module offers on top of the API Discord's JS uses, declared by the features `vesktop_device_options` and
`vesktop_noise_models`. Vesktop uses them only with this engine:

| Function / option | Purpose |
|---|---|
| `getVideoInputDeviceCapabilities(id, cb)`, `setVideoInputDeviceOptions(id, options)` | a camera's modes (resolution, frame rate) and autofocus; chosen options win over what connections ask for |
| `getNativeScreenSharePickedSource()` | what the portal picker returned: a screen or a window and the window's application (to pick its audio) |
| `setVideoEncoderPreference(pref)`, `getVideoEncoders()` | H.264 / H.265 encoder choice (see Encoders) and what is in use |
| `setNoiseModel(model)`, `getNoiseModel()`, `NoiseModel`, `isRnNoiseSupported()`, `isDeepFilterSupported()` (feature `vesktop_noise_models`) | which model runs behind Discord's noise cancellation flag (the slot of its Krisp): `STANDARD`, `RNNOISE` (default), `DEEPFILTERNET`. The flags `noiseCancellation` / `noiseSuppression` stay Discord's way to switch it on and off |
| transport options `vesktopVideoBitrate`, `vesktopAudioBitrate` | the stream's video / audio bitrate set by the user, in bit/s, `0` = Discord's preset |
| `setGoLiveDevices({videoInputDeviceId, audioInputDeviceId})` | Go Live with a camera and a microphone (also in discord_voice); the microphone id is our input device name |
