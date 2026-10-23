// getFilteredStats: fields Discord's JS reads, filled from WebRTC's stream / call stats
#include <call/audio_receive_stream.h>
#include <call/audio_send_stream.h>
#include <call/call.h>
#include <call/video_receive_stream.h>
#include <rtc_base/thread.h>

#include "common/util.h"
#include "connection/connection.h"

namespace vv {

namespace {

constexpr int kFilterTransport = 1, kFilterOutbound = 2, kFilterInbound = 4;

json Histogram(double seconds)
{
    return {{"last", seconds}, {"mean", seconds}, {"p75", seconds}, {"p95", seconds}, {"p99", seconds}, {"max", seconds}};
}

} // namespace

// inbound[].videos[i]
json Connection::VideoInboundStats(const std::string& userId, const webrtc::VideoReceiveStreamInterface::Stats& v)
{
    std::string codecName;
    if (videoDecoders_.is_array())
        for (auto& d : videoDecoders_)
            if (Get(d, "type", -1) == v.current_payload_type) codecName = Get<std::string>(d, "name", "");
    auto& rtp = v.rtp_stats;
    uint64_t packets = rtp.packet_counter.packets;
    uint64_t expected = packets + std::max(0, rtp.packets_lost);
    double lossFraction = expected ? static_cast<double>(std::max(0, rtp.packets_lost)) / expected : 0.0;
    auto dec = dave_->DecryptorStats(userId, discord::dave::MediaType::Video);
    double framesDuration = v.total_inter_frame_delay;
    return {
        {"ssrc", v.ssrc},
        {"codecPayloadType", v.current_payload_type},
        {"codecName", codecName},
        {"rtpStats",
         {{"headerBytes", rtp.packet_counter.header_bytes},
          {"payloadBytes", rtp.packet_counter.payload_bytes},
          {"paddingBytes", rtp.packet_counter.padding_bytes},
          {"packets", packets},
          {"packetsLost", rtp.packets_lost},
          {"jitter", rtp.interarrival_jitter.ms()}}},
        {"rtcpStats",
         {{"packetsLost", rtp.packets_lost},
          {"fractionLost", lossFraction},
          {"firPackets", v.rtcp_packet_type_counts.fir_packets},
          {"nackPackets", v.rtcp_packet_type_counts.nack_packets},
          {"pliPackets", v.rtcp_packet_type_counts.pli_packets}}},
        {"totalBitrate", v.total_bitrate_bps},
        {"jitterBuffer", v.jitter_buffer_ms},
        {"currentDelay", v.current_delay_ms},
        {"targetDelay", v.target_delay_ms},
        {"minPlayoutDelay", v.min_playout_delay_ms},
        {"renderDelay", v.render_delay_ms},
        {"decode", v.decode_ms},
        {"totalDecode", v.total_decode_time.ms()},
        {"width", v.width},
        {"height", v.height},
        {"decoderImplementationName", v.decoder_implementation_name.value_or("")},
        {"framesDecoded", v.frames_decoded},
        {"framesDropped", v.frames_dropped},
        {"framesDecodeErrors", 0},
        {"frameCounts", {{"keyFrames", v.received_frame_counts.key_frames}, {"deltaFrames", v.received_frame_counts.delta_frames}}},
        {"networkFramesDropped", 0},
        {"decodeFrameRate", v.decode_frame_rate},
        {"networkFrameRate", v.network_frame_rate},
        {"renderFrameRate", v.render_frame_rate},
        {"qpSum", v.qp_sum.value_or(0)},
        {"freezeCount", v.freeze_count},
        {"pauseCount", v.pause_count},
        {"totalFreezesDuration", v.total_freezes_duration_ms},
        {"totalPausesDuration", v.total_pauses_duration_ms},
        {"totalFramesDuration", static_cast<int64_t>(framesDuration * 1000)},
        {"sumOfSquaredFramesDurations", v.total_squared_inter_frame_delay},
        {"passthroughCount", dec.passthroughCount},
        {"decryptSuccessCount", dec.decryptSuccessCount},
        {"decryptFailureCount", dec.decryptFailureCount},
        {"decryptDuration", dec.decryptDuration},
        {"decryptAttempts", dec.decryptAttempts},
        {"decryptMissingKeyCount", dec.decryptMissingKeyCount},
        {"decryptInvalidNonceCount", dec.decryptInvalidNonceCount},
        {"minResolutionWidth", v.width},
        {"minResolutionHeight", v.height},
    };
}

void Connection::GetStats(int filter, JsCallbackPtr cb)
{
    if (!cb) return;
    Engine::Get().Worker()->PostTask([this, filter, cb] {
        auto stats = BuildStats(filter == 0 ? 7 : filter);
        cb->Call([stats = std::move(stats)](Napi::Env env) { return JsCallback::Args{FromJson(env, stats)}; });
    });
}

json Connection::BuildStats(int filter)
{
    json out = json::object();
    if (!call_) return out;
    auto& engine = Engine::Get();
    auto engineStats = engine.GetStats();
    int daveVersion = dave_->Engaged() ? dave_->ProtocolVersion() : 0;

    if (filter & kFilterOutbound) {
        json audio = json::object();
        if (sendStream_) {
            auto s = sendStream_->GetStats();
            auto enc = dave_->EncryptorStats(discord::dave::MediaType::Audio);
            audio = {
                {"ssrc", ssrc_},
                {"codecPayloadType", s.codec_payload_type.value_or(120)},
                {"codecName", s.codec_name.empty() ? "opus" : s.codec_name},
                {"bytesSent", s.payload_bytes_sent + s.header_and_padding_bytes_sent},
                {"packetsSent", s.packets_sent},
                {"packetsLost", s.packets_lost},
                {"fractionLost", s.fraction_lost},
                {"audioLevel", s.audio_level},
                {"mediaBitrate", s.target_bitrate_bps},
                {"targetMediaBitrate", s.target_bitrate_bps},
                {"speaking", speakingFlags_ > 0 ? 1 : 0},
                {"framesCaptured", engineStats.framesCaptured},
                {"framesRendered", engineStats.framesCaptured},
                {"noiseCancellerFrames", engineStats.noiseCancellerFrames},
                {"noiseCancellerProcessTime", engineStats.noiseCancellerTimeUs / 1000},
                {"voiceActivityDetectorProcessTime", 0},
                {"jitter", s.jitter_ms},
                {"rtt", s.rtt_ms},
                {"currentSampleRate", 48000},
                {"sampleRateMismatchPercent", 0},
                {"apmProcessTimeMs", 0},
                {"echoReturnLoss", s.apm_statistics.echo_return_loss.value_or(0)},
                {"echoReturnLossEnhancement", s.apm_statistics.echo_return_loss_enhancement.value_or(0)},
                {"residualEchoLikelihood", s.apm_statistics.residual_echo_likelihood.value_or(0)},
                {"residualEchoLikelihoodRecentMax", s.apm_statistics.residual_echo_likelihood_recent_max.value_or(0)},
                {"echo_delay_ms", s.apm_statistics.delay_ms.value_or(0)},
                {"totalPacketSendDelayMs", s.total_packet_send_delay.ms()},
                {"sendPacketCount", s.packets_sent},
                {"passthroughCount", enc.passthroughCount},
                {"encryptSuccessCount", enc.encryptSuccessCount},
                {"encryptFailureCount", enc.encryptFailureCount},
                {"encryptDuration", enc.encryptDuration},
                {"encryptAttempts", enc.encryptAttempts},
                {"encryptMaxAttempts", enc.encryptMaxAttempts},
                {"encryptMissingKeyCount", enc.encryptMissingKeyCount},
            };
        }
        out["outbound"] = {{"id", userId_}, {"audio", audio}, {"videos", OutboundVideoStats()}};
    }

    if (filter & kFilterInbound) {
        json inbound = json::array();
        std::lock_guard lock(usersMutex_);
        for (auto& [id, user] : users_) {
            json videos = json::array();
            for (auto& [ssrc, vs] : user.video) videos.push_back(VideoInboundStats(id, vs.stream->GetStats()));
            if (!user.stream) {
                if (!videos.empty()) inbound.push_back({{"id", id}, {"videos", videos}});
                continue;
            }
            auto s = user.stream->GetStats(true);
            auto dec = dave_->DecryptorStats(id, discord::dave::MediaType::Audio);
            uint64_t packets = s.packets_received;
            uint64_t expected = packets + std::max(0, s.packets_lost);
            json audio = {
                {"ssrc", user.ssrc},
                {"codecPayloadType", s.codec_payload_type.value_or(120)},
                {"codecName", s.codec_name.empty() ? "opus" : s.codec_name},
                {"bytesReceived", s.payload_bytes_received + s.header_and_padding_bytes_received},
                {"packetsReceived", s.packets_received},
                {"packetsLost", s.packets_lost},
                {"fractionLost", expected ? static_cast<double>(std::max(0, s.packets_lost)) / expected : 0.0},
                {"fecPacketsReceived", s.fec_packets_received},
                {"fecPacketsDiscarded", s.fec_packets_discarded},
                {"audioLevel", std::max(0, s.audio_level)},
                {"speaking", (user.speakingFlags & kSpeakingVoice) ? 1 : 0},
                {"currentSampleRate", 48000},
                {"jitter", s.jitter_ms},
                {"jitterBuffer", s.jitter_buffer_ms},
                {"jitterBufferPreferred", s.jitter_buffer_preferred_ms},
                {"delayEstimate", s.delay_estimate_ms},
                {"decodingCallsToNetEq", s.decoding_calls_to_neteq},
                {"decodingCallsToSilenceGenerator", s.decoding_calls_to_silence_generator},
                {"decodingNormal", s.decoding_normal},
                {"decodingPLC", s.decoding_plc},
                {"decodingCNG", s.decoding_cng},
                {"decodingPLCCNG", s.decoding_plc_cng},
                {"decodingMutedOutput", s.decoding_muted_output},
                {"nackCount", s.nacks_sent},
                {"accelerateRate", s.accelerate_rate},
                {"expandRate", s.expand_rate},
                {"preemptiveExpandRate", s.preemptive_expand_rate},
                {"speechExpandRate", s.speech_expand_rate},
                {"secondaryDecodedRate", s.secondary_decoded_rate},
                // NetEq operation counters are not exposed by libwebrtc's stats; derived from the decode counters
                {"opSilence", s.decoding_muted_output},
                {"opNormal", s.decoding_normal},
                {"opMerge", 0},
                {"opExpand", s.decoding_plc},
                {"opAccelerate", 0},
                {"opPreemptiveExpand", 0},
                {"opCNG", s.decoding_cng},
                {"passthroughCount", dec.passthroughCount},
                {"decryptSuccessCount", dec.decryptSuccessCount},
                {"decryptFailureCount", dec.decryptFailureCount},
                {"decryptDuration", dec.decryptDuration},
                {"decryptAttempts", dec.decryptAttempts},
                {"decryptMissingKeyCount", dec.decryptMissingKeyCount},
                {"decryptInvalidNonceCount", dec.decryptInvalidNonceCount},
            };
            double jb = s.jitter_buffer_emitted_count ? s.jitter_buffer_delay_seconds / s.jitter_buffer_emitted_count : 0;
            double target = s.jitter_buffer_emitted_count ? s.jitter_buffer_target_delay_seconds / s.jitter_buffer_emitted_count : 0;
            json playout = {
                {"audioJitterBuffer", Histogram(s.jitter_buffer_ms / 1000.0)},
                {"audioJitterDelay", Histogram(jb)},
                {"audioJitterTarget", Histogram(target)},
                {"audioPlayoutUnderruns", Histogram(0)},
                {"audioCaptureOverruns", Histogram(0)},
                {"relativeReceptionDelay", Histogram(s.relative_packet_arrival_delay_seconds)},
                {"relativePlayoutDelay", Histogram(jb)},
                {"audioReceiverDelayMs", s.delay_estimate_ms},
                {"audioReceiverPacketCount", s.packets_received},
            };
            if (!user.video.empty()) {
                auto v = user.video.begin()->second.stream->GetStats();
                double vjb = v.jitter_buffer_emitted_count ? v.jitter_buffer_delay.seconds<double>() / v.jitter_buffer_emitted_count : 0;
                double vtarget = v.jitter_buffer_emitted_count ? v.jitter_buffer_target_delay.seconds<double>() / v.jitter_buffer_emitted_count : 0;
                playout["videoJitterBuffer"] = Histogram(v.jitter_buffer_ms / 1000.0);
                playout["videoJitterDelay"] = Histogram(vjb);
                playout["videoJitterTarget"] = Histogram(vtarget);
            }
            inbound.push_back({{"id", id}, {"audio", audio}, {"videos", videos}, {"playout", playout}});
        }
        out["inbound"] = inbound;
    }

    if (filter & kFilterTransport) {
        auto cs = call_->GetStats();
        int rtt = cs.rtt_ms >= 0 ? static_cast<int>(cs.rtt_ms) : lastRttMs_.load();
        out["transport"] = {
            {"sendBandwidth", cs.send_bandwidth_bps},
            {"outboundBitrateEstimate", cs.send_bandwidth_bps},
            {"inboundBitrateEstimate", cs.recv_bandwidth_bps},
            {"receiverBitrateEstimate", cs.recv_bandwidth_bps},
            {"rtt", rtt},
            {"decryptionFailures", decryptionFailures_.load()},
            {"routingFailures", routingFailures_.load()},
            {"localAddress", localAddress_ + ":" + std::to_string(localPort_)},
            {"pacerDelay", cs.pacer_delay_ms},
            {"receiverReports", json::array()},
            {"packetsSent", packetsSent_.load()},
            {"packetsReceived", packetsReceived_.load()},
            {"bytesSent", bytesSent_.load()},
            {"bytesReceived", bytesReceived_.load()},
            {"maxPaddingBitrate", cs.max_padding_bitrate_bps},
            {"secureFramesProtocolVersion", daveVersion},
            {"transportDelayMs", 0},
            {"transportPacketCount", packetsSent_.load()},
        };
    }

    out["clips"] = {{"processRunning", false}};
    int64_t start = engine.StartTimeMs();
    out["audioDevice"] = {
        {"input", {{"timeFromConnectToFirstCallbackMs", engineStats.firstCaptureMs ? std::max<int64_t>(0, engineStats.firstCaptureMs - start) : 0},
                   {"totalDelayMs", 0}}},
        {"output", {{"timeFromConnectToFirstCallbackMs", 0}}},
    };
    return out;
}

} // namespace vv
