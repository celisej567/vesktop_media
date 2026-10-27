// Soundshare on Windows: WASAPI process loopback (Windows 10 2004 or newer). Target pid N records that process and
// its child processes (a browser's or a game launcher's audio comes from children); pid 1 records everything except
// Vesktop's own processes, so the call itself is never shared. Go Live with a device records that microphone (a
// Core Audio capture endpoint). Everything is mixed into 10 ms, 48 kHz stereo frames, as on Linux.
#include "audio/soundshare.h"

#include <windows.h>

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>

#include "common/log.h"
#include "common/platform.h"

namespace vv {

namespace {

constexpr int kRate = 48000, kChannels = 2, kFrame = kRate / 100; // 10 ms
constexpr size_t kPrime = kFrame * kChannels * 2;                   // start mixing an input once 20 ms are buffered
constexpr size_t kMaxBuffered = kRate * kChannels / 5;              // keep at most 200 ms
constexpr uint32_t kLoopbackKey = 1, kDeviceKey = 2;

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr()
    {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// ActivateAudioInterfaceAsync's completion (called on a thread of the audio service's choosing): free-threaded
class ActivationHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    ActivationHandler() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ActivationHandler()
    {
        if (client_) client_->Release();
        CloseHandle(done_);
    }
    // the activated client (the caller owns the reference), null on failure with `hr` saying why
    IAudioClient* Wait(HRESULT& hr)
    {
        if (WaitForSingleObject(done_, 5000) != WAIT_OBJECT_0) {
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            return nullptr;
        }
        hr = hr_;
        IAudioClient* client = client_;
        client_ = nullptr;
        return client;
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override
    {
        IUnknown* unknown = nullptr;
        HRESULT activated = E_FAIL;
        hr_ = operation->GetActivateResult(&activated, &unknown);
        if (SUCCEEDED(hr_)) hr_ = activated;
        if (SUCCEEDED(hr_) && unknown) hr_ = unknown->QueryInterface(IID_PPV_ARGS(&client_));
        if (unknown) unknown->Release();
        SetEvent(done_);
        return S_OK;
    }
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (riid == __uuidof(IAgileObject))
            *out = static_cast<IAgileObject*>(this);
        else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG left = InterlockedDecrement(&refs_);
        if (!left) delete this;
        return left;
    }

private:
    LONG refs_ = 1;
    HANDLE done_;
    HRESULT hr_ = E_FAIL;
    IAudioClient* client_ = nullptr;
};

std::string ExeName(DWORD pid)
{
    std::string name;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return name;
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) name = Narrow(std::wstring(path, size));
    CloseHandle(process);
    return name;
}

// Vesktop's main (browser) process: the module runs in a renderer, whose parent is the main process; Chromium's
// audio service, which plays everything else of Vesktop, is another child of it
DWORD OwnRootProcess()
{
    DWORD self = GetCurrentProcessId(), parent = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{sizeof(entry)};
        for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
            if (entry.th32ProcessID == self) parent = entry.th32ParentProcessID;
        CloseHandle(snapshot);
    }
    return parent && ExeName(parent) == ExeName(self) ? parent : self;
}

WAVEFORMATEX CaptureFormat()
{
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kChannels;
    format.nSamplesPerSec = kRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    return format;
}

// A client that records `pid`'s process tree (include) or everything but it (exclude); null with the reason
IAudioClient* ActivateLoopback(DWORD pid, bool include, std::string& error)
{
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        include ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT activation{};
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(params);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* handler = new ActivationHandler();
    IActivateAudioInterfaceAsyncOperation* operation = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activation, handler, &operation);
    IAudioClient* client = nullptr;
    if (SUCCEEDED(hr)) client = handler->Wait(hr);
    if (operation) operation->Release();
    handler->Release();
    if (!client) {
        char buf[96];
        snprintf(buf, sizeof(buf), "process loopback not available (0x%08lx; needs Windows 10 2004 or newer)", static_cast<unsigned long>(hr));
        error = buf;
        return nullptr;
    }
    auto format = CaptureFormat();
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                            200000 /* 20 ms */, 0, &format, nullptr);
    if (FAILED(hr)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "process loopback: Initialize failed (0x%08lx)", static_cast<unsigned long>(hr));
        error = buf;
        client->Release();
        return nullptr;
    }
    return client;
}

// A client that records a capture endpoint ("default" = the default one) as 48 kHz stereo
IAudioClient* ActivateDevice(const std::string& id, std::string& error)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr))
        hr = id.empty() || id == "default" ? enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device)
                                           : enumerator->GetDevice(Widen(id).c_str(), &device);
    IAudioClient* client = nullptr;
    if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    if (SUCCEEDED(hr)) {
        auto format = CaptureFormat();
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                200000, 0, &format, nullptr);
    }
    if (FAILED(hr)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "cannot record the device (0x%08lx)", static_cast<unsigned long>(hr));
        error = buf;
        if (client) client->Release();
        return nullptr;
    }
    return client;
}

} // namespace

struct Soundshare::Input {
    std::deque<float> samples; // interleaved stereo, under bufferMutex_
    bool primed = false;
    std::atomic<bool> recording{false};
    std::atomic<bool> stop{false};
    std::thread thread;
};

bool Soundshare::Available()
{
    return true;
}

Soundshare::Soundshare(Frames frames, Events events) : frames_(std::move(frames)), events_(std::move(events)) {}

Soundshare::~Soundshare()
{
    StopLoop();
}

