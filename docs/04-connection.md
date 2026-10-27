# 4. Connection and transport

Code: `src/connection/connection.cc`, `src/transport/`, `src/dave/`, `src/connection/connection_stats.cc`.

Discord's JS runs the voice gateway (WebSocket) itself: it learns the SFU's address, the ssrcs, the encryption key,
the codecs, and passes them to the module through the `VoiceConnection` constructor and `setTransportOptions`. The
module only speaks **UDP** to the SFU: one socket per connection for IP discovery, keepalives, RTP and RTCP.

## Life of a connection

1. `new VoiceConnection(userId, {ssrc, address, port, context, streamParameters, qosEnabled…}, onConnect)`:
   resolves the address, opens the socket (DSCP 46 when QoS is on), creates the `webrtc::Call` and the audio send
   stream on the worker, registers with the engine (which starts capture / playout) and starts the network thread.
2. **IP discovery**: a 74-byte request (type 1, length 70, our ssrc), sent twice about 1 ms apart and repeated every
   second until answered. The response (type 2) contains our public address and port. `onConnect` gets
   `{protocol: "udp", address, port, connectTime}`; JS sends them to the gateway (SELECT_PROTOCOL).
3. The gateway answers with the encryption mode and the secret key: `setTransportOptions({encryptionSettings})`.
   From then on media flows.
4. `mergeUsers` / `destroyUser` as people join and leave, `setTransportOptions` for codec, bitrate and VAD changes.
5. `destroy()`: stops the network thread, the captures, every stream, the call; the engine stops capture / playout
   if nothing else needs them.

## The network thread

`NetworkLoop` polls the socket every 10 ms and handles every datagram:

* the discovery response (74 bytes, type 2);
* **keepalive pings**: every `setPingInterval` ms (default 5 s) an 8-byte ping `13 37 ca fe` + sequence (u32 LE)
  goes out. Discord's servers answer `13 37 f0 0d` + the same sequence (not an echo, seen in a real call; an unanswered ping
  shows in Discord as a ping of 4000 ms); the answer gives the
  round trip time to the ping callback. An echo is accepted too. A ping without an answer within 4 s calls the
  ping timeout callback, from which JS judges the connection's health;
* RTCP (packet type 200 to 206) and RTP: decrypted, then posted to the worker, which delivers them to the `Call`.
  RTP is routed by ssrc to audio or video; an unknown ssrc is counted as a routing failure;
* anything else (not discovery, ping answer, RTP or RTCP) is counted, and the first few are logged with their bytes
  (`unexpected packet`).

`setUdpEndpoint` changes the destination (JS may move the connection to another SFU port); `fastUdpReconnect` sends a
ping right away to refresh NAT bindings.

`MediaTransport` is the `webrtc::Transport` the streams send through: it encrypts and sends. It also reports every
sent packet to the call (`Call::OnSentPacket` with the transport sequence number and the wire size). libwebrtc's
congestion controller matches transport-cc feedback from the SFU against those send times; without them every
feedback is dropped ("Failed to lookup send time") and the delay-based estimate never grows.

## Transport encryption

`src/transport/crypto.cc`. All seven modes Discord defines, in the order `getEncryptionModes` returns them (JS takes
the first one the server also offers):

| Mode | Implementation |
|---|---|
| `aead_aes256_gcm_rtpsize`, `aead_aes256_gcm` | BoringSSL (in libwebrtc) |
| `aead_xchacha20_poly1305_rtpsize` | BoringSSL |
| `xsalsa20_poly1305_lite_rtpsize`, `xsalsa20_poly1305_lite`, `xsalsa20_poly1305_suffix`, `xsalsa20_poly1305` | libsodium, loaded only if such a mode is chosen (the servers no longer offer them) |

Layout:

* the RTP header (12 bytes + CSRCs) stays in clear and is the AEAD's additional data. In the `rtpsize` modes the
  extension preamble (`BE DE` + length) also stays in clear, the extension values are encrypted;
* RTCP keeps its first 8 bytes in clear;
* in the AEAD and `lite` modes the nonce is a 32-bit counter, sent little endian after the ciphertext (`suffix`
  sends a random 24-byte nonce, plain `xsalsa20_poly1305` uses the RTP header as the nonce). **Audio RTP and the connection's RTCP use
  one counter, video and RTX another**, both starting at `0x80000000` (atomics, as the streams send from
  different threads).

