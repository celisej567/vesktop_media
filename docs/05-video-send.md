# 5. Video send

Code: `src/connection/video_send.cc`, `src/video/ffmpeg_encoder.cc`, `src/video/frame_source.cc`.

Each connection has at most one `webrtc::VideoSendStream`. Its source is:

* a call (`default`): the selected camera;
* one's own stream (`stream`): the desktop capture of the portal pick, or, for Go Live with a device
  (`setGoLiveDevices`), that camera.

## What decides the stream

`UpdateVideoSend` (on the worker) runs after every change that can matter: `setVideoBroadcast`, the desktop source,
the Go Live devices, and these transport options: `videoEncoder` (codec name and payload types), `streamParameters`
(the simulcast layers), `encodingVideoWidth / Height / FrameRate`, `encodingVideoBitRate / MinBitRate / MaxBitRate`,
`encodingVideoDegradationPreference`, `remoteSinkWantsPixelCount`, `remoteSinkWantsMaxFramerate`, `keyframeInterval`,
`alwaysSendVideo` and the Vesktop extension `vesktopVideoBitrate`.

A stream exists while the connection broadcasts, has a source, layers and a negotiated codec it can encode. Two keys
decide how a change is applied:

* `videoSendKey_` (codec, payload types, ssrcs, source): when it changes the stream is **recreated**;
* `videoEncoderKey_` (layers, sizes, bitrates, degradation, sink wants…): when it changes the encoder is
  **reconfigured** in place (`ReconfigureVideoEncoder`), which keeps the stream running.

A codec the module cannot encode makes it call the encoder fallback callback with the codec's name, once per codec;
Discord's JS then drops the codec and negotiates another one.

The camera captures only while a connection broadcasts it (`Camera::Acquire` / `Release`), see [07](07-capture.md).

## Layers

`streamParameters` lists the simulcast layers: `rid`, `ssrc`, `rtxSsrc`, `quality` (100 = full), `active`,
`maxBitrate`, `minBitrate`, `targetBitrate`, `maxFrameRate`, `maxPixelCount`, `maxResolution` (`{type: "source"}` or a
box). JS sends only the layers it changed, so the module merges them into what it has by `type` + `rid`.
libwebrtc wants them from the lowest to the highest quality, so they are sorted by `quality`.

**Layers start active.** Discord's JS creates every layer with `active: false` and takes the flag back from the
module's own video report (the video callback for our own user). A new stream therefore marks its layers active
itself; otherwise nothing would ever be sent. This includes a stream recreated during the call for another codec
or payload type (Go Live switches from H.265 to H.264 when a viewer cannot decode H.265): JS hands the layers back
with `active: false` then too.

That report (`ReportSelfVideo`) carries, per layer, `active` (the layer's own flag, which JS sends to the gateway in
its VIDEO message; the SFU only forwards layers marked active), the planned bitrates, pixel count and frame rate, and
the stream id of our own preview: the camera's guid for a camera, the ssrc for a stream.

### The layer plan

`PlanLayers` turns the layers, the options and the source size into each layer's size, frame rate and bitrates. It
is the `VideoStreamFactory` of the encoder config, so libwebrtc asks it again whenever the source size changes. For
each layer:

* **size**: the source scaled down (never up, never cropped, orientation agnostic) to fit the layer's
  `maxResolution` box (unless `source`), then `encodingVideoWidth` × `Height`, then the pixel limit
  min(`maxPixelCount`, `remoteSinkWantsPixelCount`); a lower layer without a box is scaled by `quality / 100`.
  Sizes are even, at least 16 pixels;
* **frame rate**: min(layer `maxFrameRate`, `encodingVideoFrameRate`, `remoteSinkWantsMaxFramerate`), 30 by default;
* **bitrates**: max = min(layer `maxBitrate`, `encodingVideoMaxBitRate`) or 2.5 Mbit/s; min likewise or
  150 kbit/s; target between them;
