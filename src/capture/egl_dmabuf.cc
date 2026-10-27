#include "capture/egl_dmabuf.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <map>
#include <string>

#include "common/log.h"
#include "common/platform.h"

namespace vv {

namespace {

#define VV_EGL_FUNCTIONS(X)                                                                                            \
    X(eglGetProcAddress)                                                                                               \
    X(eglInitialize)                                                                                                   \
    X(eglQueryString)                                                                                                  \
    X(eglBindAPI)                                                                                                      \
    X(eglChooseConfig)                                                                                                 \
    X(eglCreateContext)                                                                                                \
    X(eglDestroyContext)                                                                                               \
    X(eglCreatePbufferSurface)                                                                                         \
    X(eglDestroySurface)                                                                                               \
    X(eglMakeCurrent)                                                                                                  \
    X(eglGetCurrentContext)                                                                                            \
    X(eglGetError)

#define VV_GL_FUNCTIONS(X)                                                                                             \
    X(glGetString)                                                                                                     \
    X(glGetError)                                                                                                      \
    X(glGenTextures)                                                                                                   \
    X(glDeleteTextures)                                                                                                \
    X(glBindTexture)                                                                                                   \
    X(glTexParameteri)                                                                                                 \
    X(glTexImage2D)                                                                                                    \
    X(glActiveTexture)                                                                                                 \
    X(glGenFramebuffers)                                                                                               \
    X(glDeleteFramebuffers)                                                                                            \
    X(glBindFramebuffer)                                                                                               \
    X(glFramebufferTexture2D)                                                                                          \
    X(glCheckFramebufferStatus)                                                                                        \
    X(glCreateShader)                                                                                                  \
    X(glShaderSource)                                                                                                  \
    X(glCompileShader)                                                                                                 \
    X(glGetShaderiv)                                                                                                   \
    X(glGetShaderInfoLog)                                                                                              \
    X(glDeleteShader)                                                                                                  \
    X(glCreateProgram)                                                                                                 \
    X(glAttachShader)                                                                                                  \
    X(glBindAttribLocation)                                                                                            \
    X(glLinkProgram)                                                                                                   \
    X(glGetProgramiv)                                                                                                  \
    X(glGetProgramInfoLog)                                                                                             \
    X(glDeleteProgram)                                                                                                 \
    X(glUseProgram)                                                                                                    \
    X(glGetUniformLocation)                                                                                            \
    X(glUniform1i)                                                                                                     \
    X(glUniform1f)                                                                                                     \
    X(glUniform2f)                                                                                                     \
    X(glVertexAttribPointer)                                                                                           \
    X(glEnableVertexAttribArray)                                                                                       \
    X(glViewport)                                                                                                      \
    X(glDisable)                                                                                                       \
    X(glDrawArrays)                                                                                                    \
    X(glPixelStorei)                                                                                                   \
    X(glReadPixels)

struct EglApi {
#define VV_MEMBER(name) decltype(&::name) name = nullptr;
    VV_EGL_FUNCTIONS(VV_MEMBER)
    VV_GL_FUNCTIONS(VV_MEMBER)
#undef VV_MEMBER
    PFNEGLQUERYDEVICESEXTPROC eglQueryDevicesEXT = nullptr;
    PFNEGLQUERYDEVICESTRINGEXTPROC eglQueryDeviceStringEXT = nullptr;
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT = nullptr;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC eglQueryDmaBufModifiersEXT = nullptr;
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = nullptr;
};

const EglApi* Api()
{
    static const EglApi* api = []() -> const EglApi* {
        void* egl = OpenLibrary({"libEGL.so.1"});
        if (!egl) {
            VV_LOGW("dmabuf", "libEGL.so.1 not found");
            return nullptr;
        }
        void* gles = OpenLibrary({"libGLESv2.so.2"});
        auto* a = new EglApi();
#define VV_LOAD_EGL(name) a->name = reinterpret_cast<decltype(a->name)>(LibrarySymbol(egl, #name));
        VV_EGL_FUNCTIONS(VV_LOAD_EGL)
#undef VV_LOAD_EGL
        if (!a->eglGetProcAddress) return nullptr;
        // GL ES functions: eglGetProcAddress returns core ones too (EGL 1.5); libGLESv2 when it does not
        auto proc = [&](const char* name) -> void* {
            void* p = reinterpret_cast<void*>(a->eglGetProcAddress(name));
            return p ? p : (gles ? LibrarySymbol(gles, name) : nullptr);
        };
#define VV_LOAD_GL(name) a->name = reinterpret_cast<decltype(a->name)>(proc(#name));
        VV_GL_FUNCTIONS(VV_LOAD_GL)
#undef VV_LOAD_GL
        a->eglQueryDevicesEXT = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(proc("eglQueryDevicesEXT"));
        a->eglQueryDeviceStringEXT = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(proc("eglQueryDeviceStringEXT"));
        a->eglGetPlatformDisplayEXT = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(proc("eglGetPlatformDisplayEXT"));
        a->eglQueryDmaBufModifiersEXT = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(proc("eglQueryDmaBufModifiersEXT"));
        a->eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(proc("eglCreateImageKHR"));
        a->eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(proc("eglDestroyImageKHR"));
        a->glEGLImageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(proc("glEGLImageTargetTexture2DOES"));
        bool ok = true;
#define VV_CHECK(name)                                                                                                 \
    if (!a->name) {                                                                                                    \
        VV_LOGW("dmabuf", "%s missing", #name);                                                                       \
        ok = false;                                                                                                    \
    }
        VV_EGL_FUNCTIONS(VV_CHECK)
        VV_GL_FUNCTIONS(VV_CHECK)
        VV_CHECK(eglGetPlatformDisplayEXT)
        VV_CHECK(eglQueryDmaBufModifiersEXT)
        VV_CHECK(eglCreateImageKHR)
        VV_CHECK(eglDestroyImageKHR)
        VV_CHECK(glEGLImageTargetTexture2DOES)
#undef VV_CHECK
        return ok ? a : nullptr;
    }();
    return api;
}

bool HasExtension(const char* list, const char* name)
{
    if (!list) return false;
    size_t n = strlen(name);
    for (const char* p = list; (p = strstr(p, name)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
    return false;
}

// The frame, drawn into packed I420: a target of 2P x (h + h/2) RGBA pixels, P = ceil(w / 8). Rows 0..h-1 hold Y,
// four luma samples a pixel; below them U (columns 0..P-1) and V (P..2P-1), four chroma samples a pixel. Read back
// plane by plane it is an I420 buffer with strides 8P, 4P, 4P. The arithmetic is the CPU path's (video/rgb_to_i420.h):
// Y as libyuv computes it, U and V from the 2x2 average with rounding; every value stays an integer below 2^24, exact
// in highp floats
const char* kVertexShader = R"(
attribute vec2 pos;
void main() { gl_Position = vec4(pos, 0.0, 1.0); }
)";

const char* kFragmentShader = R"(
#extension GL_OES_EGL_image_external : require
precision highp float;
uniform samplerExternalOES frame;
uniform vec2 size;   // the frame in pixels
uniform vec2 origin; // the plane's corner in the target
uniform int plane;   // 0 Y, 1 U, 2 V
vec3 at(float x, float y) {
    return floor(texture2D(frame, (vec2(min(x, size.x - 1.0), min(y, size.y - 1.0)) + 0.5) / size).rgb * 255.0 + 0.5);
}
float luma(float x, float y) {
    vec3 c = at(x, y);
    return floor((66.0 * c.r + 129.0 * c.g + 25.0 * c.b + 4224.0) / 256.0);
}
float chroma(float x, float y) {
    vec3 c = (at(2.0 * x, 2.0 * y) + at(2.0 * x + 1.0, 2.0 * y) + at(2.0 * x, 2.0 * y + 1.0) + at(2.0 * x + 1.0, 2.0 * y + 1.0)) * 0.25;
    float v = plane == 1 ? -9714.0 * c.r - 19070.0 * c.g + 28784.0 * c.b : 28784.0 * c.r - 24103.0 * c.g - 4681.0 * c.b;
    return clamp(floor(v / 65536.0 + 128.5), 0.0, 255.0);
}
void main() {
    vec2 p = floor(gl_FragCoord.xy - origin);
    float x = p.x * 4.0;
    vec4 o;
    if (plane == 0) o = vec4(luma(x, p.y), luma(x + 1.0, p.y), luma(x + 2.0, p.y), luma(x + 3.0, p.y));
    else o = vec4(chroma(x, p.y), chroma(x + 1.0, p.y), chroma(x + 2.0, p.y), chroma(x + 3.0, p.y));
    gl_FragColor = o / 255.0;
}
)";

} // namespace

struct EglDmaBuf::Impl {
    const EglApi* api = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE; // without EGL_KHR_surfaceless_context
    bool current = false;
    bool loggedFirst = false;

