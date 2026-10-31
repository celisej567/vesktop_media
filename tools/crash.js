// The module's crash handler: VESKTOP_MEDIA_TEST_CRASH=1 makes the voice worker crash 1 s after initialize; the
// process must die of SIGSEGV and the log must end with the signal and the call stack.
//   VESKTOP_MEDIA_TEST_CRASH=1 ELECTRON_RUN_AS_NODE=1 electron tools/crash.js <path to .node> <log directory>
"use strict";
const path = require("path");
const [, , modulePath, logDir] = process.argv;
const m = require(path.resolve(modulePath));
m.initialize({ logLevel: 2, logDirectory: logDir, dataDirectory: logDir, useFakeAudioCapture: true, useFakeVideoCapture: true });
setTimeout(() => console.log("still alive: the crash did not happen"), 5000);
