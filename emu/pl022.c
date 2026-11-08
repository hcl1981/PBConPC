#include <string.h>

#include "chip.h"
#include "picocalc-sim/bus.h"

// ARM PrimeCell PL022 SSP in Motorola SPI master mode. Frames leave the TX FIFO at the bit rate
// the driver configured; the exchange with whichever device is selected happens when each frame
// finishes shifting, so the CPU sees FIFO levels and BSY exactly as it would on the chip.

bool g_fast_bus;

void pl022_reset(pl022_t *s, int idx)
{
    memset(s, 0, sizeof(*s));
    s->idx = idx;
}

static uint64_t frame_ps(chip_t *m, pl022_t *s)
{
    if (g_fast_bus)
        return 0;
    // PBConPC: the division costs more than the rest of a status poll; keep the last result
    static __thread struct { uint32_t cpsr, cr0, hz; uint64_t ps; } memo[2];
    int k = s->idx & 1;
    if (memo[k].cpsr == s->cpsr && memo[k].cr0 == s->cr0 && memo[k].hz == m->peri_hz && memo[k].ps)
        return memo[k].ps;
    uint32_t cpsdvsr = s->cpsr & 0xfe;
    uint32_t scr = (s->cr0 >> 8) & 0xff;
    unsigned bits = (s->cr0 & 0xf) + 1;
    uint64_t ps = !cpsdvsr || !m->peri_hz ? UINT64_MAX / 4
                                          : (uint64_t)bits * cpsdvsr * (1 + scr) * PS_PER_S / m->peri_hz;
    memo[k].cpsr = s->cpsr;
    memo[k].cr0 = s->cr0;
    memo[k].hz = m->peri_hz;
    memo[k].ps = ps;
    return ps;
}

static bool enabled(pl022_t *s)
{
    return s->cr1 & 2;
}

static void update_irq(chip_t *m, pl022_t *s)
{
    s->ris &= 3;
    if (s->txn <= 4)
        s->ris |= 8;
    if (s->rxn >= 4)
        s->ris |= 4;
    chip_irq(m, IRQ_SPI0 + (unsigned)s->idx, (s->ris & s->imsc) != 0);
    chip_dreq(m, DREQ_SPI0_TX + 2u * (unsigned)s->idx, (s->dmacr & 2) && s->txn < 8);
    chip_dreq(m, DREQ_SPI0_RX + 2u * (unsigned)s->idx, (s->dmacr & 1) && s->rxn > 0);
}

uint64_t pbc_spi_frames, pbc_spi_dr[3];

static void finish_frame(chip_t *m, pl022_t *s)
{
    pbc_spi_frames++;
    uint16_t mosi = s->txf[s->txh];
    s->txh = (s->txh + 1) & 7;
    s->txn--;
    unsigned bits = (s->cr0 & 0xf) + 1;
    uint16_t miso = (uint16_t)((1u << bits) - 1);
    if (s->cr1 & 1)
        miso = mosi;
    else
        devices_spi_frame(m, s->idx, mosi, (int)bits, &miso);
    if (s->rxn < 8) {
        s->rxf[(s->rxh + s->rxn) & 7] = miso & (uint16_t)((1u << bits) - 1);
        s->rxn++;
    } else {
        s->ris |= 1;
    }
}

// PBConPC: n frames straight from memory (a DMA channel's run, see dma.c bulk_spi), with instant
// transfers only: each goes to the devices and fills the receive FIFO exactly as finish_frame
// does; the interrupt and DREQ lines are brought up to date once at the end.
void pl022_bulk_tx(chip_t *m, pl022_t *s, const uint8_t *src, uint32_t n, uint32_t size)
{
    unsigned bits = (s->cr0 & 0xf) + 1;
    uint16_t mask = (uint16_t)((1u << bits) - 1);
    // The devices are those devices_spi_frame would find for each frame: no pin changes during
    // the run, so they are looked up once.
    picocalc_sim_spi_device_t *dev[4];
    int ndev = devices_spi_targets(m, s->idx, dev, 4);
    // One device that takes the whole run at once (the display's pixels): the receive FIFO then
    // fills with all-ones frames as below.
    if (ndev == 1 && dev[0]->xfer_bulk && ((bits == 8 && size == 1) || (bits == 16 && size == 2)) &&
        dev[0]->xfer_bulk(dev[0]->ctx, src, n, size, bits)) {
        uint32_t room = 8u - s->rxn;
        for (uint32_t i = 0; i < n && i < room; i++) {
            s->rxf[(s->rxh + s->rxn) & 7] = mask;
            s->rxn++;
        }
        if (n > room)
            s->ris |= 1;   // the frames that found it full overran it
        update_irq(m, s);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint16_t mosi = size == 2 ? (uint16_t)(src[2 * i] | src[2 * i + 1] << 8) : (uint16_t)(src[i] * 0x0101u);
        uint16_t in = 0xffff;
        for (int k = 0; k < ndev; k++) {   // as devices_spi_frame
            picocalc_sim_spi_device_t *d = dev[k];
            if (bits > 8) {
                uint8_t hi = d->xfer(d->ctx, (uint8_t)(mosi >> (bits - 8)));
                uint8_t lo = d->xfer(d->ctx, (uint8_t)(mosi << (16 - bits)));
                in &= (uint16_t)(((hi << 8) | lo) >> (16 - bits));
            } else {
                in &= d->xfer(d->ctx, (uint8_t)(mosi << (8 - bits)));
            }
        }
        uint16_t miso = in & mask;
        if (s->rxn < 8) {
            s->rxf[(s->rxh + s->rxn) & 7] = miso & mask;
            s->rxn++;
        } else {
            s->ris |= 1;
        }
    }
    update_irq(m, s);
}

