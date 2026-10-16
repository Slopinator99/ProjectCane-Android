#!/usr/bin/env bash
# Fetch third-party inputs for the Android build into android/third_party/.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tp="$root/third_party"; mkdir -p "$tp"

if [ ! -d "$tp/SDL2" ]; then
  git clone --depth 1 --branch release-2.30.11 https://github.com/libsdl-org/SDL.git "$tp/SDL2"
fi

# Our fixes to the SDL sources (idempotent): the USB/Bluetooth controller driver must register its receivers with an export
# flag on Android 13+, otherwise it fails to start on every launch.
for p in "$root"/patches/*.patch; do
  if git -C "$tp/SDL2" apply --check "$p" 2>/dev/null; then
    git -C "$tp/SDL2" apply "$p"
    echo "applied $(basename "$p")"
  fi
done

cat <<MSG
SDL2 is ready at $tp/SDL2.

FFmpeg must be cross-compiled for arm64-v8a (LGPL, shared, decoders only) into:
  $tp/ffmpeg/include        (libavcodec/ libavformat/ libavutil/ libswscale/)
  $tp/ffmpeg/lib/arm64-v8a  (libavcodec.so libavformat.so libavutil.so libswscale.so)
See android/README.md ("FFmpeg") for the configure line.
MSG
