// Fake Discord SFU for testing the media module without Discord's servers.
//
//   ELECTRON_RUN_AS_NODE=1 electron tools/fake-sfu.js <path to .node> [--video VP8|H264] [--recv-video VP8|H264]
//                                                   [--seconds 5] [--vad <dB>]
//
// --vad sets a manual VAD threshold (default -100 = always speaking); 0 means never speaking
// --recv-video (VP8, H264 or H265) streams a test clip (encoded with the ffmpeg CLI) as user "222"'s camera and checks that
//   frames come out of the module's video sink. --clip-dir <dir> keeps the encoded clips there and reuses them
//   (lets a test hide the system FFmpeg from the module)
//
// Must run under Electron (ELECTRON_RUN_AS_NODE=1): the module then runs as in Vesktop, with Electron's allocator
// and its libffmpeg (an H.264 decoding fallback). Uses fake audio/video capture, nothing real is recorded or sent anywhere,
// all traffic goes to 127.0.0.1.
//
// What it checks:
//   * IP discovery request/response, onConnect transport info
//   * keepalive pings (0x1337cafe + u32 LE sequence, answered with 0x1337f00d), ping callback
//   * aead_aes256_gcm_rtpsize RTP/RTCP layout: decrypts every packet with a known key
//   * receive path: re-encrypts our own audio as remote user "222" (ssrc 9999) and checks inbound stats
//   * video send path (--video VP8): our camera (fake capture) is sent back as user "222"'s camera, so it
//     must come out of the video sink again
//   * video receive path (--recv-video): video callback, first frame callback, decoded RGBA frames, stats
//   * rankRtcRegions latency pings
//   * dumps getFilteredStats(ALL) to fake-sfu-stats.json

"use strict";
const dgram = require("dgram");
const crypto = require("crypto");
const fs = require("fs");
const os = require("os");
const path = require("path");

const args = process.argv.slice(2);
const modulePath = args.find(a => !a.startsWith("--"));
if (!modulePath) {
    console.error("usage: fake-sfu.js <module.node> [--video VP8|H264] [--seconds N]");
    process.exit(2);
}
const opt = name => {
    const i = args.indexOf(name);
    return i >= 0 ? args[i + 1] : undefined;
};
const VIDEO = opt("--video");
const SECONDS = Number(opt("--seconds") ?? 5);
const VAD_THRESHOLD = Number(opt("--vad") ?? -100);
const RECV_VIDEO = opt("--recv-video");
const LOOP_VIDEO = VIDEO && !RECV_VIDEO; // our own video reflected as the remote camera
const LAYERS = Number(opt("--layers") ?? 1); // simulcast layers of our video
// --wants-late: like a real call. No alwaysSendVideo; viewers ask for pixels only 1.5 s after the camera started.
// The module must report the layer as active meanwhile (the gateway forwards only active streams), but send nothing
const WANTS_LATE = args.includes("--wants-late");
// --js-inactive: the layers arrive with `active: false`, as Discord's JS creates them (it takes the flag back
// from our setOnVideoCallback report). The module must start them anyway
const JS_INACTIVE = args.includes("--js-inactive");
// --screen <node>: Go Live (stream connection) from that PipeWire node instead of the portal pick
const SCREEN_NODE = opt("--screen");
// --expect-source-end: the screen source goes away during the test; setOnDesktopSourceEnded must fire
const EXPECT_SOURCE_END = args.includes("--expect-source-end");
// --kill <pid>: that process (the captured window's) is killed 1.5 s after the desktop source was set
const KILL_PID = Number(opt("--kill") ?? 0);
let sourceEnded = false;
// --switch-camera: the selected camera is changed during the call (voice.setVideoInputDevice); the call must send
// the new one at once (its own video report names the new camera), not when something else changes its video
const SWITCH_CAMERA = args.includes("--switch-camera");
// --switch-codec: Discord's JS changes the video codec during the call (another payload type, the layers come back with
// `active: false`, see --js-inactive); the recreated stream must send again
const SWITCH_CODEC = args.includes("--switch-codec");
let framesAtCodecSwitch = null;
let cameraSwitchedAt = 0, cameraSwitchedMs = null;
// --no-destroy: the connection is not destroyed before exiting, so Node's environment teardown (as when the window is
// reloaded or Vesktop restarts) finalizes it: that once aborted the renderer (exit code 134)
const NO_DESTROY = args.includes("--no-destroy");
// --golive-camera: Go Live with a device (setGoLiveDevices) from the fake camera
const GOLIVE_CAMERA = args.includes("--golive-camera");
// --video-bitrate <bps>: Vesktop's override (transport option vesktopVideoBitrate) on the Go Live camera
const VIDEO_BITRATE = opt("--video-bitrate");
// --soundshare <pid>: with --golive-camera, share that process's audio (setTransportOptions soundsharePid); the
// reflected audio must come back with sound in it
const SOUNDSHARE_PID = opt("--soundshare");
let soundshareAttached = false;
let soundshareOk = true;
// Linux: a PipeWire node taken as the portal's pick. Windows: "screen:<id>" or "window:<HWND>", as the Go Live picker
// passes them (--screen screen:0 captures the first screen)
if (SCREEN_NODE && process.platform !== "win32") process.env.VESKTOP_MEDIA_PIPEWIRE_NODE = SCREEN_NODE;
const CLIP_DIR = opt("--clip-dir");
// DAVE epoch 1 + transition 0 without an MLS group: DAVE turns off, which once deadlocked the JS thread
const DAVE_RESET = args.includes("--dave-reset");
// --late-join: the clip has a single key frame and streaming starts after it, as if we joined mid-stream;
// the key frame is only sent again when the module asks for it with an RTCP PLI
const LATE_JOIN = args.includes("--late-join");
const TRACE = args.includes("--trace"); // log every audio packet

