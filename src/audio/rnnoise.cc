#include "audio/rnnoise.h"

#include "common/log.h"
#include "common/platform.h"

namespace vv {

namespace {

struct Api {
    void* (*create)(void*) = nullptr;
    void (*destroy)(void*) = nullptr;
    float (*process)(void*, float*, const float*) = nullptr;
};

const Api* GetApi()
{
    static Api api = [] {
        Api a;
        std::string path;
        void* h = OpenLocalLibrary("libvesktop_rnnoise.so", &path);
        if (!h) {
            VV_LOGW("rnnoise", "libvesktop_rnnoise.so not found next to the module, noise cancellation unavailable");
            return a;
        }
        VV_LOGI("rnnoise", "loaded %s", path.c_str());
        a.create = reinterpret_cast<void* (*)(void*)>(LibrarySymbol(h, "rnnoise_create"));
        a.destroy = reinterpret_cast<void (*)(void*)>(LibrarySymbol(h, "rnnoise_destroy"));
        a.process = reinterpret_cast<float (*)(void*, float*, const float*)>(LibrarySymbol(h, "rnnoise_process_frame"));
        return a;
    }();
    return api.create && api.destroy && api.process ? &api : nullptr;
}

} // namespace

bool RnNoise::Available()
{
    return GetApi() != nullptr;
}

RnNoise::RnNoise()
{
    if (auto* api = GetApi()) state_ = api->create(nullptr);
}

RnNoise::~RnNoise()
{
    if (state_) GetApi()->destroy(state_);
}

float RnNoise::Process(float* samples)
{
    if (!state_) return -1;
    return GetApi()->process(state_, samples, samples);
}

} // namespace vv
