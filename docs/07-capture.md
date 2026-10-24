# 7. Capture

Code: `src/capture/portal.cc`, `src/capture/pipewire_capturer.cc`, `src/capture/camera.cc`, `src/video/rgb_to_i420.h`.

## Screens and windows: the system picker

On Wayland an application cannot list or capture screens and windows itself; it asks **xdg-desktop-portal**
(ScreenCast interface), which shows the compositor's own picker and hands back a PipeWire stream. The module declares
`native_screenshare_picker` only on Wayland, and then Discord's JS calls `presentNativeScreenSharePicker(style)`
instead of showing its own source list. The `screen:` / `window:` source ids of Discord's own picker (X11 / Windows
enumerations) are not supported.

`ScreenCastPortal::Present` runs the portal's request sequence on a thread of its own (with its own `GMainContext`,
so it never waits on the JS thread):

1. `CreateSession`;
2. reads `AvailableCursorModes` and asks for the cursor drawn into the frames when the portal can;
3. `SelectSources`: monitors (style anything but `window`) or windows (`window`), one source;
4. `Start`: the portal shows its dialog here and waits for the user (up to 10 minutes);
5. `OpenPipeWireRemote`: a file descriptor of a PipeWire connection that can see only the picked node.

Then the picker callbacks: picked (`pickerUpdate`, with an empty window list and one display for screens: the portal
does not tell titles), cancelled or error.

### One global session

There is one portal session for the process. Presenting the picker again, or the end of the capture that used it,
closes it (`CloseSession`).

**The D-Bus connection is held for the life of the process** (`Bus()`). The portal ties a session to the D-Bus
connection that created it and closes it as soon as that connection goes away; the compositor then destroys the
stream node. GLib keeps its session bus singleton only by a weak reference, so without our own reference the
connection (and with it the stream) could disappear between the pick and the stream's start.

### Capture starts right after the pick

Discord's JS starts the stream only after its own quality dialog (Vesktop's stream settings), possibly many seconds
after the pick. Some compositors give up on a stream nobody connects to, so the module **connects to the stream
immediately** after the pick. That capture feeds the preview in the stream settings; when the stream connection
then sets its desktop source (`setDesktopSourceWithOptions({type: "prepicked"})`), it takes the running capturer and
its frame source over (`TakeCapture`) instead of connecting again. `releaseNativeDesktopVideoSourcePickerStream`
drops a pick nobody took.

### What was picked (Vesktop extension)

`getNativeScreenSharePickedSource()` returns `{sourceType: "screen" | "window", appId}`. KWin names a window's stream
node `kwin-screencast-<desktop file name>` (for example `kwin-screencast-org.mozilla.firefox`, or
`steam_app_<id>` for a Steam game). That name is the node's `media.name`, which is not among the properties the
registry announces; it arrives with the node's info, after the capture connected. Vesktop uses the app id to choose
the window's application as the stream's audio source ([08](08-vesktop.md)). Other compositors do not name the
application, so `appId` is empty there.

## PipeWire capture

`PipeWireCapturer` (PipeWire loaded at runtime):

* connects to the portal's remote with its fd (or to the default daemon for the test hook
  `VESKTOP_MEDIA_PIPEWIRE_NODE`);
* lists the registry once, learns the picked node's `object.serial` and links by it (`target.object`; linking by node
  id is the fallback), and binds the node to log the formats it offers;
* asks for raw video, BGRx / RGBx / BGRA / RGBA, any size up to 16384², up to 240 fps, and **shared memory
  buffers** (`MemPtr` / `MemFd`). DMA-BUF is not imported: the frames are converted on the CPU anyway;
* on each `process` callback drains the queue and keeps only the newest buffer (a slow consumer skips frames rather
  than falling behind), skips corrupted buffers, converts to I420 and pushes into the frame source.

### The frame repeater

Compositors send a new frame only when something on the screen changed (damage). A still screen would send nothing,
and a viewer who joins (or a key frame request) would get nothing to show. A repeater thread pushes the last frame
again when no new one arrived for 500 ms.

### End of the source

