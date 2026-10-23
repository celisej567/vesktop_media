#include "common/platform.h"

#include <dlfcn.h>

#include <filesystem>

namespace vv {

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

} // namespace vv