    // GL objects, made by the thread that reads
    GLuint program = 0, target = 0, framebuffer = 0;
    GLint uSize = -1, uOrigin = -1, uPlane = -1, uFrame = -1;
    int targetWidth = 0, targetHeight = 0;

    struct Image {
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        GLuint texture = 0;
        uint32_t format = 0;
        uint64_t modifier = 0;
        int width = 0, height = 0;
        uint32_t offset = 0, stride = 0;
    };
    std::map<int, Image> images; // by the buffer's first plane fd

    // explicit sync: the DRM node (render node of the EGL device) and the sync objects imported from buffer fds
    std::string drmNode;
    int drmFd = -1;
    int explicitSync = -1; // unknown
    std::map<int, uint32_t> syncObjs;

    static int Ioctl(int fd, unsigned long request, void* arg)
    {
        int r;
        do r = ioctl(fd, request, arg);
        while (r == -1 && (errno == EINTR || errno == EAGAIN));
        return r;
    }

    uint32_t SyncObj(int fd)
    {
        if (auto it = syncObjs.find(fd); it != syncObjs.end()) return it->second;
        drm_syncobj_handle h{};
        h.fd = fd;
        if (Ioctl(drmFd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &h) != 0) {
            VV_LOGE("dmabuf", "cannot import a sync object: %s", strerror(errno));
            return 0;
        }
        syncObjs[fd] = h.handle;
        return h.handle;
    }

