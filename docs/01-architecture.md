# 1. Architecture

## Layers

```
Discord's JS (MediaEngineNative)            Vesktop renderer patches (src/renderer/patches/nativeVoice.ts)
        │  plain JS calls                              │
        ▼                                              ▼
index.js  ── JS wrapper: binds VoiceConnection, wraps video sinks, reads settings, initialize(), features
        │
        ▼
src/addon.cc  ── N-API export table: argument conversion (napi → json), stubs for what Linux does not have
        │
        ├── src/engine/      Engine: one per process (worker thread, audio device, processing, mixer)
        ├── src/connection/  Connection: one per VoiceConnection (UDP, webrtc::Call, streams, DAVE)
        ├── src/capture/     portal picker, PipeWire screen capture, cameras
        ├── src/audio/       ADM wrapper, mixer, level meter, RNNoise, DeepFilterNet, soundshare
        ├── src/video/       decoders, FFmpeg encoders, frame sources, frame sinks to JS
        ├── src/transport/   UDP socket, transport encryption, region latency test
        ├── src/dave/        DAVE session (libdave)
        └── src/common/      JS interop, logging, runtime library loading
                │
                ▼
        libwebrtc M156 (static): Call, audio / video streams, congestion control, NetEq, APM, libvpx, dav1d,
        BoringSSL, opus, libyuv, video_capture (V4L2), audio_device (PulseAudio)
```

### index.js

Discord's JS calls methods detached (`const f = conn.setSelfMute; f(true)`) and expects some functions that are
JavaScript conveniences on top of the native ones. `index.js`:

* turns every `VoiceConnection` into a plain object of bound methods (`bindConnection`), the methods listed in
  `CONNECTION_METHODS`;
* maps `createVoiceConnectionWithOptions`, `createOwnStreamConnectionWithOptions` (the same: the context in the
  options decides) and `createReplayConnection`;
* gives `setImageDataAllocator` a function that makes `ImageData` objects, so decoded frames are written straight
  into what the UI draws;
* multiplexes video sinks: the native side has one callback sink per stream id, JS can attach several
  (`getNextVideoOutputFrame` for previews and screenshots, the direct sink for rendering);
* reads its settings (`audioSubsystem`, `debugLogging`, `maxLogBytes`…) through `DiscordNative.settings`, which
  Vesktop provides (see [08](08-vesktop.md)), and saves the ones JS changes;
* prepares the data directory (`<module_data>/vesktop_media`, where voice message recordings go) and the log
  directory, then calls `initialize()`;
* declares the features the module supports, from `vesktopVoiceInfo().features`. Discord's JS only uses what is
  declared, so a feature that is not implemented is simply never declared. `native_screenshare_picker` is declared
  only on Wayland.

### addon.cc

The export table. Every function converts its arguments to `json` on the JS thread (`ToJson`), so no other code
touches N-API values. Callbacks become `JsCallback` objects (thread-safe functions, see below). Functions for
features Linux does not have (clips, NDI, Windows Graphics Capture previews, video filters) are stubs that accept
the arguments and answer callbacks with neutral values: the JS calls them unconditionally in some paths and must not
throw.

## Objects and ownership

| Object | Lifetime | Owns |
|---|---|---|
| `Engine` (`engine/engine.h`) | process, never destroyed (libwebrtc threads can outlive static destructors) | the WebRTC `Environment` and field trials, the "Voice" worker thread, `CaptureAdm`, `AudioProcessing`, the playout `Mixer`, the shared `AudioState`, the WebRTC VAD, the noise model, the voice message file |
| `Connection` (`connection/connection.h`) | from `new VoiceConnection` to `destroy()` | the UDP socket and the network thread, `webrtc::Call`, the audio send stream, one audio receive stream and N video receive streams per remote user, the video send stream, `DaveSession`, the soundshare capture, the desktop capture |
| `Camera` (`capture/camera.h`) | process | the camera thread, one `FrameSource` per device (kept for the process lifetime), the running captures |
| `ScreenCastPortal` (`capture/portal.h`) | process | the session D-Bus connection, the one portal session, the capture started right after a pick (until a connection takes it) |
| `PipeWireCapturer` | owned by the portal, then by the connection that streams it | a PipeWire thread loop and stream, a frame repeater thread |
| `Soundshare` | owned by a stream connection | a PulseAudio threaded main loop, a mixing thread |
| `VideoSinks` | process | the JS frame sinks by stream id, the `ImageData` allocator |

