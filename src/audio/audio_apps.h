// The applications that play audio, for the stream audio choice (Windows; on Linux Vesktop's main process lists
// PipeWire's clients). Both are Vesktop extensions: Discord's JS knows nothing of them
#pragma once

#include <cstdint>
#include <string>

#include "common/js.h"

namespace vv {

// [{name, pid}] of the processes with an audio session on the default output, Vesktop's own left out
json ListAudioApps();
// {name, pid} of the process that owns a window (HWND), null for none
json WindowAudioApp(int64_t window);

// Windows 7 has no per-process loopback: there the stream shares what plays into VB-CABLE (a virtual audio device:
// what is played to "CABLE Input" can be recorded from "CABLE Output"), and no single app can be picked.
// Null on other systems; on Windows 7 {installed, isDefault}: whether the cable is there, and whether "CABLE Input"
// is the default playback device (the apps play into it without being told)
json SoundshareCable();
// The endpoint id of "CABLE Output" (UTF-8), "" without VB-CABLE. COM initialized
std::string CableRecordingId();

} // namespace vv
