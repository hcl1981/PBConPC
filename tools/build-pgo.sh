#!/bin/sh
# build-pgo.sh [PBCEMU.uf2] -- the fastest PBConPC for this computer: builds once with profiling,
# runs the launcher and a game for a minute, then builds again with what the run measured
# (profile-guided optimisation, about 4 % faster than the normal build). Result: build-pgo/pbconpc.
# The game: the first Mega Drive or NES game on the drive of ~/.local/share/pbconpc/pbcplus.flash.
set -e
cd "$(dirname "$0")/.."
src=$PWD
data=$src/build-pgo/profile
rm -rf "$data"
mkdir -p "$data"
gen="-march=native -fprofile-generate -fprofile-update=atomic -fprofile-dir=$data"
cmake -S . -B build-pgo -DCMAKE_BUILD_TYPE=Release -DPBCONPC_NATIVE=ON \
    -DCMAKE_C_FLAGS="$gen" -DCMAKE_CXX_FLAGS="$gen" -DCMAKE_EXE_LINKER_FLAGS="-fprofile-generate" >/dev/null
cmake --build build-pgo -j"$(nproc)" --target pbconpc >/dev/null
flash=$(mktemp)
cp "$HOME/.local/share/pbconpc/pbcplus.flash" "$flash"
script=$(mktemp)
# launcher, then the Mega Drive page (14) and its first game, which runs for 40 s
{
    echo "wait 3000"
    for i in $(seq 1 13); do echo "key right"; echo "wait 400"; done
    echo "key d"; echo "wait 1200"; echo "key d"; echo "wait 40000"; echo "quit"
} > "$script"
build-pgo/pbconpc --headless --unthrottled --flash "$flash" --script "$script" --timeout 300 >/dev/null 2>&1 || true
rm -f "$flash" "$script"
use="-march=native -fprofile-use -fprofile-partial-training -fprofile-dir=$data -Wno-missing-profile"
cmake -S . -B build-pgo -DCMAKE_C_FLAGS="$use" -DCMAKE_CXX_FLAGS="$use" -DCMAKE_EXE_LINKER_FLAGS="" >/dev/null
cmake --build build-pgo -j"$(nproc)" --target pbconpc 2>&1 | grep -v 'profile\|coverage' || true
echo "done: build-pgo/pbconpc"