* **`vesktopVideoBitrate`** (the user's stream bitrate, Vesktop extension): the full layer gets exactly that as max
  and target, lower layers a quarter of it (as Discord's presets split it);
* **active**: the layer's flag, and someone wants pixels: `remoteSinkWantsPixelCount` > 0, `alwaysSendVideo`, or a
  stream. For a camera in a call, the encoder stays idle until a viewer asks for pixels.

For a **stream**, `remoteSinkWantsPixelCount` 0 does not mean "nobody watches": Discord sends 0 for the "Source"
resolution (no limit). A stream therefore always encodes while it broadcasts.

## Encoders

`CreateVideoEncoderFactory` wraps each encoder in libwebrtc's `SimulcastEncoderAdapter`, which runs one encoder per
layer:

| Codec | Encoder |
|---|---|
| VP8 | libvpx (in libwebrtc). Always available |
| H.264 | `h264_nvenc` (NVIDIA), else `libx264` (CPU), from our FFmpeg |
| H.265 | `hevc_nvenc` (NVIDIA), else `libx265` (CPU), from our FFmpeg |

AV1 and VP9 are decoded but not encoded. `getCodecCapabilities` tells Discord's JS what can be encoded and decoded,
which decides what it negotiates.

### Probing

An encoder compiled into FFmpeg does not mean it works here: NVENC needs an NVIDIA GPU and driver. Every candidate
is opened once on a 256×144 test picture (`Works`), the result cached for the process, and only encoders that open
are offered. The preference (`setVideoEncoderPreference`: `auto` = hardware first, `hardware`, `software`; or the
environment variable `VESKTOP_MEDIA_ENCODER` for tests) picks among them; a preference that cannot be met falls
back to whatever works. Vesktop's setting applies to the next stream or camera.

### The FFmpeg encoder

`FfmpegEncoder` implements `webrtc::VideoEncoder` over `avcodec_send_frame` / `avcodec_receive_packet`:

* low latency for every backend: no B-frames, CBR with `maxrate` = bitrate and a half-second buffer (few spikes,
  quick to react), a key frame on request (PLI from a viewer) and every 60 s otherwise, forced IDR;
* NVENC: preset `p6` with two passes (`multipass fullres`) and `tune ll` (or `llhq` on FFmpeg older than 4.3), `rc cbr`,
  `zerolatency`, `delay 0`, High profile for H.264;
* x264: `veryfast`, `zerolatency`, High profile, SPS / PPS repeated before every key frame (`repeat-headers`, a
  viewer who joins needs them); x265: `superfast`, `zerolatency`, repeated headers;
* the time base is one frame (1 / the layer's frame rate) and pts the frame's RTP timestamp, unwrapped, in those
  units: NVENC and x265 take their frame rate, and with it each frame's share of the bitrate, from the time base
  (FFmpeg has no option for `AVCodecContext::framerate`);
* a size change (the source was resized or adapted) reopens the encoder: a new stream starting with a key frame;
* a bitrate change of more than 10%: x264 and NVENC take it on the next frame; FFmpeg's libx265 cannot reconfigure,
  so it is reopened (with a key frame);
* it reports no QP, so libwebrtc's QP-based quality scaler is off for it.

## Sharp and smooth

Discord's "Content Type" for a stream ("Prefer Clarity" / "Prefer Smoothness") arrives as the degradation
preference. The two need opposite encoder settings:

| | Prefer Clarity (`MAINTAIN_RESOLUTION`) | Prefer Smoothness (anything else) |
|---|---|---|
| content type | screen | realtime video |
| when bits run short | frames are dropped, the image stays sharp | the frame rate stays, the image gets blurrier and the resolution may drop |
| frame dropper | on | off |
| quality scaling (lower resolution) | off | on (VP8 only) |
| max QP (VP8) | 56 | 63 |
| libvpx denoising (a screen; a camera has it on) | off | off |

The table is what the module asks of libwebrtc; how much of it applies depends on the encoder. For VP8 all of it.
For H.264 and H.265 (FFmpeg) the encoder's own CBR rate control decides the quality: the FFmpeg encoder reports no
QP, so libwebrtc's resolution scaler never runs for it (no resolution drop) and max QP is not used; NVENC also
declares a trusted rate controller, which turns libwebrtc's frame dropper off. There, sharp versus smooth mainly
changes the content type and, for x264 / x265, whether libwebrtc may drop frames.

libvpx's screen content mode and its frame dropper keep the picture sharp at whatever frame rate the bitrate
allows, so they are off for "Prefer Smoothness": with a low bitrate the stream becomes soft instead of losing frames.
Vesktop maps "Prefer Smoothness" to `MAINTAIN_FRAMERATE` for this engine
(`getDesktopDegradationPreference` in `src/renderer/patches/nativeVoice.ts`). A camera is always realtime video with
denoising.

## Other send settings

* RTP: max packet 1200 bytes, compound RTCP every second, NACK with 1 s of history, RTX when every layer has an RTX
  ssrc and the RTX payload type is known; the header extensions the SFU expects on video (transmission offset,
  abs-send-time, rotation, transport sequence number, playout delay, content type, timing).
* `suspend_below_min_bitrate = false`: the stream never pauses when the estimate falls below a layer's minimum.
  While nobody watches, the SFU acknowledges nothing and the estimate falls to the floor; the viewer who arrives next
  needs a running stream.
* `periodic_alr_bandwidth_probing` for streams: a screen rarely needs all the bits it may use, and an
  application-limited sender never learns that more is available unless it probes.
* VP8: one temporal layer, no automatic resize, key frame interval from `keyframeInterval` (3000 frames by default).
* Each sending ssrc is registered with DAVE with its codec; `VideoFrameEncryptor` encrypts the encoded frames.

## Frame sources

`FrameSource` (a libwebrtc `AdaptedVideoTrackSource`) sits between a capturer and the encoder:

* capturers push frames from any thread;
* it caps the size and rate to `encodingVideoWidth × Height` and `encodingVideoFrameRate` with the video adapter,
  by **pixel count only**: giving the adapter an aspect ratio makes it crop the frame;
* it drops frames above the requested rate;
* a screen source repeats its last frame when no new one arrived within a frame interval of the output rate: a
  compositor sends frames only when something changed, and NVENC and x265 share the bitrate out per configured frame,
  so a few frames a second would get a fraction of the bitrate. Repeats carry the capture's clock (the last frame's
  timestamp plus the time since), not ours: PipeWire's timestamps are capture times, behind ours;
* the timestamps it hands on strictly increase by at least 1 ms (the capture thread and the repeater deliver one
  after the other): libwebrtc drops a frame whose timestamp in ms is not after the previous one's;
* it also hands every (unscaled) frame to the JS preview sink of its stream id: the camera preview in the settings,
  the preview of one's own stream, or both for a camera streamed with Go Live.