const MODE = "aead_aes256_gcm_rtpsize";
const KEY = Buffer.alloc(32, 7);
const SELF_SSRC = 4321;
const REMOTE_SSRC = 9999;
const REMOTE_VIDEO_SSRC = 7000;
const VIDEO_PT = 101;

const t0 = Date.now();
const log = (...a) => console.log(String(Date.now() - t0).padStart(6), ...a);

const workDir = fs.mkdtempSync(path.join(os.tmpdir(), "fake-sfu-"));
const voice = require(path.resolve(modulePath));

const counts = { discovery: 0, ping: 0, rtp: {}, rtcp: 0, pli: 0, decryptOk: 0, decryptFail: 0 };
let keyFrameRequested = false;
let reflectCounter = 0;
let videoStarted = false;
let wantsAt = null;
let selfVideoActive = null; // `active` of the first self video report

function rtpAadLength(p) {
    const cc = p[0] & 0x0f;
    const x = (p[0] >> 4) & 1;
    return 12 + 4 * cc + (x ? 4 : 0);
}

// rtpsize AEAD: [unencrypted AAD][ciphertext][16 byte tag][4 byte nonce counter]
function decrypt(p, aadLen) {
    const nonce = p.subarray(p.length - 4);
    const body = p.subarray(aadLen, p.length - 4);
    const iv = Buffer.alloc(12);
    nonce.copy(iv, 0);
    const d = crypto.createDecipheriv("aes-256-gcm", KEY, iv);
    d.setAAD(p.subarray(0, aadLen));
    d.setAuthTag(body.subarray(body.length - 16));
    return { nonce, plain: Buffer.concat([d.update(body.subarray(0, body.length - 16)), d.final()]) };
}

function encrypt(header, plain) {
    const nonce = Buffer.alloc(4);
    nonce.writeUInt32LE(reflectCounter++, 0);
    const iv = Buffer.alloc(12);
    nonce.copy(iv, 0);
    const c = crypto.createCipheriv("aes-256-gcm", KEY, iv);
    c.setAAD(header);
    return Buffer.concat([header, c.update(plain), c.final(), c.getAuthTag(), nonce]);
}

// ------------------------------------------------------------------ test clip for --recv-video

const { execFileSync } = require("child_process");

