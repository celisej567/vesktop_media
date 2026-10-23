#include "audio/soundshare.h"

#include <pulse/pulseaudio.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <cmath>
#include <deque>

#include "common/log.h"
#include "common/platform.h"

namespace vv {

namespace {

#define VV_PULSE_FUNCTIONS(X)                                                                                          \
    X(pa_threaded_mainloop_new)                                                                                        \
    X(pa_threaded_mainloop_free)                                                                                       \
    X(pa_threaded_mainloop_start)                                                                                      \
    X(pa_threaded_mainloop_stop)                                                                                       \
    X(pa_threaded_mainloop_lock)                                                                                       \
    X(pa_threaded_mainloop_unlock)                                                                                     \
    X(pa_threaded_mainloop_get_api)                                                                                    \
    X(pa_context_new)                                                                                                  \
    X(pa_context_unref)                                                                                                \
    X(pa_context_connect)                                                                                              \
    X(pa_context_disconnect)                                                                                           \
    X(pa_context_set_state_callback)                                                                                   \
    X(pa_context_get_state)                                                                                            \
    X(pa_context_set_subscribe_callback)                                                                               \
    X(pa_context_subscribe)                                                                                            \
    X(pa_context_get_sink_input_info_list)                                                                             \
    X(pa_context_get_sink_input_info)                                                                                  \
    X(pa_context_get_sink_info_by_index)                                                                               \
    X(pa_context_get_source_info_list)                                                                                 \
    X(pa_context_errno)                                                                                                \
    X(pa_operation_unref)                                                                                              \
    X(pa_proplist_gets)                                                                                                \
    X(pa_stream_new)                                                                                                   \
    X(pa_stream_unref)                                                                                                 \
    X(pa_stream_set_monitor_stream)                                                                                    \
    X(pa_stream_connect_record)                                                                                        \
    X(pa_stream_disconnect)                                                                                            \
    X(pa_stream_peek)                                                                                                  \
    X(pa_stream_drop)                                                                                                  \
    X(pa_stream_set_read_callback)                                                                                     \
    X(pa_stream_set_state_callback)                                                                                    \
    X(pa_stream_get_state)                                                                                             \
    X(pa_strerror)

struct PulseApi {
#define VV_PULSE_MEMBER(name) decltype(&::name) name;
    VV_PULSE_FUNCTIONS(VV_PULSE_MEMBER)
#undef VV_PULSE_MEMBER
};

const PulseApi* Pulse()
{
    static const PulseApi* api = []() -> const PulseApi* {
        void* lib = OpenLibrary({"libpulse.so.0"});
        if (!lib) {
            VV_LOGE("soundshare", "libpulse not available");
            return nullptr;
        }
        auto* a = new PulseApi();
        bool ok = true;
#define VV_PULSE_LOAD(name)                                                                                            \
    a->name = reinterpret_cast<decltype(a->name)>(LibrarySymbol(lib, #name));                                         \
    if (!a->name) {                                                                                                    \
        VV_LOGE("soundshare", "libpulse symbol %s missing", #name);                                                   \
        ok = false;                                                                                                    \
    }
        VV_PULSE_FUNCTIONS(VV_PULSE_LOAD)
#undef VV_PULSE_LOAD
        return ok ? a : nullptr;
    }();
    return api;
}

constexpr uint32_t kDeviceKey = 0xffffffff;
constexpr int kRate = 48000, kChannels = 2, kFrame = kRate / 100; // 10 ms
constexpr size_t kPrime = kFrame * kChannels * 2;                   // start mixing an input once 20 ms are buffered
constexpr size_t kMaxBuffered = kRate * kChannels / 5;              // keep at most 200 ms

struct LoopLock {
    explicit LoopLock(pa_threaded_mainloop* l) : loop(l) { Pulse()->pa_threaded_mainloop_lock(loop); }
    ~LoopLock() { Pulse()->pa_threaded_mainloop_unlock(loop); }
    pa_threaded_mainloop* loop;
};

} // namespace

struct Soundshare::Input {
    Soundshare* self = nullptr;
    uint32_t key = 0;
    pa_stream* stream = nullptr;
    std::deque<float> samples; // interleaved stereo, under bufferMutex_
    bool primed = false;
    bool recording = false;
    int channels = kChannels; // a microphone is recorded in mono and centred
};

struct Soundshare::Pending {
    Soundshare* self;
    uint32_t sinkInput;
};

bool Soundshare::Available()
{
    return Pulse() != nullptr;
}

Soundshare::Soundshare(Frames frames, Events events) : frames_(std::move(frames)), events_(std::move(events)) {}

Soundshare::~Soundshare()
{
    StopLoop();
}

void Soundshare::SetPid(uint32_t pid)
{
    {
        std::lock_guard lock(mutex_);
        if (pid == pid_) return;
        VV_LOGI("soundshare", "target pid %u", pid);
        pid_ = pid;
        if (pid && !EnsureStartedLocked()) return;
    }
    if (!loop_) return;
    LoopLock lock(loop_);
    RemoveMonitors();
    if (contextReady_ && pid) Scan();
}

void Soundshare::SetDevice(const std::string& source)
{
    {
        std::lock_guard lock(mutex_);
        if (source == device_) return;
        VV_LOGI("soundshare", "device %s", source.empty() ? "(none)" : source.c_str());
        device_ = source;
        if (!source.empty() && !EnsureStartedLocked()) return;
    }
    if (!loop_) return;
    LoopLock lock(loop_);
    RemoveInput(kDeviceKey);
    if (contextReady_ && !source.empty()) AddDevice();
}

bool Soundshare::EnsureStartedLocked()
{
    if (loop_) return true;
    if (failed_) return false;
    auto* pa = Pulse();
    if (!pa) {
        failed_ = true;
        if (events_.failed) events_.failed(1, "libpulse not available");
        return false;
    }
    loop_ = pa->pa_threaded_mainloop_new();
    context_ = pa->pa_context_new(pa->pa_threaded_mainloop_get_api(loop_), "Vesktop soundshare");
    pa->pa_context_set_state_callback(context_, &Soundshare::OnContextState, this);
    if (pa->pa_context_connect(context_, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0 || pa->pa_threaded_mainloop_start(loop_) < 0) {
        VV_LOGE("soundshare", "cannot connect to PulseAudio: %s", pa->pa_strerror(pa->pa_context_errno(context_)));
        pa->pa_context_unref(context_);
        pa->pa_threaded_mainloop_free(loop_);
        context_ = nullptr;
        loop_ = nullptr;
        failed_ = true;
        if (events_.failed) events_.failed(2, "cannot connect to PulseAudio");
        return false;
    }
    mixing_ = true;
    mixer_ = std::thread([this] { MixLoop(); });
    return true;
}

void Soundshare::StopLoop()
{
    if (mixing_.exchange(false) && mixer_.joinable()) mixer_.join();
    if (!loop_) return;
    auto* pa = Pulse();
    {
        LoopLock lock(loop_);
        while (!inputs_.empty()) RemoveInput(inputs_.begin()->first);
        pa->pa_context_set_state_callback(context_, nullptr, nullptr);
        pa->pa_context_set_subscribe_callback(context_, nullptr, nullptr);
        pa->pa_context_disconnect(context_);
    }
    pa->pa_threaded_mainloop_stop(loop_);
    pa->pa_context_unref(context_);
    pa->pa_threaded_mainloop_free(loop_);
    loop_ = nullptr;
    context_ = nullptr;
}

// --------------------------------------------------------------------------------- PulseAudio thread

void Soundshare::OnContextState(pa_context* c, void* p)
{
    auto* self = static_cast<Soundshare*>(p);
    auto* pa = Pulse();
    switch (pa->pa_context_get_state(c)) {
    case PA_CONTEXT_READY: {
        VV_LOGI("soundshare", "connected to PulseAudio");
        self->contextReady_ = true;
        pa->pa_context_set_subscribe_callback(c, [](pa_context* c, pa_subscription_event_type_t t, uint32_t i, void* s) {
            OnSubscribe(c, static_cast<int>(t), i, s);
        }, self);
        if (auto* op = pa->pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK_INPUT, nullptr, nullptr)) pa->pa_operation_unref(op);
        self->Scan();
        bool device;
        {
            std::lock_guard lock(self->mutex_);
            device = !self->device_.empty();
        }
        if (device) self->AddDevice();
        break;
    }
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        if (self->contextReady_ || pa->pa_context_get_state(c) == PA_CONTEXT_FAILED)
            VV_LOGE("soundshare", "PulseAudio connection ended: %s", pa->pa_strerror(pa->pa_context_errno(c)));
        self->contextReady_ = false;
        break;
    default:
        break;
    }
}

void Soundshare::OnSubscribe(pa_context* c, int type, uint32_t index, void* p)
{
    auto* self = static_cast<Soundshare*>(p);
    auto* pa = Pulse();
    if ((type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) != PA_SUBSCRIPTION_EVENT_SINK_INPUT) return;
    switch (type & PA_SUBSCRIPTION_EVENT_TYPE_MASK) {
    case PA_SUBSCRIPTION_EVENT_NEW:
        if (self->pid_ != 0)
            if (auto* op = pa->pa_context_get_sink_input_info(c, index, &Soundshare::OnSinkInput, self)) pa->pa_operation_unref(op);
        break;
    case PA_SUBSCRIPTION_EVENT_REMOVE:
        self->RemoveInput(index);
        break;
    default:
        break;
    }
}

void Soundshare::Scan()
{
    if (pid_ == 0) return;
    if (auto* op = Pulse()->pa_context_get_sink_input_info_list(context_, &Soundshare::OnSinkInput, this)) Pulse()->pa_operation_unref(op);
}

// The parent of a process, 0 if unknown
static uint32_t ParentPid(uint32_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/stat", pid);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    // "pid (comm) state ppid ...": comm may contain spaces and parentheses, so parse after the last ')'
    const char* close = strrchr(buf, ')');
    unsigned ppid = 0;
    char state = 0;
    if (!close || sscanf(close + 1, " %c %u", &state, &ppid) != 2) return 0;
    return ppid;
}

bool Soundshare::Wanted(uint32_t pid) const
{
    uint32_t target = pid_;
    if (target == 0) return false;
    if (target == 1) return pid != static_cast<uint32_t>(getpid());
    // the process or one of its descendants: a game started by Steam (or a browser's audio process) plays
    // its sound from a child of the process the user picked
    for (int depth = 0; pid > 1 && depth < 32; depth++, pid = ParentPid(pid))
        if (pid == target) return true;
    return false;
}

void Soundshare::OnSinkInput(pa_context* c, const pa_sink_input_info* info, int eol, void* p)
{
    auto* self = static_cast<Soundshare*>(p);
    if (eol || !info) return;
    const char* pidText = Pulse()->pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_PROCESS_ID);
    uint32_t pid = pidText ? static_cast<uint32_t>(strtoul(pidText, nullptr, 10)) : 0;
    const char* app = Pulse()->pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
    if (!self->Wanted(pid) || self->inputs_.count(info->index)) return;
    VV_LOGI("soundshare", "sink input %u: %s (pid %u) on sink %u", info->index, app ? app : info->name, pid, info->sink);
    // a monitor stream records from the monitor source of the sink the input plays on
    auto* pending = new Pending{self, info->index};
    if (auto* op = Pulse()->pa_context_get_sink_info_by_index(c, info->sink, &Soundshare::OnSink, pending)) Pulse()->pa_operation_unref(op);
    else delete pending;
}

void Soundshare::OnSink(pa_context*, const pa_sink_info* info, int eol, void* p)
{
    auto* pending = static_cast<Pending*>(p);
    if (eol) {
        delete pending;
        return;
    }
    if (info && !pending->self->inputs_.count(pending->sinkInput) && pending->self->pid_ != 0)
        pending->self->AddMonitor(pending->sinkInput, info->monitor_source_name);
}

static pa_stream* NewRecordStream(pa_context* context, const char* name, Soundshare* self, void* input,
                                  void (*state)(pa_stream*, void*), void (*read)(pa_stream*, size_t, void*), int channels = kChannels)
{
    auto* pa = Pulse();
    pa_sample_spec spec{PA_SAMPLE_FLOAT32LE, static_cast<uint32_t>(kRate), static_cast<uint8_t>(channels)};
    pa_stream* s = pa->pa_stream_new(context, name, &spec, nullptr);
    if (!s) return nullptr;
    pa->pa_stream_set_state_callback(s, state, input);
    pa->pa_stream_set_read_callback(s, read, input);
    (void)self;
    return s;
}

void Soundshare::AddMonitor(uint32_t sinkInput, const char* monitorSource)
{
    auto* pa = Pulse();
    auto input = std::make_unique<Input>();
    input->self = this;
    input->key = sinkInput;
    input->stream = NewRecordStream(context_, "Go Live audio", this, input.get(), &Soundshare::OnStreamState, &Soundshare::OnStreamRead);
    if (!input->stream) return;
    pa->pa_stream_set_monitor_stream(input->stream, sinkInput);
    pa_buffer_attr attr{static_cast<uint32_t>(-1), static_cast<uint32_t>(-1), static_cast<uint32_t>(-1), static_cast<uint32_t>(-1),
                        static_cast<uint32_t>(kFrame * kChannels * sizeof(float))};
    if (pa->pa_stream_connect_record(input->stream, monitorSource, &attr,
                                     static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE)) < 0) {
        VV_LOGE("soundshare", "cannot record sink input %u: %s", sinkInput, pa->pa_strerror(pa->pa_context_errno(context_)));
        pa->pa_stream_unref(input->stream);
        return;
    }
    std::lock_guard lock(bufferMutex_);
    inputs_[sinkInput] = std::move(input);
}

void Soundshare::AddDevice()
{
    std::string device;
    {
        std::lock_guard lock(mutex_);
        device = device_;
    }
    if (device.empty() || inputs_.count(kDeviceKey)) return;
    // The ids Discord passes are our input device names: the pulse description ("default: <description>" for
    // the default entry), not the source name pa_stream_connect_record wants. Look the source up
    if (device == "default" || device.rfind("default:", 0) == 0) {
        ConnectDevice(device, nullptr);
        return;
    }
    if (auto* op = Pulse()->pa_context_get_source_info_list(context_, &Soundshare::OnSource, this)) Pulse()->pa_operation_unref(op);
}

void Soundshare::OnSource(pa_context*, const pa_source_info* info, int eol, void* p)
{
    auto* self = static_cast<Soundshare*>(p);
    std::string device;
    {
        std::lock_guard lock(self->mutex_);
        device = self->device_;
    }
    if (self->inputs_.count(kDeviceKey) || device.empty()) return;
    if (eol) {
        VV_LOGW("soundshare", "no source \"%s\", recording the default one", device.c_str());
        self->ConnectDevice(device, nullptr);
        return;
    }
    if (info && (device == info->name || (info->description && device == info->description)))
        self->ConnectDevice(device, info->name);
}

void Soundshare::ConnectDevice(const std::string& label, const char* source)
{
    auto* pa = Pulse();
    if (inputs_.count(kDeviceKey)) return;
    auto input = std::make_unique<Input>();
    input->self = this;
    input->key = kDeviceKey;
    // Mono: many USB microphones put their capsule on one channel of their "stereo" profile, which would sound off
    // to one side. Pulse downmixes the source, OnStreamRead puts it in both channels
    input->channels = 1;
    input->stream = NewRecordStream(context_, "Go Live microphone", this, input.get(), &Soundshare::OnStreamState,
                                    &Soundshare::OnStreamRead, 1);
    if (!input->stream) return;
    pa_buffer_attr attr{static_cast<uint32_t>(-1), static_cast<uint32_t>(-1), static_cast<uint32_t>(-1), static_cast<uint32_t>(-1),
                        static_cast<uint32_t>(kFrame * kChannels * sizeof(float))};
    if (pa->pa_stream_connect_record(input->stream, source, &attr, PA_STREAM_ADJUST_LATENCY) < 0) {
        VV_LOGE("soundshare", "cannot record %s: %s", label.c_str(), pa->pa_strerror(pa->pa_context_errno(context_)));
        pa->pa_stream_unref(input->stream);
        return;
    }
    VV_LOGI("soundshare", "recording %s (source %s)", label.c_str(), source ? source : "default");
    std::lock_guard lock(bufferMutex_);
    inputs_[kDeviceKey] = std::move(input);
}

void Soundshare::RemoveInput(uint32_t key)
{
    std::unique_ptr<Input> input;
    {
        std::lock_guard lock(bufferMutex_);
        auto it = inputs_.find(key);
        if (it == inputs_.end()) return;
        input = std::move(it->second);
        inputs_.erase(it);
    }
    auto* pa = Pulse();
    pa->pa_stream_set_state_callback(input->stream, nullptr, nullptr);
    pa->pa_stream_set_read_callback(input->stream, nullptr, nullptr);
    pa->pa_stream_disconnect(input->stream);
    pa->pa_stream_unref(input->stream);
    if (key != kDeviceKey) VV_LOGI("soundshare", "sink input %u gone", key);
}

void Soundshare::RemoveMonitors()
{
    std::vector<uint32_t> keys;
    {
        std::lock_guard lock(bufferMutex_);
        for (auto& [k, i] : inputs_)
            if (k != kDeviceKey) keys.push_back(k);
    }
    for (auto k : keys) RemoveInput(k);
    if (attached_ && pid_ == 0) {
        attached_ = false;
        if (events_.ended) events_.ended();
    }
}

void Soundshare::OnStreamState(pa_stream* s, void* p)
{
    auto* input = static_cast<Input*>(p);
    auto* self = input->self;
    switch (Pulse()->pa_stream_get_state(s)) {
    case PA_STREAM_READY:
        input->recording = true;
        if (!self->attached_) {
            self->attached_ = true;
            if (self->events_.attached) self->events_.attached();
        }
        break;
    case PA_STREAM_FAILED:
        VV_LOGW("soundshare", "stream %u failed", input->key);
        break;
    default:
        break;
    }
}

void Soundshare::OnStreamRead(pa_stream* s, size_t, void* p)
{
    auto* input = static_cast<Input*>(p);
    auto* self = input->self;
    auto* pa = Pulse();
    const void* data = nullptr;
    size_t bytes = 0;
    while (pa->pa_stream_peek(s, &data, &bytes) == 0 && bytes > 0) {
        if (data) {
            auto* f = static_cast<const float*>(data);
            size_t n = bytes / sizeof(float);
            std::lock_guard lock(self->bufferMutex_);
            if (input->channels == 1) {
                for (size_t i = 0; i < n; i++) {
                    input->samples.push_back(f[i]);
                    input->samples.push_back(f[i]);
                }
            } else {
                input->samples.insert(input->samples.end(), f, f + n);
            }
            if (input->samples.size() > kMaxBuffered) input->samples.erase(input->samples.begin(), input->samples.end() - kMaxBuffered);
        }
        pa->pa_stream_drop(s); // data == nullptr: a hole, skipped
    }
}

// ------------------------------------------------------------------------------------- mixing thread

void Soundshare::MixLoop()
{
    std::vector<float> mix(kFrame * kChannels);
    std::vector<int16_t> out(kFrame * kChannels);
    auto next = std::chrono::steady_clock::now();
    while (mixing_) {
        next += std::chrono::milliseconds(10);
        auto now = std::chrono::steady_clock::now();
        if (now - next > std::chrono::milliseconds(100)) next = now; // fell far behind (suspend…): start over
        std::this_thread::sleep_until(next);

        bool any = false;
        std::fill(mix.begin(), mix.end(), 0.f);
        {
            std::lock_guard lock(bufferMutex_);
            for (auto& [key, input] : inputs_) {
                if (!input->recording) continue;
                any = true;
                auto& q = input->samples;
                if (!input->primed && q.size() >= kPrime) input->primed = true;
                if (!input->primed) continue;
                size_t n = std::min(q.size(), mix.size());
                for (size_t i = 0; i < n; i++) mix[i] += q[i];
                q.erase(q.begin(), q.begin() + n);
                if (q.empty()) input->primed = false; // underrun: buffer again
            }
        }
        if (!any) continue;
        float peak = 0;
        for (size_t i = 0; i < mix.size(); i++) {
            float v = std::clamp(mix[i], -1.f, 1.f);
            peak = std::max(peak, std::fabs(v));
            out[i] = static_cast<int16_t>(std::lrint(v * 32767.f));
        }
        frames_(out.data(), peak);
    }
}

} // namespace vv
