#include "audio/audio_apps.h"

#include <windows.h>

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

#include "common/platform.h"

namespace vv {

namespace {

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr()
    {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// "Firefox" for C:\...\firefox.exe: the executable's name without the extension
std::string ProcessName(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return "";
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    std::string name;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) name = Narrow(std::filesystem::path(std::wstring(path, size)).stem().wstring());
    CloseHandle(process);
    return name;
}

// The processes of Vesktop: the module's own, its parent (the main process) and everything below that one
std::set<DWORD> OwnProcesses()
{
    std::map<DWORD, DWORD> parents;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{sizeof(entry)};
        for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
            parents[entry.th32ProcessID] = entry.th32ParentProcessID;
        CloseHandle(snapshot);
    }
    DWORD self = GetCurrentProcessId();
    DWORD root = self;
    if (auto it = parents.find(self); it != parents.end() && ProcessName(it->second) == ProcessName(self)) root = it->second;
    std::set<DWORD> own{self, root};
    for (bool grew = true; grew;) {
        grew = false;
        for (auto& [pid, parent] : parents)
            if (own.count(parent) && own.insert(pid).second) grew = true;
    }
    return own;
}

} // namespace

json ListAudioApps()
{
    json list = json::array();
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<IAudioSessionEnumerator> sessions;
        int count = 0;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
            SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
            SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager))) &&
            SUCCEEDED(manager->GetSessionEnumerator(&sessions)) && SUCCEEDED(sessions->GetCount(&count))) {
            auto own = OwnProcesses();
            std::map<DWORD, std::string> apps;
            for (int i = 0; i < count; i++) {
                ComPtr<IAudioSessionControl> control;
                ComPtr<IAudioSessionControl2> control2;
                DWORD pid = 0;
                if (FAILED(sessions->GetSession(i, &control)) ||
                    FAILED(control->QueryInterface(IID_PPV_ARGS(&control2))) || FAILED(control2->GetProcessId(&pid)))
                    continue;
                // pid 0: the system sounds session
                if (!pid || own.count(pid) || control2->IsSystemSoundsSession() == S_OK) continue;
                auto name = ProcessName(pid);
                if (!name.empty()) apps[pid] = name;
            }
            std::vector<std::pair<std::string, DWORD>> sorted;
            for (auto& [pid, name] : apps) sorted.emplace_back(name, pid);
            std::sort(sorted.begin(), sorted.end());
            for (auto& [name, pid] : sorted) list.push_back({{"name", name}, {"pid", pid}});
        }
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return list;
}

json WindowAudioApp(int64_t window)
{
    DWORD pid = 0;
    HWND hwnd = reinterpret_cast<HWND>(static_cast<intptr_t>(window));
    if (!IsWindow(hwnd) || !GetWindowThreadProcessId(hwnd, &pid) || !pid) return nullptr;
    auto name = ProcessName(pid);
    return {{"name", name.empty() ? std::to_string(pid) : name}, {"pid", pid}};
}

} // namespace vv
