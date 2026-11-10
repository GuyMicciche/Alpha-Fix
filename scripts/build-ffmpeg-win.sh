#!/usr/bin/env bash
# Run from an x64 VS 2022 Developer Command Prompt with MSYS2 usr/bin on PATH.
# Output is separate from ffmpeg-shared; this does not replace the working build.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION=8.1
WORK="${FFMPEG_WORK_DIR:-$ROOT/Win/ffmpeg-minimal}"
PREFIX="${FFMPEG_PREFIX:-$ROOT/third_party/ffmpeg-win-minimal}"
WORK="$(cygpath -am "$WORK")"
PREFIX="$(cygpath -am "$PREFIX")"
command -v cl.exe >/dev/null || { echo 'Use an x64 VS Developer Command Prompt.' >&2; exit 1; }
command -v make >/dev/null || { echo 'GNU Make is required (MSYS2: pacman -S make).' >&2; exit 1; }
mkdir -p "$WORK"
if [[ ! -f "$WORK/ffmpeg-$VERSION.tar.xz" ]]; then
    curl --fail --location --retry 3 "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz" -o "$WORK/ffmpeg-$VERSION.tar.xz"
fi
if [[ ! -d "$WORK/ffmpeg-$VERSION" ]]; then
    tar -xf "$WORK/ffmpeg-$VERSION.tar.xz" -C "$WORK"
fi
mkdir -p "$WORK/build"
cd "$WORK/build"
if ! "$WORK/ffmpeg-$VERSION/configure" \
    --prefix="$PREFIX" --toolchain=msvc --arch=x86_64 --target-os=win64 \
    --enable-shared --disable-static --disable-autodetect --disable-programs \
    --disable-doc --disable-debug --disable-x86asm --disable-everything \
    --disable-avdevice --disable-avfilter --disable-network \
    --enable-decoder=prores --enable-demuxer=mov --enable-protocol=file \
    --enable-swscale --enable-swresample; then
    tail -n 160 ffbuild/config.log >&2
    exit 1
fi
make -j "${NUMBER_OF_PROCESSORS:-4}"
make install
mkdir -p "$PREFIX/licenses"
cp "$WORK/ffmpeg-$VERSION/COPYING.LGPLv2.1" "$WORK/ffmpeg-$VERSION/LICENSE.md" "$PREFIX/licenses/"
printf '%s\n' "FFmpeg $VERSION: https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz" > "$PREFIX/licenses/SOURCE.txt"
cp ffbuild/config.mak "$PREFIX/licenses/ffmpeg-config.mak"
printf '\nMinimal Windows FFmpeg: %s\n' "$PREFIX"
