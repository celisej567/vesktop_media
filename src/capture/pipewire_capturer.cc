#include "capture/pipewire_capturer.h"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/param/buffers.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/result.h>

#include <api/video/video_frame.h>
#include <libyuv/convert.h>
#include <rtc_base/time_utils.h>

#include <drm/drm_fourcc.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <vector>

#include "common/log.h"
#include "capture/egl_dmabuf.h"
#include "common/platform.h"
#include "video/rgb_to_i420.h"

namespace vv {

namespace {
// the app of the latest capture that named one (PipeWireCapturer::LastAppId)
std::mutex gLastAppMutex;
std::string gLastAppId;
} // namespace


namespace {

#define VV_PW_FUNCTIONS(X)                                                                                             \
    X(pw_init)                                                                                                         \
    X(pw_thread_loop_new)                                                                                              \
    X(pw_thread_loop_start)                                                                                            \
    X(pw_thread_loop_stop)                                                                                             \
    X(pw_thread_loop_destroy)                                                                                          \
    X(pw_thread_loop_lock)                                                                                             \
    X(pw_thread_loop_unlock)                                                                                           \
    X(pw_thread_loop_get_loop)                                                                                         \
    X(pw_context_new)                                                                                                  \
    X(pw_context_destroy)                                                                                              \
    X(pw_context_connect_fd)                                                                                           \
    X(pw_context_connect)                                                                                              \
    X(pw_core_disconnect)                                                                                              \
    X(pw_stream_new)                                                                                                   \
    X(pw_stream_add_listener)                                                                                          \
    X(pw_stream_connect)                                                                                               \
    X(pw_stream_disconnect)                                                                                            \
    X(pw_stream_destroy)                                                                                               \
    X(pw_stream_dequeue_buffer)                                                                                        \
    X(pw_stream_queue_buffer)                                                                                          \
    X(pw_stream_update_params)                                                                                         \
    X(pw_stream_state_as_string)                                                                                       \
    X(pw_properties_new)                                                                                               \
    X(pw_proxy_add_object_listener)                                                                                    \
    X(pw_thread_loop_signal)                                                                                           \
    X(pw_properties_set)                                                                                               \
    X(pw_thread_loop_timed_wait)                                                                                       \
    X(pw_proxy_destroy)

struct PwApi {
#define VV_PW_MEMBER(name) decltype(&::name) name = nullptr;
    VV_PW_FUNCTIONS(VV_PW_MEMBER)
#undef VV_PW_MEMBER
};

const PwApi* Pw()
{
    static const PwApi* api = []() -> const PwApi* {
        void* lib = OpenLibrary({"libpipewire-0.3.so.0"});
        if (!lib) {
            VV_LOGE("pipewire", "libpipewire-0.3 not found");
            return nullptr;
        }
        auto* a = new PwApi();
        bool ok = true;
#define VV_PW_LOAD(name)                                                                                               \
    a->name = reinterpret_cast<decltype(a->name)>(LibrarySymbol(lib, #name));                                          \
    if (!a->name) {                                                                                                    \
        VV_LOGE("pipewire", "symbol %s missing", #name);                                                               \
        ok = false;                                                                                                    \
    }
        VV_PW_FUNCTIONS(VV_PW_LOAD)
#undef VV_PW_LOAD
        if (!ok) return nullptr;
        a->pw_init(nullptr, nullptr);
        return a;
    }();
    return api;
}

// Text of a SPA pod, for the log (numbers only: keys and ids are not translated)
std::string DumpPod(const spa_pod* pod, int depth = 0)
{
    if (!pod || depth > 6) return "…";
    char buf[96];
    switch (SPA_POD_TYPE(pod)) {
    case SPA_TYPE_Id: snprintf(buf, sizeof(buf), "id:%u", *static_cast<const uint32_t*>(SPA_POD_BODY_CONST(pod))); return buf;
    case SPA_TYPE_Int: snprintf(buf, sizeof(buf), "%d", *static_cast<const int32_t*>(SPA_POD_BODY_CONST(pod))); return buf;
    case SPA_TYPE_Long: snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(*static_cast<const int64_t*>(SPA_POD_BODY_CONST(pod)))); return buf;
    case SPA_TYPE_Bool: return *static_cast<const uint32_t*>(SPA_POD_BODY_CONST(pod)) ? "true" : "false";
    case SPA_TYPE_Rectangle: {
        auto* r = static_cast<const spa_rectangle*>(SPA_POD_BODY_CONST(pod));
        snprintf(buf, sizeof(buf), "%ux%u", r->width, r->height);
        return buf;
    }
    case SPA_TYPE_Fraction: {
        auto* f = static_cast<const spa_fraction*>(SPA_POD_BODY_CONST(pod));
        snprintf(buf, sizeof(buf), "%u/%u", f->num, f->denom);
        return buf;
    }
    case SPA_TYPE_Choice: {
        auto* c = reinterpret_cast<const spa_pod_choice*>(pod);
        static const char* kinds[] = {"none", "range", "step", "enum", "flags"};
        std::string s = std::string(c->body.type < 5 ? kinds[c->body.type] : "?") + "(";
        const spa_pod* child = &c->body.child;
        uint32_t n = (SPA_POD_BODY_SIZE(pod) - sizeof(spa_pod_choice_body)) / std::max<uint32_t>(1, child->size);
        const uint8_t* p = static_cast<const uint8_t*>(SPA_POD_CONTENTS(spa_pod_choice, pod));
        for (uint32_t i = 0; i < n; i++) {
            // a value without its own pod header: wrap it
            alignas(8) uint8_t tmp[64];
            if (child->size > 40) break;
            auto* wrapped = reinterpret_cast<spa_pod*>(tmp);
            wrapped->size = child->size;
            wrapped->type = child->type;
            memcpy(tmp + sizeof(spa_pod), p + i * child->size, child->size);
            s += (i ? "," : "") + DumpPod(wrapped, depth + 1);
        }
        return s + ")";
    }
    case SPA_TYPE_Object: {
        std::string s = "{";
        const spa_pod_object* o = reinterpret_cast<const spa_pod_object*>(pod);
        const spa_pod_prop* prop;
        SPA_POD_OBJECT_FOREACH(o, prop) s += " " + std::to_string(prop->key) + "=" + DumpPod(&prop->value, depth + 1);
        return s + " }";
    }
    default: snprintf(buf, sizeof(buf), "type%u", SPA_POD_TYPE(pod)); return buf;
    }
}

// The packed RGB formats a screen comes in, as PipeWire names them and as DRM (DMA-BUF) does
struct RgbFormat {
    spa_video_format spa;
    uint32_t drm;
};
constexpr RgbFormat kRgbFormats[] = {{SPA_VIDEO_FORMAT_BGRx, DRM_FORMAT_XRGB8888},
                                     {SPA_VIDEO_FORMAT_BGRA, DRM_FORMAT_ARGB8888},
                                     {SPA_VIDEO_FORMAT_RGBx, DRM_FORMAT_XBGR8888},
                                     {SPA_VIDEO_FORMAT_RGBA, DRM_FORMAT_ABGR8888}};

std::string Hex(uint64_t v)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
    return buf;
}

uint32_t DrmFormat(uint32_t spaFormat)
{
    for (auto& f : kRgbFormats)
        if (f.spa == spaFormat) return f.drm;
    return 0;
}

// Frames in GPU memory (EglDmaBuf) unless VESKTOP_MEDIA_PIPEWIRE_DMABUF=0; shared memory when that fails
bool DmaBufWanted()
{
    const char* v = getenv("VESKTOP_MEDIA_PIPEWIRE_DMABUF");
    return !v || strcmp(v, "0") != 0;
}

// The explicit sync timeline of a buffer, when it was negotiated: the metadata and the acquire / release sync object
// fds (the buffer's last two datas)
struct SyncTimeline {
    spa_meta_sync_timeline* meta = nullptr;
    int acquireFd = -1, releaseFd = -1;
};

SyncTimeline FindSyncTimeline(spa_buffer* buffer)
{
    SyncTimeline t;
    if (!buffer || buffer->n_datas < 3) return t;
    auto* meta = static_cast<spa_meta_sync_timeline*>(spa_buffer_find_meta_data(buffer, SPA_META_SyncTimeline, sizeof(spa_meta_sync_timeline)));
    auto& acquire = buffer->datas[buffer->n_datas - 2];
    auto& release = buffer->datas[buffer->n_datas - 1];
    if (!meta || acquire.type != SPA_DATA_SyncObj || release.type != SPA_DATA_SyncObj) return t;
    t.meta = meta;
    t.acquireFd = static_cast<int>(acquire.fd);
    t.releaseFd = static_cast<int>(release.fd);
    return t;
}

// What we accept: with `egl`, each RGB format as a DMA-BUF with the modifiers the GPU imports (the compositor, which
// allocates, picks one: DONT_FIXATE), then any of them in shared memory
std::vector<const spa_pod*> BuildFormats(spa_pod_builder* b, const EglDmaBuf* egl)
{
    spa_rectangle defSize = {1920, 1080}, minSize = {1, 1}, maxSize = {16384, 16384};
    spa_fraction defRate = {60, 1}, minRate = {0, 1}, maxRate = {240, 1};
    std::vector<const spa_pod*> params;
    if (egl) {
        for (auto& f : kRgbFormats) {
            auto mods = egl->Modifiers(f.drm);
            spa_pod_frame frame, choice;
            spa_pod_builder_push_object(b, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
            spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
                                SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format, SPA_POD_Id(f.spa), SPA_FORMAT_VIDEO_size,
                                SPA_POD_CHOICE_RANGE_Rectangle(&defSize, &minSize, &maxSize), SPA_FORMAT_VIDEO_framerate,
                                SPA_POD_CHOICE_RANGE_Fraction(&defRate, &minRate, &maxRate), 0);
            spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
            spa_pod_builder_push_choice(b, &choice, SPA_CHOICE_Enum, 0);
            spa_pod_builder_long(b, static_cast<int64_t>(mods[0])); // the default
            for (uint64_t m : mods) spa_pod_builder_long(b, static_cast<int64_t>(m));
            spa_pod_builder_pop(b, &choice);
            params.push_back(static_cast<const spa_pod*>(spa_pod_builder_pop(b, &frame)));
        }
    }
    params.push_back(static_cast<const spa_pod*>(spa_pod_builder_add_object(
        b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_BGRA,
                               SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&defSize, &minSize, &maxSize), SPA_FORMAT_VIDEO_framerate,
        SPA_POD_CHOICE_RANGE_Fraction(&defRate, &minRate, &maxRate))));
    return params;
}

} // namespace

struct PipeWireCapturer::Impl {
    PipeWireCapturer* owner;
    PickedSource source;
    webrtc::scoped_refptr<FrameSource> sink;
    std::mutex endedMutex;
    std::function<void(const std::string&)> ended;
    std::string endReason;
    std::atomic<bool> endedReported{false};

