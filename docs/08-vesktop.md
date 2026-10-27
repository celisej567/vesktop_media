# 8. Vesktop integration

Code in Vesktop: `src/main/discordVoice.ts`, `src/main/mainWindow.ts`, `src/main/venmic.ts`,
`src/preload/discordVoice.ts`, `src/renderer/patches/nativeVoice.ts`, `src/renderer/nativeVoiceExtensions.ts`,
`src/renderer/components/NativeStreamSettings.tsx`, `src/renderer/components/CameraOptionsEditor.tsx`,
`src/renderer/components/NoiseModelPicker.tsx`, `src/renderer/components/settings/NativeVoiceToggle.tsx`.

Vesktop's renderer code is run by the preload before the page has a document element: nothing in `src/renderer` may
touch the DOM when its file is evaluated (only when something renders), or the whole bundle stops (no Vesktop
settings, no patches, and Discord falls back to the WebRTC engine).

## Choosing and loading the engine

The setting `voiceEngine` (Settings → Voice & Video engine) is `webrtc` (Chromium's WebRTC, the web client's
default), `discord` (the official client's module, offered only when an installed Discord client was found at
start-up) or `vesktop` (vesktop_media). A change needs a restart. `getVoiceEngine()` falls back to WebRTC when the
selected module is missing; `getVesktopMediaPath()` looks for `dist/vesktop_media` next to the app's code
(`app.asar.unpacked` once packaged).

With a native engine:

* **the renderer sandbox is off** (`sandbox: !isNativeVoiceEnabled()` in `mainWindow.ts`): an N-API module can only be
  loaded into a renderer with Node integration in its preload;
* **venmic is not used**: once created it keeps a PipeWire thread in the main process for good, which does not
  survive the engine's stream audio streams coming and going. Listing audio applications uses `pw-dump`, then `pactl`.

The main process prepares `module_data` and `logs` in Vesktop's data directory and hands the paths over.

The preload (`loadDiscordVoice`) then:

1. puts a **`DiscordNative` shim** on `window` for as long as `index.js` is evaluated: `settings` (kept in Vesktop's
   settings, `nativeVoiceAppSettings`), `features.declareSupported`, `fileManager` (data and log directories),
   `nativeModules.getModulePath`, `app.relaunch`, `processUtils`. Because of context isolation it exists only in the
   preload's world, Discord's code never sees it;
2. `require`s the `.node`, hooks its video sinks (`hookVideoSinks`, see [06](06-video-receive.md)) and the mic test
   loopback, then `require`s `index.js`;
3. exposes the result to the main world as `window.VesktopDiscordVoice`: `engineKind` (`"vesktop"`), `engine`, the
   declared `features`, `setDirectFrameConsumer`. Everything crossing the context bridge must be a plain object:
   the settings store's proxy objects cannot be cloned, so `nativeVoiceExtensions.ts` always passes plain copies.

## Patches of the web client

`src/renderer/patches/nativeVoice.ts`:

