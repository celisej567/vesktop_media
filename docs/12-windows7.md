# 12. Windows 7 (experimental)

Windows 7 is not a supported system: Electron 44 and libwebrtc M156 need Windows 10. The regular Windows build still
runs on Windows 7 SP1 x64 under [VxKex](https://github.com/YuZhouRen86/VxKex-NEXT), a compatibility layer that adds
the newer Win32 functions Electron imports. The module notices Windows 7 itself and changes the few things that
behave differently there; everything else is the code described in [11](11-windows.md).

## Build

There is no build of its own. The Windows 7 user runs the same `vesktop_media.node` and the same package as on
Windows 10 / 11, with the official Electron 44.4.3:

* the module is built on Windows 10 with `scripts\build.ps1` ([11](11-windows.md#build)); Visual Studio 2022 and
  LLVM 22 do not install on Windows 7, so nothing is compiled there;
* Vesktop is packaged with `pnpm package` (or `pnpm package:dir` for the unpacked tree), as for any Windows release;
* every Windows 7 branch is decided at run time, so the same binary behaves as before on Windows 8 and newer.

## Detecting Windows 7

`Windows7()` (`common/platform_win.cc`) reads the version from `KUSER_SHARED_DATA`, the page the kernel maps into
every process at `0x7ffe0000` (`NtMajorVersion` at `0x26c`, `NtMinorVersion` at `0x270`): 6.1 or lower is Windows 7.
VxKex can make `GetVersionEx`, `RtlGetVersion` and the PEB report Windows 10, but it does not write that page.
Neither the module nor the renderer decides on `os.release()` or `RtlGetVersion` for this.

## What differs on Windows 7

**Stream audio** (`audio/soundshare_win.cc`). WASAPI process loopback exists only since Windows 10 2004. On
Windows 7 every soundshare target ("Entire System" and any pid) records the capture side of
[VB-CABLE](https://vb-audio.com/Cable/) ("CABLE Output"), found by its device interface name "VB-Audio Virtual Cable"
(`CableRecordingId`, `audio/audio_apps_win.cc`), so a renamed endpoint is still found. The stream then carries
whatever plays into "CABLE Input". Without the cable the soundshare reports "VB-CABLE is not installed" and the
stream goes on without audio. `ActivateAudioInterfaceAsync` is not called on this path: on Windows 7 the ordinal
`mmdevapi` exports under that name is a different function.

**The stream settings** (`NativeStreamSettings.tsx`). The app list (`vesktopListAudioApps`) is empty and the
streamed window's app (`vesktopWindowAudioApp`) is null, so the choice is "None" or "Entire System".
`vesktopSoundshareCable` (null on other systems) answers `{installed, isDefault}`, and `CableNotice` under the audio
choice says what is left to do: install the cable, make "CABLE Input" the default playback device and listen to
"CABLE Output", or (when all is set) keep Vesktop's own output on the real device. "Refresh Audio Sources" checks
again.

**Window thumbnails** (`capture/desktop_capture_win.cc`). With desktop composition (Aero) on, the Go Live modal's
window thumbnails are copied from each window's DC with `BitBlt` (`CaptureWindowDc`). libwebrtc's window capturer
takes a first frame with `PrintWindow`, which on Windows 7 makes the window redraw its frame in the classic style.
Since Windows 8 the thumbnails use libwebrtc's capturer, as `BitBlt` misses GPU-drawn windows there.

These apply on every Windows version but matter mainly on Windows 7:

* the capturers are created with `set_disable_effects(false)`: libwebrtc's GDI capturers otherwise turn desktop
  composition off while they run, and Windows 7 switches the desktop to the basic theme (a no-op since Windows 8);
* the stream's capturer is wrapped in `DesktopAndCursorComposer`: GDI capture (the usual path on Windows 7, where
  there is no Windows.Graphics.Capture) has no mouse cursor;
* the noise models resample a capture rate other than 48 kHz ([03](03-audio.md)); Windows 7 devices often record at
  44.1 kHz;
* Vesktop's arRPC patch (`patches/arrpc@3.7.0.patch`) does not start a process scan while one runs and kills a
  PowerShell that hangs (Windows 7 ships PowerShell 2.0).

## What the user has to do

1. **Windows updates.** Windows 7 SP1 x64 with KB2670838 (platform update), KB3020369 and KB3125574 (the
   convenience rollup). Without them `vesktop.exe` hangs at start under VxKex.
2. **VxKex.** Install it (1.2.2.2316 or VxKex NEXT), then in the properties of `vesktop.exe` turn on VxKex on its
   VxKex tab. Without it Electron 44 does not start.
3. **Vesktop.** The regular Windows package. The native engine (vesktop_media) is selected in Vesktop's settings.
4. **Stream audio, if wanted.** Install VB-CABLE as administrator and restart the computer. Then:
   * Sound → Playback: "CABLE Input" as the default device, so the applications play into the cable;
   * Sound → Recording → "CABLE Output" → Listen: "Listen to this device" on, playback through the real headphones or
     speakers, picked by name (with "Default playback device" the cable plays into itself and the stream repeats
     without end);
   * Sound → Communications: "Do nothing", so Windows does not lower the volume during calls;
   * in Vesktop: the output device set to the real headphones or speakers, and the input device to the microphone,
     both by name and not "Default" (with the default output the call itself goes into the cable and the viewers
     hear it).
5. **Go Live** with the audio source "Entire System".

A guide for the user, in Russian, with these steps and the usual problems, was written separately (not in the
repository).

## Verified

In a Windows 7 SP1 VM with VxKex 1.2.2.2316 and the updates above: the packaged client starts and reaches Discord's
login screen. On a user's Windows 7 machine with VxKex: calls and Go Live with the native engine, stream audio
through VB-CABLE, RNNoise and DeepFilterNet (with the microphone at 48 and at 44.1 kHz), the window thumbnails and
the theme with Aero on, H.264 / H.265 through FFmpeg.

Not verified: Windows 8 / 8.1.

The module's tests (`scripts/test.ps1`) do not run on Windows 7: `electron.exe` in Node mode does not start under
VxKex, so they run on Windows 10 only.

## Diagnostics

The module's log is `logs\vesktop_media.log` in Vesktop's data directory: `Data\logs` next to `vesktop.exe` for a
portable install, `%APPDATA%\vesktop\logs` otherwise. Worth looking for: `recording VB-CABLE` (the soundshare uses
the cable), `VB-CABLE is not installed`, `capture at 44100 Hz: the noise model runs on it resampled`, and the
loader's reason when `vesktop_rnnoise.dll`, `vesktop_deepfilter.dll` or FFmpeg's DLLs do not load.