    pw_thread_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_hook listener{};
    spa_video_info_raw format{};
    bool streaming = false;
    // DMA-BUF (VESKTOP_MEDIA_PIPEWIRE_DMABUF): the reader, whether the negotiated format is a DMA-BUF one, and
    // whether reading one failed (back to shared memory for good)
    std::unique_ptr<EglDmaBuf> egl;
    bool dmabufFormat = false;
    bool dmabufFailed = false;
    bool explicitSync = false; // buffers come with a SyncTimeline
    // what the compositor sent in the last 5 s (pipewire thread)
    int64_t statsStartUs = 0;
    int statsBuffers = 0, statsConverted = 0;
    int64_t statsConvertUs = 0, statsConvertMaxUs = 0;
    uint64_t statsLastPts = 0;
    int64_t statsIntervalSumUs = 0, statsIntervalMaxUs = 0;
    int statsIntervals = 0;

    // what the source offers (logged once): the answer to "no more input formats"
    pw_registry* registry = nullptr;
    spa_hook registryListener{};
    pw_node* node = nullptr;
    spa_hook nodeListener{};
    int dumped = 0;
    bool dumpPending = true;
    std::string pickSerial; // object.serial of the picked node, what target.object wants
    std::mutex nameMutex;
    std::string sourceMediaName; // media.name of the picked node (pipewire thread, under nameMutex)
    spa_hook coreListener{};
    int syncSeq = -1;
    bool synced = false;

