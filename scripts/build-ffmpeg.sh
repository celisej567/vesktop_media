#!/bin/sh
# Builds the FFmpeg vesktop_media ships next to its .node, so H.264 / H.265 do not depend on the system's FFmpeg:
# decoders h264 + hevc, encoders libx264, libx265 (static, CPU) and h264_nvenc / hevc_nvenc (NVIDIA, the driver's
# libnvidia-encode is loaded at runtime). One shared library, libvesktop_ffmpeg.so: static libavcodec + libavutil +
# x264 + x265 linked with -Bsymbolic, so FFmpeg's calls stay inside it (no clash with Electron's libffmpeg, which
# exports the same av_* names) while malloc / free are the process' (Electron's allocator) for everything. Loading
# FFmpeg with RTLD_DEEPBIND instead mixed allocators: libc's strdup (x264's parameters) went through Electron's
# malloc and the freeing through libc's free, which crashed.
# x264 / x265 are GPL: this FFmpeg is GPL (as Vesktop).
#
# Sources and build go to third_party/ffmpeg; the libraries to third_party/ffmpeg/prefix/lib.
# Needs: cc, c++, make, cmake, nasm, curl, git, pkg-config.
set -e
cd "$(dirname "$0")/.."
ROOT=$PWD
TP=$ROOT/third_party/ffmpeg
PREFIX=$TP/prefix
JOBS=$(nproc 2>/dev/null || echo 4)

FFMPEG_VERSION=7.1.1
X264_BRANCH=stable
X265_VERSION=4.1
NVHEADERS_TAG=n12.1.14.0 # NVENC SDK 12.1: drivers 530 and newer

if [ -f "$PREFIX/lib/libvesktop_ffmpeg.so" ]; then
    echo "== FFmpeg already built: $PREFIX/lib"
    exit 0
fi
mkdir -p "$TP/src" "$PREFIX"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"

echo "== nv-codec-headers $NVHEADERS_TAG"
if [ ! -d "$TP/src/nv-codec-headers" ]; then
    git clone -q --depth 1 --branch $NVHEADERS_TAG https://git.videolan.org/git/ffmpeg/nv-codec-headers.git "$TP/src/nv-codec-headers"
fi
make -C "$TP/src/nv-codec-headers" PREFIX="$PREFIX" install > /dev/null

echo "== x264"
if [ ! -d "$TP/src/x264" ]; then
    git clone -q --depth 1 --branch $X264_BRANCH https://code.videolan.org/videolan/x264.git "$TP/src/x264"
fi
(cd "$TP/src/x264" && ./configure --prefix="$PREFIX" --enable-static --enable-pic --disable-cli --disable-opencl \
    --disable-lavf --disable-swscale --disable-ffms --disable-gpac --disable-lsmash --bit-depth=8 --chroma-format=420 > /dev/null &&
    # no memalign: x264 asks it for 2 MB (huge page) alignment, which Chromium's allocator (Electron's malloc)
    # refuses with a trap. Without malloc.h x264 aligns plain malloc blocks itself
    sed -i 's/#define HAVE_MALLOC_H 1/#define HAVE_MALLOC_H 0/' config.h &&
    make -j"$JOBS" > /dev/null && make install > /dev/null)

echo "== x265 $X265_VERSION"
if [ ! -d "$TP/src/x265_$X265_VERSION" ]; then
    curl -fsSL "https://bitbucket.org/multicoreware/x265_git/downloads/x265_$X265_VERSION.tar.gz" | tar xz -C "$TP/src"
fi
# CMake 4 refuses policies set to OLD; these two only matter for Apple Clang / old if() quoting
sed -i -e 's/cmake_policy(SET CMP0025 OLD)/# &/' -e 's/cmake_policy(SET CMP0054 OLD)/# &/' \
    "$TP/src/x265_$X265_VERSION/source/CMakeLists.txt"
# configured elsewhere (the project moved): start over, CMake keeps absolute paths
if [ -f "$TP/build/x265/CMakeCache.txt" ] && ! grep -q "^CMAKE_HOME_DIRECTORY:INTERNAL=$TP/src/x265_$X265_VERSION/source\$" "$TP/build/x265/CMakeCache.txt"; then
    rm -rf "$TP/build/x265"
fi
cmake -S "$TP/src/x265_$X265_VERSION/source" -B "$TP/build/x265" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DENABLE_SHARED=OFF -DENABLE_CLI=OFF -DENABLE_LIBNUMA=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -Wno-dev > /dev/null
cmake --build "$TP/build/x265" -j"$JOBS" > /dev/null
cmake --install "$TP/build/x265" > /dev/null

echo "== FFmpeg $FFMPEG_VERSION"
if [ ! -d "$TP/src/ffmpeg-$FFMPEG_VERSION" ]; then
    curl -fsSL "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz" | tar xJ -C "$TP/src"
fi
# configured elsewhere (the project moved): the include paths pkg-config gave it are stale
if [ -f "$TP/build/ffmpeg/ffbuild/config.mak" ] && ! grep -q "$PREFIX/include" "$TP/build/ffmpeg/ffbuild/config.mak"; then
    rm -rf "$TP/build/ffmpeg"
fi
mkdir -p "$TP/build/ffmpeg" "$TP/build/bin"
# a compiler wrapper by name, so the configure line kept in the library names no path; it maps the source
# directory out of __FILE__ (FFmpeg's assertions)
printf '#!/bin/sh\nexec %s -ffile-prefix-map="%s/"= "$@"\n' "${CC:-cc}" "$TP" > "$TP/build/bin/vm-cc"
chmod +x "$TP/build/bin/vm-cc"
# configure by a relative path and installed through DESTDIR: FFmpeg keeps its configure line (and source paths in
# assertions) in the library, which then names no directory of the machine it was built on. x264, x265 and the
# NVENC headers come through pkg-config
(cd "$TP/build/ffmpeg" && PATH="$TP/build/bin:$PATH" "../../src/ffmpeg-$FFMPEG_VERSION/configure" --prefix=/prefix --build-suffix=_vm --cc=vm-cc \
    --disable-shared --enable-static --enable-pic --enable-gpl \
    --disable-programs --disable-doc --disable-network --disable-autodetect \
    --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-swresample \
    --disable-everything \
    --enable-decoder=h264,hevc --enable-parser=h264,hevc \
    --enable-libx264 --enable-libx265 --enable-encoder=libx264,libx265 \
    --enable-ffnvcodec --enable-nvenc --enable-encoder=h264_nvenc,hevc_nvenc \
    --pkg-config-flags=--static > /dev/null && export PATH="$TP/build/bin:$PATH" &&
    make -j"$JOBS" > /dev/null && make install DESTDIR="$TP" > /dev/null)

echo "== libvesktop_ffmpeg.so"
cat > "$TP/build/exports.map" << 'MAP'
{ global: av_*; avcodec_*; avutil_*; local: *; };
MAP
${CC:-cc} -shared -o "$PREFIX/lib/libvesktop_ffmpeg.so" -Wl,-soname,libvesktop_ffmpeg.so \
    -Wl,--whole-archive "$PREFIX/lib/libavcodec_vm.a" "$PREFIX/lib/libavutil_vm.a" -Wl,--no-whole-archive \
    "$PREFIX/lib/libx264.a" "$PREFIX/lib/libx265.a" \
    -Wl,-Bsymbolic -Wl,--version-script="$TP/build/exports.map" -Wl,-z,noexecstack -s \
    -lstdc++ -lm -lpthread -ldl
ls -la "$PREFIX/lib/libvesktop_ffmpeg.so"
echo "== done: $PREFIX/lib"
