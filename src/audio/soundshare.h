// Soundshare: the audio of a Go Live stream, through PulseAudio (pipewire-pulse on current systems): every playback stream
// (sink input) of the target process gets a monitor recording stream of its own; target pid 1 = every sink
// input except our own process, so the call itself is never shared. Go Live with a device records that
// microphone (a pulse source) instead. Everything is mixed into 10 ms, 48 kHz stereo frames.
//
// libpulse is loaded at runtime (the module links nothing optional).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct pa_threaded_mainloop;
struct pa_context;
struct pa_stream;
struct pa_sink_input_info;
struct pa_sink_info;
struct pa_source_info;

namespace vv {

class Soundshare {
public:
    struct Events {
        std::function<void()> attached;                                     // the first stream records
        std::function<void()> ended;                                        // a target that was attached is gone
        std::function<void(int code, const std::string& reason)> failed;    // no PulseAudio
    };
    // 480 stereo int16 frames (10 ms at 48 kHz), and the peak of the frame (0..1); on the mixing thread
    using Frames = std::function<void(const int16_t* samples, float peak)>;

    Soundshare(Frames frames, Events events);
    ~Soundshare();

    static bool Available();

    // 0 = none, 1 = all system audio except this process, otherwise that process
    void SetPid(uint32_t pid);
    // Go Live with a device: a pulse source name ("default" = the default source), "" = none
    void SetDevice(const std::string& source);

private:
    struct Input;
    struct Pending;

    bool EnsureStartedLocked(); // mutex_ held
    void StopLoop();
    void MixLoop();

    // PulseAudio thread (loop lock held)
    static void OnContextState(pa_context* c, void* self);
    static void OnSubscribe(pa_context* c, int type, uint32_t index, void* self);
    static void OnSinkInput(pa_context* c, const pa_sink_input_info* info, int eol, void* self);
    static void OnSink(pa_context* c, const pa_sink_info* info, int eol, void* pending);
    static void OnSource(pa_context* c, const pa_source_info* info, int eol, void* self);
    static void OnStreamState(pa_stream* s, void* input);
    static void OnStreamRead(pa_stream* s, size_t bytes, void* input);
    void Scan();
    bool Wanted(uint32_t pid) const;
    void AddMonitor(uint32_t sinkInput, const char* monitorSource);
    void AddDevice();
    void ConnectDevice(const std::string& label, const char* source); // source null = the default one
    void RemoveInput(uint32_t key);
    void RemoveMonitors();

    Frames frames_;
    Events events_;

    std::mutex mutex_; // pid_ / device_ / start-up
    std::atomic<uint32_t> pid_{0};
    std::string device_;
    std::atomic<bool> failed_{false};
    std::atomic<bool> attached_{false}; // loop thread (capture threads on Windows)

    pa_threaded_mainloop* loop_ = nullptr;
    pa_context* context_ = nullptr;
    bool contextReady_ = false;

    std::mutex bufferMutex_; // inputs_' sample buffers
    std::map<uint32_t, std::unique_ptr<Input>> inputs_; // by sink input index; kDeviceKey = the microphone

    std::thread mixer_;
    std::atomic<bool> mixing_{false};
};

} // namespace vv
