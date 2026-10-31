// Linux: a crash (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP) writes the signal, the faulting instruction and
// the call stack (module + offset, the symbol when exported) to the log, then goes on to the previous handler
// (Chromium's crashpad, or the default: a core dump). Installed once, when the log opens.
#pragma once

#include <string>

namespace vv {

void InstallCrashHandler();
// The GNU build id of vesktop_media.node ("unknown" before InstallCrashHandler): which build log offsets belong to
const char* ModuleBuildId();
// "realtime priority <soft> / <hard>, realtime timeout <soft> / <hard> us" of the process (RLIMIT_RTPRIO, RTTIME)
std::string RealtimeLimits();

} // namespace vv
