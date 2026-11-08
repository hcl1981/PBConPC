#!/bin/bash
# launch.sh BASE.flash PAGE OUTDIR [SECONDS] -- start the first game of one system in pbcemu.uf2
#
# Copies BASE.flash (pbcemu.uf2 already started once, games copied in with tools/pbc-addgames),
# boots the launcher, steps the carousel to PAGE (1 = Favorites, 2 = Atari 2600 ... as the
# "n / 18" counter shows), presses B twice (open the list, play the first game) and takes a
# screenshot every 5 s for SECONDS (default 40) into OUTDIR as shot_NN.png, plus run.log.
# Extra emulator options in $EXTRA, such as EXTRA="--wav out.wav".
# The launcher starts on page 1 only while its settings are fresh, hence the copy.
set -e
base=$1 page=$2 out=$3 secs=${4:-40}
here=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$out"
cp "$base" "$out/flash.img"
{
    echo "wait 3000"
    for ((i = 1; i < page; i++)); do echo "key right"; echo "wait 400"; done
    echo "wait 600"
    echo "screenshot $out/shot_00.png"
    echo "key d"; echo "wait 1200"; echo "key d"
    for ((t = 1; t <= secs / 5; t++)); do echo "wait 5000"; printf 'screenshot %s/shot_%02d.png\n' "$out" "$t"; done
    echo "quit"
} > "$out/run.pcs"
"$here/build/pbconpc" --headless --trace $EXTRA --flash "$out/flash.img" --script "$out/run.pcs" \
    --timeout $((secs + 60)) > "$out/run.log" 2>&1 || true
grep -v '^\[picocalc-sim\]   r\|screenshot:' "$out/run.log" | tail -8