    static void OnCoreDone(void* data, uint32_t id, int seq)
    {
        auto* self = static_cast<Impl*>(data);
        if (id != PW_ID_CORE || seq != self->syncSeq) return;
        self->synced = true;
        Pw()->pw_thread_loop_signal(self->loop, false);
    }

    // The node's full properties: KWin's media.name ("kwin-screencast-<app>") is not in the registry's subset
    static void OnNodeInfo(void* data, const pw_node_info* info)
    {
        auto* self = static_cast<Impl*>(data);
        const char* name = info && info->props ? spa_dict_lookup(info->props, PW_KEY_MEDIA_NAME) : nullptr;
        if (!name) return;
        std::lock_guard lock(self->nameMutex);
        if (self->sourceMediaName == name) return;
        self->sourceMediaName = name;
        VV_LOGI("pipewire", "source node media name: %s", name);
        static const std::string kKwin = "kwin-screencast-";
        std::string n = name;
        std::lock_guard last(gLastAppMutex);
        gLastAppId = n.rfind(kKwin, 0) == 0 ? n.substr(kKwin.size()) : "";
    }

    static void OnNodeParam(void* data, int, uint32_t id, uint32_t, uint32_t, const spa_pod* param)
    {
        auto* self = static_cast<Impl*>(data);
        if (id != SPA_PARAM_EnumFormat || self->dumped >= 24) return;
        self->dumped++;
        VV_LOGI("pipewire", "source format %d: %s", self->dumped, DumpPod(param).c_str());
    }

