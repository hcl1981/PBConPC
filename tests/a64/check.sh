#!/bin/sh
# Checks the ARM64 translator (emu/jit_a64.inc) without an ARM computer: builds PBConPC without a
# window for ARM64 with the Android NDK, statically linked, runs it under qemu-aarch64 and compares
# the state digest of a run of each UF2 given with that of the interpreter.
#
#   tests/a64/check.sh [-t MS] FIRMWARE.uf2 ...
#
# Needs qemu-aarch64 (qemu-user) and the NDK ($ANDROID_NDK, default the newest in ~/Android/Sdk/ndk).
set -e
ms=2000
if [ "$1" = "-t" ]; then ms=$2; shift 2; fi
src=$(cd "$(dirname "$0")/../.." && pwd)
ndk=${ANDROID_NDK:-$(ls -d "$HOME"/Android/Sdk/ndk/* | tail -n 1)}
out=${TMPDIR:-/tmp}/pbconpc-a64-check
cmake -S "$src" -B "$out" -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 -DPICOCALC_SIM_SDL=OFF -DCMAKE_EXE_LINKER_FLAGS=-static >/dev/null
cmake --build "$out" -j --target pbconpc >/dev/null
digest() { grep -o 'state digest.*' | cut -c14-; }
bad=0
for f in "$@"; do
    i=$(PICOCALC_SIM_NO_JIT=1 PICOCALC_SIM_STOP_AT_MS=$ms HOME=$out qemu-aarch64 "$out/pbconpc" --headless --flash none \
        --unthrottled --trace "$f" 2>&1 | digest)
    a=$(PICOCALC_SIM_STOP_AT_MS=$ms HOME=$out qemu-aarch64 "$out/pbconpc" --headless --flash none \
        --unthrottled --trace "$f" 2>&1 | digest)
    if [ -n "$a" ] && [ "$a" = "$i" ]; then
        echo "ok       $(basename "$f")  $a"
    else
        echo "DIFFERS  $(basename "$f")  native $a  interpreter $i"
        bad=1
    fi
done
exit $bad
