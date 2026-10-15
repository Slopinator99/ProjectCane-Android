#!/usr/bin/env bash
# Cross-build a minimal LGPL FFmpeg for Android arm64-v8a into android/third_party/ffmpeg.
# The game only decodes the extracted cutscenes: MPEG-2 video (.m2v) + PCM WAV.
# Env: ANDROID_NDK_HOME (required), FFMPEG_TAG (default n7.1), API (default 29)
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME}"
tag="${FFMPEG_TAG:-n7.1}"; api="${API:-29}"
prefix="$root/android/third_party/ffmpeg"
work="${TMPDIR:-/tmp}/ffmpeg-android"
host="$(uname -s)"
case "$host" in
  Linux*)   prebuilt=linux-x86_64 ;;
  Darwin*)  prebuilt=darwin-x86_64 ;;
  MINGW*|MSYS*|CYGWIN*) prebuilt=windows-x86_64 ;;
  *) echo "unsupported host: $host" >&2; exit 1 ;;
esac
tc="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$prebuilt"
extra=()
# MSYS2 bash may not resolve bare "gcc" inside configure's sanitized PATH.
if [ "$prebuilt" = windows-x86_64 ]; then extra+=(--host-cc=/usr/bin/gcc); fi

if [ -f "$prefix/lib/arm64-v8a/libavcodec.so" ]; then echo "FFmpeg already built"; exit 0; fi
rm -rf "$work"; git clone --depth 1 --branch "$tag" https://github.com/FFmpeg/FFmpeg.git "$work/src"
cd "$work/src"
./configure --prefix="$work/install" \
  --enable-cross-compile --target-os=android --arch=aarch64 \
  --cc="$tc/bin/aarch64-linux-android${api}-clang" --cxx="$tc/bin/aarch64-linux-android${api}-clang++" \
  --ar="$tc/bin/llvm-ar" --nm="$tc/bin/llvm-nm" --ranlib="$tc/bin/llvm-ranlib" --strip="$tc/bin/llvm-strip" \
  --enable-shared --disable-static --enable-pic \
  --disable-everything --disable-programs --disable-doc --disable-avdevice --disable-avfilter \
  --disable-postproc --disable-network --disable-autodetect \
  --enable-swscale \
  --enable-decoder=mpeg2video --enable-parser=mpegvideo --enable-demuxer=mpegvideo \
  --enable-decoder=pcm_s16le --enable-decoder=pcm_s16be --enable-demuxer=wav \
  --enable-protocol=file "${extra[@]}"
make -j"$(nproc)" && make install
mkdir -p "$prefix/include" "$prefix/lib/arm64-v8a"
cp -r "$work/install/include/." "$prefix/include/"
# Android needs unversioned sonames: libavcodec.so, not libavcodec.so.61
for l in avcodec avformat avutil swscale; do cp -L "$work/install/lib/lib$l.so" "$prefix/lib/arm64-v8a/lib$l.so"; done
echo "FFmpeg installed to $prefix"
