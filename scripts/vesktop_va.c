// Linked into libvesktop_ffmpeg.so (scripts/build-ffmpeg.sh) in place of libva: FFmpeg's libva calls go to the
// system's libva.so.2 / libva-drm.so.2, loaded when FFmpeg first asks for a display (vaGetDisplayDRM). A VA driver
// (Mesa's radeonsi_drv_video.so, Intel's iHD) is built against the system's libva and exports its init function
// for that libva's version only: an older libva of our own could not load a newer driver. Without a system libva,
// or with one missing a function FFmpeg needs, there is no display and no VA-API; the rest of the library does not
// depend on it. x86-64 only (the trampolines), as the module is.
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>

#include <libavutil/log.h>
#include <va/va.h>
#include <va/va_drm.h>

// Every libva function FFmpeg calls but the three defined below (build-ffmpeg.sh checks the list against FFmpeg's
// undefined symbols): index, name
#define VV_VA_FUNCTIONS(X)                                                                                             \
    X(0, vaBeginPicture)                                                                                               \
    X(1, vaCreateBuffer)                                                                                               \
    X(2, vaCreateConfig)                                                                                               \
    X(3, vaCreateContext)                                                                                              \
    X(4, vaCreateImage)                                                                                                \
    X(5, vaCreateSurfaces)                                                                                             \
    X(6, vaDeriveImage)                                                                                                \
    X(7, vaDestroyBuffer)                                                                                              \
    X(8, vaDestroyConfig)                                                                                              \
    X(9, vaDestroyContext)                                                                                             \
    X(10, vaDestroyImage)                                                                                              \
    X(11, vaDestroySurfaces)                                                                                           \
    X(12, vaEndPicture)                                                                                                \
    X(13, vaEntrypointStr)                                                                                             \
    X(14, vaErrorStr)                                                                                                  \
    X(15, vaGetConfigAttributes)                                                                                       \
    X(16, vaGetImage)                                                                                                  \
    X(17, vaInitialize)                                                                                                \
    X(18, vaMapBuffer)                                                                                                 \
    X(19, vaMaxNumEntrypoints)                                                                                         \
    X(20, vaMaxNumImageFormats)                                                                                        \
    X(21, vaMaxNumProfiles)                                                                                            \
    X(22, vaProfileStr)                                                                                                \
    X(23, vaPutImage)                                                                                                  \
    X(24, vaQueryConfigEntrypoints)                                                                                    \
    X(25, vaQueryConfigProfiles)                                                                                       \
    X(26, vaQueryImageFormats)                                                                                         \
    X(27, vaQuerySurfaceAttributes)                                                                                    \
    X(28, vaQueryVendorString)                                                                                         \
    X(29, vaRenderPicture)                                                                                             \
    X(30, vaSetDriverName)                                                                                             \
    X(31, vaSetErrorCallback)                                                                                          \
    X(32, vaSetInfoCallback)                                                                                           \
    X(33, vaSyncSurface)                                                                                               \
    X(34, vaTerminate)                                                                                                 \
    X(35, vaUnmapBuffer)
#define VV_VA_COUNT 36

static void* NoLibva(void)
{
    return NULL; // never reached: nothing calls libva before a display, and there is none without libva
}

__attribute__((visibility("hidden"))) void* vv_va_table[VV_VA_COUNT] = {[0 ... VV_VA_COUNT - 1] = (void*)NoLibva};

#define VV_NAME(index, name) #name,
static const char* const kNames[VV_VA_COUNT] = {VV_VA_FUNCTIONS(VV_NAME)};

// One trampoline per function: a jump through its slot of the table, whatever the arguments
#define VV_STR2(x) #x
#define VV_STR(x) VV_STR2(x)
#define VV_TRAMPOLINE(index, name)                                                                                     \
    __asm__(".text\n.globl " #name "\n.hidden " #name "\n.type " #name ",@function\n" #name                            \
            ":\n\tjmp *(vv_va_table+8*" VV_STR(index) ")(%rip)\n.size " #name ",.-" #name "\n");
VV_VA_FUNCTIONS(VV_TRAMPOLINE)

static pthread_once_t gOnce = PTHREAD_ONCE_INIT;
static VADisplay (*gGetDisplayDRM)(int);
static VAStatus (*gMapBuffer2)(VADisplay, VABufferID, void**, uint32_t);
static VAStatus (*gSyncBuffer)(VADisplay, VABufferID, uint64_t);

static void Load(void)
{
    void* va = dlopen("libva.so.2", RTLD_NOW | RTLD_LOCAL);
    void* drm = va ? dlopen("libva-drm.so.2", RTLD_NOW | RTLD_LOCAL) : NULL;
    if (!va || !drm) {
        av_log(NULL, AV_LOG_WARNING, "vesktop: the system's libva.so.2 / libva-drm.so.2 not found: no VA-API\n");
        return;
    }
    void* table[VV_VA_COUNT];
    for (int i = 0; i < VV_VA_COUNT; i++) {
        if (!(table[i] = dlsym(va, kNames[i]))) {
            av_log(NULL, AV_LOG_WARNING, "vesktop: the system's libva has no %s (too old): no VA-API\n", kNames[i]);
            return;
        }
    }
    VADisplay (*getDisplay)(int) = (VADisplay(*)(int))dlsym(drm, "vaGetDisplayDRM");
    if (!getDisplay) return;
    memcpy(vv_va_table, table, sizeof(table));
    // newer than some libva FFmpeg works with: replaced below when missing
    gMapBuffer2 = (VAStatus(*)(VADisplay, VABufferID, void**, uint32_t))dlsym(va, "vaMapBuffer2");
    gSyncBuffer = (VAStatus(*)(VADisplay, VABufferID, uint64_t))dlsym(va, "vaSyncBuffer");
    gGetDisplayDRM = getDisplay;
}

VADisplay vaGetDisplayDRM(int fd)
{
    pthread_once(&gOnce, Load);
    return gGetDisplayDRM ? gGetDisplayDRM(fd) : NULL;
}

// libva before 2.21: the plain map (the flags are a hint)
VAStatus vaMapBuffer2(VADisplay dpy, VABufferID buf_id, void** pbuf, uint32_t flags)
{
    if (gMapBuffer2) return gMapBuffer2(dpy, buf_id, pbuf, flags);
    return ((VAStatus(*)(VADisplay, VABufferID, void**))vv_va_table[18])(dpy, buf_id, pbuf);
}

// libva before 2.9: "unimplemented" makes FFmpeg wait on the surface instead (vaSyncSurface)
VAStatus vaSyncBuffer(VADisplay dpy, VABufferID buf_id, uint64_t timeout_ns)
{
    return gSyncBuffer ? gSyncBuffer(dpy, buf_id, timeout_ns) : VA_STATUS_ERROR_UNIMPLEMENTED;
}
