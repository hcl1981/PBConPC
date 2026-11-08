#pragma once

// PBConPC: the link cable between two PicoBoys (pbc-plus common/usblink.c), as a wire between two
// PBConPC processes. The firmware drives D+ (DATA) and D- (CLK) itself through the USB PHY's
// direct-override registers; this module carries what each side drives, stamped with the exact
// emulated time of the register write, to the other side through shared memory.
//
// Time: each side counts "link time" in picoseconds, its own emulated time plus an offset chosen
// when it joins, so both start level. A side that reads the wire at link time t first waits until
// the other side has published that it has run up to t, so every edge arrives at the exact
// moment it was made, however far apart the two processes run on the host. The side that is
// behind never waits, so the two can never block each other. A side that stops (closed, halted,
// paused) is noticed within half a second and then counts as unplugged.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct chip;

// Plugs the cable in under a name (two PBConPC with the same name are connected); NULL or ""
// takes "cable". Returns false when two others already use that name.
bool link_open(const char *name, uint64_t local_now);
void link_close(void);
bool link_is_open(void);
// Whether another PBConPC is on the other end.
bool link_partner_present(void);
// This end's side of the cable, 0 or 1 (-1 when unplugged).
int link_side(void);

// From usb.c: the PHY registers changed (what this side drives may have), or the firmware reads
// them (gives phy_direct with RX_DP and RX_DM as the wire shows them).
void link_phy_written(struct chip *m);
uint32_t link_phy_read(struct chip *m, uint32_t phy_direct);
// From the machine loop after each chunk: this side has run up to local time now.
void link_publish(uint64_t local_now);
// Before the machine sleeps (pacing to real time) or holds: publish now, so the other side does
// not wait out the sleep.
void link_publish_now(uint64_t local_now);

#ifdef __cplusplus
}
#endif
