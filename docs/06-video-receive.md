# 6. Video receive

Code: `UpdateVideoStreams` in `src/connection/connection.cc`, `src/video/decoder_factory.cc`, `src/video/ffmpeg.cc`,
`src/video/video_sinks.cc`, and on Vesktop's side `hookVideoSinks` in `src/preload/discordVoice.ts`.

## Receive streams

`mergeUsers` gives a remote user's video ssrcs (`videoSsrcs`, one per layer the SFU sends us; RTX = ssrc + 1).
For each one the worker creates a `VideoReceiveStream` with every decoder from `videoDecoders` (payload type, RTX
payload type, codec parameters), NACK with 1 s of history, PLI for key frame requests, the user's `FrameDecryptor`
and the user's sync group (lip sync with their audio).

* `setDisableLocalVideo(user, true)` destroys the user's video streams (nothing is decoded for a hidden video);
  `false` creates them again.
* New `videoDecoders` recreate the streams whose decoders changed.
* An ssrc that disappears from a user is forgotten, and JS hears that the stream ended (the video callback with
  ssrc 0).

**Frame decryption is forced on.** libwebrtc runs a video stream's frame decryptor only when frame encryption is
required (`crypto_options.sframe.require_frame_encryption`); otherwise DAVE frames would reach the decoder
encrypted. `FrameDecryptor` passes frames through unchanged while DAVE is off, so requiring it costs nothing.

**The key frame watchdog.** A receive stream created after the sender's last key frame (a late join, or a stream
recreated by `setDisableLocalVideo`) shows nothing until the next key frame, and libwebrtc does not ask for one while
nothing has been decrypted yet (a consequence of the required frame encryption). Each stream therefore requests a key
frame every second until its first frame is rendered, and logs what it has received in the meantime.

## Callbacks

* **video**: on the first RTP packet of an ssrc, `(userId, ssrc, streamId, …)`; the stream id is the ssrc as a
  string. JS then attaches a sink to that id.
* **first frame**: when the first decoded frame is rendered, with the size in the log.
* For our own user the same callback carries the self video report ([05](05-video-send.md)).

## Decoders

| Codec | Decoder |
|---|---|
| VP8 | libvpx (libwebrtc) |
| VP9 | libvpx (libwebrtc) |
| AV1 | dav1d (libwebrtc) |
| H.264 | FFmpeg |
| H.265 | FFmpeg |

FFmpeg for decoding is looked up in this order (`ApiFor`): our `libvesktop_ffmpeg.so`, then Electron's libffmpeg
(already in the process, has H.264 but no HEVC), then a system libavcodec 58 to 63, only for what is still missing.
Decoders are found by **name** (`h264`, `hevc`), not by codec id: the ids are not stable across FFmpeg versions (173
is HEVC in FFmpeg 4 and another codec in later ones). The decoder uses slice threads (frame threads add a frame of
latency) and `low_delay`.

## Frame sinks: from the decoder to the screen

```
decoder ─► RemoteVideoSink ─► VideoSinks::Deliver(streamId)
                                   │ one frame in flight; I420 → RGBA (libyuv) into an ImageData
                                   ▼
               setVideoOutputSink callback (JS thread)
                                   │
           Vesktop preload: hookVideoSinks ─► VideoFrame("RGBX") ─► main world
                                   │
           renderer: createDiscordStream ─► MediaStreamTrackGenerator ─► MediaStream ─► <video>
```

* `VideoSinks` keeps **one frame in flight per stream id**: after delivering a frame it waits for
  `signalVideoOutputSinkReady(streamId)` before converting the next one. A slow consumer (a hidden tab, a busy
  renderer) makes the module drop frames instead of queueing them.
* Frames are converted to RGBA and written into an `ImageData` made by the allocator `index.js` registered, so JS
  gets an object it can draw directly.
* Discord's JS asks for "direct" sinks and renders them with `window.createDiscordStream`, which only exists in the
  official client's own Electron build. Vesktop's preload replaces that: `hookVideoSinks` turns the direct sink
  functions into the callback sink, wraps each RGBA frame in a `VideoFrame` (whose ownership passes through the
  context bridge without a copy) and hands it to the main world, where `createDiscordStream`
  (`src/renderer/patches/nativeVoice.ts`) writes it into a `MediaStreamTrackGenerator` and returns a `MediaStream`.
  It also drops frames when more than two are pending.
* `getNextVideoOutputFrame(streamId)` (previews, screenshots) is another callback sink on the same stream id, added
  by `index.js` next to the direct one.
* Our own previews (camera in the settings, one's own stream) use the same sinks: the camera's frame source delivers
  to the device guid, the desktop source to the stream's ssrc. A preview sink on a camera's guid is also what starts
  the camera for the settings page ([07](07-capture.md)).