    static void OnGlobal(void* data, uint32_t id, uint32_t permissions, const char* type, uint32_t, const spa_dict* props)
    {
        auto* self = static_cast<Impl*>(data);
        if (id == self->source.nodeId && type && strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
            auto get = [&](const char* key) {
                const char* v = props ? spa_dict_lookup(props, key) : nullptr;
                return v ? v : "-";
            };
            VV_LOGI("pipewire", "source node %u: class %s, name %s, media %s, serial %s, permissions %x", id,
                    get("media.class"), get("node.name"), get("media.name"), get("object.serial"), permissions);
            if (const char* m = props ? spa_dict_lookup(props, PW_KEY_MEDIA_NAME) : nullptr) {
                std::lock_guard lock(self->nameMutex);
                self->sourceMediaName = m;
            }
            if (strcmp(get("object.serial"), "-") != 0) self->pickSerial = get("object.serial");
        }
        if (id != self->source.nodeId || self->node || !type || strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
        static const pw_node_events events = [] {
            pw_node_events e{};
            e.version = PW_VERSION_NODE_EVENTS;
            e.param = &Impl::OnNodeParam;
            e.info = &Impl::OnNodeInfo;
            return e;
        }();
        self->node = static_cast<pw_node*>(pw_registry_bind(self->registry, id, type, PW_VERSION_NODE, 0));
        if (!self->node) return;
        Pw()->pw_proxy_add_object_listener(reinterpret_cast<pw_proxy*>(self->node), &self->nodeListener, &events, self);
        pw_node_enum_params(self->node, 0, SPA_PARAM_EnumFormat, 0, 0xffffffff, nullptr);
    }

    static void OnGlobalRemove(void* data, uint32_t id)
    {
        // the compositor removed the stream (the window was closed, the output unplugged): our stream only
        // pauses, and the repeater would keep sending the last frame
        auto* self = static_cast<Impl*>(data);
        if (id == self->source.nodeId) self->End("source closed");
    }

    // last frame, repeated while the screen does not change (the compositor only sends damage)
    std::mutex lastMutex;
    webrtc::scoped_refptr<webrtc::I420Buffer> last;
    int64_t lastPushUs = 0;
    std::thread repeater;
    std::atomic<bool> running{false};

    void End(const std::string& reason)
    {
        std::function<void(const std::string&)> cb;
        {
            std::lock_guard lock(endedMutex);
            if (endedReported.exchange(true)) return;
            endReason = reason;
            cb = ended;
        }
        VV_LOGI("pipewire", "capture ended: %s", reason.c_str());
        {
            std::lock_guard lock(lastMutex);
            last = nullptr; // nothing to repeat any more
        }
        if (cb) cb(reason);
    }

    // VESKTOP_MEDIA_PIPEWIRE_DUMP=<file>: the 60th converted frame goes there as raw I420 (comparing the shared memory
    // and DMA-BUF paths)
    int dumpCountdown = 60;
    void MaybeDump(const webrtc::I420Buffer& b)
    {
        const char* path = getenv("VESKTOP_MEDIA_PIPEWIRE_DUMP");
        if (!path || dumpCountdown <= 0 || --dumpCountdown > 0) return;
        FILE* f = fopen(path, "wb");
        if (!f) return;
        for (int y = 0; y < b.height(); y++) fwrite(b.DataY() + y * b.StrideY(), 1, b.width(), f);
        for (int y = 0; y < b.ChromaHeight(); y++) fwrite(b.DataU() + y * b.StrideU(), 1, b.ChromaWidth(), f);
        for (int y = 0; y < b.ChromaHeight(); y++) fwrite(b.DataV() + y * b.StrideV(), 1, b.ChromaWidth(), f);
        fclose(f);
        VV_LOGI("pipewire", "frame dumped to %s (%dx%d I420)", path, b.width(), b.height());
    }

    void Push(webrtc::scoped_refptr<webrtc::I420Buffer> buffer)
    {
        int64_t now = webrtc::TimeMicros();
        sink->PushFrame(webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer).set_timestamp_us(now).build());
        owner->frames_++;
        std::lock_guard lock(lastMutex);
        last = buffer;
        lastPushUs = now;
    }

