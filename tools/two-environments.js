// Two Node environments in one process (a worker next to the main thread; in Vesktop: the page loaded again in the
// same renderer process): when the worker's environment is torn down, the callbacks of the main thread's must go on
// working. Closing was once a state of the whole process: after the first page load ended, no callback reached
// JavaScript any more (connections never got their keys: "Connecting to RTC" for ever, no cameras listed).
//   ELECTRON_RUN_AS_NODE=1 electron tools/two-environments.js <path to .node> <log directory>
"use strict";
const path = require("path");
const { Worker, isMainThread, workerData } = require("worker_threads");

function load(modulePath, logDir) {
    const m = require(path.resolve(modulePath));
    m.initialize({ logLevel: 2, logDirectory: logDir, dataDirectory: logDir, useFakeAudioCapture: true, useFakeVideoCapture: true });
    return m;
}

if (isMainThread) {
    const [, , modulePath, logDir] = process.argv;
    const m = load(modulePath, logDir);
    const worker = new Worker(__filename, { workerData: { modulePath, logDir } });
    worker.on("error", e => {
        console.error("FAILED: the worker failed:", e);
        process.exit(1);
    });
    worker.on("exit", () => {
        // the worker's environment is gone now
        const timer = setTimeout(() => {
            console.error("FAILED: no callback after the other environment ended");
            process.exit(1);
        }, 3000);
        m.getInputDevices(() => {
            clearTimeout(timer);
            console.log("callback after the other environment ended: ok");
            process.exit(0);
        });
    });
} else {
    const m = load(workerData.modulePath, workerData.logDir);
    // callbacks of this environment, never released by the script
    const conn = new m.VoiceConnection("111", { ssrc: 4321, address: "127.0.0.1", port: 59999, context: "default", streamParameters: [] }, () => {});
    conn.setOnSpeakingCallback(() => {});
    conn.setPingCallback(() => {});
    setTimeout(() => {}, 200);
}
