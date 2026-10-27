// Optional runtime dependencies (RNNoise, libsodium, FFmpeg) are loaded on demand, never linked
#pragma once

#include <cstdio>
#include <filesystem>
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

// Directory containing vesktop_media.node (UTF-8)
std::string ModuleDirectory();

#ifdef _WIN32
// UTF-8 <-> UTF-16 for Win32 calls
std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& w);
#endif

// UTF-8 strings <-> filesystem paths (a path made from a narrow string on Windows uses the ANSI code page)
std::filesystem::path PathFromUtf8(const std::string& utf8);
std::string PathToUtf8(const std::filesystem::path& path);
// fopen with a UTF-8 path
FILE* OpenFile(const std::string& utf8Path, const char* mode);

// File names of the libraries that ship next to the module
#ifdef _WIN32
#define VV_LOCAL_LIBRARY(name) "vesktop_" name ".dll"
#else
#define VV_LOCAL_LIBRARY(name) "libvesktop_" name ".so"
#endif

} // namespace vv