The capture ends (and the stream connection's desktop source ended callback stops the stream in Discord) when:

* the stream reports an error or disconnects;
* **the registry removes the picked node** (`global_remove`): the window was closed or the output unplugged. The
  PipeWire stream itself only pauses in that case, and the repeater would have kept sending the last frame
  forever: a frozen stream. `scripts/test.sh` checks it (`--expect-source-end`);
* the portal session is closed from the system UI.

### RGB to I420

`RgbToI420` takes Y from libyuv, but recomputes U and V with rounding. libyuv's `ARGBToI420` truncates the chroma:
U and V come out half a step low on average. One conversion is invisible, but a stream that shows itself (or a
viewer's screen streamed back) repeats the conversion each generation, and the picture turned green within a few.
The coefficients are BT.601 limited range, scaled so that grey stays exactly 128.

## Cameras

`Camera` (`capture/camera.cc`): V4L2 through libwebrtc's `video_capture` module, or the fake "Strobe Video Source"
(moving colour bars and a bouncing square, so a frozen stream is visible) with `--use-fake-video-capture`.

### The camera thread and the cached device list

Everything that touches a device runs on the camera's own thread, never on the "Voice" worker or the JS thread:

* opening a V4L2 device takes 0.5 to 2 s;
* **libwebrtc's device enumeration probes every camera**: it finds the modes with a `VIDIOC_TRY_FMT` per format and
  size (about 200 per node), and each is a UVC probe request to the camera. That stalls or kills a capture that is
  running (measured: frames stop, each enumeration takes 3 to 7 s), and it made a camera that another program (OBS)
  was streaming drop off the USB bus and come back, again and again, while Vesktop was open (kernel log: `Failed to
  set UVC probe control`, `USB disconnect`, in bursts that matched the enumerations).

So the module lists cameras itself (`ListDevices`): every `/dev/videoN` is opened once for reading, and only
`VIDIOC_QUERYCAP`, `ENUM_FMT`, `ENUM_FRAMESIZES` and `ENUM_FRAMEINTERVALS` are used. They answer from the driver's
descriptors and send nothing to the camera. A node whose `device_caps` have video capture is a camera (the nodes next
to it carry metadata); its guid is its `bus_info` and its name the `card`, as libwebrtc names it, so a guid still opens
the device; a camera with two nodes is listed once. The modes (size and real frame rates; a range, as v4l2loopback
reports, becomes the usual sizes inside it) come from the same queries. `Camera::Start` picks the mode closest to the
request from that list instead of asking libwebrtc, and whether a camera has an autofocus control (a request to the
camera) is asked only when the camera options are opened.

The device list is cached and follows the cameras while they come and go. It is rebuilt when what is in
`/sys/class/video4linux` changed (the nodes, their names, and for v4l2loopback nodes such as OBS Virtual Camera their
`state` and `format`, all read without opening a device), also during a call or a preview: listing sends nothing to
the cameras, so a running capture is not disturbed. The virtual camera's state counts because its node exists all the
time: it is a capture device (`state: capture`) only while OBS feeds it and an output device (`state: output`, no
format) otherwise, so it appears in the list when OBS starts its virtual camera and goes when OBS stops it. A change
has to stay for one more poll (2 s), and enumerations are at least 3 s apart: a camera that is plugging in or
resetting makes its nodes come and go every few seconds. `getVideoInputDevices` and the device change callback report
the cache; nothing is reported before the first enumeration finished, because an empty list would look to Discord as
if every camera had been unplugged.

### Users

A device captures while something uses it:

* a connection that broadcasts it (`Acquire(connection, width, height, fps, guid)`): a call's camera, or a camera
  streamed with Go Live. Several devices can run at once (the call's camera and a Go Live device);
* a JS preview sink on the device's guid (the camera preview in the settings).

Each device has one frame source, shared by all its users and kept for the process lifetime. When several users
want a device, the largest requested size and the highest frame rate win (default 1280×720 at 30 fps); a change
restarts the capture in the new mode. A device that was just closed by us can still be busy for a moment, so then
starting is retried three times, 500 ms apart; otherwise a failure means another program holds the camera (each
request to such a camera is a probe of it), so there is one attempt, and `Apply()` does not try that device again for
15 s.

**Changing the selected camera in a call.** `Camera::SetDevice` starts the new camera and stops the old one, and
tells the calls (`Connection::OnVideoDeviceSelected`, through the callback `Engine` sets): a call takes its frame
source from the selected camera when its video send is set up (`UpdateVideoSend`), so without being told it went on
sending the old camera (for 10 to 25 seconds in a real call) until something else made Discord change its video, and
then switched abruptly. A Go Live stream sends its own source and ignores the selection.

### Capture options (Vesktop extension)

* `getVideoInputDeviceCapabilities(id, cb)`: the formats the device offers (largest first) and whether it has an
  autofocus control (`null` = none).
* `setVideoInputDeviceOptions(id, {width, height, frameRate, autofocus})`: **these win over what connections ask
  for**. 0 or missing = whatever the users ask; `autofocus` null = leave it. Autofocus is set with
  `V4L2_CID_FOCUS_AUTO`, when a capture of that camera starts and on one that keeps running; a camera that this module
is not capturing (it may be OBS's) is not touched when the options are applied at start-up.

Vesktop keeps the chosen options per camera in its settings and applies them at start-up; a device stream can use
its own options for that stream, and the saved ones come back when it ends ([08](08-vesktop.md)).
