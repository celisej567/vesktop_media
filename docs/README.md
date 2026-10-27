# vesktop_media documentation

How vesktop_media works and why it is built the way it is. The [README](../README.md) one level up covers
building, running and testing in short; these pages explain the design.

| Page | Content |
|---|---|
| [01 Architecture](01-architecture.md) | layers, the objects and who owns them, threads and locks |
| [02 Build and toolchain](02-build.md) | libwebrtc and Chromium's libc++, the dependencies, our FFmpeg, RNNoise and DeepFilterNet, relocatable and portable builds |
| [03 Audio](03-audio.md) | the engine, audio device, processing and the noise models (standard, RNNoise, DeepFilterNet), voice activity and the send gate, playout mixer, mic test, voice messages, stream audio (soundshare) |
| [04 Connection and transport](04-connection.md) | `VoiceConnection`: UDP, IP discovery, pings, transport encryption, DAVE, receive streams, who is speaking, bandwidth estimation, stats |
| [05 Video send](05-video-send.md) | simulcast layers, the layer plan, encoders (VP8, H.264, H.265), sharp and smooth streams, bitrate |
| [06 Video receive](06-video-receive.md) | decoders, the key frame watchdog, frame sinks, how frames reach Discord's UI |
| [07 Capture](07-capture.md) | the screen share picker (xdg-desktop-portal), PipeWire capture, cameras |
| [08 Vesktop integration](08-vesktop.md) | loading the module, the `DiscordNative` shim, the patches, the Go Live flow, the extensions, the noise suppression setting and the call panel button |
| [09 Diagnostics and tests](09-diagnostics.md) | the log and how to read it, the fake SFU and `scripts/test.sh` |
| [10 Open problems](10-open-problems.md) | what is not solved or not verified yet |
| [11 Windows](11-windows.md) | the Windows build (clang-cl, Node-API import library), libraries, Core Audio, process loopback soundshare, screen and window capture, cameras, tests |

## In one paragraph

Discord's web client (which Vesktop runs) contains two media engines: one on top of the browser's WebRTC
(`RTCPeerConnection`) and one that drives a native N-API module, `MediaEngineNative`. vesktop_media is such a
native module, written from scratch on top of libwebrtc's low-level `webrtc::Call` API. It exposes the same
JavaScript API that Discord's code expects from a native media engine (99 engine functions, `VoiceConnection` with
66 methods), so Discord's own voice, video and Go Live code runs unchanged; Vesktop loads the module and patches
places of the web client (and uses a few extensions of ours: camera options, encoder choice, noise models). On Linux it captures screens through xdg-desktop-portal and PipeWire, cameras through
V4L2, stream audio through PulseAudio (on Windows: libwebrtc's desktop capturers, DirectShow, WASAPI process loopback,
see [11](11-windows.md)), encodes VP8 with libvpx and H.264 / H.265 with our own FFmpeg (NVIDIA NVENC
or the CPU), cleans the microphone with libwebrtc's suppression, RNNoise or DeepFilterNet, and talks UDP to Discord's media servers (SFU) with transport encryption and DAVE end-to-end
encryption.

## Terms

| Term | Meaning |
|---|---|
| SFU | Discord's media server ("selective forwarding unit"): receives every participant's RTP and forwards it |
| gateway | the voice WebSocket (JSON). Handled entirely by Discord's JS; the module never sees it |
| connection | one `VoiceConnection`: one UDP socket and one `webrtc::Call`. A call is one connection (context `default`); one's own Go Live stream is another (context `stream`); watching someone's stream is another |
| ssrc | RTP stream id. Each user has an audio ssrc and video ssrcs (one per simulcast layer, the RTX ssrc is ssrc + 1) |
| layer | a simulcast layer: the same video at another resolution / bitrate, chosen per viewer by the SFU |
| soundshare | the audio of a Go Live stream (an application's sound, or a microphone for a device stream) |
| DAVE | Discord's audio and video end-to-end encryption (MLS group keys, per-frame encryption, libdave) |
| JS / Discord's JS | the web client's code that drives the module (`MediaEngineNative`) |
