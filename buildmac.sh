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
    -DALPHAFIX_VERSION="${ALPHAFIX_VERSION:-$(python3 "$ROOT/scripts/version.py")}" \
    -DALPHAFIX_CODESIGN_IDENTITY="${CODESIGN_IDENTITY:--}"
cmake --build "$BUILD" --parallel "$(sysctl -n hw.ncpu)"
ctest --test-dir "$BUILD" --output-on-failure
# Keep third-party notices inside the final bundle before its final signature.
if [[ -d "$FFMPEG_PATH/licenses" ]]; then
    mkdir -p "$BUILD/Alpha_Fix.plugin/Contents/Resources/licenses"
    ditto "$FFMPEG_PATH/licenses" "$BUILD/Alpha_Fix.plugin/Contents/Resources/licenses"
    codesign --force --sign "${CODESIGN_IDENTITY:--}" "$BUILD/Alpha_Fix.plugin"
fi
codesign --verify --deep --strict "$BUILD/Alpha_Fix.plugin"
lipo "$BUILD/Alpha_Fix.plugin/Contents/MacOS/Alpha_Fix" -verify_arch "$ARCH"
for lib in "$BUILD/Alpha_Fix.plugin/Contents/Frameworks/"*.dylib; do
    lipo "$lib" -verify_arch "$ARCH"
done
if [[ "$INSTALL" == 1 ]]; then
    # Elevation is only needed when copying into /Applications.
    sudo cmake --install "$BUILD" --config "$CONFIG"
fi
printf '\nBuilt: %s/Alpha_Fix.plugin\n' "$BUILD"
