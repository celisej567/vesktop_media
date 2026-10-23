#!/bin/sh
# Builds the noise suppression libraries vesktop_media ships next to its .node: RNNoise (C, BSD) as
# libvesktop_rnnoise.so and DeepFilterNet (Rust, MIT / Apache-2.0) as libvesktop_deepfilter.so. They are loaded from
# the module's own directory only, never from the system (see audio/rnnoise.cc).
# RNNoise is linked static into one library with -Bsymbolic and a version script, like FFmpeg (build-ffmpeg.sh), so
# its internal symbols cannot clash with the host's. DeepFilterNet is Rust's cdylib, which exports only its C API.
#
# Sources and build go to third_party/denoise; the libraries to third_party/denoise/prefix/lib.
# Needs: cc, make, git, curl, autoconf, automake, libtool, cargo (Rust 1.70 or newer).
set -e
cd "$(dirname "$0")/.."
ROOT=$PWD
TP=$ROOT/third_party/denoise
PREFIX=$TP/prefix
JOBS=$(nproc 2>/dev/null || echo 4)

RNNOISE_TAG=v0.2
DEEPFILTER_TAG=v0.5.6

# our changes to DeepFilterNet's C API (scripts/patches): a library built from other ones is built again
DF_PATCH=$ROOT/scripts/patches/deepfilter-capi.patch
DF_STAMP="deepfilter $DEEPFILTER_TAG $(cksum < "$DF_PATCH")"
if [ -f "$PREFIX/lib/libvesktop_deepfilter.so" ] && [ "$(cat "$PREFIX/lib/.deepfilter-stamp" 2>/dev/null)" != "$DF_STAMP" ]; then
    rm -f "$PREFIX/lib/libvesktop_deepfilter.so"
fi

if [ -f "$PREFIX/lib/libvesktop_rnnoise.so" ] && [ -f "$PREFIX/lib/libvesktop_deepfilter.so" ]; then
    echo "== denoise libraries already built: $PREFIX/lib"
    exit 0
fi

# what a build needs, said up front (a missing autoreconf would only show as "configure not found" later)
missing=
for tool in cc make git curl cksum autoconf automake libtoolize cargo rustc; do
    command -v "$tool" > /dev/null 2>&1 || missing="$missing $tool"
done
if [ -n "$missing" ]; then
    echo "build-denoise.sh: missing:$missing (RNNoise needs autoconf, automake and libtool; DeepFilterNet needs cargo)" >&2
    exit 1
fi
mkdir -p "$TP/src" "$TP/build" "$PREFIX/lib"

fetch() { # dir url tag
    [ -d "$TP/src/$1/.git" ] && return
    git clone -q --depth 1 --branch "$3" "$2" "$TP/src/$1"
}

if [ ! -f "$PREFIX/lib/libvesktop_rnnoise.so" ]; then
    echo "== RNNoise $RNNOISE_TAG"
    fetch rnnoise https://github.com/xiph/rnnoise.git $RNNOISE_TAG
    RN=$TP/src/rnnoise
    # autogen.sh downloads the model with wget; fetch it with curl to the name it looks for
    hash=$(cat "$RN/model_version")
    [ -f "$RN/rnnoise_data-$hash.tar.gz" ] || curl -fsSL -o "$RN/rnnoise_data-$hash.tar.gz" "https://media.xiph.org/rnnoise/models/rnnoise_data-$hash.tar.gz"
    (cd "$RN" && sh ./autogen.sh > /dev/null 2>&1)
    # configured elsewhere (the project moved): start over
    if [ -f "$TP/build/rnnoise/config.log" ] && ! grep -q "^  \$ $RN/configure" "$TP/build/rnnoise/config.log"; then
        rm -rf "$TP/build/rnnoise"
    fi
    mkdir -p "$TP/build/rnnoise"
    (cd "$TP/build/rnnoise" && CFLAGS="-O2 -fPIC -ffile-prefix-map=$TP/=" "$RN/configure" --disable-shared --enable-static \
        --disable-examples --disable-doc --enable-x86-rtcd > /dev/null && make -j"$JOBS" > /dev/null && make install DESTDIR="$TP/build/rnnoise-root" > /dev/null)
    cat > "$TP/build/rnnoise.map" << 'MAP'
{ global: rnnoise_*; local: *; };
MAP
    ${CC:-cc} -shared -o "$PREFIX/lib/libvesktop_rnnoise.so" -Wl,-soname,libvesktop_rnnoise.so \
        -Wl,--whole-archive "$(find "$TP/build/rnnoise-root" -name librnnoise.a | head -1)" -Wl,--no-whole-archive \
        -Wl,-Bsymbolic -Wl,--version-script="$TP/build/rnnoise.map" -Wl,-z,noexecstack -s -lm
fi

if [ ! -f "$PREFIX/lib/libvesktop_deepfilter.so" ]; then
    echo "== DeepFilterNet $DEEPFILTER_TAG"
    fetch DeepFilterNet https://github.com/Rikorose/DeepFilterNet.git $DEEPFILTER_TAG
    # The C API (libDF/src/capi.rs), patched: df_create(NULL) loads the model built into the library (no separate
    # model file to ship), and a Rust panic in df_create / df_process_frame / df_free returns NULL / NaN instead of
    # unwinding into vesktop_media (which aborts the whole process)
    if ! git -C "$TP/src/DeepFilterNet" apply --reverse --check "$DF_PATCH" 2> /dev/null; then
        git -C "$TP/src/DeepFilterNet" checkout -q -- libDF/src/capi.rs
        git -C "$TP/src/DeepFilterNet" apply "$DF_PATCH"
    fi
    # no source or cargo paths of this machine in the library; the C API only (no vorbis / flac / hdf5). The crate's own
    # library kind is rlib only (its C API is meant for cargo-c), so the cdylib is asked for here
    export RUSTFLAGS="--remap-path-prefix=$TP/src/DeepFilterNet=deepfilter --remap-path-prefix=${CARGO_HOME:-$HOME/.cargo}=cargo --remap-path-prefix=$(rustc --print sysroot)=rust -C strip=symbols -C link-arg=-Wl,-soname,libvesktop_deepfilter.so"
    export CARGO_TARGET_DIR=$TP/build/deepfilter
    # the locked `time` 0.3.x does not compile with Rust 1.80 or newer (type inference): the first release that does
    (cd "$TP/src/DeepFilterNet" && cargo update -q -p time --precise 0.3.36 && cargo rustc --release --locked -p deep_filter --lib --crate-type cdylib --no-default-features --features capi)
    cp -L "$CARGO_TARGET_DIR/release/libdf.so" "$PREFIX/lib/libvesktop_deepfilter.so"
    echo "$DF_STAMP" > "$PREFIX/lib/.deepfilter-stamp"
fi
ls -la "$PREFIX/lib"
echo "== done: $PREFIX/lib"
