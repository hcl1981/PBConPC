#pragma once

// PBConPC: a GDB remote server (gdb-multiarch, arm-none-eabi-gdb, VS Code's cortex-debug), so a
// firmware can be debugged in PBConPC as with a debug probe on the real board:
//
//   build/pbconpc --gdb 3333 firmware.uf2
//   gdb-multiarch firmware.elf -ex "target extended-remote :3333"
//
// The two cores are threads 1 and 2. Breakpoints (break/hbreak), single steps, continue, Ctrl-C,
// registers (core, MSP/PSP/PRIMASK/BASEPRI/FAULTMASK/CONTROL, FPU) and memory work; `load`
// writes the flash as programming it would; `monitor reset` presses RESET. While breakpoints
// are set or a step runs, the cores interpret instruction by instruction instead of running
// translated code, a few times slower than the device.

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cpu;

// Listens on [ADDR:]PORT (127.0.0.1 unless ADDR is given).
bool gdb_start(const char *spec);
void gdb_stop(void);

// The machine holds while this is set (a breakpoint, a step done, Ctrl-C, a connecting debugger).
extern atomic_bool gdb_halt_req;
// Set by the machine thread while it holds, so the debugger may look at the chip.
extern atomic_bool gdb_held;
// The cores take one instruction at a time and ask before each.
extern volatile bool gdb_slow;

// From cpu_run in slow mode: before an instruction (true: stop here, do not run it) and after it.
bool gdb_before_step(struct cpu *c);
bool gdb_after_step(struct cpu *c);

#ifdef __cplusplus
}
#endif