    void DestroyImage(Image& image)
    {
        if (image.texture && current) api->glDeleteTextures(1, &image.texture);
        if (image.image != EGL_NO_IMAGE_KHR) api->eglDestroyImageKHR(display, image.image);
        image = {};
    }

    GLuint Compile(GLenum type, const char* source)
    {
        GLuint s = api->glCreateShader(type);
        api->glShaderSource(s, 1, &source, nullptr);
        api->glCompileShader(s);
        GLint ok = 0;
        api->glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            api->glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            VV_LOGE("dmabuf", "shader: %s", log);
            api->glDeleteShader(s);
            return 0;
        }
        return s;
    }

    bool MakeCurrent()
    {
        if (current) return program != 0;
        if (!api->eglMakeCurrent(display, surface, surface, context)) {
            VV_LOGE("dmabuf", "eglMakeCurrent failed: 0x%x", api->eglGetError());
            return false;
        }
        current = true;
        if (!HasExtension(reinterpret_cast<const char*>(api->glGetString(GL_EXTENSIONS)), "GL_OES_EGL_image_external")) {
            VV_LOGW("dmabuf", "GL_OES_EGL_image_external missing");
            return false;
        }
        GLuint vs = Compile(GL_VERTEX_SHADER, kVertexShader), fs = Compile(GL_FRAGMENT_SHADER, kFragmentShader);
        if (!vs || !fs) return false;
        program = api->glCreateProgram();
        api->glAttachShader(program, vs);
        api->glAttachShader(program, fs);
        api->glBindAttribLocation(program, 0, "pos");
        api->glLinkProgram(program);
        api->glDeleteShader(vs);
        api->glDeleteShader(fs);
        GLint ok = 0;
        api->glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            api->glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            VV_LOGE("dmabuf", "program: %s", log);
            api->glDeleteProgram(program);
            program = 0;
            return false;
        }
        uSize = api->glGetUniformLocation(program, "size");
        uOrigin = api->glGetUniformLocation(program, "origin");
        uPlane = api->glGetUniformLocation(program, "plane");
        uFrame = api->glGetUniformLocation(program, "frame");
        api->glGenFramebuffers(1, &framebuffer);
        api->glGenTextures(1, &target);
        VV_LOGI("dmabuf", "GL: %s, %s", reinterpret_cast<const char*>(api->glGetString(GL_RENDERER)),
                reinterpret_cast<const char*>(api->glGetString(GL_VERSION)));
        return true;
    }