| Patch (what it finds) | Why |
|---|---|
| `"injectMediaEngine"` | the native engine's dependencies report "supported" only inside the official client; ours say supported and return the module |
| `MEDIA_ENGINE_SET_GO_LIVE_SOURCE` | the stream's audio source (`soundsharePid`) comes from our stream settings (`getSoundshare`); the quality from our settings (`getStreamQuality`) |
| `getMediaEngine().getDesktopSource(` | the Go Live button opens the desktop Go Live modal (otherwise it calls `getDisplayMedia`, which only the WebRTC engine implements) |
| `sourceDesktopDegradationPreference=` | the degradation preference from our Content Type: `MAINTAIN_RESOLUTION` for Prefer Clarity, `MAINTAIN_FRAMERATE` for Prefer Smoothness with vesktop_media ([05](05-video-send.md)) |
| `"set_source_candidates"` | Linux detection, the picked-content hook (`onModalContentPicked`), removes parts of the official client's modal Vesktop replaces |
| `Attempted to observe candidate games…` | game detection exists only in the official client: return instead of throwing |
| `Can't get desktop sources outside of native app` | allows desktop sources without the official client's `DiscordNative` |
| `"change-windows"`, `getUseSystemScreensharePicker(){` | use the system picker (portal) on Linux |
| `max_bitrate:e.maxBitrate` | the gateway's VIDEO message declares the user's stream bitrate for the layers of our own stream (`streamMaxBitrate`) |
| `location:"UserSettingsCameraSelect"` | adds our camera options (resolution, frame rate, autofocus) under Discord's camera select |
| `VOICE_NOISE_SUPPRESSION_SETTING,{` | the noise suppression setting: RNNoise and DeepFilterNet in the place of Krisp (see Noise suppression below) |
| `vadKrispActivationThreshold=.8` | Discord's settings read-back: the noise cancellation flag stays as it was saved (`getNoiseOverrides`) |
| `noiseCancellation:!1}),(0,` | start-up: the saved noise cancellation flag is not reset (`keepsNoiseCancellation`) |
| `canGoLive:(0,` and `"WGWHv/"` | the noise button of the call panel and its popout |

## The Go Live flow

```
Go Live button
  │  Discord: presentNativeScreenSharePicker
  ▼
portal dialog (compositor) ── pick ──► module connects to the stream at once (capture feeds the preview)
  │  onModalContentPicked: audio source = system, "awaiting stream settings"
  ▼
STREAM_CREATE (our own stream) ── Vesktop pauses the stream (STREAM_SET_PAUSED)
  │
  ▼
Vesktop's Stream Settings modal: preview, resolution, frame rate, content type, video / audio bitrate, audio source
  │  a picked window: its application is preselected as the audio source
  │      (getNativeScreenSharePickedSource → appId → findAudioApp in the main process)
  ├── Cancel ──► STREAM_CLOSE + STREAM_STOP
  └── Go Live ──► applyStreamSettings:
                    MEDIA_ENGINE_SET_GO_LIVE_SOURCE (quality, audio through getSoundshare)
                    setTransportOptions({vesktopVideoBitrate, vesktopAudioBitrate})
                  ──► unpause
```

* The stream starts **paused** because the module can only capture into a stream connection, and the modal needs a
  preview of what will be streamed.
* `getStreamQuality` returns the resolution and frame rate of our settings (0 = Source) for the `stream` context;
  `applyStreamSettings` sends them with `PRESET_CUSTOM`, so Discord does not switch presets on its own.
* The settings go through `MEDIA_ENGINE_SET_GO_LIVE_SOURCE` rather than the connection, so that Discord's store
  remembers the new source: otherwise it re-applied the old audio source the next time it updated the stream.
* **Matching a window to its audio** (`NATIVE_VOICE_FIND_AUDIO_APP` in `venmic.ts`): the compositor only names the
  window's application (KWin: the desktop file name). The main process compares it with the PulseAudio clients'
  binary and application names; for `steam_app_<id>` it looks for the client whose environment has
  `SteamAppId=<id>`. Vesktop's own processes are never offered. Soundshare also captures descendants of the chosen
  process ([03](03-audio.md)).

### While streaming

The menu of the running stream (the arrow next to the stream button, `manage-streams`) gets two items
(`addStreamAudioSourceMenu` in `NativeStreamSettings.tsx`):

* **Audio Source**: None, Entire System, or any application with audio; applied at once;
* **Stream Settings**: the same modal as at the start, with the current audio source preselected and "Apply" instead
  of "Go Live"; Apply goes through `applyStreamSettings`, Cancel changes nothing.

### Device streams

Go Live from the "Devices" tab (a camera and a microphone) has no picker. `STREAM_START` carries
`sourceId: "camera:<device>"` and the audio device; on `STREAM_CREATE` Vesktop pauses the stream and, with
vesktop_media only, shows the device stream settings (camera mode, frame rate, autofocus, microphone). The camera
gets those options for the duration of the stream (`setVideoInputDeviceOptions`), the connection its devices
(`setGoLiveDevices`), and the saved camera options come back when the stream ends. If the dialog fails the stream is
unpaused with the defaults rather than left paused.

