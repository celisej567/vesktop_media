#include "common/platform.h"

#include <dlfcn.h>
#include <unistd.h>

#include <filesystem>

namespace vv {

std::filesystem::path PathFromUtf8(const std::string& utf8)
{
    return std::filesystem::path(utf8);
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    return path.string();
}

FILE* OpenFile(const std::string& utf8Path, const char* mode)
{
    return fopen(utf8Path.c_str(), mode);
}

std::string ModuleDirectory()
{
    static const std::string dir = [] {
        Dl_info info{};
        if (!dladdr(reinterpret_cast<void*>(&ModuleDirectory), &info) || !info.dli_fname) return std::string();
        return std::filesystem::path(info.dli_fname).parent_path().string();
    }();
    return dir;
}

void* OpenLibrary(std::initializer_list<const char*> names, bool isolated)
{
    int flags = RTLD_NOW | RTLD_LOCAL | (isolated ? RTLD_DEEPBIND : 0);
    auto dir = ModuleDirectory();
    if (!dir.empty())
        for (const char* name : names) {
            auto local = std::filesystem::path(dir) / name;
            std::error_code ec;
            if (std::filesystem::exists(local, ec))
                if (void* h = dlopen(local.c_str(), flags)) return h;
        }
    for (const char* name : names)
        if (void* h = dlopen(name, flags)) return h;
    return nullptr;
}

void* OpenLocalLibrary(const char* name, std::string* path, std::string* error)
{
    auto dir = ModuleDirectory();
    auto local = std::filesystem::path(dir) / name;
    std::error_code ec;
    if (dir.empty() || !std::filesystem::exists(local, ec)) {
        if (error) *error = "not found next to the module";
        return nullptr;
    }
    void* h = dlopen(local.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (h && path) *path = local.string();
    if (!h && error) *error = dlerror();
    return h;
}

void* LoadedLibrary(const char* name)
{
    return dlopen(name, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
}

void* LibrarySymbol(void* library, const char* name)
{
    return library ? dlsym(library, name) : nullptr;
}

void CloseLibrary(void* library)
{
    if (library) dlclose(library);
}

void* HostSymbol(const char* name)
{
    return dlsym(RTLD_DEFAULT, name);
}

int ProcessMemoryMb()
{
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long size = 0, resident = 0;
    int n = fscanf(f, "%ld %ld", &size, &resident);
    fclose(f);
    return n == 2 ? static_cast<int>(resident * sysconf(_SC_PAGESIZE) / (1024 * 1024)) : -1;
}

int SystemAvailableMemoryMb()
{
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[128];
    long kb = -1;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb < 0 ? -1 : static_cast<int>(kb / 1024);
}

} // namespace vv