    Image* Import(uint32_t format, uint64_t modifier, int width, int height, const std::vector<Plane>& planes)
    {
        auto& image = images[planes[0].fd];
        if (image.image != EGL_NO_IMAGE_KHR && image.format == format && image.modifier == modifier && image.width == width &&
            image.height == height && image.offset == planes[0].offset && image.stride == planes[0].stride)
            return &image;
        DestroyImage(image);

        static const EGLint kFd[] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
        static const EGLint kOffset[] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT,
                                         EGL_DMA_BUF_PLANE3_OFFSET_EXT};
        static const EGLint kPitch[] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
                                        EGL_DMA_BUF_PLANE3_PITCH_EXT};
        static const EGLint kModLo[] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                                        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
        static const EGLint kModHi[] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
                                        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
        std::vector<EGLint> attribs = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(format)};
        for (size_t i = 0; i < planes.size() && i < 4; i++) {
            attribs.insert(attribs.end(), {kFd[i], planes[i].fd, kOffset[i], static_cast<EGLint>(planes[i].offset), kPitch[i],
                                           static_cast<EGLint>(planes[i].stride)});
            if (modifier != DRM_FORMAT_MOD_INVALID)
                attribs.insert(attribs.end(), {kModLo[i], static_cast<EGLint>(modifier & 0xffffffff), kModHi[i],
                                               static_cast<EGLint>(modifier >> 32)});
        }
        attribs.push_back(EGL_NONE);
        image.image = api->eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs.data());
        if (image.image == EGL_NO_IMAGE_KHR) {
            VV_LOGE("dmabuf", "cannot import a %dx%d frame (format 0x%x, modifier 0x%llx, %zu planes): EGL error 0x%x", width,
                    height, format, static_cast<unsigned long long>(modifier), planes.size(), api->eglGetError());
            images.erase(planes[0].fd);
            return nullptr;
        }
        api->glGenTextures(1, &image.texture);
        api->glBindTexture(GL_TEXTURE_EXTERNAL_OES, image.texture);
        api->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        api->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        api->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        api->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        api->glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image.image);
        image.format = format;
        image.modifier = modifier;
        image.width = width;
        image.height = height;
        image.offset = planes[0].offset;
        image.stride = planes[0].stride;
        return &image;
    }
};

EglDmaBuf::EglDmaBuf() : impl_(std::make_unique<Impl>()) {}

EglDmaBuf::~EglDmaBuf()
{
    auto& m = *impl_;
    if (!m.api) return;
    for (auto& [fd, image] : m.images) m.DestroyImage(image);
    for (auto& [fd, handle] : m.syncObjs) {
        drm_syncobj_destroy d{};
        d.handle = handle;
        Impl::Ioctl(m.drmFd, DRM_IOCTL_SYNCOBJ_DESTROY, &d);
    }
    if (m.drmFd >= 0) close(m.drmFd);
    if (m.context != EGL_NO_CONTEXT) m.api->eglDestroyContext(m.display, m.context);
    if (m.surface != EGL_NO_SURFACE) m.api->eglDestroySurface(m.display, m.surface);
    // the display stays initialized: EGL displays are per process, other captures may use it
}

bool EglDmaBuf::Init()
{
    auto& m = *impl_;
    m.api = Api();
    if (!m.api) return false;
    auto* api = m.api;

    // The GPU's own display (EGL_EXT_platform_device), the first device with a DRM render node; else Mesa's
    // surfaceless platform. Both need no window system
    if (api->eglQueryDevicesEXT && api->eglQueryDeviceStringEXT) {
        EGLDeviceEXT devices[16];
        EGLint count = 0;
        if (api->eglQueryDevicesEXT(16, devices, &count)) {
            for (EGLint i = 0; i < count && m.display == EGL_NO_DISPLAY; i++) {
                const char* node = HasExtension(api->eglQueryDeviceStringEXT(devices[i], EGL_EXTENSIONS), "EGL_EXT_device_drm_render_node")
                                       ? api->eglQueryDeviceStringEXT(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT)
                                       : nullptr;
                if (!node) continue;
                EGLDisplay d = api->eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr);
                if (d != EGL_NO_DISPLAY && api->eglInitialize(d, nullptr, nullptr)) {
                    m.display = d;
                    m.drmNode = node;
                    VV_LOGI("dmabuf", "EGL device %s", node);
                }
            }
        }
    }
    if (m.display == EGL_NO_DISPLAY) {
        EGLDisplay d = api->eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (d != EGL_NO_DISPLAY && api->eglInitialize(d, nullptr, nullptr)) {
            m.display = d;
            m.drmNode = "/dev/dri/renderD128"; // Mesa's default GPU, for the sync objects
            VV_LOGI("dmabuf", "EGL surfaceless platform");
        }
    }
    if (m.display == EGL_NO_DISPLAY) {
        VV_LOGW("dmabuf", "no EGL display");
        return false;
    }
    const char* ext = api->eglQueryString(m.display, EGL_EXTENSIONS);
    if (!HasExtension(ext, "EGL_EXT_image_dma_buf_import") || !HasExtension(ext, "EGL_EXT_image_dma_buf_import_modifiers")) {
        VV_LOGW("dmabuf", "EGL has no DMA-BUF import with modifiers");
        return false;
    }
    if (!api->eglBindAPI(EGL_OPENGL_ES_API)) return false;
    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLConfig config = nullptr;
    if (!HasExtension(ext, "EGL_KHR_no_config_context") || !HasExtension(ext, "EGL_KHR_surfaceless_context")) {
        const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
        EGLint n = 0;
        if (!api->eglChooseConfig(m.display, configAttribs, &config, 1, &n) || n < 1) return false;
        const EGLint pbufferAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        m.surface = api->eglCreatePbufferSurface(m.display, config, pbufferAttribs);
    }
    m.context = api->eglCreateContext(m.display, config ? config : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, contextAttribs);
    if (m.context == EGL_NO_CONTEXT) {
        VV_LOGW("dmabuf", "eglCreateContext failed: 0x%x", api->eglGetError());
        return false;
    }
    return true;
}

