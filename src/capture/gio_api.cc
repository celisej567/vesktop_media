#include "capture/gio_api.h"

#include "common/log.h"
#include "common/platform.h"

namespace vv {

const GioApi* Gio()
{
    static const GioApi* api = []() -> const GioApi* {
        void* gio = OpenLibrary({"libgio-2.0.so.0"});
        void* glib = OpenLibrary({"libglib-2.0.so.0"});
        void* gobject = OpenLibrary({"libgobject-2.0.so.0"});
        if (!gio || !glib || !gobject) {
            VV_LOGE("portal", "GIO not available");
            return nullptr;
        }
        auto* a = new GioApi();
        bool ok = true;
        auto find = [&](const char* name) {
            for (void* lib : {gio, glib, gobject})
                if (void* p = LibrarySymbol(lib, name)) return p;
            VV_LOGE("portal", "GIO symbol %s missing", name);
            ok = false;
            return static_cast<void*>(nullptr);
        };
#define VV_GIO_LOAD(name) a->name = reinterpret_cast<decltype(a->name)>(find(#name));
        VV_GIO_FUNCTIONS(VV_GIO_LOAD)
#undef VV_GIO_LOAD
        return ok ? a : nullptr;
    }();
    return api;
}

} // namespace vv
