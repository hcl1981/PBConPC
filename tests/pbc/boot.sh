#!/bin/bash
# boot.sh OUTDIR SECONDS FIRMWARE.uf2 [MORE.uf2 ...] -- run firmware from erased flash
#
# Writes the UF2s into a fresh flash image (as dropping them on the BOOTSEL drive one after the
# other), boots, takes shot_NN.png every 3 s of emulated time and presses the joystick centre at
# 9 s and button A at 15 s, to get past title screens. Extra emulator options in $EXTRA.
set -e
out=$1 secs=$2
shift 2
here=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$out"
rm -f "$out/flash.img" "$out"/shot_*.png
"$here/build/uf2flash" "$out/flash.img" "$@" > "$out/uf2flash.log"
{
    for ((t = 1; t <= secs / 3; t++)); do
        echo "wait 3000"
        printf 'screenshot %s/shot_%02d.png\n' "$out" "$t"
        [ $t -eq 3 ] && echo "key a"
        [ $t -eq 5 ] && echo "key s"
    done
    echo "quit"
} > "$out/run.pcs"
"$here/build/pbconpc" --headless --trace $EXTRA --flash "$out/flash.img" --script "$out/run.pcs" \
    --timeout $((secs * 4 + 60)) > "$out/run.log" 2>&1 || true
grep -v '^\[picocalc-sim\]   r\|screenshot:' "$out/run.log" | tail -6
