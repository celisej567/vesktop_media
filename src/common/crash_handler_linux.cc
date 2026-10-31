#include "common/crash_handler.h"

#include <dlfcn.h>
#include <elf.h>
#include <execinfo.h>
#include <link.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <string>

#include "common/log.h"

namespace vv {

namespace {

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};
struct sigaction gPrevious[65];
std::atomic<bool> gHandling{false};
char gBuildId[64] = "unknown"; // of vesktop_media.node: which build the offsets belong to

// The GNU build id note of the object that contains this code
int FindBuildId(dl_phdr_info* info, size_t, void*)
{
    auto self = reinterpret_cast<ElfW(Addr)>(&FindBuildId);
    bool ours = false;
    for (int i = 0; i < info->dlpi_phnum && !ours; i++) {
        auto& ph = info->dlpi_phdr[i];
        ours = ph.p_type == PT_LOAD && self >= info->dlpi_addr + ph.p_vaddr && self < info->dlpi_addr + ph.p_vaddr + ph.p_memsz;
    }
    if (!ours) return 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        auto& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE) continue;
        auto* p = reinterpret_cast<const uint8_t*>(info->dlpi_addr + ph.p_vaddr);
        auto* end = p + ph.p_memsz;
        while (p + sizeof(ElfW(Nhdr)) <= end) {
            auto* note = reinterpret_cast<const ElfW(Nhdr)*>(p);
            const uint8_t* name = p + sizeof(ElfW(Nhdr));
            const uint8_t* desc = name + ((note->n_namesz + 3) & ~3u);
            if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 && memcmp(name, "GNU", 4) == 0) {
                size_t n = 0;
                for (uint32_t k = 0; k < note->n_descsz && n + 2 < sizeof(gBuildId); k++) {
                    gBuildId[n++] = "0123456789abcdef"[desc[k] >> 4];
                    gBuildId[n++] = "0123456789abcdef"[desc[k] & 15];
                }
                gBuildId[n] = 0;
                return 1;
            }
            p = desc + ((note->n_descsz + 3) & ~3u);
        }
    }
    return 1;
}

// Async-signal-safe output: no stdio, no allocation
struct Out {
    char buf[1024];
    size_t n = 0;
    void Str(const char* s)
    {
        while (s && *s && n < sizeof(buf) - 1) buf[n++] = *s++;
    }
    void Num(unsigned long long v, int base)
    {
        char tmp[24];
        int i = 0;
        do tmp[i++] = "0123456789abcdef"[v % base];
        while ((v /= base) && i < 24);
        if (base == 16) Str("0x");
        while (i > 0 && n < sizeof(buf) - 1) buf[n++] = tmp[--i];
    }
    void Flush()
    {
        buf[n++] = '\n';
        int fd = LogFileDescriptor();
        if (fd >= 0) (void)!write(fd, buf, n);
        (void)!write(STDERR_FILENO, buf, n);
        n = 0;
    }
};

const char* SignalName(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    case SIGTRAP: return "SIGTRAP";
    }
    return "?";
}

// "module+0xoffset (symbol+0xoffset)" for an address
void Frame(Out& o, void* address)
{
    Dl_info info{};
    if (dladdr(address, &info) && info.dli_fname) {
        const char* name = strrchr(info.dli_fname, '/');
        o.Str(name ? name + 1 : info.dli_fname);
        o.Str("+");
        o.Num(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(info.dli_fbase), 16);
        if (info.dli_sname) {
            o.Str(" (");
            o.Str(info.dli_sname);
            o.Str("+");
            o.Num(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(info.dli_saddr), 16);
            o.Str(")");
        }
    } else {
        o.Num(reinterpret_cast<uintptr_t>(address), 16);
    }
}

void Handler(int sig, siginfo_t* si, void* context)
{
    if (!gHandling.exchange(true)) {
        Out o;
        char thread[17] = {};
        prctl(PR_GET_NAME, thread, 0, 0, 0);
        o.Str("[crash] ");
        o.Str(SignalName(sig));
        o.Str(" (code ");
        o.Num(static_cast<unsigned>(si ? si->si_code : 0), 10);
        o.Str(") at address ");
        o.Num(reinterpret_cast<uintptr_t>(si ? si->si_addr : nullptr), 16);
        o.Str(", thread ");
        o.Num(static_cast<unsigned long>(syscall(SYS_gettid)), 10);
        o.Str(" \"");
        o.Str(thread);
        o.Str("\", vesktop_media.node build id ");
        o.Str(gBuildId);
        o.Flush();
#if defined(__x86_64__)
        if (context) {
            auto* uc = static_cast<ucontext_t*>(context);
            o.Str("[crash] instruction ");
            Frame(o, reinterpret_cast<void*>(uc->uc_mcontext.gregs[REG_RIP]));
            o.Flush();
        }
#endif
        void* frames[64];
        int count = backtrace(frames, 64);
        for (int i = 0; i < count; i++) {
            o.Str("[crash]   #");
            o.Num(static_cast<unsigned>(i), 10);
            o.Str(" ");
            Frame(o, frames[i]);
            o.Flush();
        }
    }
    // Hand the signal on: Chromium's crash handler (crashpad) or the default action (a core dump)
    sigaction(sig, &gPrevious[sig], nullptr);
    if (!si || si->si_code <= 0) raise(sig); // sent (abort, kill): delivered again once this returns
    // a fault: returning runs the faulting instruction again, now under the previous handler
}

// SIGXCPU: a realtime thread ran past the soft RLIMIT_RTTIME without blocking (at the hard limit the kernel sends
// SIGKILL, which nothing can catch). Logged; the process goes on (the default action would end it)
void CpuLimitHandler(int, siginfo_t*, void*)
{
    Out o;
    char thread[17] = {};
    prctl(PR_GET_NAME, thread, 0, 0, 0);
    o.Str("[crash] SIGXCPU: realtime thread ");
    o.Num(static_cast<unsigned long>(syscall(SYS_gettid)), 10);
    o.Str(" \"");
    o.Str(thread);
    o.Str("\" went past its CPU time limit (RLIMIT_RTTIME); at the hard limit the kernel kills the process");
    o.Flush();
}

} // namespace

void InstallCrashHandler()
{
    static bool installed = false;
    if (installed) return;
    installed = true;
    void* warm[1];
    backtrace(warm, 1); // loads libgcc's unwinder now, not inside the handler
    dl_iterate_phdr(FindBuildId, nullptr);
    struct sigaction sa {};
    sa.sa_sigaction = CpuLimitHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGXCPU, &sa, nullptr);
    for (int sig : kSignals) {
        struct sigaction sa {};
        sa.sa_sigaction = Handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, &gPrevious[sig]);
    }
}

const char* ModuleBuildId()
{
    return gBuildId;
}

std::string RealtimeLimits()
{
    auto text = [](rlim_t v) { return v == RLIM_INFINITY ? std::string("unlimited") : std::to_string(v); };
    rlimit prio{}, time{};
    getrlimit(RLIMIT_RTPRIO, &prio);
    getrlimit(RLIMIT_RTTIME, &time);
    return "realtime priority " + text(prio.rlim_cur) + " / " + text(prio.rlim_max) + ", realtime timeout " + text(time.rlim_cur) +
           " / " + text(time.rlim_max) + " us";
}

} // namespace vv