// Encoded frames of a 2 s 320x240 test pattern at 15 fps
function encodeClip(codec) {
    const gop = LATE_JOIN ? "1000" : "15";
    const out = path.join(CLIP_DIR ?? workDir, `clip-g${gop}.` + codec.toLowerCase());
    const enc = {
        VP8: ["-c:v", "libvpx", "-deadline", "realtime", "-g", gop, "-b:v", "300k", "-f", "ivf"],
        H264: ["-c:v", "libx264", "-profile:v", "baseline", "-tune", "zerolatency", "-g", gop, "-x264-params", "aud=1", "-f", "h264"],
        H265: ["-c:v", "libx265", "-tune", "zerolatency", "-g", gop, "-x265-params", "aud=1:repeat-headers=1:log-level=error", "-f", "hevc"]
    }[codec];
    if (!fs.existsSync(out)) execFileSync("ffmpeg", ["-loglevel", "error", "-f", "lavfi", "-i", "testsrc=size=320x240:rate=15", "-t", "2", "-pix_fmt", "yuv420p", ...enc, "-y", out]);
    const data = fs.readFileSync(out);
    const frames = [];
    if (codec === "VP8") {
        for (let o = data.readUInt16LE(6); o + 12 <= data.length; ) {
            const size = data.readUInt32LE(o);
            frames.push(data.subarray(o + 12, o + 12 + size));
            o += 12 + size;
        }
        return frames;
    }
    // Annex B NAL units grouped into access units at each access unit delimiter
    const nals = [];
    let start = -1;
    for (let i = 0; i + 3 <= data.length; i++) {
        if (data[i] === 0 && data[i + 1] === 0 && data[i + 2] === 1) {
            if (start >= 0) nals.push(data.subarray(start, data[i - 1] === 0 ? i - 1 : i));
            start = i + 3;
            i += 2;
        }
    }
    if (start >= 0) nals.push(data.subarray(start));
    let au = null;
    for (const n of nals) {
        if (codec === "H264" ? (n[0] & 0x1f) === 9 : ((n[0] >> 1) & 0x3f) === 35) {
            if (au?.length) frames.push(au);
            au = [];
        } else au?.push(n);
    }
    if (au?.length) frames.push(au);
    return frames;
}

// RTP payloads of one frame (RFC 7741 VP8, RFC 6184 H.264 / RFC 7798 H.265 single NAL or fragmentation
// units), at most 1100 bytes each
function packetize(codec, frame) {
    const MAX = 1100;
    const out = [];
    if (codec === "VP8") {
        for (let o = 0; o < frame.length; o += MAX)
            out.push(Buffer.concat([Buffer.from([o === 0 ? 0x10 : 0x00]), frame.subarray(o, o + MAX)]));
        return out;
    }
    for (const nal of frame) {
        if (nal.length <= MAX) {
            out.push(nal);
            continue;
        }
        const h265 = codec === "H265";
        const headerSize = h265 ? 2 : 1;
        const type = h265 ? (nal[0] >> 1) & 0x3f : nal[0] & 0x1f;
        const indicator = h265 ? [(nal[0] & 0x81) | (49 << 1), nal[1]] : [(nal[0] & 0xe0) | 28];
        for (let o = headerSize; o < nal.length; o += MAX) {
            const first = o === headerSize, last = o + MAX >= nal.length;
            const fu = (first ? 0x80 : 0) | (last ? 0x40 : 0) | type;
            out.push(Buffer.concat([Buffer.from([...indicator, fu]), nal.subarray(o, o + MAX)]));
        }
    }
    return out;
}

