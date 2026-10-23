// N-API entry point: the complete export table Discord's JS uses (99 engine exports, VoiceConnection with its
// 66 methods, VoiceReplayConnection) and our own 11 (the Vesktop extensions, NoiseModel, vesktopVoiceInfo). What is not implemented yet is a stub that
// accepts the arguments and answers callbacks with neutral values.
#include <napi.h>

#include <dave/version.h>
#include <rtc_base/thread.h>

#include <thread>

#include "audio/deepfilter.h"
#include "audio/rnnoise.h"
#include "common/js.h"
#include "common/log.h"
#include "common/util.h"
#include "connection/connection.h"
#include "engine/engine.h"
#include "transport/crypto.h"
#include "transport/region_tester.h"
#include "capture/camera.h"
#include "capture/portal.h"
#include "video/ffmpeg_encoder.h"
#include "video/decoder_factory.h"
#include "video/video_sinks.h"

#ifndef VESKTOP_MEDIA_VERSION
#define VESKTOP_MEDIA_VERSION "0.0.0"
#endif

namespace vv {

namespace {

using Info = const Napi::CallbackInfo&;

Engine& E()
{
    return Engine::Get();
}

// Calls `cb` asynchronously (callbacks are never invoked synchronously)
void Later(const Napi::Value& fn, JsCallback::ArgsBuilder build = [](Napi::Env) { return JsCallback::Args{}; })
{
    if (auto cb = JsCallback::Create(fn, "callback")) cb->Call(std::move(build));
}

Napi::Value Undefined(Info info)
{
    return info.Env().Undefined();
}

Napi::Value Arg(Info info, size_t i)
{
    return i < info.Length() ? info[i] : info.Env().Undefined();
}

Napi::Value Stub(Info info)
{
    return info.Env().Undefined();
}

// a stub whose last function argument is called with no arguments
Napi::Value StubCallLast(Info info)
{
    for (size_t i = info.Length(); i-- > 0;)
        if (info[i].IsFunction()) {
            Later(info[i]);
            break;
        }
    return info.Env().Undefined();
}

// a stub that calls the given error callback with a JSON error
Napi::Value StubError(Info info, size_t errIndex)
{
    if (info.Length() > errIndex && info[errIndex].IsFunction())
        Later(info[errIndex], [](Napi::Env env) { return JsCallback::Args{Napi::String::New(env, "{\"error\":\"not supported\"}")}; });
    return info.Env().Undefined();
}

#define SLOT_SETTER(slot)                                                                                              \
    [](Info info) -> Napi::Value {                                                                                     \
        E().callbacks.slot.Set(JsCallback::Create(Arg(info, 0), #slot));                                               \
        return info.Env().Undefined();                                                                                 \
    }

std::vector<std::string> SupportedFeatures()
{
    return {"voice_panning", "voice_multiple_connections", "media_devices", "debug_logging", "set_audio_device_by_id",
            "loopback", "experiment_config", "mls_pairwise_fingerprints", "offload_adm_controls",
            "port_aware_latency_testing", "krisp_native_error", "udp_endpoint_update",
            // video receive
            "media_video", "first_frame_callback", "remote_user_multi_stream", "electron_video",
            // video send
            "set_video_device_by_id",
            // Vesktop extensions (Discord's JS knows nothing of them): getVideoInputDeviceCapabilities, setVideoInputDeviceOptions,
            // getNativeScreenSharePickedSource, setVideoEncoderPreference, getVideoEncoders, setNoiseModel, transport options
            // vesktopVideoBitrate / vesktopAudioBitrate
            "vesktop_device_options", "vesktop_noise_models"};
}

// ------------------------------------------------------------------------------------------ VoiceConnection

class VoiceConnectionWrap : public Napi::ObjectWrap<VoiceConnectionWrap> {
public:
    static Napi::Function Define(Napi::Env env, const char* name, bool replay);

    explicit VoiceConnectionWrap(Info info) : Napi::ObjectWrap<VoiceConnectionWrap>(info)
    {
        if (!E().Initialized()) {
            Napi::Error::New(info.Env(), "initialize() must be called first").ThrowAsJavaScriptException();
            return;
        }
        // VoiceReplayConnection(replayLog, audioEngineId, cb) has no connection behind it
        if (info.Length() >= 3 && info[0].IsString() && info[1].IsObject() && !info[1].IsFunction()) {
            conn_ = std::make_shared<Connection>(ToStr(info[0]), ToJson(info[1]), JsCallback::Create(Arg(info, 2), "onConnect"));
        }
    }
    ~VoiceConnectionWrap() override
    {
        if (conn_) conn_->Destroy();
    }

private:
    Connection* C() { return conn_.get(); }

#define CONN_METHOD(name, body)                                                                                        \
    Napi::Value name(Info info)                                                                                        \
    {                                                                                                                  \
        if (auto* c = C()) {                                                                                           \
            body;                                                                                                      \
        }                                                                                                              \
        return info.Env().Undefined();                                                                                 \
    }
#define CONN_SLOT(name, slot) CONN_METHOD(name, c->callbacks.slot.Set(JsCallback::Create(Arg(info, 0), #slot)))

    CONN_METHOD(Destroy, c->Destroy())
    CONN_METHOD(SetTransportOptions, c->SetTransportOptions(ToJson(Arg(info, 0))))
    CONN_METHOD(SetSelfMute, c->SetSelfMute(ToBool(Arg(info, 0))))
    CONN_METHOD(SetSelfDeafen, c->SetSelfDeafen(ToBool(Arg(info, 0))))
    CONN_METHOD(MergeUsers, c->MergeUsers(ToJson(Arg(info, 0))))
    CONN_METHOD(DestroyUser, c->DestroyUser(ToStr(Arg(info, 0))))
    CONN_METHOD(SetLocalVolume, c->SetLocalVolume(ToStr(Arg(info, 0)), static_cast<float>(ToNum(Arg(info, 1), 1))))
    CONN_METHOD(SetLocalMute, c->SetLocalMute(ToStr(Arg(info, 0)), ToBool(Arg(info, 1))))
    CONN_METHOD(SetLocalPan, c->SetLocalPan(ToStr(Arg(info, 0)), static_cast<float>(ToNum(Arg(info, 1), 1)),
                                            static_cast<float>(ToNum(Arg(info, 2), 1))))
    CONN_METHOD(SetRemoteUserSpeakingStatus, c->SetRemoteUserSpeakingStatus(ToStr(Arg(info, 0)), static_cast<int>(ToNum(Arg(info, 1)))))
    CONN_METHOD(SetRemoteUserCanHavePriority, c->SetRemoteUserCanHavePriority(ToStr(Arg(info, 0)), ToBool(Arg(info, 1))))
    CONN_METHOD(SetPTTActive, c->SetPTTActive(ToBool(Arg(info, 0)), ToBool(Arg(info, 1))))
    CONN_METHOD(SetMinimumOutputDelay, c->SetMinimumOutputDelay(static_cast<int>(ToNum(Arg(info, 0)))))
    CONN_METHOD(SetDisableLocalVideo, c->SetDisableLocalVideo(ToStr(Arg(info, 0)), ToBool(Arg(info, 1))))
    CONN_METHOD(SetVideoBroadcast, c->SetVideoBroadcast(ToBool(Arg(info, 0))))
    CONN_METHOD(SetDesktopSourceWithOptions, c->SetDesktopSource(ToJson(Arg(info, 0))))
    CONN_METHOD(ClearDesktopSource, c->ClearDesktopSource())
    CONN_METHOD(SetGoLiveDevices, c->SetGoLiveDevices(ToJson(Arg(info, 0))))
    CONN_METHOD(ClearGoLiveDevices, c->ClearGoLiveDevices())
    // legacy setDesktopSource(id, videoHook, type): ("", false, "") stops
    Napi::Value SetDesktopSourceLegacy(Info info)
    {
        if (auto* c = C()) {
            auto id = ToStr(Arg(info, 0));
            auto colon = id.find(':');
            json options = {{"type", colon == std::string::npos ? ToStr(Arg(info, 2)) : id.substr(0, colon)}, {"sourceId", id}};
            if (id.empty()) c->ClearDesktopSource();
            else c->SetDesktopSource(options);
        }
        return info.Env().Undefined();
    }
    CONN_METHOD(SetPingInterval, c->SetPingInterval(static_cast<int>(ToNum(Arg(info, 0), 5000))))
    CONN_METHOD(FastUdpReconnect, c->FastUdpReconnect())
    CONN_METHOD(SetUdpEndpoint, c->SetUdpEndpoint(ToStr(Arg(info, 0)), static_cast<int>(ToNum(Arg(info, 1)))))
    CONN_METHOD(ConfigureConnectionRetries, c->ConfigureRetries(static_cast<int>(ToNum(Arg(info, 0))), static_cast<int>(ToNum(Arg(info, 1))),
                                                                static_cast<int>(ToNum(Arg(info, 2)))))
    CONN_METHOD(GetFilteredStats, c->GetStats(static_cast<int>(ToNum(Arg(info, 0), 7)), JsCallback::Create(Arg(info, 1), "stats")))
    CONN_METHOD(StopSamplesLocalPlayback, c->StopSamples(ToStr(Arg(info, 0))))
    CONN_METHOD(StopAllSamplesLocalPlayback, c->StopAllSamples())
    CONN_METHOD(SetNoInputThreshold, E().SetNoInputThreshold(static_cast<float>(ToNum(Arg(info, 0), -100))))
    CONN_METHOD(SetNoInputCallback, E().callbacks.noInput.Set(JsCallback::Create(Arg(info, 0), "noInput")))

    CONN_SLOT(SetOnSpeakingCallback, speaking)
    CONN_SLOT(SetOnNativeMuteChangedCallback, nativeMuteChanged)
    CONN_SLOT(SetOnSpeakingWhileMutedCallback, speakingWhileMuted)
    CONN_SLOT(SetPingCallback, ping)
    CONN_SLOT(SetPingTimeoutCallback, pingTimeout)
    CONN_SLOT(SetOnConnectionFailedCallback, connectionFailed)
    CONN_SLOT(SetOnVideoCallback, video)
    CONN_SLOT(SetOnFirstFrameCallback, firstFrame)
    CONN_SLOT(SetOnFirstFrameDeliveredStatsCallback, firstFrameDelivered)
    CONN_SLOT(SetOnFirstFrameEncryptedStatsCallback, firstFrameEncrypted)
    CONN_SLOT(SetOnDesktopSourceEnded, desktopSourceEnded)
    CONN_SLOT(SetDesktopSourceStatusCallback, desktopSourceStatus)
    CONN_SLOT(SetOnSoundshare, soundshare)
    CONN_SLOT(SetOnSoundshareEnded, soundshareEnded)
    CONN_SLOT(SetOnSoundshareFailed, soundshareFailed)
    CONN_SLOT(SetOnVideoEncoderFallbackCallback, encoderFallback)
    CONN_SLOT(SetOnVideoDecoderFallbackCallback, decoderFallback)
    CONN_SLOT(SetVideoCodecErrorCallback, codecError)
    CONN_SLOT(SetOnRtcpMessageCallback, rtcpMessage)
    CONN_SLOT(SetOnMLSFailureCallback, mlsFailure)
    CONN_SLOT(SetSecureFramesStateUpdateCallback, secureFramesState)

    Napi::Value GetEncryptionModes(Info info)
    {
        Later(Arg(info, 0), [](Napi::Env env) {
            auto& modes = SupportedEncryptionModes();
            auto arr = Napi::Array::New(env, modes.size());
            for (uint32_t i = 0; i < modes.size(); i++) arr.Set(i, modes[i]);
            return JsCallback::Args{arr};
        });
        return info.Env().Undefined();
    }

    Napi::Value StartSamplesLocalPlayback(Info info)
    {
        auto cb = JsCallback::Create(Arg(info, 3), "samples");
        if (!C()) {
            if (cb) cb->Call([](Napi::Env env) { return JsCallback::Args{Napi::Number::New(env, 3), Napi::String::New(env, "Not supported")}; });
            return info.Env().Undefined();
        }
        std::vector<std::vector<float>> channels;
        if (Arg(info, 2).IsArray()) {
            auto arr = info[2].As<Napi::Array>();
            for (uint32_t i = 0; i < arr.Length() && i < 2; i++) {
                auto v = arr.Get(i);
                if (!v.IsTypedArray() || v.As<Napi::TypedArray>().TypedArrayType() != napi_float32_array) continue;
                auto f = v.As<Napi::Float32Array>();
                channels.emplace_back(f.Data(), f.Data() + f.ElementLength());
            }
        }
        if (channels.empty()) {
            if (cb) cb->Call([](Napi::Env env) { return JsCallback::Args{Napi::Number::New(env, 1), Napi::String::New(env, "No samples")}; });
            return info.Env().Undefined();
        }
        C()->StartSamples(ToStr(Arg(info, 0)), ToJson(Arg(info, 1)), std::move(channels), cb);
        return info.Env().Undefined();
    }

    // DAVE
    Napi::Value PrepareSecureFramesTransition(Info info)
    {
        if (auto* c = C()) c->Dave().PrepareTransition(static_cast<int>(ToNum(Arg(info, 0))), static_cast<int>(ToNum(Arg(info, 1))));
        Later(Arg(info, 2));
        return info.Env().Undefined();
    }
    CONN_METHOD(PrepareSecureFramesEpoch, c->Dave().PrepareEpoch(ToStr(Arg(info, 0)), static_cast<int>(ToNum(Arg(info, 1))), ToStr(Arg(info, 2))))
    CONN_METHOD(ExecuteSecureFramesTransition, c->Dave().ExecuteTransition(static_cast<int>(ToNum(Arg(info, 0)))))
    CONN_METHOD(UpdateMLSExternalSender, c->Dave().UpdateExternalSender(ToBytes(Arg(info, 0))))

    Napi::Value GetMLSKeyPackage(Info info)
    {
        if (auto* c = C()) {
            auto pkg = c->Dave().GetKeyPackage();
            Later(Arg(info, 0), [pkg](Napi::Env env) { return JsCallback::Args{ToArrayBuffer(env, pkg)}; });
        }
        return info.Env().Undefined();
    }

    Napi::Value ProcessMLSProposals(Info info)
    {
        if (auto* c = C()) {
            auto commit = c->Dave().ProcessProposals(ToBytes(Arg(info, 0)));
            if (commit) Later(Arg(info, 1), [commit](Napi::Env env) { return JsCallback::Args{ToArrayBuffer(env, *commit)}; });
        }
        return info.Env().Undefined();
    }

    static JsCallback::ArgsBuilder RosterResult(bool ok, int version, DaveSession::Roster roster)
    {
        return [ok, version, roster = std::move(roster)](Napi::Env env) {
            auto obj = Napi::Object::New(env);
            for (auto& [id, key] : roster) obj.Set(id, ToArrayBuffer(env, key));
            return JsCallback::Args{Napi::Boolean::New(env, ok), Napi::Number::New(env, version), obj};
        };
    }

    Napi::Value PrepareMLSCommitTransition(Info info)
    {
        if (auto* c = C()) {
            auto res = c->Dave().ProcessCommit(static_cast<int>(ToNum(Arg(info, 0))), ToBytes(Arg(info, 1)));
            if (res) Later(Arg(info, 2), RosterResult(res->first, c->Dave().ProtocolVersion(), res->second));
        }
        return info.Env().Undefined();
    }

    Napi::Value ProcessMLSWelcome(Info info)
    {
        if (auto* c = C()) {
            auto res = c->Dave().ProcessWelcome(static_cast<int>(ToNum(Arg(info, 0))), ToBytes(Arg(info, 1)));
            Later(Arg(info, 2), RosterResult(res.first, c->Dave().ProtocolVersion(), res.second));
        }
        return info.Env().Undefined();
    }

    Napi::Value GetMLSPairwiseFingerprint(Info info)
    {
        if (auto* c = C()) {
            auto cb = JsCallback::Create(Arg(info, 2), "fingerprint");
            c->Dave().GetPairwiseFingerprint(static_cast<int>(ToNum(Arg(info, 0))), ToStr(Arg(info, 1)), [cb](const Bytes& fp) {
                if (cb) cb->Call([fp](Napi::Env env) { return JsCallback::Args{ToArrayBuffer(env, fp)}; });
            });
        }
        return info.Env().Undefined();
    }

    Napi::Value StubMethod(Info info) { return info.Env().Undefined(); }

    std::shared_ptr<Connection> conn_;
};

Napi::Function VoiceConnectionWrap::Define(Napi::Env env, const char* name, bool)
{
    using W = VoiceConnectionWrap;
    auto stub = [](const char* n) { return InstanceMethod(n, &W::StubMethod); };
    return DefineClass(
        env, name,
        {
            InstanceMethod("destroy", &W::Destroy),
            InstanceMethod("setTransportOptions", &W::SetTransportOptions),
            InstanceMethod("getEncryptionModes", &W::GetEncryptionModes),
            InstanceMethod("setSelfMute", &W::SetSelfMute),
            InstanceMethod("setSelfDeafen", &W::SetSelfDeafen),
            InstanceMethod("mergeUsers", &W::MergeUsers),
            InstanceMethod("destroyUser", &W::DestroyUser),
            InstanceMethod("setLocalVolume", &W::SetLocalVolume),
            InstanceMethod("setLocalMute", &W::SetLocalMute),
            InstanceMethod("setLocalPan", &W::SetLocalPan),
            InstanceMethod("setRemoteUserSpeakingStatus", &W::SetRemoteUserSpeakingStatus),
            InstanceMethod("setRemoteUserCanHavePriority", &W::SetRemoteUserCanHavePriority),
            InstanceMethod("setPTTActive", &W::SetPTTActive),
            InstanceMethod("setMinimumOutputDelay", &W::SetMinimumOutputDelay),
            InstanceMethod("setPingInterval", &W::SetPingInterval),
            InstanceMethod("fastUdpReconnect", &W::FastUdpReconnect),
            InstanceMethod("setUdpEndpoint", &W::SetUdpEndpoint),
            InstanceMethod("configureConnectionRetries", &W::ConfigureConnectionRetries),
            InstanceMethod("getFilteredStats", &W::GetFilteredStats),
            InstanceMethod("startSamplesLocalPlayback", &W::StartSamplesLocalPlayback),
            InstanceMethod("stopSamplesLocalPlayback", &W::StopSamplesLocalPlayback),
            InstanceMethod("stopAllSamplesLocalPlayback", &W::StopAllSamplesLocalPlayback),
            InstanceMethod("setNoInputThreshold", &W::SetNoInputThreshold),
            InstanceMethod("setNoInputCallback", &W::SetNoInputCallback),
            InstanceMethod("setOnSpeakingCallback", &W::SetOnSpeakingCallback),
            InstanceMethod("setOnNativeMuteChangedCallback", &W::SetOnNativeMuteChangedCallback),
            InstanceMethod("setOnSpeakingWhileMutedCallback", &W::SetOnSpeakingWhileMutedCallback),
            InstanceMethod("setPingCallback", &W::SetPingCallback),
            InstanceMethod("setPingTimeoutCallback", &W::SetPingTimeoutCallback),
            InstanceMethod("setOnConnectionFailedCallback", &W::SetOnConnectionFailedCallback),
            InstanceMethod("setOnVideoCallback", &W::SetOnVideoCallback),
            InstanceMethod("setOnFirstFrameCallback", &W::SetOnFirstFrameCallback),
            InstanceMethod("setOnFirstFrameDeliveredStatsCallback", &W::SetOnFirstFrameDeliveredStatsCallback),
            InstanceMethod("setOnFirstFrameEncryptedStatsCallback", &W::SetOnFirstFrameEncryptedStatsCallback),
            InstanceMethod("setOnDesktopSourceEnded", &W::SetOnDesktopSourceEnded),
            InstanceMethod("setDesktopSourceStatusCallback", &W::SetDesktopSourceStatusCallback),
            InstanceMethod("setOnSoundshare", &W::SetOnSoundshare),
            InstanceMethod("setOnSoundshareEnded", &W::SetOnSoundshareEnded),
            InstanceMethod("setOnSoundshareFailed", &W::SetOnSoundshareFailed),
            InstanceMethod("setOnVideoEncoderFallbackCallback", &W::SetOnVideoEncoderFallbackCallback),
            InstanceMethod("setOnVideoDecoderFallbackCallback", &W::SetOnVideoDecoderFallbackCallback),
            InstanceMethod("setVideoCodecErrorCallback", &W::SetVideoCodecErrorCallback),
            InstanceMethod("setOnRtcpMessageCallback", &W::SetOnRtcpMessageCallback),
            InstanceMethod("setOnMLSFailureCallback", &W::SetOnMLSFailureCallback),
            InstanceMethod("setSecureFramesStateUpdateCallback", &W::SetSecureFramesStateUpdateCallback),
            InstanceMethod("prepareSecureFramesTransition", &W::PrepareSecureFramesTransition),
            InstanceMethod("prepareSecureFramesEpoch", &W::PrepareSecureFramesEpoch),
            InstanceMethod("executeSecureFramesTransition", &W::ExecuteSecureFramesTransition),
            InstanceMethod("updateMLSExternalSender", &W::UpdateMLSExternalSender),
            InstanceMethod("getMLSKeyPackage", &W::GetMLSKeyPackage),
            InstanceMethod("processMLSProposals", &W::ProcessMLSProposals),
            InstanceMethod("prepareMLSCommitTransition", &W::PrepareMLSCommitTransition),
            InstanceMethod("processMLSWelcome", &W::ProcessMLSWelcome),
            InstanceMethod("getMLSPairwiseFingerprint", &W::GetMLSPairwiseFingerprint),
            // video, Go Live; spatial audio, A/V sync, clips and replay are stubs
            stub("setUserPosition"),
            InstanceMethod("setDisableLocalVideo", &W::SetDisableLocalVideo),
            stub("setAvSyncEnabled"),
            InstanceMethod("setVideoBroadcast", &W::SetVideoBroadcast),
            InstanceMethod("setDesktopSource", &W::SetDesktopSourceLegacy),
            InstanceMethod("setDesktopSourceWithOptions", &W::SetDesktopSourceWithOptions),
            InstanceMethod("clearDesktopSource", &W::ClearDesktopSource),
            InstanceMethod("setGoLiveDevices", &W::SetGoLiveDevices),
            InstanceMethod("clearGoLiveDevices", &W::ClearGoLiveDevices),
            stub("presentDesktopSourcePicker"),
            stub("startReplay"),
            stub("setClipRecordUser"),
        });
}


// ------------------------------------------------------------------------------------------ engine exports

Napi::Value Initialize(Info info)
{
    E().Initialize(ToJson(Arg(info, 0)));
    return info.Env().Undefined();
}

Napi::Value ConsoleLog(Info info)
{
    LogRaw(ToStr(Arg(info, 0), "info"), ToStr(Arg(info, 1)));
    return info.Env().Undefined();
}

Napi::Value SetTransportOptions(Info info)
{
    E().SetTransportOptions(ToJson(Arg(info, 0)));
    return info.Env().Undefined();
}

Napi::Value PingVoiceThread(Info info)
{
    auto cb = JsCallback::Create(Arg(info, 0), "pingVoiceThread");
    if (cb && E().Initialized()) E().Worker()->PostTask([cb] { cb->Call(); });
    else if (cb) cb->Call();
    return info.Env().Undefined();
}

Napi::Value PollQueueMetrics(Info info)
{
    Later(Arg(info, 0), [](Napi::Env env) {
        auto o = Napi::Object::New(env);
        o.Set("queueName", "Voice");
        o.Set("taskMetrics", Napi::Array::New(env));
        return JsCallback::Args{o};
    });
    return info.Env().Undefined();
}

Napi::Value GetSystemSteadyClockNowMs(Info info)
{
    return Napi::Number::New(info.Env(), static_cast<double>(NowMs()));
}

Napi::Value UpdateFieldTrial(Info info)
{
    VV_LOGI("engine", "updateFieldTrial %s=%s (ignored)", ToStr(Arg(info, 0)).c_str(), ToStr(Arg(info, 1)).c_str());
    return info.Env().Undefined();
}

Napi::Value RankRtcRegions(Info info)
{
    std::vector<RegionEndpoints> regions;
    auto j = ToJson(Arg(info, 0));
    if (j.is_array())
        for (auto& r : j) {
            RegionEndpoints e{Get<std::string>(r, "region", ""), {}};
            if (r.contains("ips") && r["ips"].is_array())
                for (auto& ip : r["ips"])
                    if (ip.is_string()) e.ips.push_back(ip.get<std::string>());
            regions.push_back(std::move(e));
        }
    auto cb = JsCallback::Create(Arg(info, 1), "rankRtcRegions");
    RankRtcRegions(std::move(regions), [cb](std::vector<std::string> ranked) {
        if (cb) cb->Call([ranked](Napi::Env env) { return JsCallback::Args{FromJson(env, json(ranked))}; });
    });
    return info.Env().Undefined();
}

Napi::Value GetSupportedBandwidthEstimationExperiments(Info info)
{
    Later(Arg(info, 0), [](Napi::Env env) { return JsCallback::Args{Napi::Array::New(env)}; });
    return info.Env().Undefined();
}

Napi::Value GetCodecCapabilities(Info info)
{
    auto caps = CodecCapabilities().dump() + "\n";
    Later(Arg(info, 0), [caps](Napi::Env env) { return JsCallback::Args{Napi::String::New(env, caps)}; });
    return info.Env().Undefined();
}

Napi::Value GetCodecSurvey(Info info)
{
    Later(Arg(info, 0), [](Napi::Env env) { return JsCallback::Args{Napi::String::New(env, "null\n")}; });
    return info.Env().Undefined();
}

Napi::Value ReturnFalse(Info info)
{
    return Napi::Boolean::New(info.Env(), false);
}

Napi::Value GetAudioSubsystem(Info info)
{
    Later(Arg(info, 0), [](Napi::Env env) {
        return JsCallback::Args{Napi::String::New(env, "standard"), Napi::String::New(env, E().FakeAudio() ? "dummy" : "linuxPulseAudio")};
    });
    return info.Env().Undefined();
}

Napi::Value GetMLSSigningKey(Info info)
{
    auto sessionId = ToStr(Arg(info, 0));
    int version = static_cast<int>(ToNum(Arg(info, 1), 1));
    auto cb = JsCallback::Create(Arg(info, 2), "getMLSSigningKey");
    std::thread([sessionId, version, cb] {
        Bytes signature;
        auto key = DaveSession::MlsSigningKey(sessionId, version, signature);
        if (cb) cb->Call([key, signature](Napi::Env env) { return JsCallback::Args{ToArrayBuffer(env, key), ToArrayBuffer(env, signature)}; });
    }).detach();
    return info.Env().Undefined();
}

Napi::Value GetInputDevices(Info info)
{
    auto devices = E().InputDevices();
    Later(Arg(info, 0), [devices](Napi::Env env) { return JsCallback::Args{FromJson(env, devices)}; });
    return info.Env().Undefined();
}

Napi::Value GetOutputDevices(Info info)
{
    auto devices = E().OutputDevices();
    Later(Arg(info, 0), [devices](Napi::Env env) { return JsCallback::Args{FromJson(env, devices)}; });
    return info.Env().Undefined();
}

Napi::Value GetVideoInputDevices(Info info)
{
    auto cb = JsCallback::Create(Arg(info, 0), "getVideoInputDevices");
    if (!cb) return info.Env().Undefined();
    Camera::Get().GetDevices([cb](const json& devices) {
        cb->Call([devices](Napi::Env env) { return JsCallback::Args{FromJson(env, devices)}; });
    });
    return info.Env().Undefined();
}

Napi::Value SetVideoInputDevice(Info info)
{
    Camera::Get().SetDevice(ToStr(Arg(info, 0)));
    return info.Env().Undefined();
}

// Vesktop extension: getVideoInputDeviceCapabilities(deviceId, cb({guid, formats, autofocus, options}))
Napi::Value GetVideoInputDeviceCapabilities(Info info)
{
    auto id = ToStr(Arg(info, 0));
    auto cb = JsCallback::Create(Arg(info, 1), "getVideoInputDeviceCapabilities");
    if (!cb) return info.Env().Undefined();
    Camera::Get().GetDevices([cb, id](const json&) {
        auto caps = Camera::Get().Capabilities(id);
        cb->Call([caps](Napi::Env env) { return JsCallback::Args{FromJson(env, caps)}; });
    });
    return info.Env().Undefined();
}

// Vesktop extension: getNativeScreenSharePickedSource() -> {sourceType: "screen" | "window", appId} | null
Napi::Value GetNativeScreenSharePickedSource(Info info)
{
    return FromJson(info.Env(), ScreenCastPortal::Get().PickedInfo());
}

// Vesktop extension: setVideoEncoderPreference("auto" | "hardware" | "software") for H.264 / H.265
Napi::Value SetVideoEncoderPreference(Info info)
{
    SetEncoderPreference(ToStr(Arg(info, 0)));
    return info.Env().Undefined();
}

// Vesktop extension: getVideoEncoders(cb({h264, h265})): the FFmpeg encoder each would use ("" = none)
Napi::Value GetVideoEncoders(Info info)
{
    json r = {{"h264", FfmpegEncoderName(EncodeCodec::H264)}, {"h265", FfmpegEncoderName(EncodeCodec::H265)},
              {"preference", EncoderPreference()}};
    return FromJson(info.Env(), r);
}

// Vesktop extension: which noise model answers Discord's noise cancellation flag. NoiseModel: STANDARD, RNNOISE
// (default), DEEPFILTERNET
Napi::Value SetNoiseModel(Info info)
{
    int model = static_cast<int>(ToNum(Arg(info, 0), 1));
    if (model < 0 || model > 2) model = 1;
    E().SetNoiseModel(static_cast<NoiseModel>(model));
    return info.Env().Undefined();
}

Napi::Value GetNoiseModel(Info info)
{
    return Napi::Number::New(info.Env(), static_cast<int>(E().GetNoiseModel()));
}

// Vesktop extension: whether a model really processes audio here
Napi::Value IsRnNoiseSupported(Info info)
{
    return Napi::Boolean::New(info.Env(), Engine::NoiseModelSupported(NoiseModel::RnNoise));
}

Napi::Value IsDeepFilterSupported(Info info)
{
    return Napi::Boolean::New(info.Env(), Engine::NoiseModelSupported(NoiseModel::DeepFilter));
}

// Vesktop extension: setVideoInputDeviceOptions(deviceId, {width, height, frameRate, autofocus})
Napi::Value SetVideoInputDeviceOptions(Info info)
{
    Camera::Get().SetDeviceOptions(ToStr(Arg(info, 0)), ToJson(Arg(info, 1)));
    return info.Env().Undefined();
}

// System screen share picker (xdg-desktop-portal)
Napi::Value SetNativeScreenSharePickerCallbacks(Info info)
{
    E().callbacks.pickerUpdate.Set(JsCallback::Create(Arg(info, 0), "pickerUpdate"));
    E().callbacks.pickerCancel.Set(JsCallback::Create(Arg(info, 1), "pickerCancel"));
    E().callbacks.pickerError.Set(JsCallback::Create(Arg(info, 2), "pickerError"));
    return info.Env().Undefined();
}

Napi::Value PresentNativeScreenSharePicker(Info info)
{
    auto style = ToStr(Arg(info, 0));
    ScreenCastPortal::Get().Present(style, [style](ScreenCastPortal::Outcome outcome, const std::string& error) {
        auto& cb = E().callbacks;
        switch (outcome) {
        case ScreenCastPortal::Outcome::Picked:
            // content as Discord's NativeScreenSharePickerStore reads it; the portal tells us no titles
            cb.pickerUpdate.Call([style](Napi::Env env) {
                auto content = Napi::Object::New(env);
                content.Set("applications", Napi::Array::New(env));
                content.Set("windows", Napi::Array::New(env));
                auto displays = Napi::Array::New(env);
                if (style != "window") {
                    auto d = Napi::Object::New(env);
                    d.Set("id", 0);
                    displays.Set(0u, d);
                }
                content.Set("displays", displays);
                return JsCallback::Args{Napi::Boolean::New(env, false), content};
            });
            break;
        case ScreenCastPortal::Outcome::Cancelled:
            cb.pickerCancel.Call([](Napi::Env env) { return JsCallback::Args{Napi::Boolean::New(env, false)}; });
            break;
        case ScreenCastPortal::Outcome::Error:
            cb.pickerError.Call([error](Napi::Env env) { return JsCallback::Args{Napi::String::New(env, error)}; });
            break;
        }
    });
    return info.Env().Undefined();
}

Napi::Value ReleaseNativeDesktopVideoSourcePickerStream(Info info)
{
    ScreenCastPortal::Get().Release();
    return info.Env().Undefined();
}

Napi::Value SetInputDevice(Info info)
{
    E().SetInputDevice(ToStr(Arg(info, 0)));
    return info.Env().Undefined();
}

Napi::Value SetOutputDevice(Info info)
{
    E().SetOutputDevice(ToStr(Arg(info, 0)));
    return info.Env().Undefined();
}

Napi::Value SetInputVolume(Info info)
{
    E().SetInputVolume(static_cast<float>(ToNum(Arg(info, 0), 1)));
    return info.Env().Undefined();
}

Napi::Value SetOutputVolume(Info info)
{
    E().SetOutputVolume(static_cast<float>(ToNum(Arg(info, 0), 1)));
    return info.Env().Undefined();
}

Napi::Value SetEmitVADLevel2(Info info)
{
    E().SetEmitVadLevel(ToBool(Arg(info, 0)));
    return info.Env().Undefined();
}

Napi::Value SetEmitVADLevel(Info info)
{
    E().SetEmitVadLevel(ToBool(Arg(info, 0)));
    if (info.Length() > 1) E().SetLoopback(ToBool(Arg(info, 1)), ToJson(Arg(info, 2)));
    return info.Env().Undefined();
}

Napi::Value SetNoInputThreshold(Info info)
{
    E().SetNoInputThreshold(static_cast<float>(ToNum(Arg(info, 0), -100)));
    return info.Env().Undefined();
}

Napi::Value SetLoopback(Info info)
{
    E().SetLoopback(ToBool(Arg(info, 0)), ToJson(Arg(info, 1)));
    return info.Env().Undefined();
}

Napi::Value SetDeviceHardwareMutedChangeCallback(Info info)
{
    E().callbacks.deviceHardwareMuted.Set(JsCallback::Create(Arg(info, 1), "deviceHardwareMuted"));
    return info.Env().Undefined();
}

Napi::Value GetNoiseCancellationStats(Info info)
{
    auto s = E().GetStats();
    Later(Arg(info, 0), [s](Napi::Env env) {
        if (!s.noiseCancellerFrames) return JsCallback::Args{Napi::String::New(env, "null\n")};
        json j = {{"lowNoiseMs", 0}, {"mediumNoiseMs", 0}, {"highNoiseMs", 0}, {"talkTimeMs", s.noiseCancellerFrames * 10}};
        return JsCallback::Args{Napi::String::New(env, j.dump() + "\n")};
    });
    return info.Env().Undefined();
}

Napi::Value GetSystemMicrophoneMode(Info info)
{
    auto deferred = Napi::Promise::Deferred::New(info.Env());
    deferred.Resolve(Napi::String::New(info.Env(), ""));
    return deferred.Promise();
}

Napi::Value SetClipsPerfMonitoring(Info info)
{
    auto deferred = Napi::Promise::Deferred::New(info.Env());
    deferred.Resolve(info.Env().Undefined());
    return deferred.Promise();
}

Napi::Value StartLocalAudioRecording(Info info)
{
    bool ok = E().StartLocalRecording(ToJson(Arg(info, 0)));
    Later(Arg(info, 1), [ok](Napi::Env env) { return JsCallback::Args{Napi::Boolean::New(env, ok)}; });
    return info.Env().Undefined();
}

Napi::Value StopLocalAudioRecording(Info info)
{
    auto [file, ms] = E().StopLocalRecording();
    Later(Arg(info, 0), [file, ms](Napi::Env env) {
        return JsCallback::Args{Napi::String::New(env, file), Napi::Number::New(env, static_cast<double>(ms))};
    });
    return info.Env().Undefined();
}

Napi::Value SaveScreenshot(Info info)
{
    return StubError(info, 6);
}

Napi::Value SaveClipEx(Info info)
{
    return StubError(info, 2);
}

Napi::Value ExportClipToFile(Info info)
{
    return StubError(info, 4);
}

Napi::Value UpdateClipMetadata(Info info)
{
    return StubError(info, 3);
}

Napi::Value GetPreviews(Info info)
{
    Later(Arg(info, 2), [](Napi::Env env) { return JsCallback::Args{Napi::Array::New(env)}; });
    return info.Env().Undefined();
}

// Callback video sinks
Napi::Value SetImageDataAllocator(Info info)
{
    VideoSinks::Get().SetAllocator(Arg(info, 0));
    return info.Env().Undefined();
}

Napi::Value SetVideoOutputSink(Info info)
{
    VideoSinks::Get().SetSink(ToStr(Arg(info, 0)), JsCallback::Create(Arg(info, 1), "videoOutputSink"));
    return info.Env().Undefined();
}

Napi::Value SignalVideoOutputSinkReady(Info info)
{
    VideoSinks::Get().SignalReady(ToStr(Arg(info, 0)));
    return info.Env().Undefined();
}

Napi::Value VesktopVoiceInfo(Info info)
{
    auto env = info.Env();
    auto o = Napi::Object::New(env);
    o.Set("version", VESKTOP_MEDIA_VERSION);
    auto features = SupportedFeatures();
    auto arr = Napi::Array::New(env, features.size());
    for (uint32_t i = 0; i < features.size(); i++) arr.Set(i, features[i]);
    o.Set("features", arr);
    o.Set("video", true);
    o.Set("screenCapture", true);
    o.Set("dave", static_cast<double>(discord::dave::MaxSupportedProtocolVersion()));
    o.Set("noiseCancellation", RnNoise::Available());
    o.Set("deepFilter", DeepFilter::Available());
    return o;
}

} // namespace

Napi::Object Init(Napi::Env env, Napi::Object exports)
{
    auto fn = [&](const char* name, Napi::Value (*f)(Info)) { exports.Set(name, Napi::Function::New(env, f, name)); };

    auto degradation = Napi::Object::New(env);
    degradation.Set("MAINTAIN_RESOLUTION", 0);
    degradation.Set("MAINTAIN_FRAMERATE", 1);
    degradation.Set("BALANCED", 2);
    degradation.Set("MAINTAIN_FRAMERATE_AND_RESOLUTION", 3);
    exports.Set("DegradationPreference", degradation);
    exports.Set("SupportedSecureFramesProtocolVersion", static_cast<double>(discord::dave::MaxSupportedProtocolVersion()));
    exports.Set("VoiceConnection", VoiceConnectionWrap::Define(env, "VoiceConnection", false));
    exports.Set("VoiceReplayConnection", VoiceConnectionWrap::Define(env, "VoiceReplayConnection", true));

    // lifecycle & capabilities
    fn("initialize", Initialize);
    fn("consoleLog", ConsoleLog);
    fn("setTransportOptions", SetTransportOptions);
    fn("pingVoiceThread", PingVoiceThread);
    fn("pollQueueMetrics", PollQueueMetrics);
    fn("getSystemSteadyClockNowMs", GetSystemSteadyClockNowMs);
    fn("updateFieldTrial", UpdateFieldTrial);
    fn("rankRtcRegions", RankRtcRegions);
    fn("getSupportedBandwidthEstimationExperiments", GetSupportedBandwidthEstimationExperiments);
    fn("getCodecCapabilities", GetCodecCapabilities);
    fn("getCodecSurvey", GetCodecSurvey);
    fn("isVaapiEnabled", ReturnFalse);
    fn("isSpatialAudioEnabled", ReturnFalse);
    fn("getAudioSubsystem", GetAudioSubsystem);
    fn("getMLSSigningKey", GetMLSSigningKey);
    fn("setKrispPath", Stub);
    fn("setMLPath", Stub);
    fn("setImageDataAllocator", SetImageDataAllocator);
    exports.Set("setSpatialAudioStatusCallback", Napi::Function::New(env, SLOT_SETTER(spatialAudioStatus)));

    // audio devices and processing
    fn("getInputDevices", GetInputDevices);
    fn("getOutputDevices", GetOutputDevices);
    fn("getVideoInputDevices", GetVideoInputDevices);
    fn("setInputDevice", SetInputDevice);
    fn("setOutputDevice", SetOutputDevice);
    fn("setVideoInputDevice", SetVideoInputDevice);
    fn("getVideoInputDeviceCapabilities", GetVideoInputDeviceCapabilities);
    fn("setVideoInputDeviceOptions", SetVideoInputDeviceOptions);
    fn("getNativeScreenSharePickedSource", GetNativeScreenSharePickedSource);
    fn("setVideoEncoderPreference", SetVideoEncoderPreference);
    fn("getVideoEncoders", GetVideoEncoders);
    fn("setNoiseModel", SetNoiseModel);
    fn("getNoiseModel", GetNoiseModel);
    fn("isRnNoiseSupported", IsRnNoiseSupported);
    fn("isDeepFilterSupported", IsDeepFilterSupported);
    auto noiseModel = Napi::Object::New(env);
    noiseModel.Set("STANDARD", 0);
    noiseModel.Set("RNNOISE", 1);
    noiseModel.Set("DEEPFILTERNET", 2);
    exports.Set("NoiseModel", noiseModel);
    fn("setInputVolume", SetInputVolume);
    fn("setOutputVolume", SetOutputVolume);
    exports.Set("setDeviceChangeCallback", Napi::Function::New(env, SLOT_SETTER(deviceChange)));
    exports.Set("setVolumeChangeCallback", Napi::Function::New(env, SLOT_SETTER(volumeChange)));
    fn("setDeviceHardwareMutedChangeCallback", SetDeviceHardwareMutedChangeCallback);
    exports.Set("setOnVoiceCallback", Napi::Function::New(env, SLOT_SETTER(onVoice)));
    fn("setEmitVADLevel", SetEmitVADLevel);
    fn("setEmitVADLevel2", SetEmitVADLevel2);
    fn("setNoInputThreshold", SetNoInputThreshold);
    exports.Set("setNoInputCallback", Napi::Function::New(env, SLOT_SETTER(noInput)));
    fn("setLoopback", SetLoopback);
    fn("setVoiceSampleRateCap", Stub);
    fn("setVoiceChannelCountCap", Stub);
    fn("setSidechainCompression", Stub);
    fn("applySidechainCompressionSettings", Stub);
    fn("setNoiseCancellationEnableStats", Stub);
    fn("getNoiseCancellationStats", GetNoiseCancellationStats);
    fn("getSystemMicrophoneMode", GetSystemMicrophoneMode);
    fn("setSystemMicrophoneModeChangeCallback", Stub);
    fn("showSystemCaptureConfigurationUI", Stub);
    exports.Set("setAudioDeviceModuleErrorCallback", Napi::Function::New(env, SLOT_SETTER(admError)));
    exports.Set("setAudioInputInitializationCallback", Napi::Function::New(env, SLOT_SETTER(audioInputInit)));
    exports.Set("setVideoInputInitializationCallback", Napi::Function::New(env, SLOT_SETTER(videoInputInit)));
    exports.Set("setVoiceProcessingErrorCallback", Napi::Function::New(env, SLOT_SETTER(voiceProcessingError)));
    fn("setAecDump", Stub);
    fn("writeAudioDebugState", Stub);
    fn("startRecordingRawSamples", Stub);
    fn("stopRecordingRawSamples", Stub);
    fn("processBatchAudioFiles", StubCallLast);
    fn("cancelBatchAudioProcessing", Stub);
    fn("startLocalAudioRecording", StartLocalAudioRecording);
    fn("stopLocalAudioRecording", StopLocalAudioRecording);

    // video output: callback sinks (index.js and Vesktop's preload build the direct sinks on top of them)
    fn("setVideoOutputSink", SetVideoOutputSink);
    fn("signalVideoOutputSinkReady", SignalVideoOutputSinkReady);
    fn("addDirectVideoOutputSink", Stub);
    fn("removeDirectVideoOutputSink", Stub);
    fn("setMaxSyncDelayOverride", Stub);

    // screen share
    fn("presentNativeScreenSharePicker", PresentNativeScreenSharePicker);
    fn("setNativeScreenSharePickerCallbacks", SetNativeScreenSharePickerCallbacks);
    fn("releaseNativeDesktopVideoSourcePickerStream", ReleaseNativeDesktopVideoSourcePickerStream);
    fn("setNativeDesktopVideoSourcePickerActive", Stub);
    fn("getScreenPreviews", GetPreviews);
    fn("getWindowPreviews", GetPreviews);
    fn("setPreviewsUseWgc", Stub);
    fn("saveScreenshot", SaveScreenshot);

    // media effects
    fn("applyMediaFilterSettings", Stub);
    fn("applyMediaFilterSettingsWithCallback", StubCallLast);
    exports.Set("setVideoFilterErrorCallback", Napi::Function::New(env, SLOT_SETTER(videoFilterError)));

    // clips / NDI (Windows features, never declared on Linux)
    for (const char* name : {"setClipsSource", "applyClipsSettings", "applyClipsQualitySettings", "setClipBufferLength",
                             "setClipsRecordingEnabled", "setClipsDataPath", "setClipsModulePath", "setClipsSentryConfig",
                             "setClipsUIActive", "setClipsV3MLEnabled", "setClipsAudioModelOverride", "setOnClipsMlDetection",
                             "setOnClipsRecordingEvent", "setNdiEnabled", "setNdiModulePath", "setNdiParticipantInfo",
                             "setNdiSentryConfig", "setOnNdiEvent"})
        fn(name, Stub);
    fn("saveClipEx", SaveClipEx);
    fn("exportClipToFile", ExportClipToFile);
    fn("updateClipMetadata", UpdateClipMetadata);
    fn("setClipsPerfMonitoring", SetClipsPerfMonitoring);

    // not part of the API Discord uses: what this build supports (index.js declares features from it)
    fn("vesktopVoiceInfo", VesktopVoiceInfo);
    return exports;
}

} // namespace vv

static Napi::Object InitModule(Napi::Env env, Napi::Object exports)
{
    return vv::Init(env, exports);
}

NODE_API_MODULE(vesktop_media, InitModule)
