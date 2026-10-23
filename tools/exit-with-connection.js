// A connection with callbacks that is never destroyed: nothing keeps the event loop alive, so the process ends by
// itself and Node's environment teardown (as when a window reloads or Vesktop restarts) finalizes the connection. The
// thread-safe functions of its callbacks are already closed by Node then: releasing them aborted the renderer (exit
// code 134, the window froze) and made this script hang.
//   ELECTRON_RUN_AS_NODE=1 electron tools/exit-with-connection.js <path to .node> <log directory>
"use strict";
const path = require("path");
const [, , modulePath, logDir] = process.argv;
const m = require(path.resolve(modulePath));
m.initialize({ logLevel: 2, logDirectory: logDir, dataDirectory: logDir, useFakeAudioCapture: true, useFakeVideoCapture: true });
const conn = new m.VoiceConnection("111", { ssrc: 4321, address: "127.0.0.1", port: 59999, context: "default", streamParameters: [] }, () => {});
conn.setOnSpeakingCallback(() => {});
conn.setPingCallback(() => {});
conn.setOnVideoCallback(() => {});
setTimeout(() => console.log("end of the script, the connection is not destroyed"), 300);