let videoSeq = 0;
function startRemoteVideo(port, address) {
    const frames = encodeClip(RECV_VIDEO);
    log(`streaming ${frames.length} ${RECV_VIDEO} frames as 222 (ssrc ${REMOTE_VIDEO_SSRC})`);
    let i = 0;
    const timer = setInterval(() => {
        if (LATE_JOIN) {
            // skip the key frame (index 0) unless it was asked for
            if (i % frames.length === 0 && !keyFrameRequested) i++;
            if (keyFrameRequested) {
                keyFrameRequested = false;
                i = Math.ceil(i / frames.length) * frames.length;
            }
        }
        const frame = frames[i++ % frames.length];
        const ts = (i * 6000) >>> 0; // 90 kHz / 15 fps
        const payloads = packetize(RECV_VIDEO, frame);
        payloads.forEach((payload, n) => {
            const header = Buffer.alloc(12);
            header[0] = 0x80;
            header[1] = VIDEO_PT | (n === payloads.length - 1 ? 0x80 : 0);
            header.writeUInt16BE(videoSeq++ & 0xffff, 2);
            header.writeUInt32BE(ts, 4);
            header.writeUInt32BE(REMOTE_VIDEO_SSRC, 8);
            sock.send(encrypt(header, payload), port, address);
        });
    }, 1000 / 15);
    timer.unref();
}

const video = { callbacks: [], firstFrame: null, frames: 0, size: null, nonBlack: false };

const sock = dgram.createSocket("udp4");
sock.on("message", (msg, from) => {
    // IP discovery: u16 type=1, u16 len=70, u32 ssrc, char[64] address, u16 port
    if (msg.length === 74 && msg.readUInt16BE(0) === 1) {
        counts.discovery++;
        log("discovery request, ssrc", msg.readUInt32BE(4));
        const r = Buffer.alloc(74);
        r.writeUInt16BE(2, 0);
        r.writeUInt16BE(70, 2);
        msg.copy(r, 4, 4, 8);
        r.write("10.20.30.40", 8);
        r.writeUInt16BE(4242, 72);
        sock.send(r, from.port, from.address);
        return;
    }

    // keepalive / latency ping: 0x1337cafe + u32 LE sequence. Discord's voice servers answer 0x1337f00d + the same
    // sequence (seen in a real call), so does this one
    if (msg.length === 8 && msg.readUInt32BE(0) === 0x1337cafe) {
        counts.ping++;
        log("ping seq", msg.readUInt32LE(4));
        const answer = Buffer.from(msg);
        answer.writeUInt32BE(0x1337f00d, 0);
        sock.send(answer, from.port, from.address);
        return;
    }

    if (msg[0] >> 6 !== 2) {
        log("unknown packet", msg.length, msg.toString("hex"));
        return;
    }

    const pt = msg[1];
    if (pt >= 200 && pt <= 206) {
        counts.rtcp++;
        try {
            const { plain } = decrypt(msg, 8);
            counts.decryptOk++;
            // compound packet: the first 8 bytes are clear, the rest is the decrypted plaintext
            const rtcp = Buffer.concat([msg.subarray(0, 8), plain]);
            for (let o = 0; o + 4 <= rtcp.length; ) {
                const fmt = rtcp[o] & 0x1f, type = rtcp[o + 1], len = (rtcp.readUInt16BE(o + 2) + 1) * 4;
                if (type === 206 && fmt === 1 && o + 12 <= rtcp.length && rtcp.readUInt32BE(o + 8) === REMOTE_VIDEO_SSRC) {
                    counts.pli++;
                    keyFrameRequested = true;
                }
                o += len;
            }
        } catch {
            counts.decryptFail++;
        }
        return;
    }

    const payloadType = pt & 0x7f;
    counts.rtp[payloadType] = (counts.rtp[payloadType] ?? 0) + 1;
    const ssrcKey = "ssrc" + msg.readUInt32BE(8);
    counts[ssrcKey] = (counts[ssrcKey] ?? 0) + 1;
    const aadLen = rtpAadLength(msg);
    let plain;
    try {
        plain = decrypt(msg, aadLen).plain;
        counts.decryptOk++;
    } catch {
        counts.decryptFail++;
        return;
    }

    if (TRACE && payloadType === 120) {
        const extLen = (msg[0] & 0x10) ? msg.readUInt16BE(14) * 4 : 0;
        const payload = plain.subarray(extLen);
        log("rtp", "seq", msg.readUInt16BE(2), "ts", msg.readUInt32BE(4), "m", msg[1] >> 7, "ext", plain.subarray(0, extLen).toString("hex"), "payload", payload.length, payload.subarray(0, 4).toString("hex"));
    }

    if (RECV_VIDEO && !videoStarted && payloadType === 120) {
        videoStarted = true;
        startRemoteVideo(from.port, from.address);
    }

    // our own video (media, not RTX) back as 222's camera
    // with --layers 2 the lower layer: nothing here gives bandwidth feedback, so the upper one gets no bitrate
    const loopSsrc = LAYERS > 1 ? SELF_SSRC + 3 : SELF_SSRC + 1;
    if (LOOP_VIDEO && payloadType === VIDEO_PT && msg.readUInt32BE(8) === loopSsrc) {
        const header = Buffer.from(msg.subarray(0, aadLen));
        header.writeUInt32BE(REMOTE_VIDEO_SSRC, 8);
        sock.send(encrypt(header, plain), from.port, from.address);
    }

    // send our own audio back as if it came from another user
    if (payloadType === 120) {
        const header = Buffer.from(msg.subarray(0, aadLen));
        header.writeUInt32BE(REMOTE_SSRC, 8);
        sock.send(encrypt(header, plain), from.port, from.address);
    }
});

