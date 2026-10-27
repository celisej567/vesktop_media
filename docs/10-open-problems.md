# 10. Open problems

What is not solved or not verified yet. The reasons behind the fixes already made are on the page of each part.

* **Bitrate above about 4.2 Mbit/s for a stream.** The SFU's REMB was observed at 4200 kbit/s. Vesktop now also
  declares the user's bitrate in the VIDEO message (`max_bitrate`); whether the server then allows more is
  unverified.
* **Voice sometimes missing in a call.** The 10-second `audio packets` line tells whether the server sends nothing
  (`udp in: 0 rtp`) or the module drops what arrives; the cause is not known yet.
* **Stream settings change.** Chromium's GPU process logs `SharedImageManager::ProduceSkia: ... non-existent mailbox`
  once in a while (seen after changing a stream's settings): a frame released while it was being drawn. Not looked
  into further; nothing visible was reported.
* **System FFmpeg fallback.** Without `libvesktop_ffmpeg.so` the encoders fall back to a system FFmpeg loaded with
  `RTLD_DEEPBIND`, which mixes allocators with x264 ([02](02-build.md)). It is reached only when the bundled library is missing.
* **glibc.** A build runs only with a glibc at least as new as the build machine's. A build in an older container has
  not been set up.
* **Not implemented:** macOS backends (Windows: [11](11-windows.md), with what is not verified there), AV1 / VP9 encoding, AMD / Intel hardware encoders, Krisp.
