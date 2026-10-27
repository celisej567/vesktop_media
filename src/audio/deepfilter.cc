#include "audio/deepfilter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#include "common/log.h"
#include "common/platform.h"

namespace vv {

namespace {

struct Api {
    void* (*create)(const char*, float) = nullptr;
    size_t (*frameLength)(void*) = nullptr;
    float (*process)(void*, float*, float*) = nullptr;
    void (*free)(void*) = nullptr;
    void (*setThresholds)(void*, float, float, float) = nullptr; // our addition to the C API
};

const Api* GetApi()
{
    static Api api = [] {
        Api a;
        std::string path;
        void* h = OpenLocalLibrary(VV_LOCAL_LIBRARY("deepfilter"), &path);
        if (!h) {
            VV_LOGW("deepfilter", VV_LOCAL_LIBRARY("deepfilter") " not found next to the module");
            return a;
        }
        a.create = reinterpret_cast<void* (*)(const char*, float)>(LibrarySymbol(h, "df_create"));
        a.frameLength = reinterpret_cast<size_t (*)(void*)>(LibrarySymbol(h, "df_get_frame_length"));
        a.process = reinterpret_cast<float (*)(void*, float*, float*)>(LibrarySymbol(h, "df_process_frame"));
        a.free = reinterpret_cast<void (*)(void*)>(LibrarySymbol(h, "df_free"));
        a.setThresholds = reinterpret_cast<void (*)(void*, float, float, float)>(LibrarySymbol(h, "df_set_thresholds"));
        if (a.create && a.frameLength && a.process && a.free) VV_LOGI("deepfilter", "loaded %s", path.c_str());
        return a;
    }();
    return api.create && api.frameLength && api.process && api.free ? &api : nullptr;
}

std::atomic<bool> gFailed{false};

// No attenuation limit: as much suppression as the model finds
constexpr float kAttenuationLimitDb = 100;
// The local SNR thresholds of the processing stages. DeepFilterNet's defaults (-10 / 30 / 20 dB) skip its stages for
// frames that look clean enough; with a good microphone the SNR hovers around those values, the stages switch on
// and off from frame to frame and the voice pulses (measured on clean speech with noise 30 dB below: 6 % of the
// speech frames jumped by more than 3 dB, none with both stages always on). The lower threshold (frames treated as
// noise only) stays DeepFilterNet's: it is what silences the pauses
constexpr float kMinDb = -10, kMaxErbDb = 100, kMaxDfDb = 100;

} // namespace

bool DeepFilter::Available()
{
    return GetApi() != nullptr;
}

bool DeepFilter::Failed()
{
    return gFailed;
}

DeepFilter::DeepFilter(void (*onFailed)())
{
    const Api* api = GetApi();
    if (!api) return;
    creator_ = std::thread([this, api, onFailed] {
        auto t0 = std::chrono::steady_clock::now();
        // NULL: the model built into the library (our patch of its C API)
        void* state = api->create(nullptr, kAttenuationLimitDb);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (state && api->frameLength(state) != 480) {
            VV_LOGE("deepfilter", "unexpected frame length %zu", api->frameLength(state));
            api->free(state);
            state = nullptr;
        }
        if (!state) {
            VV_LOGE("deepfilter", "cannot create the DeepFilterNet state");
            gFailed = true;
            if (onFailed) onFailed();
            return;
        }
        if (api->setThresholds) api->setThresholds(state, kMinDb, kMaxErbDb, kMaxDfDb);
        else VV_LOGW("deepfilter", "the library has no df_set_thresholds: its stages may pulse");
        VV_LOGI("deepfilter", "ready in %lld ms", static_cast<long long>(ms));
        state_ = state;
    });
}

DeepFilter::~DeepFilter()
{
    if (creator_.joinable()) creator_.join();
    if (void* state = state_.exchange(nullptr)) GetApi()->free(state);
}

bool DeepFilter::Process(float* samples)
{
    void* state = state_;
    if (!state) return false;
    float in[480], out[480];
    for (int i = 0; i < 480; i++) in[i] = samples[i] / 32768.0f;
    float lsnr = GetApi()->process(state, in, out);
    if (std::isnan(lsnr)) return false; // failed inside (caught there): the frame stays as it was
    for (int i = 0; i < 480; i++) samples[i] = std::clamp(out[i], -1.0f, 1.0f) * 32767.0f;
    return true;
}

} // namespace vv
