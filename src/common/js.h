// JS interop: every callback into JS goes through a thread-safe function (resolving or
// calling into V8 from another thread crashes), and option objects are converted to JSON on the JS thread
// so the rest of the engine never touches napi values.
#pragma once

#include <napi.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vv {

using json = nlohmann::json;
using Bytes = std::vector<uint8_t>;

// What the module knows of one Node environment. A renderer process runs the module in a new environment each time
// the page is loaded again (the process, and with it the module's statics, stay), and an old environment is torn down
// while a new one already works: "closing" is therefore a state of one environment, not of the process
struct EnvState {
    std::atomic<bool> closing{false};
};
// Once per environment, from the module's Init: notices the end of that environment (see js.cc)
void JsEnvInit(Napi::Env env);
// The state of the environment `env`
std::shared_ptr<EnvState> CurrentEnvState(Napi::Env env);

class JsCallback {
public:
    using Args = std::vector<napi_value>;
    using ArgsBuilder = std::function<Args(Napi::Env)>;

    // Returns null when `fn` is not a function (setters accept null/undefined to clear)
    static std::shared_ptr<JsCallback> Create(const Napi::Value& fn, const char* name);
    ~JsCallback();

    // Thread-safe and non-blocking; `build` runs on the JS thread
    void Call(ArgsBuilder build);
    void Call() { Call([](Napi::Env) { return Args{}; }); }

    JsCallback(const JsCallback&) = delete;
    JsCallback& operator=(const JsCallback&) = delete;

private:
    explicit JsCallback(Napi::ThreadSafeFunction tsfn, std::string name, std::shared_ptr<EnvState> env)
        : tsfn_(std::move(tsfn)), name_(std::move(name)), env_(std::move(env)) {}

    Napi::ThreadSafeFunction tsfn_;
    std::shared_ptr<EnvState> env_; // the environment of tsfn_: once it closes, Node has closed tsfn_ too
    std::string name_;
};

using JsCallbackPtr = std::shared_ptr<JsCallback>;

// A settable callback (setXxxCallback). Calls are dropped while unset
class CallbackSlot {
public:
    void Set(JsCallbackPtr cb)
    {
        std::lock_guard lock(mutex_);
        cb_ = std::move(cb);
    }
    JsCallbackPtr Get() const
    {
        std::lock_guard lock(mutex_);
        return cb_;
    }
    explicit operator bool() const { return Get() != nullptr; }
    void Call(JsCallback::ArgsBuilder build) const
    {
        if (auto cb = Get()) cb->Call(std::move(build));
    }
    void Call() const
    {
        if (auto cb = Get()) cb->Call();
    }

private:
    mutable std::mutex mutex_;
    JsCallbackPtr cb_;
};

// napi <-> json. Typed arrays / ArrayBuffers become arrays of numbers
json ToJson(const Napi::Value& value);
Napi::Value FromJson(Napi::Env env, const json& value);

// Bytes from ArrayBuffer, any typed array, Buffer, DataView or number[]
Bytes ToBytes(const Napi::Value& value);
Napi::ArrayBuffer ToArrayBuffer(Napi::Env env, const Bytes& bytes);

std::string ToStr(const Napi::Value& value, const std::string& fallback = "");
double ToNum(const Napi::Value& value, double fallback = 0);
bool ToBool(const Napi::Value& value, bool fallback = false);

// json helpers that tolerate missing / mistyped keys
template <typename T>
T Get(const json& j, const char* key, T fallback)
{
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    try {
        if constexpr (std::is_same_v<T, bool>) {
            if (it->is_number()) return it->template get<double>() != 0;
        }
        if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
            if (it->is_boolean()) return static_cast<T>(it->template get<bool>());
            if (it->is_string()) return static_cast<T>(std::stod(it->template get<std::string>()));
        }
        return it->template get<T>();
    } catch (...) {
        return fallback;
    }
}

} // namespace vv
