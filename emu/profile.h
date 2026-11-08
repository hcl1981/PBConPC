#pragma once

// PBConPC: a sampling profiler in emulated time. Every few microseconds of emulated time it notes
// where each core is (or that it sleeps in WFI/WFE); the report names the functions from the
// firmware's ELF files and gives each one's share of the core's time on the real device, as the
// emulator's timing models it (flash cache misses, wait states, divider ...). So "why does this
// run at 85 %" can be answered on the PC.
//
//   build/pbconpc --profile=report.txt --profile-elf=fw.elf fw.uf2      the whole run
//   script: profile start / profile stop report.txt                    a window of it
//   window: Machine -> Profiler

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct chip;

// ELF files whose function names the report uses (several: pbcemu's launcher and emulators).
void profile_add_elf(const char *path);
void profile_start(void);
bool profile_running(void);
// Stops and writes the report to path (NULL: only the log gets the top lines).
void profile_stop(const char *path);
// From the machine loop after each chunk.
void profile_sample(struct chip *m);

#ifdef __cplusplus
}
#endif
