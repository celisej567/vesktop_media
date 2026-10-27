# 9. Diagnostics and tests

## The log

`~/.config/vesktop/logs/vesktop_media.log` (the log directory Vesktop gives `initialize`), rotated to
`vesktop_media.log.1` at `maxLogBytes` (5 MB by default). Each line: time, level (`V` / `I` / `W` / `E`), area, message.
`VESKTOP_MEDIA_LOG_STDERR=1` also prints to stderr; before `initialize` everything goes to stderr. libwebrtc's own
warnings and errors appear under `webrtc`; Vesktop's renderer adds its Go Live trace lines under the area `js`,
prefixed `[Vesktop]` (`consoleLog`).

### Lines worth knowing

| Line | Meaning |
|---|---|
| `engine: initialize: {…}` | the options `index.js` passed |
| `conn: VoiceConnection user … ssrc … context …` | a connection (call `default`, stream `stream`) |
| `conn: IP discovery: <ip>:<port> (<n> ms)` | UDP works; no such line = nothing came back from the SFU |
| `conn: encryption: aead_aes256_gcm_rtpsize` | the key arrived, media can flow |
| `conn: audio packets in the last 10 s (default): sent N, from <user> M; udp in: R rtp, C rtcp, D undecryptable, U unknown ssrc` | every 10 s. A user that cannot be heard: `from <user> 0` with `udp in: 0 rtp` means the SFU sends nothing; packets that are undecryptable or of an unknown ssrc mean we drop them |
| `...; pings: N sent, M echoed, T timed out (rtt R ms), S other short packets` | the end of the same line: the keepalive pings. Discord shows the ping from the server's answers (`13 37 f0 0d` + sequence); every timed out one counts as 4000 ms there (`echoed` is the answers received). The first few packets that are neither discovery, ping answer nor RTP / RTCP are logged as `unexpected packet` with their bytes |
| `engine: capture processing at 48000 Hz, 1 channel(s)` | the rate libwebrtc processes the capture at (16000 outside a call before the engine's own sender is installed, see [03](03-audio.md)) |
| `engine: processing applied: aec=1 ns=0 agc=1 nc=1 (model 1, denoiser RNNoise)` | the processing that really runs: echo cancellation, suppression, gain control, the noise cancellation flag, the selected model and the denoiser behind the flag (`none`, `RNNoise`, `DeepFilterNet`); only when it changes |
| `engine: noise model: N` | `setNoiseModel` (0 standard, 1 RNNoise, 2 DeepFilterNet) |
| `rnnoise: loaded <path>`, `deepfilter: loaded <path>`, `deepfilter: ready in N ms` | the denoiser libraries, from the module's own directory; `ready` is the model being loaded. `cannot create the DeepFilterNet state` means the fallback to the standard suppression |
| `engine: noise canceller: N us per 10 ms frame on average` | every 10 s while RNNoise / DeepFilterNet runs: its cost on the capture thread (the budget is 10000) |
| `engine: mic test: queue A to B samples, U underruns, D dropped in the last 2 s` | every 2 s while the mic test runs: the loopback queue (30 ms = 1440 samples is the normal depth) |
| `conn: SFU REMB (stream): 4200 kbit/s` | the server's limit for what we send, when it changes |
| `video: video send stream: H265 (hevc_nvenc) pt 101 rtx 102, 1 layers, ssrc …, screen (smooth)` | the codec, the encoder in brackets (`libvpx`, `libx264`, `libx265`, `h264_nvenc`, `hevc_nvenc`), what is sent |
| `video: layer: max … kbit/s, encoder active` / `idle until viewers ask for pixels` | the plan of each layer |
| `video: send: N frames encoded, 1920x1080, M kbit/s media (target T), X fps in / Y encoded, source S frames, wants P px, smooth[, SUSPENDED][, bw limited][, cpu limited]` | every 5 s while sending: the actual encoder resolution, bitrate and frame rate, and why it is limited |
| `video: encoder h264_nvenc (libvesktop_ffmpeg.so (bundled)): available` / `not available` | the result of probing an encoder |
| `video: ffmpeg … (avcodec …): h264=1 h265=1` | which FFmpeg decodes what |
| `video: waiting for a frame from <user> ssrc …: packets, pt, frames key / delta, decoded` | a remote video that shows nothing yet (key frame watchdog) |
| `portal: Portal picker: node_id=…, source type 2, app <desktop file>` | what the system picker returned |
| `pipewire: format: 2560x1440, video format 8, 60/1 fps` | the negotiated capture format |
| `pipewire: capture ended: source closed` | the window / output went away |
| `soundshare: sink input N: <app> (pid P) on sink S` | an application's audio being captured |
| `camera: camera <guid> started: 1920x1080 @ 30` | a camera mode actually in use |

To see which codec a stream uses:

```bash
grep -a "video send stream" ~/.config/vesktop/logs/vesktop_media.log | tail -3
```

## The fake SFU

`tools/fake-sfu.js` hosts the module under Electron's Node (`ELECTRON_RUN_AS_NODE=1`) against a local fake SFU. It
needs no Discord servers, uses fake capture and sends traffic only to 127.0.0.1. It checks:

* IP discovery and the `onConnect` info, keepalive pings;
* the `aead_aes256_gcm_rtpsize` layout: it decrypts every packet with the known key;
* the receive path: it re-encrypts our own audio as remote user 222 and checks the inbound stats;
* video send: our fake camera (or a PipeWire node) is sent back as user 222's camera and must come out of the
  video sink again;
* video receive: a test clip encoded with the `ffmpeg` CLI (VP8, H.264, H.265) must be decoded into RGBA frames;
* the speaking callback for the remote user (from the reflected audio) and the ping callback (the fake SFU answers
  pings the way Discord's servers do, `13 37 f0 0d`);
* `rankRtcRegions`; it dumps `getFilteredStats` to `fake-sfu-stats.json`.

Options: `--seconds`, `--vad <dB>`, `--video VP8|H264|H265`, `--recv-video`, `--late-join`, `--layers 2`,
`--wants-late`, `--js-inactive`, `--switch-camera`, `--golive-camera`, `--screen <node>`, `--expect-source-end`, `--soundshare <pid>`,
`--video-bitrate <bps>`, `--dave-reset`, `--real-audio`, `--trace`, `--clip-dir <dir>`. Environment of the module for tests:
`VESKTOP_MEDIA_PIPEWIRE_NODE`, `VESKTOP_MEDIA_ENCODER`, `VESKTOP_MEDIA_LOG_STDERR`, and for the fake microphone
`VESKTOP_MEDIA_FAKE_MIC=speech` (noise floor and voiced bursts) or `file:<48 kHz mono 16-bit WAV>` (looped).

`tools/noise-models.js` checks the noise models (below).

## scripts/test.sh

Runs against `build/out/vesktop_media.node`:

| Case | Guards against |
|---|---|
| undefined symbols | a dependency that would only fail when its code path runs ([02](02-build.md)) |
| noise models (`tools/noise-models.js`) | the extension: RNNoise by default, DeepFilterNet runs and its state is created, Standard is libwebrtc's suppression, nothing runs with the flag off |
| two environments (`tools/two-environments.js`: a worker next to the main thread) | the callbacks of one environment go on working after the other one was torn down (the page loaded again in the same renderer process) |
| exit with a connection (`tools/exit-with-connection.js`) | Node's environment teardown finalizing a connection that was not destroyed (as when Vesktop restarts) must not abort the renderer ([01](01-architecture.md)) |
| speaking / silent | the send gate: every packet decrypts; silence = 10 Opus silence frames, then nothing; the remote user is reported as speaking; the ping callback gets the server's answer |
| `--dave-reset` | a transition that turns DAVE off without an MLS group: the JS thread stays responsive |
| receive VP8 / H.264 / H.265, H.264 joined mid-stream | decoders; the key frame watchdog |
| send camera, `--wants-late`, `--js-inactive`, two layers | the encoder idles until viewers ask; layers forced active; simulcast |
| `--switch-camera` (the fake source has a second camera, "Strobe Video Source 2") | the selected camera changed in a call: the call's own video report names the new camera within 1.5 s |
| `--golive-camera` | Go Live with a device |
| Go Live from a PipeWire node (GStreamer test pattern, "Source" size) | screen capture; sink wants 0 must not idle a stream |
| source closed mid-stream | `global_remove` ends the stream instead of freezing it |
| stream audio (`VV_TEST_AUDIO=1` only) | soundshare of one process |

Not covered by `test.sh` (they need a real microphone, a compositor or a call): which microphone source the module
records from, the mic test's sound (levels, pulsing), the portal picker, real calls. Those were checked by hand.

The stream audio test plays a tone into a temporary PulseAudio null sink. That changes the machine's audio graph
(the default output, or Easy Effects, can follow the new sink and lose the real device), so it runs only on
request. No other test touches the audio setup.

The PipeWire cases need `gst-launch-1.0` with `pipewiresink` and `pw-dump`; the video receive cases need the `ffmpeg`
CLI. Missing tools skip their cases.