    static void OnStateChanged(void* data, pw_stream_state, pw_stream_state state, const char* error)
    {
        auto* self = static_cast<Impl*>(data);
        VV_LOGI("pipewire", "stream state: %s%s%s", Pw()->pw_stream_state_as_string(state), error ? " " : "", error ? error : "");
        if (state == PW_STREAM_STATE_STREAMING) self->streaming = true;
        if (state == PW_STREAM_STATE_ERROR) self->End(error ? error : "stream error");
        else if (state == PW_STREAM_STATE_UNCONNECTED && self->streaming) self->End("stream disconnected");
    }

    static void OnParamChanged(void* data, uint32_t id, const spa_pod* param)
    {
        auto* self = static_cast<Impl*>(data);
        if (!param || id != SPA_PARAM_Format) return;
        uint32_t mediaType = 0, mediaSubtype = 0;
        if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 || mediaType != SPA_MEDIA_TYPE_video ||
            mediaSubtype != SPA_MEDIA_SUBTYPE_raw)
            return;
        spa_format_video_raw_parse(param, &self->format);
        self->dmabufFormat = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier) != nullptr;
        VV_LOGI("pipewire", "format: %ux%u, video format %u, %u/%u fps, max %u/%u, %s", self->format.size.width,
                self->format.size.height, self->format.format, self->format.framerate.num, self->format.framerate.denom,
                self->format.max_framerate.num, self->format.max_framerate.denom,
                self->dmabufFormat ? ("DMA-BUF modifier 0x" + Hex(self->format.modifier)).c_str() : "shared memory");