## The extensions

What vesktop_media offers beyond the API Discord's JS uses, declared by the features `vesktop_device_options` and
`vesktop_noise_models`. `vesktopVoice()` and `hasNoiseModels()` in `nativeVoiceExtensions.ts` return the engine only
when `engineKind` is `vesktop` and the feature is declared; with WebRTC or the official module all of these UI parts
are hidden.

| Function / option | Used for |
|---|---|
| `getVideoInputDeviceCapabilities(id, cb)` | the camera options editor: modes and whether autofocus exists |
| `setVideoInputDeviceOptions(id, options)` | saved camera options (applied at start-up), a device stream's options |
| `getNativeScreenSharePickedSource()` | screen or window, and the window's application, to preselect its audio |
| `setVideoEncoderPreference(pref)`, `getVideoEncoders()` | the H.264 / H.265 encoder setting under the engine select, and what is in use |
| `setNoiseModel(model)`, `getNoiseModel()`, `NoiseModel`, `isRnNoiseSupported()`, `isDeepFilterSupported()` | the noise suppression choice of Voice & Video settings, see below |
| transport options `vesktopVideoBitrate`, `vesktopAudioBitrate` (bit/s, 0 = Discord's) | the stream settings' bitrates |
| `setGoLiveDevices({videoInputDeviceId, audioInputDeviceId})` | part of Discord's API too; the microphone id is one of our input device names |

### Noise suppression

Discord hands the processing choices to the engine as four flags (`echoCancellation`, `noiseSuppression`,
`automaticGainControl`, `noiseCancellation`) in the engine's `setTransportOptions`: for calls (`nd()` / `no()` in
Discord's media engine store), for the mic test (`setLoopback` options), for voice messages
(`startLocalAudioRecording` options) and through the input profiles ("Voice Isolation" sets `noiseCancellation`).
They stay the only way to switch noise suppression on and off. `noiseCancellation` is the slot of Discord's Krisp, which
vesktop_media cannot load (Discord checks for the `discord_krisp` module itself), so the engine answers it with a model
of its own: `setNoiseModel` selects `RNNOISE` (default), `DEEPFILTERNET` or `STANDARD` (libwebrtc's suppression, also the fallback when a
model cannot run). The patch of
`VOICE_NOISE_SUPPRESSION_SETTING` puts RNNoise and DeepFilterNet in the place of the Krisp entry; both set the flag
and the model (`onNoiseOptionSelected`), Vesktop's setting `nativeVoiceNoiseModel` remembers the model and applies it
at start-up. Discord forces the flag off when Krisp is unsupported in two places, so two more patches leave that out:
in the settings it reads back (`getNoiseOverrides`), and once at start-up, where it saves the flag off when the engine
does not declare Krisp's feature (`keepsNoiseCancellation`; without it the saved choice of a model would be lost on
restart).

**The noise button of the call panel.** Discord shows it (a microphone with a wave, next to the connection info, in the
voice panel at the bottom left) only when Krisp is supported. A patch makes the panel treat noise cancellation as
supported with vesktop_media, so the button is there, and a second one replaces what its popout shows: the Krisp
switch becomes a list with the same four choices as the settings (`NoiseModelPicker.tsx`: none, standard, RNNoise,
DeepFilterNet), and the Krisp advertisement and the text under the list (replaced by one that describes the choices)
are left out. A choice dispatches what Discord's own setting does (the
noise cancellation flag, and the suppression the other way) and selects the model.

Settings Vesktop stores for this engine: `voiceEngine`, `nativeVoiceVideoEncoder`, `nativeVoiceDeviceOptions`
(per camera guid), `nativeVoiceNoiseModel`, `nativeVoiceAppSettings` (the module's own settings) and, in its renderer state,
`nativeScreenshareQuality` (resolution, frame rate, content type, video and audio bitrate).
