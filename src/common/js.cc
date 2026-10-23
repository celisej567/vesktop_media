#include "common/js.h"

#include "common/log.h"

namespace vv {

namespace {

void CallJs(Napi::Env env, Napi::Function fn, JsCallback::ArgsBuilder* data)
{
    std::unique_ptr<JsCallback::ArgsBuilder> build(data);
    if (env == nullptr || fn.IsEmpty()) return;
    try {
        auto args = (*build)(env);
        fn.Call(env.Undefined(), args);
    } catch (const Napi::Error& e) {
        VV_LOGE("js", "callback threw: %s", e.Message().c_str());
    } catch (const std::exception& e) {
        VV_LOGE("js", "callback failed: %s", e.what());
    }
}

} // namespace

std::shared_ptr<JsCallback> JsCallback::Create(const Napi::Value& fn, const char* name)
{
    if (fn.IsEmpty() || !fn.IsFunction()) return nullptr;
    auto env = fn.Env();
    auto tsfn = Napi::ThreadSafeFunction::New(env, fn.As<Napi::Function>(), name, 0, 1);
    // Callbacks must not keep the process alive (fake-sfu exits on its own, Electron on window close)
    tsfn.Unref(env);
    return std::shared_ptr<JsCallback>(new JsCallback(std::move(tsfn), name));
}

JsCallback::~JsCallback()
{
    tsfn_.Release();
}

void JsCallback::Call(ArgsBuilder build)
{
    auto* data = new ArgsBuilder(std::move(build));
    auto status = tsfn_.NonBlockingCall(data, CallJs);
    if (status != napi_ok) delete data;
}

json ToJson(const Napi::Value& value)
{
    if (value.IsEmpty() || value.IsUndefined() || value.IsNull()) return nullptr;
    if (value.IsBoolean()) return value.As<Napi::Boolean>().Value();
    if (value.IsNumber()) {
        double d = value.As<Napi::Number>().DoubleValue();
        if (d == static_cast<double>(static_cast<int64_t>(d)) && std::abs(d) < 9.0e15) return static_cast<int64_t>(d);
        return d;
    }
    if (value.IsBigInt()) {
        bool lossless;
        return value.As<Napi::BigInt>().Int64Value(&lossless);
    }
    if (value.IsString()) return value.As<Napi::String>().Utf8Value();
    if (value.IsTypedArray() || value.IsArrayBuffer() || value.IsDataView() || value.IsBuffer()) {
        json arr = json::array();
        if (value.IsTypedArray() && value.As<Napi::TypedArray>().TypedArrayType() == napi_float32_array) {
            auto f = value.As<Napi::Float32Array>();
            for (size_t i = 0; i < f.ElementLength(); i++) arr.push_back(f[i]);
            return arr;
        }
        for (auto b : ToBytes(value)) arr.push_back(b);
        return arr;
    }
    if (value.IsArray()) {
        auto a = value.As<Napi::Array>();
        json arr = json::array();
        for (uint32_t i = 0; i < a.Length(); i++) arr.push_back(ToJson(a.Get(i)));
        return arr;
    }
    if (value.IsObject() && !value.IsFunction()) {
        auto o = value.As<Napi::Object>();
        json obj = json::object();
        auto names = o.GetPropertyNames();
        for (uint32_t i = 0; i < names.Length(); i++) {
            auto key = names.Get(i).ToString().Utf8Value();
            obj[key] = ToJson(o.Get(key));
        }
        return obj;
    }
    return nullptr;
}

Napi::Value FromJson(Napi::Env env, const json& value)
{
    switch (value.type()) {
    case json::value_t::null: return env.Null();
    case json::value_t::boolean: return Napi::Boolean::New(env, value.get<bool>());
    case json::value_t::number_integer: return Napi::Number::New(env, static_cast<double>(value.get<int64_t>()));
    case json::value_t::number_unsigned: return Napi::Number::New(env, static_cast<double>(value.get<uint64_t>()));
    case json::value_t::number_float: return Napi::Number::New(env, value.get<double>());
    case json::value_t::string: return Napi::String::New(env, value.get<std::string>());
    case json::value_t::array: {
        auto arr = Napi::Array::New(env, value.size());
        for (size_t i = 0; i < value.size(); i++) arr.Set(static_cast<uint32_t>(i), FromJson(env, value[i]));
        return arr;
    }
    case json::value_t::object: {
        auto obj = Napi::Object::New(env);
        for (auto& [k, v] : value.items()) obj.Set(k, FromJson(env, v));
        return obj;
    }
    default: return env.Undefined();
    }
}

Bytes ToBytes(const Napi::Value& value)
{
    if (value.IsEmpty()) return {};
    if (value.IsArrayBuffer()) {
        auto ab = value.As<Napi::ArrayBuffer>();
        auto* p = static_cast<uint8_t*>(ab.Data());
        return Bytes(p, p + ab.ByteLength());
    }
    if (value.IsTypedArray()) {
        auto ta = value.As<Napi::TypedArray>();
        auto* p = static_cast<uint8_t*>(ta.ArrayBuffer().Data()) + ta.ByteOffset();
        return Bytes(p, p + ta.ByteLength());
    }
    if (value.IsDataView()) {
        auto dv = value.As<Napi::DataView>();
        auto* p = static_cast<uint8_t*>(dv.ArrayBuffer().Data()) + dv.ByteOffset();
        return Bytes(p, p + dv.ByteLength());
    }
    if (value.IsArray()) {
        auto a = value.As<Napi::Array>();
        Bytes out;
        out.reserve(a.Length());
        for (uint32_t i = 0; i < a.Length(); i++) out.push_back(static_cast<uint8_t>(ToNum(a.Get(i))));
        return out;
    }
    return {};
}

Napi::ArrayBuffer ToArrayBuffer(Napi::Env env, const Bytes& bytes)
{
    auto ab = Napi::ArrayBuffer::New(env, bytes.size());
    if (!bytes.empty()) memcpy(ab.Data(), bytes.data(), bytes.size());
    return ab;
}

std::string ToStr(const Napi::Value& value, const std::string& fallback)
{
    if (value.IsEmpty() || value.IsUndefined() || value.IsNull()) return fallback;
    if (value.IsString()) return value.As<Napi::String>().Utf8Value();
    return value.ToString().Utf8Value();
}

double ToNum(const Napi::Value& value, double fallback)
{
    if (value.IsEmpty()) return fallback;
    if (value.IsNumber()) return value.As<Napi::Number>().DoubleValue();
    if (value.IsBoolean()) return value.As<Napi::Boolean>().Value() ? 1 : 0;
    if (value.IsString()) {
        try {
            return std::stod(value.As<Napi::String>().Utf8Value());
        } catch (...) {
        }
    }
    return fallback;
}

bool ToBool(const Napi::Value& value, bool fallback)
{
    if (value.IsEmpty() || value.IsUndefined() || value.IsNull()) return fallback;
    return value.ToBoolean().Value();
}

} // namespace vv
