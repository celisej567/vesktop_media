// The applications that play audio, for the stream audio choice (Windows; on Linux Vesktop's main process lists
// PipeWire's clients). Both are Vesktop extensions: Discord's JS knows nothing of them
#pragma once

#include <cstdint>

#include "common/js.h"

namespace vv {

// [{name, pid}] of the processes with an audio session on the default output, Vesktop's own left out
json ListAudioApps();
// {name, pid} of the process that owns a window (HWND), null for none
json WindowAudioApp(int64_t window);

} // namespace vv