        uint8_t buf[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        const spa_pod* params[5];
        uint32_t n = 0;
        // Explicit sync first (the producer signals when a frame is rendered, we signal when we are done with it:
        // nobody waits for the GPU), required by its buffer layout (mandatory metaType); the plain layout after it
        // (VESKTOP_MEDIA_PIPEWIRE_EXPLICIT_SYNC=0 leaves it out, for comparisons)
        const char* es = getenv("VESKTOP_MEDIA_PIPEWIRE_EXPLICIT_SYNC");
        if (self->dmabufFormat && self->egl && !(es && strcmp(es, "0") == 0) && self->egl->ExplicitSync()) {
            spa_pod_frame f;
            spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
            spa_pod_builder_add(&b, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16), SPA_PARAM_BUFFERS_dataType,
                                SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_DmaBuf), 0);
            spa_pod_builder_prop(&b, SPA_PARAM_BUFFERS_metaType, SPA_POD_PROP_FLAG_MANDATORY);
            spa_pod_builder_int(&b, 1 << SPA_META_SyncTimeline);
            params[n++] = static_cast<const spa_pod*>(spa_pod_builder_pop(&b, &f));
            params[n++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
                &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_SyncTimeline),
                SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_sync_timeline))));
        }
        params[n++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
            SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int(self->dmabufFormat ? (1 << SPA_DATA_DmaBuf) : ((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)))));
        params[n++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
            SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header))));
        Pw()->pw_stream_update_params(self->stream, params, n);
    }

    static void OnProcess(void* data)
    {
        auto* self = static_cast<Impl*>(data);
        auto* pw = Pw();
        pw_buffer* newest = nullptr;
        // drain the queue, keep only the latest frame
        while (pw_buffer* b = pw->pw_stream_dequeue_buffer(self->stream)) {
            if (newest) self->Return(newest);
            newest = b;
        }
        if (!newest) return;
        self->Convert(newest->buffer);
        self->Return(newest);
    }

    // Every buffer goes back with its release point signalled (explicit sync), read or not: the producer reuses a
    // buffer only after that
    void Return(pw_buffer* b)
    {
        auto sync = FindSyncTimeline(b->buffer);
        if (sync.meta && egl) egl->SignalRelease(sync.releaseFd, sync.meta->release_point);
        Pw()->pw_stream_queue_buffer(stream, b);
    }

    static void OnRemoveBuffer(void* data, pw_buffer* buffer)
    {
        auto* self = static_cast<Impl*>(data);
        if (!self->egl || !buffer || !buffer->buffer) return;
        for (uint32_t i = 0; i < buffer->buffer->n_datas; i++) {
            auto& d = buffer->buffer->datas[i];
            if ((i == 0 && d.type == SPA_DATA_DmaBuf) || d.type == SPA_DATA_SyncObj) self->egl->Forget(static_cast<int>(d.fd));
        }
    }

    // Reading a DMA-BUF failed: offer shared memory only from now on
    void FallBackToSharedMemory()
    {
        dmabufFailed = true;
        VV_LOGW("pipewire", "DMA-BUF frames cannot be read, back to shared memory");
        uint8_t buf[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
        auto params = BuildFormats(&b, nullptr);
        Pw()->pw_stream_update_params(stream, params.data(), static_cast<uint32_t>(params.size()));
    }

    void CountFrame(spa_buffer* buffer, bool converted, int64_t convertUs)
    {
        int64_t now = webrtc::TimeMicros();
        if (!statsStartUs) statsStartUs = now;
        statsBuffers++;
        if (converted) {
            statsConverted++;
            statsConvertUs += convertUs;
            statsConvertMaxUs = std::max(statsConvertMaxUs, convertUs);
        }
        auto* header = static_cast<spa_meta_header*>(spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof(spa_meta_header)));
        if (header && header->pts > 0) {
            if (statsLastPts && static_cast<uint64_t>(header->pts) > statsLastPts) {
                int64_t interval = static_cast<int64_t>((static_cast<uint64_t>(header->pts) - statsLastPts) / 1000);
                statsIntervalSumUs += interval;
                statsIntervalMaxUs = std::max(statsIntervalMaxUs, interval);
                statsIntervals++;
            }
            statsLastPts = static_cast<uint64_t>(header->pts);
        }
        if (now - statsStartUs < 5'000'000) return;
        VV_LOGI("pipewire", "last 5 s: %d frames from the compositor (%s), %d converted in %lld us on average (max %lld), "
                            "frame interval %.1f ms on average (max %.1f)",
                statsBuffers, dmabufFormat ? (explicitSync ? "DMA-BUF, explicit sync" : "DMA-BUF") : "shared memory", statsConverted,
                static_cast<long long>(statsConverted ? statsConvertUs / statsConverted : 0), static_cast<long long>(statsConvertMaxUs),
                statsIntervals ? statsIntervalSumUs / 1000.0 / statsIntervals : 0.0, statsIntervalMaxUs / 1000.0);
        statsStartUs = now;
        statsBuffers = statsConverted = statsIntervals = 0;
        statsConvertUs = statsConvertMaxUs = statsIntervalSumUs = statsIntervalMaxUs = 0;
    }

    void Convert(spa_buffer* buffer)
    {
        if (!buffer || buffer->n_datas < 1) return;
        auto& d = buffer->datas[0];
        auto* header = static_cast<spa_meta_header*>(spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof(spa_meta_header)));
        if (header && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED)) return;
        if (!d.chunk || (d.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) return;
        int width = static_cast<int>(format.size.width), height = static_cast<int>(format.size.height);
        int64_t t0 = webrtc::TimeMicros();
        if (d.type == SPA_DATA_DmaBuf) {
            // the planes are the leading DMA-BUF datas (explicit sync would add its sync objects after them)
            std::vector<EglDmaBuf::Plane> planes;
            for (uint32_t i = 0; i < buffer->n_datas && buffer->datas[i].type == SPA_DATA_DmaBuf; i++)
                planes.push_back({static_cast<int>(buffer->datas[i].fd), buffer->datas[i].chunk->offset,
                                  static_cast<uint32_t>(buffer->datas[i].chunk->stride)});
            auto sync = FindSyncTimeline(buffer);
            if (sync.meta && !explicitSync) {
                explicitSync = true;
                VV_LOGI("pipewire", "explicit sync in use");
            }
            // with explicit sync the frame may still be rendering: wait for the producer's acquire point
            if (sync.meta && egl && !egl->WaitAcquire(sync.acquireFd, sync.meta->acquire_point)) return;
            auto i420 = egl && !dmabufFailed ? egl->ReadI420(DrmFormat(format.format), format.modifier, width, height, planes) : nullptr;
            CountFrame(buffer, i420 != nullptr, webrtc::TimeMicros() - t0);
            if (i420) {
                MaybeDump(*i420);
                Push(i420);
            } else if (!dmabufFailed) {
                FallBackToSharedMemory();
            }
            return;
        }
        if (!d.data || d.chunk->size == 0) return;
        int stride = d.chunk->stride > 0 ? d.chunk->stride : width * 4;
        if (width <= 0 || height <= 0 || static_cast<size_t>(stride) * height > d.maxsize) return;
        auto* src = static_cast<const uint8_t*>(d.data) + d.chunk->offset;
        auto i420 = webrtc::I420Buffer::Create(width & ~1, height & ~1);
        int w = i420->width(), h = i420->height();
        bool ok = false;
        switch (format.format) {
        case SPA_VIDEO_FORMAT_BGRx:
        case SPA_VIDEO_FORMAT_BGRA: // B, G, R, A in memory
        case SPA_VIDEO_FORMAT_RGBx:
        case SPA_VIDEO_FORMAT_RGBA: // R, G, B, A in memory
            ok = RgbToI420(src, stride, format.format == SPA_VIDEO_FORMAT_BGRx || format.format == SPA_VIDEO_FORMAT_BGRA,
                           i420->MutableDataY(), i420->StrideY(), i420->MutableDataU(), i420->StrideU(), i420->MutableDataV(),
                           i420->StrideV(), w, h);
            break;
        default:
            break;
        }
        CountFrame(buffer, ok, webrtc::TimeMicros() - t0);
        if (!ok) return;
        MaybeDump(*i420);
        Push(i420);
    }
};

