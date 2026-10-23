#!/bin/sh
# Builds vesktop_media into build/out/{index.js,vesktop_media.node} (Vesktop's build copies it into dist/vesktop_media).
# Needs: clang, cmake, ninja, git, curl, pnpm; downloads the prebuilt libwebrtc and libc++ headers on first run.
set -e
cd "$(dirname "$0")/.."
ROOT=$PWD
TP=$ROOT/third_party
BUILD=${BUILD_DIR:-$ROOT/build}
TC=$ROOT/cmake/toolchain.cmake

WEBRTC_BUILD=m156.8078.5.1
LIBCXX_COMMIT=97b436da4c33663581d394f4ee0a5977fc38c2f4
LIBCXXABI_COMMIT=136b7ca4a0a837995584e728758a07a502dd4a08
BUILDTOOLS_COMMIT=61df5d8b18e317ea9fbf5e55d86e865388200fca
LIBDAVE_REPO=https://github.com/discord/libdave
MLSPP_COMMIT=1cc50a124a3bc4e143a787ec934280dc70c1034d

# own install, outside the Vesktop workspace (packages/vesktop_media is excluded from it)
# A CMake build directory remembers the absolute source path: after the project moved, start it over
fresh() { # build dir, source dir
    if [ -f "$1/CMakeCache.txt" ] && ! grep -q "^CMAKE_HOME_DIRECTORY:INTERNAL=$(cd "$2" && pwd)\$" "$1/CMakeCache.txt"; then
        echo "== $1 was configured elsewhere, starting it over"
        rm -rf "$1"
    fi
}

[ -d node_modules/node-addon-api ] || [ -d ../../node_modules/node-addon-api ] || pnpm install --ignore-workspace

if [ ! -f "$TP/webrtc/lib/libwebrtc.a" ]; then
    echo "== fetching prebuilt libwebrtc $WEBRTC_BUILD"
    mkdir -p "$TP/webrtc-dl"
    curl -fL -o "$TP/webrtc-dl/webrtc.tar.gz" \
        "https://github.com/shiguredo-webrtc-build/webrtc-build/releases/download/$WEBRTC_BUILD/webrtc.ubuntu-24.04_x86_64.tar.gz"
    rm -rf "$TP/webrtc-x" && mkdir -p "$TP/webrtc-x"
    tar xzf "$TP/webrtc-dl/webrtc.tar.gz" -C "$TP/webrtc-x"
    rm -rf "$TP/webrtc" && mv "$TP/webrtc-x/webrtc" "$TP/webrtc" && rmdir "$TP/webrtc-x"
fi

fetch() { # dir url commit
    [ -d "$TP/$1/.git" ] && return
    mkdir -p "$TP/$1"
    git -C "$TP/$1" init -q
    git -C "$TP/$1" fetch -q --depth 1 "$2" "$3"
    git -C "$TP/$1" checkout -q FETCH_HEAD
}
fetch libcxx https://chromium.googlesource.com/external/github.com/llvm/llvm-project/libcxx.git $LIBCXX_COMMIT
fetch libcxxabi https://chromium.googlesource.com/external/github.com/llvm/llvm-project/libcxxabi.git $LIBCXXABI_COMMIT
fetch mlspp https://github.com/cisco/mlspp $MLSPP_COMMIT
[ -d "$TP/libdave/.git" ] || git clone -q --depth 1 $LIBDAVE_REPO "$TP/libdave"

if [ ! -f "$TP/libcxx-config/include/__config_site" ]; then
    mkdir -p "$TP/libcxx-config/include"
    for f in __config_site __assertion_handler; do
        curl -fsS "https://chromium.googlesource.com/chromium/src/buildtools/+/$BUILDTOOLS_COMMIT/third_party/libc++/$f?format=TEXT" \
            | base64 -d > "$TP/libcxx-config/include/$f"
    done
fi

DEPS=$BUILD/deps
if [ ! -f "$DEPS/lib/libdave.a" ]; then
    echo "== building mlspp + libdave"
    export CXXFLAGS="-include ctime"
    fresh "$BUILD/mlspp" "$TP/mlspp"
    fresh "$BUILD/libdave" "$TP/libdave/cpp"
    cmake -S "$TP/mlspp" -B "$BUILD/mlspp" -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_BUILD_TYPE=Release \
        -DDISABLE_GREASE=ON -DMLS_CXX_NAMESPACE=mlspp -DREQUIRE_BORINGSSL=ON -DCMAKE_INSTALL_PREFIX=$DEPS > /dev/null
    cmake --build "$BUILD/mlspp" && cmake --install "$BUILD/mlspp" > /dev/null
    cmake -S "$TP/libdave/cpp" -B "$BUILD/libdave" -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=$DEPS -DPERSISTENT_KEYS=ON -DREQUIRE_BORINGSSL=ON -DCMAKE_INSTALL_PREFIX=$DEPS > /dev/null
    cmake --build "$BUILD/libdave" && cmake --install "$BUILD/libdave" > /dev/null
    unset CXXFLAGS
fi

# our own FFmpeg (H.264 / H.265 decoders, x264 / x265 / NVENC encoders), shipped next to the module
sh "$ROOT/scripts/build-ffmpeg.sh"
# RNNoise and DeepFilterNet, shipped next to the module too
sh "$ROOT/scripts/build-denoise.sh"

echo "== building vesktop_media"
fresh "$BUILD/main" .
cmake -S . -B "$BUILD/main" -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_BUILD_TYPE=${BUILD_TYPE:-Release} -DVV_DEPS_DIR=$DEPS > /dev/null
cmake --build "$BUILD/main"
mkdir -p "$BUILD/out"
# replace atomically: overwriting the .node in place crashes (SIGBUS) a Vesktop that has it loaded
for f in vesktop_media.node index.js; do
    cp "$BUILD/main/out/$f" "$BUILD/out/.$f.tmp" && mv -f "$BUILD/out/.$f.tmp" "$BUILD/out/$f"
done
FFLIB=$TP/ffmpeg/prefix/lib
for f in libvesktop_ffmpeg.so; do
    cp -L "$FFLIB/$f" "$BUILD/out/.$f.tmp" && mv -f "$BUILD/out/.$f.tmp" "$BUILD/out/$f"
done
for f in libvesktop_rnnoise.so libvesktop_deepfilter.so; do
    cp -L "$TP/denoise/prefix/lib/$f" "$BUILD/out/.$f.tmp" && mv -f "$BUILD/out/.$f.tmp" "$BUILD/out/$f"
done
echo "== done: $BUILD/out"
