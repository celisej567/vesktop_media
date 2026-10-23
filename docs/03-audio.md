# 3. Audio

Code: `src/engine/engine.cc`, `src/audio/`, the send gate in `src/connection/connection.cc`.

## The audio graph

```
microphone ─► libwebrtc ADM (PulseAudio) ─► CaptureAdm: input gain, RNNoise ─► APM ─► CaptureTap
                                                                                       │
                       Engine::OnProcessedCapture: level, WebRTC VAD, mic test loopback, voice message
                                                                                       │
                     every "default" connection: OnCaptureFrame → send gate (FrameGate) ─► Opus ─► RTP
remote users' Opus ─► NetEq (one receive stream per user) ─┐
mic test loopback ─────────────────────────────────────────┼─► Mixer (pan, mute, output volume, soft limit) ─► ADM ─► speakers
soundboard / local samples ────────────────────────────────┘
application audio (PulseAudio monitors) ─► Soundshare ─► stream connection's own AudioState ─► Opus (stereo) ─► RTP
```

There is one engine, one audio device module and one playout mixer per process: every connection (a call, one's
own stream, a watched stream) sends and receives through the same devices.

## Engine start-up

`Engine::Initialize` (called by `index.js`):

* opens the log (`common/log.cc`: `vesktop_media.log`, rotated to `.1` at `maxLogBytes`) and routes libwebrtc's own
  log into it (warnings and errors; everything at `logLevel` 3 and above);
* creates the WebRTC environment with the field trials in `kFieldTrials`: NetEq re-initialises after long expands,
  the robust throughput estimator, `LossBasedBweV2` (see [04](04-connection.md)) and the H.26x packet buffer;
* starts the "Voice" worker thread; creates the platform ADM on it (`kPlatformDefaultAudio` = PulseAudio, which is
  pipewire-pulse on current systems), or the fake ADM with `--use-fake-audio-capture` or when no ADM opens;
* builds the APM with a capture post-processor (`CaptureTap`) and the shared `AudioState` (mixer + APM + ADM);
* selects device 0 (the default) for input and output, stereo playout when available, mono recording;
* starts polling devices every 2 s.

Capture and playout run only while something needs them (`UpdateAudioRunning`): capture for a connection, the
level meter (`setEmitVadLevel`), the mic test or a voice message; playout for a connection, the mic test or samples.
Nothing records the microphone while Vesktop is not in a call.

## Devices

* `getInputDevices` / `getOutputDevices`: the ADM's list as `{name, guid, index}`.
* `setInputDevice` / `setOutputDevice` accept a guid, a name, an index or `"default"` (`FindDevice`). **Setting the
  device that is already in use does nothing**: Discord sets the device again on every device list change (a
  Bluetooth headset appearing), and restarting the same device would drop the audio for a moment each time.
* `PollDevices` compares the lists every 2 s and calls the device change callback with inputs, outputs and cameras.
  The camera list comes from the camera's cache, which never opens a device (see [07](07-capture.md)).
* Input volume is a gain applied in `CaptureAdm` (0 to 4); output volume a gain in the mixer. The system volumes are
  never touched.

## Processing

`ApplyProcessing` configures libwebrtc's APM from the options Discord sends with `setTransportOptions`:

* high-pass filter always on, 48 kHz internal rate;
* echo cancellation (AEC3) and noise suppression (level high) as Discord asks;
* automatic gain control: **AGC2 adaptive digital only**, with Discord's parameters (headroom 5 dB, max gain 50 dB,
  initial gain 15 dB, 6 dB/s, max output noise -50 dBFS). AGC1's analog stage is left off: it would work by changing the
  system microphone volume;
* noise cancellation is RNNoise (`audio/rnnoise.cc`, librnnoise loaded at runtime), run in `CaptureAdm` before the
  APM on each channel. If librnnoise is not there, the voice processing error callback tells JS (code 2). Krisp is not available: Krisp's module only loads into the official client;
  `setKrispPath` is accepted and ignored.

While no connection exists, the mic test (`setLoopbackOptions`) and the voice message recorder use their own
processing options, so the test sounds like the settings being tried.

## Voice activity, push to talk and the send gate

Every processed 10 ms capture frame (`OnProcessedCapture`) gets:

* a level (`VolumeAnalyzer`, dBFS, smoothed);
* a WebRTC GMM VAD decision at the aggressiveness from `vadAutoThreshold`;
* RNNoise's speech probability when noise cancellation runs.

Each "default" connection then decides whether to send (`Connection::OnCaptureFrame`):

* **automatic sensitivity** (`vadAutoThreshold` ≥ 0): speech is the WebRTC VAD's decision, or RNNoise's probability
  above `vadKrispActivationThreshold` when Discord asks for its ML VAD (`vadUseKrisp`). The level becomes 0 or
  -100 dB against a fixed -50 dB threshold;
