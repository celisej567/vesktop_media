// Windows: the module imports Node-API from "node.exe" (the import library made from node-api-headers' def file),
// delay-loaded. The host is Electron (Vesktop.exe), which exports Node-API itself: when the loader asks for
// node.exe it gets the executable instead. The same hook node-gyp builds into every addon
#include <windows.h>

#include <delayimp.h>

namespace {

FARPROC WINAPI LoadExeHook(unsigned int event, DelayLoadInfo* info)
{
    if (event != dliNotePreLoadLibrary || _stricmp(info->szDll, "node.exe") != 0) return nullptr;
    return reinterpret_cast<FARPROC>(GetModuleHandleW(nullptr));
}

} // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = LoadExeHook;
