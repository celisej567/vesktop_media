"use strict";
// JS wrapper of vesktop_media: binds VoiceConnection instances into plain objects, wraps the video sinks, reads its settings through
// DiscordNative, calls initialize() and declares the supported features.

const fs = require("fs");
const path = require("path");

const engine = require("./vesktop_media.node");

const native = globalThis.window?.DiscordNative;
const inRenderer = native != null && native.isRenderer;
const settings = inRenderer ? native.settings : globalThis.appSettings;
const features = inRenderer ? native.features : globalThis.features;
const argv = inRenderer && native.processUtils?.getMainArgvSync ? native.processUtils.getMainArgvSync() : [];

function setting(key, fallback) {
    try {
        return settings ? settings.getSync(key, fallback) : fallback;
    } catch {
        return fallback;
    }
}

function log(level, message) {
    (console[level] ?? console.info)(message);
    engine.consoleLog(level, message);
}

function parseArgv(args) {
    const out = { "log-level": -1 };
    for (let i = 0; i < args.length; i++) {
        const [name, ...rest] = args[i].split("=");
        const value = () => (rest.length ? rest.join("=") : args[++i]);
        switch (name) {
            case "--log-level": out["log-level"] = parseInt(value(), 10); break;
            case "--use-fake-video-capture": out.fakeVideo = true; break;
            case "--use-file-for-fake-video-capture": out.fakeVideoFile = value(); break;
            case "--use-fake-audio-capture": out.fakeAudio = true; break;
            case "--use-files-for-fake-audio-capture": out.fakeAudioFiles = value(); break;
        }
    }
    return out;
}

let dataDirectory = null;
let logDirectory;
if (inRenderer) {
    try {
        const base = native.fileManager?.getModuleDataPathSync?.();
        if (base) {
            dataDirectory = path.join(base, "vesktop_media");
            fs.mkdirSync(dataDirectory, { recursive: true });
        }
    } catch (e) {
        console.warn("[vesktop_media] no data directory", e);
    }
    logDirectory = native.fileManager?.getAndCreateLogDirectorySync?.() ?? undefined;
}

const audioSubsystem = setting("audioSubsystem", "standard");
const offloadAdmControls = setting("offloadAdmControls", true);
const debugLogging = setting("debugLogging", true);
const maxLogBytesSetting = setting("maxLogBytes", 5000000);
const maxLogBytes = Number.isFinite(maxLogBytesSetting) && maxLogBytesSetting > 0 ? Math.min(Math.trunc(maxLogBytesSetting), 0xffffffff) : 5000000;
const args = parseArgv(argv.slice(1));
const logLevel = args["log-level"] === -1 ? (debugLogging ? 2 : -1) : args["log-level"];

// Every VoiceConnection method, bound into a plain object (Discord calls them detached)
const CONNECTION_METHODS = [
    "destroy", "setTransportOptions", "setSelfMute", "setSelfDeafen", "mergeUsers", "destroyUser",
    "prepareSecureFramesTransition", "prepareSecureFramesEpoch", "executeSecureFramesTransition",
    "updateMLSExternalSender", "getMLSKeyPackage", "processMLSProposals", "prepareMLSCommitTransition",
    "processMLSWelcome", "getMLSPairwiseFingerprint", "setOnMLSFailureCallback", "setSecureFramesStateUpdateCallback",
    "setLocalVolume", "setLocalMute", "setUserPosition", "fastUdpReconnect", "setUdpEndpoint",
    "setOnConnectionFailedCallback", "setLocalPan", "setDisableLocalVideo", "setAvSyncEnabled", "setMinimumOutputDelay",
    "getEncryptionModes", "configureConnectionRetries", "setOnSpeakingCallback", "setOnNativeMuteChangedCallback",
    "setOnSpeakingWhileMutedCallback", "setPingInterval", "setPingCallback", "setPingTimeoutCallback",
    "setRemoteUserSpeakingStatus", "setRemoteUserCanHavePriority", "setOnVideoCallback", "setOnFirstFrameCallback",
    "setOnFirstFrameDeliveredStatsCallback", "setOnFirstFrameEncryptedStatsCallback", "setVideoBroadcast",
    "setDesktopSource", "setDesktopSourceWithOptions", "setGoLiveDevices", "clearGoLiveDevices", "clearDesktopSource",
    "setDesktopSourceStatusCallback", "setOnDesktopSourceEnded", "setOnSoundshare", "setOnSoundshareEnded",
    "setOnSoundshareFailed", "setPTTActive", "getStats", "getFilteredStats", "startReplay", "setClipRecordUser",
    "setRtcLogMarker", "startSamplesLocalPlayback", "stopSamplesLocalPlayback", "stopAllSamplesLocalPlayback",
    "setOnVideoEncoderFallbackCallback", "setOnVideoDecoderFallbackCallback", "setVideoCodecErrorCallback",
    "setOnRtcpMessageCallback", "presentDesktopSourcePicker"
];

function bindConnection(instance) {
    const bound = {};
    for (const name of CONNECTION_METHODS) bound[name] = (...a) => instance[name]?.(...a);
    return bound;
}

engine.createTransport = engine._createTransport;
if (inRenderer) engine.setImageDataAllocator((w, h) => new globalThis.window.ImageData(w, h));