PipeWireCapturer::PipeWireCapturer(PickedSource source, webrtc::scoped_refptr<FrameSource> sink,
                                   std::function<void(const std::string&)> ended)
    : impl_(std::make_unique<Impl>())
{
    impl_->owner = this;
    impl_->source = source;
    impl_->sink = std::move(sink);
    impl_->ended = std::move(ended);
}

bool PipeWireCapturer::Start()
{
    auto* pw = Pw();
    auto& m = *impl_;
    if (!pw) return false;
    m.loop = pw->pw_thread_loop_new("vv-pipewire", nullptr);
    if (!m.loop) return false;
    m.context = pw->pw_context_new(pw->pw_thread_loop_get_loop(m.loop), nullptr, 0);
    if (!m.context || pw->pw_thread_loop_start(m.loop) < 0) return false;

    pw->pw_thread_loop_lock(m.loop);
    // the portal's remote (takes the fd); -1 = the default daemon (test hook)
    m.core = m.source.fd >= 0 ? pw->pw_context_connect_fd(m.context, m.source.fd, nullptr, 0)
                              : pw->pw_context_connect(m.context, nullptr, 0);
    m.source.fd = -1;
    if (!m.core) {
        pw->pw_thread_loop_unlock(m.loop);
        VV_LOGE("pipewire", "cannot connect to the portal's PipeWire remote");
        return false;
    }
    // The portal's remote lists the picked node; learn its object.serial first (one roundtrip) and link by it
    // (target.object). The registry also tells when the compositor removes the node (OnGlobalRemove)
    {
        static const pw_registry_events events = [] {
            pw_registry_events e{};
            e.version = PW_VERSION_REGISTRY_EVENTS;
            e.global = &Impl::OnGlobal;
            e.global_remove = &Impl::OnGlobalRemove;
            return e;
        }();
        static const pw_core_events coreEvents = [] {
            pw_core_events e{};
            e.version = PW_VERSION_CORE_EVENTS;
            e.done = &Impl::OnCoreDone;
            return e;
        }();
        m.registry = pw_core_get_registry(m.core, PW_VERSION_REGISTRY, 0);
        if (m.registry) pw_registry_add_listener(m.registry, &m.registryListener, &events, &m);
        pw_core_add_listener(m.core, &m.coreListener, &coreEvents, &m);
        m.syncSeq = pw_core_sync(m.core, PW_ID_CORE, 0);
        for (int i = 0; i < 3 && !m.synced; i++) pw->pw_thread_loop_timed_wait(m.loop, 1);
    }
    pw_properties* props = pw->pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE,
                                                 "Screen", PW_KEY_NODE_DONT_RECONNECT, "true", nullptr);
    uint32_t targetId = m.source.nodeId;
    if (!m.pickSerial.empty()) {
        pw->pw_properties_set(props, PW_KEY_TARGET_OBJECT, m.pickSerial.c_str());
        targetId = PW_ID_ANY;
        VV_LOGI("pipewire", "linking to node %u by serial %s", m.source.nodeId, m.pickSerial.c_str());
    } else {
        VV_LOGW("pipewire", "node %u not listed by the remote, linking by id", m.source.nodeId);
    }
    m.stream = pw->pw_stream_new(m.core, "vesktop_media screen capture", props);
    static const pw_stream_events events = [] {
        pw_stream_events e{};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.state_changed = &Impl::OnStateChanged;
        e.param_changed = &Impl::OnParamChanged;
        e.process = &Impl::OnProcess;
        e.remove_buffer = &Impl::OnRemoveBuffer;
        return e;
    }();
    pw->pw_stream_add_listener(m.stream, &m.listener, &events, &m);

    if (DmaBufWanted()) {
        m.egl = std::make_unique<EglDmaBuf>();
        if (!m.egl->Init()) {
            VV_LOGW("pipewire", "DMA-BUF import not available, shared memory");
            m.egl.reset();
        }
    }
    uint8_t buf[8192];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    auto params = BuildFormats(&b, m.egl.get());
    int rc = pw->pw_stream_connect(m.stream, PW_DIRECTION_INPUT, targetId,
                                   static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                                   params.data(), static_cast<uint32_t>(params.size()));
    pw->pw_thread_loop_unlock(m.loop);
    if (rc < 0) {
        VV_LOGE("pipewire", "pw_stream_connect failed (%d)", rc);
        return false;
    }

    m.running = true;
    m.repeater = std::thread([&m] {
        while (m.running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            webrtc::scoped_refptr<webrtc::I420Buffer> last;
            {
                std::lock_guard lock(m.lastMutex);
                if (!m.last || webrtc::TimeMicros() - m.lastPushUs < 500'000) continue;
                last = m.last;
            }
            m.Push(last);
        }
    });
    VV_LOGI("pipewire", "capturing node %u%s", m.source.nodeId, m.egl ? ", DMA-BUF offered" : "");
    return true;
}

