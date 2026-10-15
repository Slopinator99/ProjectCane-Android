#!/usr/bin/env bash
# Fast portability checks that need no Android NDK. Run locally or in CI.
#   scripts/ci/check-host.sh            shader + aarch64 checks
#   scripts/ci/check-host.sh --full     also a full host build with -DCANE_GLES=ON
# Packages (Ubuntu): clang cmake ninja libsdl2-dev libgles-dev libegl-dev libglm-dev
#   libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libfaudio-dev g++-aarch64-linux-gnu
#   libgl1-mesa-dri libegl-mesa0 mesa-libgallium
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${CANE_CI_OUT:-$root/build-ci}"; mkdir -p "$out/generated"
fail=0

echo "== 1/3 embed shaders"
cmake -DSOURCE="$root/Sly1-Release" -DOUTPUT="$out/generated/embedded_shaders.h" -P "$root/cmake/embed-shaders.cmake"

echo "== 2/3 shaders in an OpenGL ES 3.2 context (Mesa llvmpipe)"
${CXX:-clang++} -std=c++17 "$root/tools/shadertest.cpp" -I"$out/generated" $(sdl2-config --cflags --libs) -lGLESv2 -o "$out/shadertest"
SDL_VIDEODRIVER=offscreen "$out/shadertest" || { echo "SHADER CHECK FAILED"; fail=1; }

echo "== 3/3 AArch64 syntax check of all game sources"
pch=""; for h in platform/platform.h Sly1-Release/{sw,save,freeze,act,wr,shadow,fader,splicemap,gc}.h; do pch="$pch -include $root/$h"; done
errs="$out/aarch64-errors.txt"; : > "$errs"
ls "$root"/Sly1-Release/*.cpp | grep -v 'spu_reverb_xapo' | xargs -P "$(nproc)" -I{} sh -c \
  "aarch64-linux-gnu-g++ -std=c++20 -fsyntax-only -w -DCANE_GLES -DCANE_PORTABLE_SHADERS \
   -include $root/platform/gles_compat.h $pch \
   -I$root/platform/android -I$root/Libraries/include -I$root/platform -I$root/Sly1-Release \
   -I/usr/include/SDL2 -I$out/generated -idirafter /usr/include -idirafter /usr/include/x86_64-linux-gnu -DIMGUI_DEFINE_MATH_OPERATORS {} 2>&1 | grep -E 'error' | head -3 >> $errs || true"
if [ -s "$errs" ]; then echo "AARCH64 CHECK FAILED:"; head -30 "$errs"; fail=1; else echo "ok"; fi

if [ "${1:-}" = "--full" ]; then
  echo "== full host build (CANE_GLES)"
  cmake -G Ninja -S "$root" -B "$out/host" -DCANE_GLES=ON -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ${FAUDIO_SRC:+-DFETCHCONTENT_SOURCE_DIR_FAUDIO=$FAUDIO_SRC}
  ninja -C "$out/host" || fail=1
fi
exit $fail
