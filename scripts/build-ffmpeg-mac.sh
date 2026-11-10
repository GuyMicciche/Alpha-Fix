#!/usr/bin/env bash
# Build only the FFmpeg components used by AlphaFix. No Homebrew runtime deps.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION=8.1
ARCH="${ARCH:-$(uname -m)}"
case "$ARCH" in arm64|x86_64) ;; *) echo "Unsupported ARCH: $ARCH" >&2; exit 1 ;; esac
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-12.0}"
# Calling the compiler inside Xcode directly does not select the macOS SDK.
# Give FFmpeg an explicit sysroot for its compiler checks and the library build.
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
CC_MAC="$(xcrun --sdk macosx --find clang)"
[[ -d "$SDKROOT/usr/lib" ]] || { echo "Invalid macOS SDK: $SDKROOT" >&2; exit 1; }
printf 'FFmpeg target: %s, macOS %s\nCompiler: %s\nSDK: %s\n' \
    "$ARCH" "$MACOSX_DEPLOYMENT_TARGET" "$CC_MAC" "$SDKROOT"
PREFIX="$ROOT/third_party/ffmpeg-mac-$ARCH"
WORK="$ROOT/Mac/ffmpeg-$VERSION-$ARCH"
mkdir -p "$WORK"
cd "$WORK"
if [[ ! -f ffmpeg-$VERSION.tar.xz ]]; then
    curl --fail --location --retry 3 "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz" -o "ffmpeg-$VERSION.tar.xz"
fi
if [[ ! -d ffmpeg-$VERSION ]]; then
    tar -xf "ffmpeg-$VERSION.tar.xz"
fi
cd "ffmpeg-$VERSION"
if ! ./configure --prefix="$PREFIX" --arch="$ARCH" --target-os=darwin \
    --sysroot="$SDKROOT" --cc="$CC_MAC" --enable-shared --disable-static \
    --install-name-dir=@rpath --disable-autodetect --disable-programs \
    --disable-doc --disable-debug --disable-x86asm --disable-everything \
    --disable-avdevice --disable-avfilter --disable-network \
    --enable-decoder=prores --enable-demuxer=mov --enable-protocol=file \
    --enable-swscale --enable-swresample \
    --extra-cflags="-arch $ARCH -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" \
    --extra-ldflags="-arch $ARCH -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -Wl,-headerpad_max_install_names"
then
    echo 'FFmpeg configure failed. Compiler diagnostics from ffbuild/config.log:' >&2
    if [[ -f ffbuild/config.log ]]; then
        tail -n 160 ffbuild/config.log >&2
    fi
    exit 1
fi
make -j "$(sysctl -n hw.ncpu)"
make install
mkdir -p "$PREFIX/licenses"
cp COPYING.LGPLv2.1 LICENSE.md "$PREFIX/licenses/"
printf '%s\n' "FFmpeg $VERSION: https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz" > "$PREFIX/licenses/SOURCE.txt"
cp ffbuild/config.mak "$PREFIX/licenses/ffmpeg-config.mak"