std::string PipeWireCapturer::LastAppId()
{
    std::lock_guard lock(gLastAppMutex);
    return gLastAppId;
}

std::string PipeWireCapturer::SourceAppId() const
{
    static const std::string kKwin = "kwin-screencast-";
    std::string name;
    {
        std::lock_guard lock(impl_->nameMutex);
        name = impl_->sourceMediaName;
    }
    if (name.rfind(kKwin, 0) != 0) return "";
    return name.substr(kKwin.size());
}

void PipeWireCapturer::SetEnded(std::function<void(const std::string&)> ended)
{
    std::string reason;
    {
        std::lock_guard lock(impl_->endedMutex);
        if (!impl_->endedReported) {
            impl_->ended = std::move(ended);
            return;
        }
        reason = impl_->endReason;
    }
    if (ended && !reason.empty()) ended(reason); // ended before anyone listened
}

PipeWireCapturer::~PipeWireCapturer()
{
    auto* pw = Pw();
    auto& m = *impl_;
    {
        std::lock_guard lock(m.endedMutex);
        m.ended = nullptr;
        m.endedReported = true; // no ended() from here on
    }
    m.running = false;
    if (m.repeater.joinable()) m.repeater.join();
    if (!pw) return;
    // the EGL context is current on the loop's thread: release it there (no lock held: the loop runs the call)
    if (m.egl && m.loop)
        pw_loop_invoke(
            pw->pw_thread_loop_get_loop(m.loop),
            [](spa_loop*, bool, uint32_t, const void*, size_t, void* data) {
                static_cast<Impl*>(data)->egl->Release();
                return 0;
            },
            0, nullptr, 0, true, &m);
    if (m.loop) pw->pw_thread_loop_stop(m.loop);
    if (m.node) pw->pw_proxy_destroy(reinterpret_cast<pw_proxy*>(m.node));
    if (m.registry) pw->pw_proxy_destroy(reinterpret_cast<pw_proxy*>(m.registry));
    if (m.stream) {
        pw->pw_stream_disconnect(m.stream);
        pw->pw_stream_destroy(m.stream);
    }
    if (m.core) pw->pw_core_disconnect(m.core);
    if (m.context) pw->pw_context_destroy(m.context);
    if (m.loop) pw->pw_thread_loop_destroy(m.loop);
    if (m.source.fd >= 0) close(m.source.fd);
}

} // namespace vv
