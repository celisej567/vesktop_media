# 3. Audio

Code: `src/engine/engine.cc`, `src/audio/`, the send gate in `src/connection/connection.cc`.

## The audio graph

```
microphone ─► libwebrtc ADM (PulseAudio) ─► CaptureAdm: input gain, RNNoise / DeepFilterNet ─► APM ─► CaptureTap
                                                                                       │
                       Engine::OnProcessedCapture: level, WebRTC VAD, mic test loopback, voice message
                                                                                       │
                     every "default" connection: OnCaptureFrame → send gate (FrameGate) ─► Opus ─► RTP
remote users' Opus ─► NetEq (one receive stream per user) ─┐
soundboard / local samples ────────────────────────────────┴─► Mixer (pan, mute, output volume, soft limit) ─► ADM ─► speakers
mic test loopback (from OnProcessedCapture) ─► added in CaptureAdm::NeedMorePlayData, after the echo reference was taken ─► speakers
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
* **The microphone stays on the source that was picked.** Tools such as Easy Effects move every ordinary recording
  stream onto their own source, whichever one it asked for (checked: the stream asked for the microphone's source and
  was linked to Easy Effects Source, so the microphone and "Easy Effects Source" sounded the same). PipeWire's pulse server
  copies the properties of the client to its streams, and a stream with `node.dont-move` is left alone. libwebrtc's
  PulseAudio module cannot set stream properties, and the `PA_STREAM_DONT_MOVE` flag does not stop that move, so
  `Engine::Initialize` sets `PULSE_PROP=node.dont-move=true` while the audio device module connects and gives the
  variable back right after. Consequence: "default" and a named microphone give that microphone, without
  Easy Effects' filters (pick "Easy Effects Source" for them), and a stream on "default" does not follow a change of
  the system's default source until the capture restarts.
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
* the `noiseCancellation` flag (the slot of Discord's Krisp, which is not available: Krisp's module only loads into
  the official client, `setKrispPath` is accepted and ignored) runs the model selected with `setNoiseModel`
  (`Engine::SetNoiseModel`): **RNNoise** by default (`audio/rnnoise.cc`, `libvesktop_rnnoise.so` loaded from the
  module's directory), run in `CaptureAdm` before the APM on each channel; if the library is not there, the voice
  processing error callback tells JS (code 2). **Standard**, and a model that cannot run (no library, a DeepFilterNet
  state that could not be created) is libwebrtc's suppression: `ApplyProcessing` turns the flag into
  `noiseSuppression` then. Options and model that did not change are not applied again, and the log says what was
  applied (`processing applied: aec ns agc nc (model, denoiser)`);
* the model can also be **DeepFilterNet** (`audio/deepfilter.cc`, `setNoiseModel(DEEPFILTERNET)`), run at the
  same place on 10 ms mono frames. Creating its state loads the model (about 0.1 s), so that happens on a thread of
  its own and the audio passes unchanged meanwhile; a state is freed away from the audio thread too. A frame costs
  about 0.9 ms inside Vesktop (the log reports the average every 10 s). Its stages always run (`df_set_thresholds`, our
  addition to its C API, upper thresholds at 100 dB): with DeepFilterNet's defaults a clean microphone's local SNR
  hovers around the thresholds where it skips its stages, they switch on and off from frame to frame and the voice
  pulsed (clean speech, noise 30 dB below: 6 % of the speech frames jumped by more than 3 dB, none after). It gives no speech probability, so voice
  activity uses the WebRTC VAD with it. If the state cannot be created the engine falls back to the standard
  suppression. Measured on DeepFilterNet's own speech sample mixed with noise at 5 dB SNR (`VESKTOP_MEDIA_FAKE_MIC=file:`):
  speech above the noise in pauses by 6.9 dB without suppression, 12.5 dB standard, 21.1 dB RNNoise, 24.0 dB DeepFilterNet.

While no connection exists, the mic test (`setLoopback` options) and the voice message recorder use their own
processing options, so the test sounds like the settings being tried.

## Voice activity, push to talk and the send gate

Every processed 10 ms capture frame (`OnProcessedCapture`) gets:

* a level (`VolumeAnalyzer`, dBFS, smoothed);
* a WebRTC GMM VAD decision at the aggressiveness from `vadAutoThreshold`;
* RNNoise's speech probability when RNNoise runs.

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

The speaking callback reports flags (1 voice, 2 soundshare, 4 priority) whenever they change, for our own user here
and for the others from their received audio ([04](04-connection.md));
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
* local samples (soundboard previews, `startSamplesLocalPlayback`), resampled to 48 kHz stereo, with a done callback.

Then the output volume and a soft limiter (tanh above about -2.7 dBFS): several people talking at once saturate
softly instead of clipping. User volumes (`setLocalVolume`) are applied as the receive stream's gain.

## Mic test and voice messages

Outside a call libwebrtc captures at its default of 8 kHz (the APM then runs at 16 kHz), because the capture rate
follows the audio *senders*, and a call's Opus sender is what raises it to 48 kHz. Left like that the signal is band
limited before any processing: the mic test and voice messages sounded like a phone, and, played at the wrong rate,
too high and crackling. `Engine::UpdateAudioRunning` therefore gives the audio transport a sender that discards
everything (`NullAudioSender`, 48 kHz mono) while the capture runs without a connection. A connection's own send
stream sets the senders itself, so nothing is installed while one exists. As a fallback (a stream connection only, no
call), `OnProcessedCapture` resamples a lower capture rate to 48 kHz, so the loopback, the analysis and the WAV
(48 kHz in its header) never get another rate.

* `setLoopback(true, options)` plays the processed capture back (queue of at most 200 ms in the mixer, played by
  `MixLoopbackInto`, see below). The loopback follows the input device: `setInputDevice` restarts the capture, and the
  mixer keeps reading the processed frames.
* `startLocalAudioRecording` writes the processed capture as a 48 kHz mono 16-bit WAV to the data directory
  (`voice-message-<ms>.wav`); `stopLocalAudioRecording` patches the header and returns the path and duration. This is
  the only use of the data directory.

The loopback is **not** part of the mixer's normal output. `CaptureAdm::NeedMorePlayData` lets libwebrtc mix and take
its echo reference first, then `Mixer::MixLoopbackInto` adds the loopback to the samples that go to the device
(resampled when the device does not play at 48 kHz). With the loopback in the reference the echo canceller would take the
test's own voice for echo from the speakers and suppress the microphone while it plays back. The cost: with speakers (not headphones) the test
is no longer protected from acoustic feedback by the echo canceller.

Discord sends the loopback options again with every audio setting (three calls for one click), so repeating
`setLoopback(true)` keeps the queue; only turning it on starts a fresh one. The mixer starts playing once 30 ms are
queued and buffers again after running dry, and `vesktop_media.log` reports the queue depth, underruns and dropped
samples every 2 s while the test runs. When the processing options really change, the mic test is silenced for about
350 ms (short ramps): a stage that was just switched on (noise suppression, echo cancellation, RNNoise) starts
without its noise estimate and lets noise through for a moment. Options that did not change are not applied again.

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
