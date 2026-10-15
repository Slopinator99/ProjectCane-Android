#!/usr/bin/env bash
# Fetch third-party inputs for the Android build into android/third_party/.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tp="$root/third_party"; mkdir -p "$tp"

if [ ! -d "$tp/SDL2" ]; then
  git clone --depth 1 --branch release-2.30.11 https://github.com/libsdl-org/SDL.git "$tp/SDL2"
fi

cat <<MSG
SDL2 is ready at $tp/SDL2.

FFmpeg must be cross-compiled for arm64-v8a (LGPL, shared, decoders only) into:
  $tp/ffmpeg/include        (libavcodec/ libavformat/ libavutil/ libswscale/)
  $tp/ffmpeg/lib/arm64-v8a  (libavcodec.so libavformat.so libavutil.so libswscale.so)
See android/README.md ("FFmpeg") for the configure line.
MSG