std::vector<uint64_t> EglDmaBuf::Modifiers(uint32_t drmFormat) const
{
    auto& m = *impl_;
    std::vector<uint64_t> mods;
    EGLint count = 0;
    if (m.api && m.api->eglQueryDmaBufModifiersEXT(m.display, static_cast<EGLint>(drmFormat), 0, nullptr, nullptr, &count) && count > 0) {
        mods.resize(count);
        m.api->eglQueryDmaBufModifiersEXT(m.display, static_cast<EGLint>(drmFormat), count, reinterpret_cast<EGLuint64KHR*>(mods.data()),
                                          nullptr, &count);
        mods.resize(count);
    }
    if (mods.empty()) mods.push_back(DRM_FORMAT_MOD_INVALID);
    return mods;
}

webrtc::scoped_refptr<webrtc::I420Buffer> EglDmaBuf::ReadI420(uint32_t drmFormat, uint64_t modifier, int width, int height,
                                                              const std::vector<Plane>& planes)
{
    auto& m = *impl_;
    if (planes.empty() || width < 2 || height < 2 || !m.MakeCurrent()) return nullptr;
    auto* api = m.api;
    auto* image = m.Import(drmFormat, modifier, width, height, planes);
    if (!image) return nullptr;

    int w = width & ~1, h = height & ~1;
    int p = (w + 7) / 8;
    int tw = 2 * p, th = h + h / 2;
    if (tw != m.targetWidth || th != m.targetHeight) {
        api->glBindTexture(GL_TEXTURE_2D, m.target);
        api->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        api->glBindFramebuffer(GL_FRAMEBUFFER, m.framebuffer);
        api->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m.target, 0);
        if (api->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            VV_LOGE("dmabuf", "framebuffer incomplete");
            return nullptr;
        }
        m.targetWidth = tw;
        m.targetHeight = th;
    }
    api->glBindFramebuffer(GL_FRAMEBUFFER, m.framebuffer);
    api->glUseProgram(m.program);
    api->glActiveTexture(GL_TEXTURE0);
    api->glBindTexture(GL_TEXTURE_EXTERNAL_OES, image->texture);
    api->glUniform1i(m.uFrame, 0);
    api->glUniform2f(m.uSize, static_cast<float>(width), static_cast<float>(height));
    api->glDisable(GL_BLEND);
    static const GLfloat kQuad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    api->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, kQuad);
    api->glEnableVertexAttribArray(0);
    struct Region {
        int plane, x, y, w, h;
    };
    const Region regions[] = {{0, 0, 0, tw, h}, {1, 0, h, p, h / 2}, {2, p, h, p, h / 2}};
    for (auto& r : regions) {
        api->glViewport(r.x, r.y, r.w, r.h);
        api->glUniform1i(m.uPlane, r.plane);
        api->glUniform2f(m.uOrigin, static_cast<float>(r.x), static_cast<float>(r.y));
        api->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // read straight into the planes: their strides are the packed rows
    auto buffer = webrtc::I420Buffer::Create(w, h, 8 * p, 4 * p, 4 * p);
    api->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    api->glReadPixels(0, 0, tw, h, GL_RGBA, GL_UNSIGNED_BYTE, buffer->MutableDataY());
    api->glReadPixels(0, h, p, h / 2, GL_RGBA, GL_UNSIGNED_BYTE, buffer->MutableDataU());
    api->glReadPixels(p, h, p, h / 2, GL_RGBA, GL_UNSIGNED_BYTE, buffer->MutableDataV());
    if (GLenum e = api->glGetError(); e != GL_NO_ERROR) {
        VV_LOGE("dmabuf", "GL error 0x%x reading a frame", e);
        return nullptr;
    }
    if (!m.loggedFirst) {
        m.loggedFirst = true;
        VV_LOGI("dmabuf", "first frame read: %dx%d, format 0x%x, modifier 0x%llx, %zu planes", width, height, drmFormat,
                static_cast<unsigned long long>(modifier), planes.size());
    }
    return buffer;
}

