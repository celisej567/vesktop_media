#!/bin/sh
# Acceptance test: tools/fake-sfu.js against build/out/vesktop_media.node,
# once speaking (every packet must decrypt, the reflected audio must be received), once silent, and
# receiving video in each codec decoded by an FFmpeg.
set -e
cd "$(dirname "$0")/.."
ELECTRON=${ELECTRON:-../../node_modules/electron/dist/electron}
MODULE=${1:-build/out/vesktop_media.node}
# absolute: the cases run from a temporary directory
case $ELECTRON in /*) ;; *) ELECTRON=$PWD/$ELECTRON ;; esac
case $MODULE in /*) ;; *) MODULE=$PWD/$MODULE ;; esac
[ -x "$ELECTRON" ] || { echo "Electron not found at $ELECTRON (set ELECTRON=/path/to/electron)"; exit 1; }
WORK=$(mktemp -d)
SINK=
GST=
# (a gst already gone is no failure: under set -e a failing kill here would become the script's status)
trap 'rm -rf "$WORK"; [ -n "$SINK" ] && pactl unload-module "$SINK"; [ -z "$GST" ] || kill $GST 2> /dev/null || true' EXIT
# check <grep pattern> <fake-sfu args>: prints the matching lines, fails when fake-sfu reports a failure
check() {
    pattern=$1
    shift
    status=0
    (cd "$WORK" && ELECTRON_RUN_AS_NODE=1 timeout 60 "$ELECTRON" "$OLDPWD/tools/fake-sfu.js" "$MODULE" "$@") > "$WORK/out" 2>&1 || status=$?
    grep -E "$pattern" "$WORK/out" || true
    [ $status -eq 0 ] || { echo "FAILED ($*), full output:"; cat "$WORK/out"; exit 1; }
}
# everything the module needs from outside comes from the host (Node-API) or libc; optional libraries are
# loaded at runtime. An undefined symbol here would only fail when that code path runs
echo "== undefined symbols"
UNDEF=$(nm -D --undefined-only "$MODULE" | awk '$1 == "U" {print $2}' | grep -v -E '^(napi_|node_api_)|@GLIBC|^X(OpenDisplay|CloseDisplay|QueryKeymap)$|^_ITM_|^__gmon_start__$|^_Jv_' || true)
if [ -n "$UNDEF" ]; then echo "unexpected undefined symbols:"; echo "$UNDEF"; exit 1; fi
echo "== noise models (RNNoise by default, DeepFilterNet, Standard = libwebrtc's suppression)"
mkdir -p "$WORK/noise"
VESKTOP_MEDIA_FAKE_MIC=speech ELECTRON_RUN_AS_NODE=1 timeout 60 "$ELECTRON" tools/noise-models.js "$MODULE" "$WORK/noise" || { echo "FAILED (noise models)"; exit 1; }
echo "== noise models with a 44.1 kHz microphone (Windows records at the device's rate)"
mkdir -p "$WORK/noise44"
VESKTOP_MEDIA_FAKE_MIC=speech VESKTOP_MEDIA_FAKE_MIC_RATE=44100 ELECTRON_RUN_AS_NODE=1 timeout 60 "$ELECTRON" tools/noise-models.js "$MODULE" "$WORK/noise44" || { echo "FAILED (noise models at 44.1 kHz)"; exit 1; }
echo "== exit with a connection that was not destroyed (Node's teardown finalizes it; once aborted the renderer on restart)"
mkdir -p "$WORK/exit"
ELECTRON_RUN_AS_NODE=1 timeout 20 "$ELECTRON" tools/exit-with-connection.js "$MODULE" "$WORK/exit" > /dev/null 2>&1 || { echo "FAILED (exit with a connection)"; exit 1; }
echo "== two environments in one process: the callbacks of one go on after the other one ended (a page loaded again)"
mkdir -p "$WORK/two"
ELECTRON_RUN_AS_NODE=1 timeout 30 "$ELECTRON" tools/two-environments.js "$MODULE" "$WORK/two" > /dev/null 2>&1 || { echo "FAILED (two environments)"; exit 1; }
echo "== a crash writes its signal and call stack to the log, then goes on to the default action"
mkdir -p "$WORK/crash"
status=0
(ulimit -c 0; VESKTOP_MEDIA_TEST_CRASH=1 ELECTRON_RUN_AS_NODE=1 exec timeout 20 "$ELECTRON" tools/crash.js "$MODULE" "$WORK/crash") > /dev/null 2>&1 || status=$?  # no core dump
grep -E "\[crash\]" "$WORK/crash/vesktop_media.log" | head -4
[ $status -eq 139 ] && grep -q "\[crash\] SIGSEGV" "$WORK/crash/vesktop_media.log" && grep -q "\[crash\]   #1 " "$WORK/crash/vesktop_media.log" ||
    { echo "FAILED (crash handler, exit $status)"; exit 1; }
echo "== speaking"
check "onConnect|inbound|summary" --seconds 4
# the reflected audio (user 222) must be reported as speaking: Discord's client shows who speaks from that callback
grep -q 'speaking \["222",1' "$WORK/out" || { echo "FAILED: no speaking callback for the remote user"; exit 1; }
# the keepalive answer (0x1337f00d, as Discord's servers send it) must reach the ping callback: Discord shows the ping from it
grep -q 'ping callback \[' "$WORK/out" || { echo "FAILED: no ping callback"; exit 1; }
echo "== silent (expect 10 Opus silence frames, then nothing)"
check "summary" --seconds 3 --vad 0
echo "== DAVE turned off by a transition (once deadlocked the JS thread)"
check "dave|secure frames|summary" --seconds 2 --dave-reset
# video receive: a test clip as a remote camera must come out of the video sink (needs the ffmpeg CLI to
# encode the clips; H.265 also needs a system libavcodec with HEVC, Electron's libffmpeg has none)
if command -v ffmpeg > /dev/null; then
    for codec in VP8 H264 H265; do
        echo "== receive $codec video"
        check "inbound video|video sink" --seconds 3 --recv-video $codec
    done
    echo "== receive H264 video joined mid-stream (needs a PLI for the key frame)"
    check "summary|video sink" --seconds 4 --recv-video H264 --late-join
else
    echo "== video receive skipped (no ffmpeg CLI)"
fi
# video send: our fake camera, sent back as user 222's camera, must be decoded again
echo "== send camera video (VP8, looped back)"
check "outbound video|video sink" --seconds 3 --video VP8
echo "== send camera video the way a real call does: layer reported active, encoder idle until viewers ask"
check "late wants|viewers want|outbound video|video sink" --seconds 4 --video VP8 --wants-late
echo "== same, with the layer created inactive by JS (as Discord does)"
check "late wants|viewers want|outbound video|video sink" --seconds 4 --video VP8 --wants-late --js-inactive
echo "== send camera video with 2 simulcast layers (lower layer looped back)"
check "outbound video|video sink" --seconds 3 --video VP8 --layers 2
echo "== the selected camera changed during a call: the call sends the new one at once (not when something else changes its video)"
check "camera switch|outbound video|video sink" --seconds 4 --video VP8 --switch-camera
echo "== video codec changed during the call: the recreated stream starts its layers again"
check "codec switch|outbound video" --seconds 4 --video VP8 --switch-codec
echo "== Go Live with a device (setGoLiveDevices, fake camera)"
check "outbound video|video sink" --seconds 3 --video VP8 --golive-camera

# soundshare: a tone played by paplay into a temporary null sink (nothing audible), shared by its pid.
# Opt-in (VV_TEST_AUDIO=1): adding a sink changes the user's audio graph, and the default output or
# Easy Effects may follow it and lose the real device
if [ "$VV_TEST_AUDIO" = 1 ] && command -v pactl > /dev/null && command -v paplay > /dev/null && command -v python3 > /dev/null; then
    echo "== Go Live audio (soundshare of one process)"
    python3 -c 'import math,struct,wave
w=wave.open("'"$WORK"'/tone.wav","wb");w.setnchannels(2);w.setsampwidth(2);w.setframerate(48000)
w.writeframes(b"".join(struct.pack("<hh",int(8000*math.sin(i*2*math.pi*440/48000)),int(8000*math.sin(i*2*math.pi*440/48000))) for i in range(48000*12)))'
    SINK=$(pactl load-module module-null-sink sink_name=vesktop_media_test sink_properties=device.description=vesktop-voice-test)
    paplay --device=vesktop_media_test "$WORK/tone.wav" &
    PLAY=$!
    sleep 0.5
    status=0
    check "soundshare|inbound from" --seconds 3 --video VP8 --golive-camera --soundshare "$PLAY" || status=1
    kill $PLAY 2> /dev/null || true
    pactl unload-module "$SINK"
    SINK=
    [ $status -eq 0 ] || exit 1
else
    echo "== Go Live audio skipped (VV_TEST_AUDIO=1 to run it; it adds a temporary PulseAudio sink)"
fi

# Go Live from a GStreamer test pattern node instead of the portal (needs gst-launch-1.0 + pipewiresink)
if command -v gst-launch-1.0 > /dev/null && command -v pw-dump > /dev/null && gst-inspect-1.0 pipewiresink > /dev/null 2>&1; then
    echo "== Go Live from a PipeWire node (1920x1080 BGRx, "Source" resolution: no size, sink wants 0)"
    gst-launch-1.0 -q videotestsrc is-live=true pattern=ball ! video/x-raw,format=BGRx,width=1920,height=1080,framerate=30/1 ! \
        pipewiresink mode=provide stream-properties="properties,node.name=vesktop-voice-test,media.class=Video/Source" > /dev/null 2>&1 &
    GST=$!
    sleep 2
    NODE=$(pw-dump | node -e 'let s="";process.stdin.on("data",d=>s+=d).on("end",()=>{const n=JSON.parse(s).find(o=>o.info?.props?.["node.name"]==="vesktop-voice-test");console.log(n?.id??"")})')
    status=0
    [ -n "$NODE" ] && check "outbound video|video sink" --seconds 3 --video VP8 --screen "$NODE" || status=1
    kill $GST
    GST=
    [ $status -eq 0 ] || exit 1
    echo "== Go Live source closed mid-stream (window closed): the stream must end, not freeze"
    gst-launch-1.0 -q videotestsrc is-live=true pattern=ball ! video/x-raw,format=BGRx,width=1920,height=1080,framerate=30/1 ! \
        pipewiresink mode=provide stream-properties="properties,node.name=vesktop-voice-test2,media.class=Video/Source" > /dev/null 2>&1 &
    GST=$!
    sleep 2
    NODE=$(pw-dump | node -e 'let s="";process.stdin.on("data",d=>s+=d).on("end",()=>{const n=JSON.parse(s).find(o=>o.info?.props?.["node.name"]==="vesktop-voice-test2");console.log(n?.id??"")})')
    (sleep 1.5; kill $GST) &
    [ -n "$NODE" ] && check "desktop source ended|capture ended" --seconds 3 --video VP8 --screen "$NODE" --expect-source-end || exit 1
else
    echo "== Go Live skipped (no gst-launch-1.0 / pipewiresink / pw-dump)"
fi
echo "== ok"