## DAVE (end-to-end encryption)

`src/dave/dave_session.cc`, on top of libdave (with mlspp and BoringSSL). It does what Discord's web client does in
its own DAVE session manager, with the callback shapes the native API uses:

* the MLS side: `getMLSKeyPackage`, `updateMLSExternalSender`, `processMLSProposals`, `prepareMLSCommitTransition`,
  `processMLSWelcome`, `prepareSecureFramesTransition` / `Epoch`, `executeSecureFramesTransition`,
  `getMLSPairwiseFingerprint`. Commits and welcomes answer with the roster `{userId: key}`; failures go to the MLS
  failure callback; the secure frames state callback gets `{version, epochAuthenticator}`;
* the frame side: our audio frames are encrypted in `FrameGate`, our video frames in `VideoFrameEncryptor`, remote
  frames are decrypted by one `FrameDecryptor` per user. Each sending ssrc is assigned its codec (Opus, VP8, H.264,
  H.265), because DAVE leaves codec-specific parts of a frame in clear so the SFU can still read them;
* while DAVE is not engaged (or a transition turned it off) frames pass through unchanged. Opus silence frames may
  also go out in clear (before the first key), as receivers pass them through.

libdave calls the protocol-version-changed callback synchronously from inside a call the session already holds its
lock for; the callback therefore does not lock again. Locking there deadlocked the JS thread when a transition turned
DAVE off (`scripts/test.sh` checks that case: `--dave-reset`).

## Receiving audio

`mergeUsers` gives each remote user an audio ssrc; the worker creates an audio receive stream for it (Opus at the
payload types from `audioDecoders`, NetEq jitter buffer up to 200 packets, the user's `FrameDecryptor`, a sync group
per user). Volume, local mute, pan and deafen are applied through the stream's gain and the mixer
([03](03-audio.md)).

**Who is speaking.** Discord's client learns that from the module: the speaking callback, for every user, not only
our own. The gateway's SPEAKING message only gives a user's flags (voice, soundshare, priority), which JS passes on
with `setRemoteUserSpeakingStatus`. `UpdateRemoteSpeaking` (worker, every 50 ms) reads each receive stream's decoded
audio level; a user speaks while it is above about -40 dBFS and 300 ms after, and the callback gets their flags (voice
if the gateway gave none). Without it nobody would light up although everybody is heard.

## Bandwidth estimation

The `Call`'s bitrate configuration:

| | min | start | max |
|---|---|---|---|
| call (`default`) | 16 kbit/s | 700 kbit/s | 10 Mbit/s |
| stream (`stream`) | 16 kbit/s | 2.5 Mbit/s | 50 Mbit/s |

A stream starts higher because starting at a call's 700 kbit/s made the first half minute of every stream blurred
and downscaled while the estimate crept up. libwebrtc's initial probes go to 3× and 6× the start bitrate, so a
stream checks 7.5 and 15 Mbit/s right away. The ceiling for a stream is the encoder's and the SFU's, not the call's.

The field trial `WebRTC-Bwe-LossBasedBweV2` is on: with the older loss-based controller every lossy report from the
SFU halved the estimate, and it collapsed to the floor on a slightly lossy line.

The SFU also limits what we may send with **REMB** messages. They are logged when they change (`SFU REMB (stream):
4200 kbit/s`): when a stream will not go above some bitrate, this line tells whether the limit is ours or the
server's. Observed so far: 4200 kbit/s for a stream. Vesktop also declares the user's video bitrate in the gateway's
VIDEO message (`max_bitrate`), but whether the server then raises its REMB has not been verified.

## Stats

`getStats` / `getFilteredStats(filter)` (`connection_stats.cc`) build the object Discord's JS reads (its debug panel among others): transport (bytes, packets, RTT from the pings,
decryption and routing failures), outbound audio and video (per substream: resolution, frames, bitrate,
retransmissions, losses, NACK / PLI counts, encoder name, limitation reasons, DAVE encryption counters), inbound
audio and video per user (jitter buffer, concealment, decoded frames, DAVE decryption counters). The fields come
from libwebrtc's stream and call stats.

## Region latency test

`rankRtcRegions` (`transport/region_tester.cc`) pings every candidate region's addresses with the same 8-byte ping
on a thread of its own (the answers are recognised as above) and returns the regions ordered by round trip time.
Discord's JS uses it to pick a voice region.
