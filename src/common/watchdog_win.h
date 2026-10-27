#pragma once

namespace webrtc {
class Thread;
}

namespace vv {

// Windows diagnostics: when the voice worker thread does not run a task for 3 s, the call stacks of every thread with
// a frame in this module go to the log, as module + offset (symbolized afterwards with the build's PDB). Once per
// stall, at most 3 times per process
void StartWorkerWatchdog(webrtc::Thread* worker);

} // namespace vv