engine.createVoiceConnectionWithOptions = (userId, options, onConnect) =>
    bindConnection(new engine.VoiceConnection(userId, options, onConnect));
engine.createOwnStreamConnectionWithOptions = engine.createVoiceConnectionWithOptions;
engine.createReplayConnection = (audioEngineId, callback, replayLog) =>
    replayLog == null ? null : bindConnection(new engine.VoiceReplayConnection(replayLog, audioEngineId, callback));

function saveSetting(key, value) {
    if (!settings) return log("warn", "Unable to access app settings.");
    settings.set(key, value);
}
engine.setAudioSubsystem = s => {
    saveSetting("audioSubsystem", s);
    if (inRenderer && s !== audioSubsystem) void native.app?.relaunch();
};
engine.queueAudioSubsystem = s => saveSetting("audioSubsystem", s);
engine.setOffloadAdmControls = b => saveSetting("offloadAdmControls", b);
engine.setDebugLogging = b => {
    if (b === debugLogging) return;
    saveSetting("debugLogging", b);
    if (inRenderer) void native.app?.relaunch();
};
engine.getDebugLogging = () => debugLogging;

// Video sinks: several JS consumers per stream on top of the single native callback sink
const callbackSinks = new Map(); // streamId -> Map(sinkId -> fn)
const directSinks = new Set();
let activeSinksChanged = null;
engine.setActiveSinksChangeCallback = cb => (activeSinksChanged = cb);
const notifySinks = id => activeSinksChanged?.(id, (callbackSinks.get(id)?.size ?? 0) > 0 || directSinks.has(id));

const setVideoOutputSink = engine.setVideoOutputSink;
const signalVideoOutputSinkReady = engine.signalVideoOutputSinkReady;
delete engine.setVideoOutputSink;
delete engine.signalVideoOutputSinkReady;

function addCallbackSink(sinkId, streamId, fn) {
    let sinks = callbackSinks.get(streamId);
    if (!sinks) callbackSinks.set(streamId, (sinks = new Map()));
    const first = sinks.size === 0;
    sinks.set(sinkId, fn);
    if (!first) return;
    setVideoOutputSink(streamId, image => {
        for (const cb of callbackSinks.get(streamId)?.values() ?? []) cb?.(image);
        signalVideoOutputSinkReady(streamId);
    }, true);
    notifySinks(streamId);
}

function removeCallbackSink(sinkId, streamId) {
    const sinks = callbackSinks.get(streamId);
    if (!sinks) return;
    sinks.delete(sinkId);
    if (sinks.size) return;
    callbackSinks.delete(streamId);
    setVideoOutputSink(streamId);
    notifySinks(streamId);
}

const addDirect = engine.addDirectVideoOutputSink;
const removeDirect = engine.removeDirectVideoOutputSink;
engine.addDirectVideoOutputSink = streamId => {
    addDirect(streamId);
    directSinks.add(streamId);
    notifySinks(streamId);
};
engine.removeDirectVideoOutputSink = streamId => {
    removeDirect(streamId);
    directSinks.delete(streamId);
    notifySinks(streamId);
};

let nextSinkId = 0;
engine.getNextVideoOutputFrame = streamId => {
    const sinkId = `getNextVideoFrame_${++nextSinkId}`;
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            removeCallbackSink(sinkId, streamId);
            reject(new Error("getNextVideoOutputFrame timeout"));
        }, 5000);
        addCallbackSink(sinkId, streamId, image => {
            clearTimeout(timer);
            removeCallbackSink(sinkId, streamId);
            resolve({ width: image.width, height: image.height, data: new Uint8ClampedArray(image.data.buffer) });
        });
    });
};

engine.platform = process.platform;
engine.initialize({
    audioSubsystem,
    logLevel,
    dataDirectory,
    logDirectory,
    maxLogBytes,
    useFakeVideoCapture: args.fakeVideo,
    useFileForFakeVideoCapture: args.fakeVideoFile,
    useFakeAudioCapture: args.fakeAudio,
    useFilesForFakeAudioCapture: args.fakeAudioFiles,
    offloadAdmControls,
    asyncVideoInputDeviceInit: process.platform === "win32"
});

engine.setupKrispPath = () => {
    const p = native?.nativeModules?.getModulePath?.("discord_krisp");
    if (p != null) engine.setKrispPath(p);
};
engine.setupMLPath = () => {
    const p = native?.nativeModules?.getModulePath?.("discord_ml");
    if (p != null) engine.setMLPath(p);
};

// Only what this build actually implements (engine.vesktopVoiceInfo lists it)
if (features) {
    const info = engine.vesktopVoiceInfo();
    for (const f of info.features) features.declareSupported(f);
    if (engine.isSpatialAudioEnabled()) features.declareSupported("spatial_audio");
    if (process.platform === "linux" && info.video) {
        const wayland = process.env.XDG_SESSION_TYPE?.startsWith("wayland") && process.env.WAYLAND_DISPLAY != null;
        if (wayland && info.screenCapture) features.declareSupported("native_screenshare_picker");
        if (engine.isVaapiEnabled()) features.declareSupported("vaapi");
    }
}

module.exports = engine;