* **manual sensitivity**: the level against `vadThreshold`;
* speech keeps the gate open for `vadTrailing` × 10 ms afterwards;
* **push to talk** (`inputMode` 2): open while the key is down (`setPTTActive`) and `pttReleaseDelay` ms after;
* self mute closes it.

The gate is `FrameGate`, the **frame encryptor** of the audio send stream. It is a frame encryptor because that is
the last place a frame passes before packetization: a closed gate drops the frame there, so no RTP sequence numbers
are used and receivers see no gap. When the gate closes, and when encryption is first configured, it sends 10 Opus
silence frames (`f8 ff fe`) first, so receivers' jitter buffers end cleanly instead of concealing. The same object
then does the DAVE encryption of the frame ([04](04-connection.md)).

The speaking callback reports flags (1 voice, 2 soundshare, 4 priority) whenever they change;
"speaking while muted" is reported once per stretch of speech.

## Opus

`ConfigureSendStream`: payload type from `audioEncoder` (120 by default), mono for calls and stereo for streams, in-band
FEC as Discord asks, no DTX (the gate already stops sending), the bitrate from `encodingVoiceBitRate` (64 kbit/s
default). The Vesktop extension `vesktopAudioBitrate` (6 to 510 kbit/s, 0 = Discord's) wins over Discord's value: it
is how the stream settings set a stream's audio bitrate. Changes reconfigure the stream in place.

Receive streams decode stereo Opus, use the jitter buffer minimum Discord asks for (`minimumJitterBufferLevel`,
`setMinimumOutputDelay`) and are in a sync group with the user's video for lip sync.

## The playout mixer

`Mixer` (a `webrtc::AudioMixer`) mixes:

* every receive stream of every connection, with per-ssrc pan (`setLocalPan`) and mute (local mute, deafen). A muted
  stream is still pulled, so its jitter buffer keeps draining and unmuting does not play stale audio;
* the mic test loopback (at most 200 ms queued);
* local samples (soundboard previews, `startSamplesLocalPlayback`), resampled to 48 kHz stereo, with a done callback.

Then the output volume and a soft limiter (tanh above about -2.7 dBFS): several people talking at once saturate
softly instead of clipping. User volumes (`setLocalVolume`) are applied as the receive stream's gain.

## Mic test and voice messages

* `setLoopbackOptions(true, options)` plays the processed capture back through the mixer. The loopback follows the
  input device: `setInputDevice` restarts the capture, and the mixer keeps reading the processed frames.
* `startLocalAudioRecording` writes the processed capture as a 48 kHz mono 16-bit WAV to the data directory
  (`voice-message-<ms>.wav`); `stopLocalAudioRecording` patches the header and returns the path and duration. This is
  the only use of the data directory.

## Stream audio (soundshare)

`src/audio/soundshare.cc`. The audio of a Go Live stream comes from PulseAudio (pipewire-pulse), loaded at runtime:

* JS gives a process id (`soundsharePid` in the stream connection's transport options; Vesktop passes the audio source
  chosen in its stream settings). For each **sink input** (playback stream) of that process the module opens a
  **monitor recording stream** of its own on the monitor source of the sink the input plays on, limited to that one
  sink input. Only that application is captured, whatever else plays on the same output.
* **pid 1** means all system audio: every sink input except our own process, so the call itself (other people's
  voices played by Vesktop) is never sent back into the stream.
* **Descendants count.** A sink input belongs to the target when its process or any ancestor (up to 32 levels,
  `/proc/<pid>/stat`) is the target: a game started by Steam, or a browser's separate audio process, plays from a
  child of the process the user picked.
* New sink inputs are picked up through a subscription; removed ones are dropped.
* **Device streams** (Go Live with a camera and a microphone, `setGoLiveDevices`) record a pulse source instead. The
  ids Discord passes are our input device *names* (`default: <description>` for the default entry), so the source is
  looked up by description. It is recorded in **mono** and put in both channels: many USB microphones put their capsule
  on one channel of a "stereo" profile, which sounded off to one side.
* A mixing thread runs on a 10 ms clock, waits until an input has 20 ms buffered, keeps at most 200 ms per input,
  restarts buffering after an underrun, and mixes everything to 48 kHz stereo frames.

The stream connection pushes those frames into its own `AudioState` (`OnSoundshareFrame`), where they are encoded
like a microphone, without echo cancellation or noise suppression (there is no APM in that state). The gate opens
while the peak is above about -60 dBFS and 500 ms after, with the speaking flag SOUNDSHARE; silence is not sent. The
first recording stream calls the soundshare callback, after which Discord's JS raises the stream's Opus bitrate.
