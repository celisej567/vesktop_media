#include "common/watchdog_win.h"

#include <windows.h>
#include <tlhelp32.h>

#include <rtc_base/thread.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"

namespace vv {
namespace {

constexpr int kMaxFrames = 40;
constexpr int kStallMs = 3000;
constexpr int kMaxDumps = 3;

struct Stack {
    DWORD tid;
    int count;
    DWORD64 frames[kMaxFrames];
};

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The stack of a suspended thread, unwound with the x64 unwind tables. Nothing here allocates: the thread may hold
// the heap lock
int Unwind(CONTEXT ctx, DWORD64* frames)
{
    int n = 0;
    while (n < kMaxFrames && ctx.Rip) {
        frames[n++] = ctx.Rip;
        DWORD64 imageBase = 0;
        auto* fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (fn) {
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
        } else {
            // a leaf function: the return address is on top of the stack
            if (!ctx.Rsp) break;
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
        }
    }
    return n;
}

std::string Describe(DWORD64 address)
{
    HMODULE module = nullptr;
    char text[MAX_PATH + 32];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module)) {
        snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
        return text;
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, sizeof(path));
    const char* name = strrchr(path, '\\');
    snprintf(text, sizeof(text), "%s+0x%llx", name ? name + 1 : path,
             static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
    return text;
}

std::string ThreadName(DWORD tid)
{
    std::string name;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!h) return name;
    PWSTR description = nullptr;
    if (SUCCEEDED(GetThreadDescription(h, &description)) && description) {
        int len = WideCharToMultiByte(CP_UTF8, 0, description, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1) {
            name.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, description, -1, name.data(), len, nullptr, nullptr);
        }
        LocalFree(description);
    }
    CloseHandle(h);
    return name;
}

void DumpStacks(DWORD workerTid)
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&DumpStacks), &self);
    auto base = reinterpret_cast<DWORD64>(self);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    DWORD64 end = base + nt->OptionalHeader.SizeOfImage;

    std::vector<DWORD> tids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId()) tids.push_back(te.th32ThreadID);
    CloseHandle(snap);

    std::vector<Stack> stacks(tids.size());
    size_t count = 0;
    for (DWORD tid : tids) {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
        if (!h) continue;
        if (SuspendThread(h) != static_cast<DWORD>(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            Stack& s = stacks[count];
            s.tid = tid;
            s.count = GetThreadContext(h, &ctx) ? Unwind(ctx, s.frames) : 0;
            ResumeThread(h);
            count++;
        }
        CloseHandle(h);
    }

    VV_LOGW("watchdog", "module base 0x%llx; stacks of the threads in it (worker is thread %lu):", static_cast<unsigned long long>(base),
            static_cast<unsigned long>(workerTid));
    for (size_t i = 0; i < count; i++) {
        auto& s = stacks[i];
        bool ours = false;
        for (int f = 0; f < s.count && !ours; f++) ours = s.frames[f] >= base && s.frames[f] < end;
        if (!ours) continue;
        std::string line;
        for (int f = 0; f < s.count; f++) line += " " + Describe(s.frames[f]);
        VV_LOGW("watchdog", "thread %lu \"%s\"%s:%s", static_cast<unsigned long>(s.tid), ThreadName(s.tid).c_str(),
                s.tid == workerTid ? " (worker)" : "", line.c_str());
    }
}

} // namespace

void StartWorkerWatchdog(webrtc::Thread* worker)
{
    auto workerTid = std::make_shared<std::atomic<DWORD>>(0);
    worker->PostTask([workerTid] { *workerTid = GetCurrentThreadId(); });
    std::thread([worker, workerTid] {
        auto answered = std::make_shared<std::atomic<int64_t>>(-1);
        int dumps = 0;
        while (true) {
            int64_t sent = NowMs();
            answered->store(-1);
            worker->PostTask([answered] { answered->store(NowMs()); });
            bool dumped = false;
            while (answered->load() < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                int64_t waited = NowMs() - sent;
                if (!dumped && waited >= kStallMs) {
                    dumped = true;
                    VV_LOGW("watchdog", "the worker thread has not run a task for %lld ms", static_cast<long long>(waited));
                    if (dumps++ < kMaxDumps) DumpStacks(*workerTid);
                }
            }
            if (dumped)
                VV_LOGW("watchdog", "the worker thread answered after %lld ms", static_cast<long long>(answered->load() - sent));
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }).detach();
}

} // namespace vv