### Contexts

A connection's `context` comes from its options:

* `default`: a call. Sends the microphone (shared `AudioState` of the engine) and the selected camera.
* `stream`: one's own Go Live stream, or someone else's stream being watched. It gets an `AudioState` of its own
  whose device module records nothing (`webrtc::FakeAudioDeviceModule`): it never sends the microphone, only
  soundshare frames pushed into it. Received audio still goes to the shared playout mixer. Its video source is the
  desktop capture, or a camera for a device stream. Its bandwidth estimation starts higher (see [04](04-connection.md)).

## Threads

| Thread | Created by | Runs |
|---|---|---|
| JS thread (the renderer's main thread) | Electron | every exported function; every callback into JS (through `Napi::ThreadSafeFunction`, never directly) |
| "Voice" worker (`webrtc::Thread`) | `Engine::Initialize` | every `webrtc::Call` (created with a joined worker / network queue), stream creation and reconfiguration, `UpdateVideoSend`, device polling every 2 s, the stats tasks, the remote speaking task (every 50 ms, `UpdateRemoteSpeaking`) |
| network thread, one per connection | `Connection` constructor | `NetworkLoop`: polls the socket every 10 ms, IP discovery, keepalive pings, transport decryption; posts RTP / RTCP to the worker |
| audio device threads | libwebrtc's PulseAudio module | capture → `CaptureAdm` (gain, RNNoise or DeepFilterNet) → APM → `CaptureTap` → `Engine::OnProcessedCapture` → every connection's send gate; playout → `Mixer::Mix`, then the mic test's loopback is added in `CaptureAdm::NeedMorePlayData` |
| DeepFilterNet creator, one per microphone channel | `DeepFilter` constructor | loading the model (about 0.1 s) without holding the audio thread; freeing an old state also happens on a short-lived thread |
| libwebrtc's own task queues | `webrtc::Call` | encoding, pacing, RTCP |
| camera thread | `Camera` | everything that touches a V4L2 device (enumeration, start, stop, autofocus) |
| portal thread, one per picker | `ScreenCastPortal::Present` | the D-Bus requests of a pick, with its own `GMainContext` |
| `vv-pipewire` loop + repeater, per capture | `PipeWireCapturer::Start` | PipeWire events and frame conversion; re-sending the last frame of a still screen |
| PulseAudio loop + mixing thread | `Soundshare` | sink input discovery and monitor streams; a 10 ms clock that mixes them |
| fake camera / fake audio threads | tests only | synthetic frames on their own clocks |

The rule that keeps this safe: **nothing but the JS thread touches V8**. Results of other threads go through
`JsCallback::Call(builder)`: the builder lambda runs later on the JS thread and creates the N-API values there. The
callbacks do not keep the process alive (they are unreferenced), so the fake SFU and Electron can exit while a
callback is still registered. Callbacks are never invoked synchronously, not even for immediate answers (`Later`),
because Discord's JS does not expect re-entrancy.

## Locks

| Mutex | Guards |
|---|---|
| `Engine::mutex_` | processing options (normal, mic test, voice message) |
| `Engine::connectionsMutex_` | the connection list; held while a capture frame is handed to the connections, so a connection cannot go away in the middle |
| `Engine::recordingMutex_` | the voice message file |
| `Connection::usersMutex_` | remote users, their streams, the ssrc → user maps (network thread routing reads them) |
| `Connection::desktopMutex_` | the desktop capture and its frame source (JS thread sets, worker reads) |
| `Connection::soundshareMutex_` | creation of the soundshare capture |
| `Connection::addressMutex_` | the SFU address (`setUdpEndpoint` on the JS thread, sending on others) |
| `TransportCrypto::mutex_` | mode and key |
| `DaveSession::mutex_` | the MLS session, encryptor, decryptors |
| `Mixer::mutex_` | sources and their gains, the mic test queue, local samples |
| `VideoSinks::mutex_` | the sink table and their "ready" flags |
| `Camera::mutex_` | devices, users, options, frame sources (the camera thread's own state needs no lock) |
| `ScreenCastPortal::mutex_` / `busMutex_` | the session, the pick, the pre-started capture / the bus connection |

Most connection state that the audio thread reads (mute, VAD settings, push to talk) is `std::atomic` instead of
locked: the audio thread must not wait for the JS thread.
