#!/usr/bin/env bash
# Usage: bash buildmac.sh [--no-install] [--arch arm64|x86_64] [--debug]
# Optional env: AE_SDK_PATH, FFMPEG_PATH, AE_BASE_DIR, ALPHAFIX_VERSION,
# MACOSX_DEPLOYMENT_TARGET, CODESIGN_IDENTITY.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[[ "$(uname -s)" == Darwin ]] || { echo 'Run this script on macOS.' >&2; exit 1; }
ARCH="$(uname -m)"
CONFIG=Release
INSTALL=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-install) INSTALL=0; shift ;;
        --arch) ARCH="${2:?Missing architecture}"; shift 2 ;;
        --debug) CONFIG=Debug; shift ;;
        -h|--help) sed -n '2,4p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done
case "$ARCH" in arm64|x86_64) ;; *) echo 'Use arm64 or x86_64.' >&2; exit 1 ;; esac
command -v cmake >/dev/null
command -v python3 >/dev/null
xcrun --find Rez >/dev/null || { echo 'Install and select full Xcode (Rez is required).' >&2; exit 1; }
SDK="${AE_SDK_PATH:-$ROOT/AfterEffectsSDK}"
if [[ ! -f "$SDK/Headers/AE_Effect.h" && -f "$SDK/AfterEffectsSDK.zip" ]]; then
    python3 "$ROOT/scripts/extract-dependency.py" "$SDK/AfterEffectsSDK.zip" "$SDK" Headers/AE_Effect.h
fi
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-12.0}"
if [[ -z "${FFMPEG_PATH:-}" ]]; then
    FFMPEG_PATH="$ROOT/third_party/ffmpeg-mac-$ARCH"
    if [[ ! -f "$FFMPEG_PATH/include/libavformat/avformat.h" ]]; then
        ARCH="$ARCH" bash "$ROOT/scripts/build-ffmpeg-mac.sh"
    fi
fi
BUILD="$ROOT/Mac/$ARCH-$CONFIG"
cmake -S "$ROOT" -B "$BUILD" -G 'Unix Makefiles' \
    -DCMAKE_BUILD_TYPE="$CONFIG" -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
    -DAE_SDK_PATH="$SDK" -DFFMPEG_PATH="$FFMPEG_PATH" \
    -DAE_BASE_DIR="${AE_BASE_DIR:-}" \
    -DALPHAFIX_VERSION="${ALPHAFIX_VERSION:-1.0.0}" \
    -DALPHAFIX_CODESIGN_IDENTITY="${CODESIGN_IDENTITY:--}"
cmake --build "$BUILD" --parallel "$(sysctl -n hw.ncpu)"
# Keep third-party notices inside the final bundle before its final signature.
if [[ -d "$FFMPEG_PATH/licenses" ]]; then
    mkdir -p "$BUILD/AlphaFix.plugin/Contents/Resources/licenses"
    ditto "$FFMPEG_PATH/licenses" "$BUILD/AlphaFix.plugin/Contents/Resources/licenses"
    codesign --force --sign "${CODESIGN_IDENTITY:--}" "$BUILD/AlphaFix.plugin"
fi
codesign --verify --deep --strict "$BUILD/AlphaFix.plugin"
lipo -verify_arch "$ARCH" "$BUILD/AlphaFix.plugin/Contents/MacOS/AlphaFix"
for lib in "$BUILD/AlphaFix.plugin/Contents/Frameworks/"*.dylib; do
    lipo -verify_arch "$ARCH" "$lib"
done
if [[ "$INSTALL" == 1 ]]; then
    # Elevation is only needed when copying into /Applications.
    sudo cmake --install "$BUILD" --config "$CONFIG"
fi
printf '\nBuilt: %s/AlphaFix.plugin\n' "$BUILD"
