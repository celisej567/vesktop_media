// Optional runtime dependencies (RNNoise, libsodium, FFmpeg) are loaded on demand, never linked
#pragma once

#include <initializer_list>
#include <string>

namespace vv {

// Opens the first library that loads. Each name is tried next to vesktop_media.node first, then through the
// system search path. Returns null if none loads.
// `isolated` (RTLD_DEEPBIND): the library and its dependencies resolve their own symbols first, for
// libraries whose symbol names may already be in the process from another version (system FFmpeg next to
// Electron's libffmpeg)
void* OpenLibrary(std::initializer_list<const char*> names, bool isolated = false);
// Like OpenLibrary, but only from the directory of vesktop_media.node: never the system's copy. For the libraries
// that ship with the module (noise suppression). `path`, if given, gets the file that was loaded
void* OpenLocalLibrary(const char* name, std::string* path = nullptr);
// A library already loaded into the process, or null (never loads anything)
void* LoadedLibrary(const char* name);
void* LibrarySymbol(void* library, const char* name);
void CloseLibrary(void* library);
// A symbol from the global scope of the host process (e.g. Electron's libffmpeg)
void* HostSymbol(const char* name);

// Directory containing vesktop_media.node
std::string ModuleDirectory();

} // namespace vv
