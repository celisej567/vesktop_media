// Windows side of common/platform.h. Paths and names are UTF-8 in our code and converted at every Win32 call:
// the user's profile directory (where Vesktop lives) is often not ASCII
#include "common/platform.h"

#include <windows.h>

#include <filesystem>

namespace vv {

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::filesystem::path PathFromUtf8(const std::string& utf8)
{
    return std::filesystem::path(Widen(utf8));
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    return Narrow(path.wstring());
}

FILE* OpenFile(const std::string& utf8Path, const char* mode)
{
    return _wfopen(Widen(utf8Path).c_str(), Widen(mode).c_str());
}

std::string ModuleDirectory()
{
    static const std::string dir = [] {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&ModuleDirectory), &self))
            return std::string();
        std::wstring path(MAX_PATH, L'\0');
        for (;;) {
            DWORD n = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
            if (n == 0) return std::string();
            if (n < path.size()) {
                path.resize(n);
                break;
            }
            path.resize(path.size() * 2);
        }
        return Narrow(std::filesystem::path(path).parent_path().wstring());
    }();
    return dir;
}

namespace {
// A library's own dependencies are looked up in its directory first (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR), so
// the FFmpeg DLLs next to the module find each other and never a copy elsewhere on PATH
HMODULE LoadFile(const std::wstring& path)
{
    return LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}
} // namespace

// `isolated` has no meaning here: a DLL's imports always bind to the DLL they name, never to a same-named
// symbol elsewhere in the process
void* OpenLibrary(std::initializer_list<const char*> names, bool)
{
    auto dir = ModuleDirectory();
    if (!dir.empty())
        for (const char* name : names) {
            auto local = std::filesystem::path(Widen(dir)) / Widen(name);
            std::error_code ec;
            if (std::filesystem::exists(local, ec))
                if (HMODULE h = LoadFile(local.wstring())) return h;
        }
    for (const char* name : names)
        if (HMODULE h = LoadLibraryExW(Widen(name).c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) return h;
    return nullptr;
}

void* OpenLocalLibrary(const char* name, std::string* path)
{
    auto dir = ModuleDirectory();
    if (dir.empty()) return nullptr;
    auto local = std::filesystem::path(Widen(dir)) / Widen(name);
    std::error_code ec;
    if (!std::filesystem::exists(local, ec)) return nullptr;
    HMODULE h = LoadFile(local.wstring());
    if (h && path) *path = Narrow(local.wstring());
    return h;
}

void* LoadedLibrary(const char* name)
{
    return GetModuleHandleW(Widen(name).c_str());
}

void* LibrarySymbol(void* library, const char* name)
{
    return library ? reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name)) : nullptr;
}

void CloseLibrary(void* library)
{
    if (library) FreeLibrary(static_cast<HMODULE>(library));
}

// The executable's exports (Electron exports nothing of FFmpeg on Windows: its ffmpeg.dll is a separate DLL)
void* HostSymbol(const char* name)
{
    return reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), name));
}

} // namespace vv