bool EglDmaBuf::ExplicitSync()
{
    auto& m = *impl_;
    if (m.explicitSync >= 0) return m.explicitSync == 1;
    m.explicitSync = 0;
    if (m.drmNode.empty()) return false;
    m.drmFd = open(m.drmNode.c_str(), O_RDWR | O_CLOEXEC);
    if (m.drmFd < 0) return false;
    drm_get_cap syncobj{DRM_CAP_SYNCOBJ, 0}, timeline{DRM_CAP_SYNCOBJ_TIMELINE, 0};
    if (Impl::Ioctl(m.drmFd, DRM_IOCTL_GET_CAP, &syncobj) == 0 && Impl::Ioctl(m.drmFd, DRM_IOCTL_GET_CAP, &timeline) == 0 &&
        syncobj.value && timeline.value)
        m.explicitSync = 1;
    VV_LOGI("dmabuf", "explicit sync (timeline sync objects on %s): %s", m.drmNode.c_str(), m.explicitSync ? "yes" : "no");
    return m.explicitSync == 1;
}

bool EglDmaBuf::WaitAcquire(int syncFd, uint64_t point)
{
    auto& m = *impl_;
    uint32_t handle = m.SyncObj(syncFd);
    if (!handle) return false;
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    drm_syncobj_timeline_wait w{};
    w.handles = reinterpret_cast<uintptr_t>(&handle);
    w.points = reinterpret_cast<uintptr_t>(&point);
    w.count_handles = 1;
    w.timeout_nsec = now.tv_sec * 1'000'000'000LL + now.tv_nsec + 100'000'000LL; // absolute
    w.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
    if (Impl::Ioctl(m.drmFd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &w) != 0) {
        VV_LOGW("dmabuf", "waiting for a frame's acquire point %llu: %s", static_cast<unsigned long long>(point), strerror(errno));
        return false;
    }
    return true;
}

bool EglDmaBuf::SignalRelease(int syncFd, uint64_t point)
{
    auto& m = *impl_;
    uint32_t handle = m.SyncObj(syncFd);
    if (!handle) return false;
    drm_syncobj_timeline_array a{};
    a.handles = reinterpret_cast<uintptr_t>(&handle);
    a.points = reinterpret_cast<uintptr_t>(&point);
    a.count_handles = 1;
    if (Impl::Ioctl(m.drmFd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &a) != 0) {
        VV_LOGW("dmabuf", "signalling a frame's release point %llu: %s", static_cast<unsigned long long>(point), strerror(errno));
        return false;
    }
    return true;
}

void EglDmaBuf::Forget(int fd)
{
    auto& m = *impl_;
    if (auto it = m.syncObjs.find(fd); it != m.syncObjs.end()) {
        drm_syncobj_destroy d{};
        d.handle = it->second;
        Impl::Ioctl(m.drmFd, DRM_IOCTL_SYNCOBJ_DESTROY, &d);
        m.syncObjs.erase(it);
        return;
    }
    auto it = m.images.find(fd);
    if (it == m.images.end()) return;
    m.DestroyImage(it->second);
    m.images.erase(it);
}

void EglDmaBuf::Release()
{
    auto& m = *impl_;
    if (!m.api || !m.current) return;
    for (auto& [fd, image] : m.images) m.DestroyImage(image);
    m.images.clear();
    if (m.program) m.api->glDeleteProgram(m.program);
    if (m.target) m.api->glDeleteTextures(1, &m.target);
    if (m.framebuffer) m.api->glDeleteFramebuffers(1, &m.framebuffer);
    m.program = m.target = m.framebuffer = 0;
    m.targetWidth = m.targetHeight = 0;
    m.api->eglMakeCurrent(m.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    m.current = false;
}

} // namespace vv
