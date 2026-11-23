#!/bin/sh
# Checks the RISC-V core (emu/riscv.c) and its translator (emu/rvjit.inc): builds itest.c for the
# RP2350's Hazard3 cores, boots it in PBConPC through Raspberry Pi's ROM (which switches the chip to
# RISC-V) and looks at what it prints: every integer, bit-manipulation, atomic and custom instruction
# against a model written in C, then load-reserved/store-conditional and the order nested and pending
# interrupts are taken in. Then it runs itest and every further UF2 given with native code and with
# the interpreter alone (PBCONPC_RVJ_OFF=1) and compares the state digests, which must be equal.
#
#   tests/riscv/check.sh [-e "EMULATOR COMMAND"] [-t MS] [RISCV.uf2 ...]
#
# The emulator command defaults to build/pbconpc; "qemu-aarch64 /path/to/pbconpc" checks the ARM64
# translator with a static build as tests/a64/check.sh makes. Further UF2s run on the pbcplus board.
# Needs the Pico SDK ($PICO_SDK_PATH, default ~/pico/pico-sdk) and Raspberry Pi's RISC-V toolchain
# ($PICO_RISCV_TOOLCHAIN, default ~/pico/riscv-toolchain; riscv-toolchain-*.tar.gz from
# github.com/raspberrypi/pico-sdk-tools/releases).
set -e
here=$(cd "$(dirname "$0")" && pwd)
emu=$here/../../build/pbconpc
ms=3000
while [ "$1" = "-e" ] || [ "$1" = "-t" ]; do
    if [ "$1" = "-e" ]; then emu=$2; else ms=$2; fi
    shift 2
done
export PICO_SDK_PATH=${PICO_SDK_PATH:-$HOME/pico/pico-sdk}
tc=${PICO_RISCV_TOOLCHAIN:-$HOME/pico/riscv-toolchain}
triple=$(basename "$(ls "$tc"/bin/*-gcc | head -n 1)" -gcc)
out=${TMPDIR:-/tmp}/pbconpc-riscv-check
cmake -S "$here" -B "$out" -DPICO_TOOLCHAIN_PATH="$tc" -DPICO_GCC_TRIPLE="$triple" >/dev/null
cmake --build "$out" -j >/dev/null
log=$(PICOCALC_SIM_STOP_AT_MS=20000 HOME=$out $emu --board pico2 --headless --flash none --unthrottled --console \
    "$out/itest.uf2" 2>&1 | grep -v '^\[')
echo "$log"
for want in "checks, 0 fails" "(want 6 0 1 6)" "preempt order: 10 20 11 " "pending order: 2 10 20 11 " "itest done"; do
    case "$log" in
    *"$want"*) ;;
    *) echo "FAILED: no \"$want\""; exit 1 ;;
    esac
done
case "$log" in *"val 6 first 0 second 1 cell 6"*) ;; *) echo "FAILED: lr/sc"; exit 1 ;; esac
digest() { grep -o 'state digest.*' | cut -c14-; }
bad=0
for f in "$out/itest.uf2" "$@"; do
    board=pbcplus
    [ "$f" = "$out/itest.uf2" ] && board=pico2
    n=$(PICOCALC_SIM_STOP_AT_MS=$ms HOME=$out $emu --board $board --headless --flash none --unthrottled --trace "$f" 2>&1 | digest)
    i=$(PBCONPC_RVJ_OFF=1 PICOCALC_SIM_STOP_AT_MS=$ms HOME=$out $emu --board $board --headless --flash none --unthrottled \
        --trace "$f" 2>&1 | digest)
    if [ -n "$n" ] && [ "$n" = "$i" ]; then
        echo "ok       $(basename "$f")  $n"
    else
        echo "DIFFERS  $(basename "$f")  native $n  interpreter $i"
        bad=1
    fi
done
exit $bad