bool Soundshare::EnsureStartedLocked()
{
    if (mixing_) return true;
    mixing_ = true;
    mixer_ = std::thread([this] { MixLoop(); });
    return true;
}

void Soundshare::StopLoop()
{
    std::map<uint32_t, std::unique_ptr<Input>> inputs;
    {
        std::lock_guard lock(bufferMutex_);
        inputs.swap(inputs_);
    }
    for (auto& [key, input] : inputs) {
        input->stop = true;
        if (input->thread.joinable()) input->thread.join();
    }
    if (mixing_.exchange(false) && mixer_.joinable()) mixer_.join();
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
    input->stop = true;
    if (input->thread.joinable()) input->thread.join();
}

void Soundshare::SetPid(uint32_t pid)
{
    std::lock_guard lock(mutex_);
    if (pid == pid_) return;
    VV_LOGI("soundshare", "target pid %u", pid);
    pid_ = pid;
    RemoveInput(kLoopbackKey);
    if (!pid) return;
    EnsureStartedLocked();
    attached_ = false;
    auto input = std::make_unique<Input>();
    Input* in = input.get();
    {
        std::lock_guard bufferLock(bufferMutex_);
        inputs_[kLoopbackKey] = std::move(input);
    }
    in->thread = std::thread([this, in, pid] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SetThreadDescription(GetCurrentThread(), L"vv soundshare");
        DWORD target = pid == 1 ? OwnRootProcess() : pid;
        std::string error;
        ComPtr<IAudioClient> client;
        client.p = ActivateLoopback(target, pid != 1, error);
        // the target process, to notice when it exits (pid 1 records the system: nothing to end)
        HANDLE process = pid == 1 ? nullptr : OpenProcess(SYNCHRONIZE, FALSE, target);
        if (!client) {
            VV_LOGE("soundshare", "%s", error.c_str());
            if (!failed_) {
                failed_ = true;
                if (events_.failed) events_.failed(1, error);
            }
        } else {
            VV_LOGI("soundshare", "recording %s %lu", pid == 1 ? "everything except" : "process tree of",
                    static_cast<unsigned long>(target));
            ComPtr<IAudioCaptureClient> capture;
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (SUCCEEDED(client->GetService(IID_PPV_ARGS(&capture))) && SUCCEEDED(client->SetEventHandle(event)) &&
                SUCCEEDED(client->Start())) {
                in->recording = true;
                if (!attached_.exchange(true) && events_.attached) events_.attached();
                while (!in->stop) {
                    WaitForSingleObject(event, 50);
                    if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
                        VV_LOGI("soundshare", "process %lu exited", static_cast<unsigned long>(target));
                        if (events_.ended) events_.ended();
                        break;
                    }
                    UINT32 packet = 0;
                    while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
                        BYTE* data = nullptr;
                        UINT32 frames = 0;
                        DWORD flags = 0;
                        if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                        const auto* pcm = reinterpret_cast<const int16_t*>(data);
                        bool silent = flags & AUDCLNT_BUFFERFLAGS_SILENT;
                        {
                            std::lock_guard bufferLock(bufferMutex_);
                            for (UINT32 i = 0; i < frames * kChannels; i++) in->samples.push_back(silent ? 0.f : pcm[i] / 32768.f);
                            while (in->samples.size() > kMaxBuffered) in->samples.pop_front();
                        }
                        capture->ReleaseBuffer(frames);
                    }
                }
                client->Stop();
            }
            CloseHandle(event);
        }
        if (process) CloseHandle(process);
        CoUninitialize();
    });
}

void Soundshare::SetDevice(const std::string& source)
{
    std::lock_guard lock(mutex_);
    if (source == device_) return;
    VV_LOGI("soundshare", "device %s", source.empty() ? "(none)" : source.c_str());
    device_ = source;
    RemoveInput(kDeviceKey);
    if (source.empty()) return;
    EnsureStartedLocked();
    auto input = std::make_unique<Input>();
    Input* in = input.get();
    {
        std::lock_guard bufferLock(bufferMutex_);
        inputs_[kDeviceKey] = std::move(input);
    }
    in->thread = std::thread([this, in, source] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SetThreadDescription(GetCurrentThread(), L"vv soundshare device");
        std::string error;
        ComPtr<IAudioClient> client;
        client.p = ActivateDevice(source, error);
        ComPtr<IAudioCaptureClient> capture;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!client) {
            VV_LOGE("soundshare", "device %s: %s", source.c_str(), error.c_str());
        } else if (SUCCEEDED(client->GetService(IID_PPV_ARGS(&capture))) && SUCCEEDED(client->SetEventHandle(event)) &&
                   SUCCEEDED(client->Start())) {
            VV_LOGI("soundshare", "recording device %s", source.c_str());
            in->recording = true;
            while (!in->stop) {
                WaitForSingleObject(event, 50);
                UINT32 packet = 0;
                while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                    const auto* pcm = reinterpret_cast<const int16_t*>(data);
                    bool silent = flags & AUDCLNT_BUFFERFLAGS_SILENT;
                    {
                        std::lock_guard bufferLock(bufferMutex_);
                        for (UINT32 i = 0; i < frames * kChannels; i++) in->samples.push_back(silent ? 0.f : pcm[i] / 32768.f);
                        while (in->samples.size() > kMaxBuffered) in->samples.pop_front();
                    }
                    capture->ReleaseBuffer(frames);
                }
            }
            client->Stop();
        }
        CloseHandle(event);
        CoUninitialize();
    });
}

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