sock.bind(0, "127.0.0.1", () => {
    const port = sock.address().port;
    log("fake SFU on 127.0.0.1:" + port);

    voice.initialize({
        audioSubsystem: "standard",
        logLevel: 2,
        dataDirectory: workDir,
        logDirectory: workDir,
        maxLogBytes: 5000000,
        offloadAdmControls: true,
        asyncVideoInputDeviceInit: false,
        useFakeAudioCapture: !args.includes("--real-audio"), // --real-audio: the real microphone
        useFakeVideoCapture: true
    });

    voice.rankRtcRegions([{ region: "local", ips: [`127.0.0.1:${port}`] }], ranked =>
        log("rankRtcRegions ->", JSON.stringify(ranked))
    );

    const streamParameters = [{ type: "audio", ssrc: SELF_SSRC, rid: "", maxBitrate: 64000, soundshare: false }];
    if (VIDEO)
        streamParameters.push({
            type: "video", rid: "100", ssrc: SELF_SSRC + 1, rtxSsrc: SELF_SSRC + 2, quality: 100, active: !JS_INACTIVE,
            maxBitrate: 2500000, maxFrameRate: 30, maxResolution: { type: "fixed", width: 1280, height: 720 }
        });
    if (VIDEO && LAYERS > 1)
        streamParameters.push({
            type: "video", rid: "50", ssrc: SELF_SSRC + 3, rtxSsrc: SELF_SSRC + 4, quality: 50, active: true,
            maxBitrate: 600000, maxFrameRate: 30, maxResolution: { type: "fixed", width: 640, height: 360 }
        });

    const conn = new voice.VoiceConnection(
        "111",
        {
            ssrc: SELF_SSRC, address: "127.0.0.1", port, modes: [MODE], experiments: [], streamParameters,
            context: SCREEN_NODE || GOLIVE_CAMERA ? "stream" : "default", videoSupported: !!VIDEO, qosEnabled: false
        },
        (err, info) => {
            log("onConnect", JSON.stringify(err), JSON.stringify(info));
            if (err) process.exit(1);

            conn.getEncryptionModes(modes => {
                log("encryption modes", JSON.stringify(modes));
                conn.setOnSpeakingCallback((...a) => log("speaking", JSON.stringify(a)));
                conn.setPingCallback((...a) => log("ping callback", JSON.stringify(a)));
                conn.setOnVideoCallback((...a) => {
                    log("video callback", JSON.stringify(a));
                    const [userId, ssrc, streamId] = a;
                    video.callbacks.push(a);
                    if (SWITCH_CAMERA && cameraSwitchedAt && userId === "111" && streamId === "Strobe Video Source 2" && cameraSwitchedMs === null) {
                        cameraSwitchedMs = Date.now() - cameraSwitchedAt;
                        log("camera switched: own video is now", streamId, "after", cameraSwitchedMs, "ms");
                    }
                    if (userId === "111" && ssrc && selfVideoActive === null) selfVideoActive = a[3]?.every(l => l.active);
                    if (userId !== "222" || !ssrc || !streamId || !(RECV_VIDEO || LOOP_VIDEO)) return;
                    voice.setVideoOutputSink(streamId, image => {
                        video.frames++;
                        video.size = `${image.width}x${image.height}`;
                        // the test pattern is colourful: some pixel must be far from black
                        for (let p = 0; p < image.data.length && !video.nonBlack; p += 4 * 97)
                            if (image.data[p] + image.data[p + 1] + image.data[p + 2] > 200) video.nonBlack = true;
                        voice.signalVideoOutputSinkReady(streamId);
                    }, true);
                });
                conn.setOnFirstFrameCallback?.((...a) => {
                    log("first frame callback", JSON.stringify(a));
                    video.firstFrame = a;
                });
                conn.setTransportOptions({
                    selfMute: false,
                    inputMode: 1,
                    inputModeOptions: { vadThreshold: VAD_THRESHOLD, vadAutoThreshold: -1, vadUseKrisp: false, vadLeading: 5, vadTrailing: 25 },
                    fec: true,
                    packetLossRate: 0.3,
                    encodingVoiceBitRate: 64000
                });
                conn.setSelfMute(false);
                const remoteVideo = RECV_VIDEO || LOOP_VIDEO ? [REMOTE_VIDEO_SSRC] : [];
                conn.mergeUsers([{
                    id: "222", ssrc: REMOTE_SSRC, videoSsrc: remoteVideo[0] ?? 0, videoSsrcs: remoteVideo,
                    rtxSsrc: remoteVideo.length ? REMOTE_VIDEO_SSRC + 1 : 0, mute: false, volume: 1
                }]);
                conn.setTransportOptions({
                    audioEncoder: { type: 120, name: "opus", freq: 48000, pacsize: 960, channels: 1, rate: 64000 },
                    audioDecoders: [{ type: 120, name: "opus", freq: 48000, channels: 2, params: { stereo: "1" } }],
                    videoEncoder: VIDEO
                        ? { name: VIDEO, type: 101, rtxType: 102, params: VIDEO === "H264" ? { "level-asymmetry-allowed": "1", "packetization-mode": "1", "profile-level-id": "4d0033" } : {} }
                        : { name: "", type: 0, rtxType: 0, params: {} },
                    videoDecoders: VIDEO || RECV_VIDEO ? [{ name: VIDEO || RECV_VIDEO, type: VIDEO_PT, rtxType: VIDEO_PT + 1, params: {} }] : []
                });
                conn.setTransportOptions({ encryptionSettings: { mode: MODE, secretKey: [...KEY] } });

                if (DAVE_RESET) {
                    conn.setSecureFramesStateUpdateCallback?.(state => log("secure frames state", JSON.stringify(state)));
                    conn.prepareSecureFramesEpoch("1", 1, "123456789");
                    conn.prepareSecureFramesTransition(0, 1, () => log("dave transition 0 prepared"));
                    log("dave reset done, JS thread alive");
                }

                if (VIDEO && GOLIVE_CAMERA) {
                    conn.setGoLiveDevices({ videoInputDeviceId: "Strobe Video Source", audioInputDeviceId: SOUNDSHARE_PID ? "" : "default" });
                    if (SOUNDSHARE_PID) {
                        conn.setOnSoundshare?.(attached => { soundshareAttached = attached; log("soundshare attached", attached); });
                        conn.setOnSoundshareFailed?.((...a) => log("soundshare failed", JSON.stringify(a)));
                        conn.setTransportOptions({ soundsharePid: Number(SOUNDSHARE_PID), soundshareEventDriven: true, soundshareLoopback: false });
                    }
                    conn.setTransportOptions({
                        encodingVideoWidth: 1280, encodingVideoHeight: 720, encodingVideoFrameRate: 30,
                        encodingVideoBitRate: 2500000, encodingVideoMinBitRate: 500000, encodingVideoMaxBitRate: 3500000,
                        encodingVideoDegradationPreference: 1, remoteSinkWantsPixelCount: 921600, remoteSinkWantsMaxFramerate: 30
                    });
                    if (VIDEO_BITRATE) conn.setTransportOptions({ vesktopVideoBitrate: Number(VIDEO_BITRATE) });
                    conn.setVideoBroadcast(true);
                } else if (VIDEO && SCREEN_NODE) {
                    if (KILL_PID) setTimeout(() => process.kill(KILL_PID), 1500);
                    if (process.platform === "win32") {
                        const [type, sourceId] = SCREEN_NODE.split(":");
                        conn.setDesktopSourceWithOptions({ type, sourceId });
                    } else {
                        conn.setDesktopSourceWithOptions({ type: "prepicked", sourceId: "prepicked:0" });
                    }
                    conn.setOnDesktopSourceEnded?.((...a) => { sourceEnded = true; log("desktop source ended", JSON.stringify(a)); });
                    // what Discord sends for the "Source" resolution: no size, remoteSinkWantsPixelCount 0, no
                    // alwaysSendVideo. The encoder must still run
                    conn.setTransportOptions({
                        encodingVideoWidth: 0, encodingVideoHeight: 0, encodingVideoFrameRate: 30,
                        encodingVideoBitRate: 2500000, encodingVideoMinBitRate: 500000, encodingVideoMaxBitRate: 3500000,
                        encodingVideoDegradationPreference: 1, remoteSinkWantsPixelCount: 0, remoteSinkWantsMaxFramerate: 30
                    });
                    conn.setVideoBroadcast(true);
                } else if (VIDEO)
                    voice.getVideoInputDevices(devices => {
                        log("video devices", JSON.stringify(devices));
                        if (devices[0]) voice.setVideoInputDevice(devices[0].guid || devices[0].name);
                        conn.setTransportOptions({
                            encodingVideoWidth: 1280, encodingVideoHeight: 720, encodingVideoFrameRate: 30,
                            encodingVideoBitRate: 2500000, encodingVideoMinBitRate: 150000, encodingVideoMaxBitRate: 2500000,
                            encodingVideoDegradationPreference: 2,
                            remoteSinkWantsPixelCount: 921600, remoteSinkWantsMaxFramerate: 30,
                            // nothing gives bandwidth feedback here, so force the encoder on
                            ...(WANTS_LATE ? { remoteSinkWantsPixelCount: 0 } : { keyframeInterval: 2000, alwaysSendVideo: true })
                        });
                        if (WANTS_LATE)
                            setTimeout(() => {
                                log("viewers want pixels now; video packets so far:", counts.rtp[VIDEO_PT] ?? 0);
                                wantsAt = counts.rtp[VIDEO_PT] ?? 0;
                                conn.setTransportOptions({ remoteSinkWantsPixelCount: 921600, remoteSinkWantsMaxFramerate: 30 });
                            }, 1500);
                        conn.setVideoBroadcast(true);
                        if (SWITCH_CAMERA)
                            setTimeout(() => {
                                cameraSwitchedAt = Date.now();
                                voice.setVideoInputDevice("Strobe Video Source 2");
                            }, 1500);
                        if (SWITCH_CODEC)
                            setTimeout(() => {
                                framesAtCodecSwitch = video.frames;
                                log("codec switch: frames so far", video.frames);
                                conn.setTransportOptions({
                                    streamParameters: streamParameters.map(p => (p.type === "video" ? { ...p, active: false } : p)),
                                    videoEncoder: { name: VIDEO, type: VIDEO_PT, rtxType: VIDEO_PT + 7, params: {} }
                                });
                            }, 1500);
                    });

                setTimeout(() => {
                    conn.getFilteredStats(7, stats => {
                        const s = typeof stats === "string" ? JSON.parse(stats) : stats;
                        fs.writeFileSync("fake-sfu-stats.json", JSON.stringify(s, null, 1));
                        const inbound = s.inbound?.find(u => u.id === "222")?.audio;
                        log("inbound from 222:", inbound ? `${inbound.packetsReceived} packets, ssrc ${inbound.ssrc}, level ${inbound.audioLevel}` : "none");
                        if (SOUNDSHARE_PID) {
                            const heard = soundshareAttached && inbound?.packetsReceived > 0 && inbound?.audioLevel > 0;
                            log("soundshare:", JSON.stringify({ attached: soundshareAttached, packets: inbound?.packetsReceived ?? 0, level: inbound?.audioLevel ?? 0 }));
                            soundshareOk = heard;
                        }
                        log("summary", JSON.stringify(counts));
                        let videoOk = soundshareOk && (!EXPECT_SOURCE_END || sourceEnded);
                        if (RECV_VIDEO || LOOP_VIDEO) {
                            const v = s.inbound?.find(u => u.id === "222")?.videos?.[0];
                            log("inbound video from 222:", v ? JSON.stringify({ ssrc: v.ssrc, codec: v.codecName, decoder: v.decoderImplementationName, framesDecoded: v.framesDecoded, width: v.width, height: v.height, packets: v.rtpStats?.packets }) : "none");
                            log("video sink:", JSON.stringify({ frames: video.frames, size: video.size, nonBlack: video.nonBlack, firstFrame: video.firstFrame }));
                            const size = LOOP_VIDEO ? (LAYERS > 1 ? "640x360" : "1280x720") : "320x240";
                            // a screen or window keeps its size and aspect ratio, scaled down into 1280x720 when larger
                            const fitted = sz => {
                                const [w, h] = (sz ?? "0x0").split("x").map(Number);
                                return w > 0 && h > 0 && w <= 1280 && h <= 720;
                            };
                            const sizeOk = SCREEN_NODE ? fitted(video.size) : video.size === size;
                            videoOk = video.frames >= 10 && sizeOk && video.nonBlack && video.firstFrame != null && v?.framesDecoded > 0;
                            if (WANTS_LATE) {
                                log("late wants:", JSON.stringify({ selfVideoActive, videoPacketsBeforeWants: wantsAt, total: counts.rtp[VIDEO_PT] }));
                                videoOk = videoOk && selfVideoActive === true && wantsAt === 0;
                            }
                            if (LOOP_VIDEO) {
                                const out = s.outbound?.videos?.[0];
                                log("outbound video:", out ? JSON.stringify({ ssrc: out.ssrc, codec: out.codecName, encoder: out.encoderImplementationName, framesEncoded: out.framesEncoded, mediaBitrate: out.mediaBitrate, substreams: out.substreams?.length }) : "none");
                                videoOk = videoOk && out?.framesEncoded > 0;
                            }
                        }
                        if (SWITCH_CODEC) {
                            const after = framesAtCodecSwitch === null ? 0 : video.frames - framesAtCodecSwitch;
                            log("codec switch:", JSON.stringify({ framesBefore: framesAtCodecSwitch, framesAfter: after }));
                            videoOk = videoOk && framesAtCodecSwitch > 0 && after >= 10;
                        }
                        if (SWITCH_CAMERA) {
                            log("camera switch:", cameraSwitchedMs === null ? "not sent" : `${cameraSwitchedMs} ms`);
                            videoOk = videoOk && cameraSwitchedMs !== null && cameraSwitchedMs < 1500;
                        }
                        log("native logs in", workDir);
                        if (!NO_DESTROY) conn.destroy();
                        // a stream connection sends no microphone, only soundshare
                        const speaking = VAD_THRESHOLD < 0 && !SCREEN_NODE && !GOLIVE_CAMERA;
                        const ok = videoOk && counts.decryptFail === 0 && (speaking ? inbound?.packetsReceived > 0 : true);
                        setTimeout(() => process.exit(ok ? 0 : 1), 300);
                    });
                }, SECONDS * 1000);
            });
        }
    );
});

setTimeout(() => {
    log("timeout");
    process.exit(1);
}, (SECONDS + 15) * 1000);