static void spi_event(chip_t *m, emu_event_t *ev);

static void sync(chip_t *m, pl022_t *s)
{
    if (!enabled(s))
        return;
    uint64_t fp = frame_ps(m, s);
    bool any = false;
    while (s->txn > 0 && s->busy_until + fp <= m->now) {
        s->busy_until += fp;
        finish_frame(m, s);
        any = true;
    }
    if (s->txn == 0 && s->busy_until < m->now)
        s->busy_until = m->now;
    if (any)
        update_irq(m, s);
    if (s->txn > 0 && (s->imsc || s->dmacr)) {
        s->ev.fn = spi_event;
        s->ev.ctx = s;
        ev_schedule(m, &s->ev, s->busy_until + fp);
    } else {
        ev_cancel(m, &s->ev);
    }
}

static void spi_event(chip_t *m, emu_event_t *ev)
{
    sync(m, ev->ctx);
}

void pl022_sync_all(chip_t *m)
{
    sync(m, &m->spi[0]);
    sync(m, &m->spi[1]);
}

bool pl022_cs_low(chip_t *m, int idx)
{
    pl022_t *s = &m->spi[idx];
    return enabled(s) && s->txn > 0 && s->busy_until <= m->now;
}

uint32_t pl022_read(chip_t *m, pl022_t *s, uint32_t off, bool peek)
{
    if (!peek)
        sync(m, s);
    switch (off) {
    case 0x00: return s->cr0;
    case 0x04: return s->cr1;
    case 0x08: {
        if (!s->rxn || peek)
            return s->rxn ? s->rxf[s->rxh] : 0;
        uint16_t v = s->rxf[s->rxh];
        s->rxh = (s->rxh + 1) & 7;
        s->rxn--;
        update_irq(m, s);
        return v;
    }
    case 0x0c: {
        bool busy = enabled(s) && s->txn > 0;
        return (s->txn == 0 ? 1u : 0) | (s->txn < 8 ? 2u : 0) | (s->rxn > 0 ? 4u : 0) | (s->rxn == 8 ? 8u : 0) |
               (busy ? 16u : 0);
    }
    case 0x10: return s->cpsr;
    case 0x14: return s->imsc;
    case 0x18: return s->ris;
    case 0x1c: return s->ris & s->imsc;
    case 0x24: return s->dmacr;
    case 0xfe0: return 0x22;
    case 0xfe4: return 0x10;
    case 0xfe8: return 0x34;
    case 0xfec: return 0x00;
    case 0xff0: return 0x0d;
    case 0xff4: return 0xf0;
    case 0xff8: return 0x05;
    case 0xffc: return 0xb1;
    default: return 0;
    }
}

void pl022_write(chip_t *m, pl022_t *s, uint32_t off, uint32_t v)
{
    sync(m, s);
    switch (off) {
    case 0x00: s->cr0 = v & 0xffff; break;
    case 0x04: {
        bool was = enabled(s);
        s->cr1 = v & 0xf;
        if (!was && enabled(s) && s->busy_until < m->now)
            s->busy_until = m->now;
        break;
    }
    case 0x08:
        pbc_spi_dr[m->access_core ? m->access_core->id + 1 : 0]++;
        if (s->txn < 8) {
            if (s->txn == 0 && s->busy_until < m->now)
                s->busy_until = m->now;
            s->txf[(s->txh + s->txn) & 7] = (uint16_t)v;
            s->txn++;
        }
        break;
    case 0x10: s->cpsr = v & 0xff; break;
    case 0x14: s->imsc = v & 0xf; break;
    case 0x20: s->ris &= ~(v & 3); break;
    case 0x24: s->dmacr = v & 3; break;
    default: break;
    }
    update_irq(m, s);
    sync(m, s);
}
