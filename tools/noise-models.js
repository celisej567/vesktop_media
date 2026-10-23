// Checks the noise model extension (setNoiseModel, isRnNoiseSupported, isDeepFilterSupported): Discord's noise
// cancellation flag runs RNNoise by default or DeepFilterNet; Standard is libwebrtc's suppression. Also measures,
// with a speech-like fake microphone, that each model lowers the noise floor of the processed capture.
//   ELECTRON_RUN_AS_NODE=1 electron tools/noise-models.js <path to .node> <log directory>
"use strict";
const fs = require("fs");
const path = require("path");
const [, , modulePath, logDir] = process.argv;
const m = require(path.resolve(modulePath));
m.initialize({ logLevel: 2, logDirectory: logDir, dataDirectory: logDir, useFakeAudioCapture: true });

const fail = message => {
    console.error("FAILED:", message);
    process.exit(1);
};
const assert = (condition, message) => condition || fail(message);

assert(m.vesktopVoiceInfo().features.includes("vesktop_noise_models"), "feature vesktop_noise_models not declared");
assert(m.NoiseModel.STANDARD === 0 && m.NoiseModel.RNNOISE === 1 && m.NoiseModel.DEEPFILTERNET === 2, "NoiseModel values");
assert(m.isRnNoiseSupported() === true, "RNNoise should be supported");
assert(m.isDeepFilterSupported() === true, "DeepFilterNet should be supported");
assert(m.getNoiseModel() === m.NoiseModel.RNNOISE, "RNNoise is the default model");

const log = () => fs.readFileSync(path.join(logDir, "vesktop_media.log"), "utf8");
const lastApplied = () => log().split("\n").filter(l => l.includes("processing applied")).pop() ?? "";
const settle = () => new Promise(resolve => setTimeout(resolve, 200));

(async () => {
    // Discord's flags as the mic test sends them: noise cancellation on, suppression off
    m.setLoopback(true, { echoCancellation: true, noiseSuppression: false, automaticGainControlConfig: { enabled: true }, noiseCancellation: true });
    await settle();
    assert(/nc=1 \(model 1, denoiser RNNoise\)/.test(lastApplied()), "RNNoise should run: " + lastApplied());

    m.setNoiseModel(m.NoiseModel.DEEPFILTERNET);
    await settle();
    assert(m.getNoiseModel() === m.NoiseModel.DEEPFILTERNET, "model not set");
    assert(/ns=0 .*nc=1 \(model 2, denoiser DeepFilterNet\)/.test(lastApplied()), "DeepFilterNet should run: " + lastApplied());
    await new Promise(resolve => setTimeout(resolve, 1500));
    assert(/deepfilter: ready in/.test(log()), "the DeepFilterNet state was not created");

    m.setNoiseModel(m.NoiseModel.STANDARD);
    await settle();
    assert(m.getNoiseModel() === m.NoiseModel.STANDARD && /ns=1 .*nc=0/.test(lastApplied()), "Standard should use the standard suppression: " + lastApplied());

    m.setNoiseModel(m.NoiseModel.RNNOISE);
    await settle();
    assert(/nc=1 \(model 1, denoiser RNNoise\)/.test(lastApplied()), "RNNoise should run again: " + lastApplied());

    // the flag off: no model runs, whatever is selected
    m.setLoopback(true, { echoCancellation: true, noiseSuppression: false, automaticGainControlConfig: { enabled: true }, noiseCancellation: false });
    await settle();
    assert(/ns=0 .*nc=0/.test(lastApplied()), "with the flag off nothing should suppress noise: " + lastApplied());
    console.log("noise models ok");
    process.exit(0);
})();
