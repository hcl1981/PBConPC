#include "link.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "chip.h"
#include "picocalc-sim/sim.h"

// See link.h. The shared memory holds one record per side; each side writes only its own.

#define LINK_MAGIC 0x324b4e4cu   // "LNK2"
#define RING 65536u              // edges a side may run ahead of the other's reading
#define STALL_NS 500000000ull    // a side that publishes nothing for this long counts as gone
#define BEAT_EVERY 256u
#define PUB_EVERY 16u   // chunks (about 1 us each) between publishing while nobody waits

// What a side puts on the wire, as bits.
enum { DP_DRV = 1, DP_HI = 2, DM_DRV = 4, DM_HI = 8, DP_PULLUP = 16, DM_PULLUP = 32 };

#define PHY_TX_DP_OE (1u << 8)
#define PHY_TX_DM_OE (1u << 9)
#define PHY_TX_DP (1u << 10)
#define PHY_TX_DM (1u << 11)
#define PHY_DP_PULLUP (1u << 1)
#define PHY_DM_PULLUP (1u << 5)
#define PHY_RX_DP (1u << 17)
#define PHY_RX_DM (1u << 18)
#define OVR_TX_DP_OE (1u << 5)
#define OVR_TX_DM_OE (1u << 6)
#define OVR_TX_DP (1u << 7)
#define OVR_TX_DM (1u << 8)
#define OVR_DP_PULLUP (1u << 2)
#define OVR_DM_PULLUP (1u << 12)

typedef struct {
    uint64_t t;
    uint32_t lines, pad;
} entry_t;

typedef struct {
    _Atomic uint32_t used;   // 1 while a PBConPC owns this side
    _Atomic uint32_t gen;    // counts the times the side was taken, so the other starts over
    _Atomic uint64_t pub;    // link time (ps) up to which what this side drives is final
    _Atomic uint64_t beat;   // host time (ns) of its last sign of life
    _Atomic uint32_t head;   // entries written to ring
    _Atomic uint32_t seen;   // entries of the OTHER side's ring this side has taken in
    // Set while this side waits for the other to publish; the other then publishes at every chunk
    // rather than every PUB_EVERY, so polling stays cheap for both. Its own cache line, as the
    // other side reads it after every chunk.
    _Alignas(64) _Atomic uint32_t waiting;
    entry_t ring[RING];
} side_t;

typedef struct {
    _Atomic uint32_t magic;
    side_t side[2];
} shm_t;

static shm_t *shm;
static int me = -1;
#ifdef _WIN32
static HANDLE mapping;
#endif

static int64_t offset;           // link time = local time + offset
static uint64_t last_link;       // never goes back, across restarts of this machine
static uint32_t my_lines;        // what this side drives now
static uint32_t partner_lines;   // what the other side drove at the last time taken in
static uint32_t partner_gen;
static uint32_t publishes;

static uint64_t host_ns(void)
{
    return picocalc_sim_mono_ns();
}

static side_t *mine(void)
{
    return &shm->side[me];
}

static side_t *theirs(void)
{
    return &shm->side[me ^ 1];
}

static bool alive(side_t *s)
{
    return atomic_load(&s->used) && host_ns() - atomic_load(&s->beat) < STALL_NS * 3;
}

static uint64_t to_link(uint64_t local)
{
    int64_t l = (int64_t)local + offset;
    if (l < (int64_t)last_link) {
        // the machine restarted and its clock began again
        offset = (int64_t)last_link - (int64_t)local;
        l = (int64_t)last_link;
    }
    last_link = (uint64_t)l;
    return (uint64_t)l;
}

static void raise_pub(uint64_t t)
{
    if (t > atomic_load_explicit(&mine()->pub, memory_order_relaxed))
        atomic_store_explicit(&mine()->pub, t, memory_order_release);
}

// Takes in the other side's edges up to link time t.
static void take_in(uint64_t t)
{
    side_t *o = theirs();
    uint32_t g = atomic_load_explicit(&o->gen, memory_order_acquire);
    if (g != partner_gen) {
        partner_gen = g;
        partner_lines = 0;
        atomic_store(&mine()->seen, 0);
    }
    uint32_t seen = atomic_load_explicit(&mine()->seen, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&o->head, memory_order_acquire);
    if (seen == head)
        return;
    uint32_t was = seen;
    while (seen != head && o->ring[seen % RING].t <= t) {
        partner_lines = o->ring[seen % RING].lines;
        seen++;
    }
    if (seen != was)
        atomic_store_explicit(&mine()->seen, seen, memory_order_release);
}

static void detach_mapping(void)
{
#ifdef _WIN32
    if (shm)
        UnmapViewOfFile(shm);
    if (mapping)
        CloseHandle(mapping);
    mapping = NULL;
#else
    if (shm)
        munmap(shm, sizeof(shm_t));
#endif
    shm = NULL;
    me = -1;
}

bool link_open(const char *name, uint64_t local_now)
{
    if (shm)
        link_close();
    char id[96];
    snprintf(id, sizeof(id), "pbconpc-link-%s", name && *name ? name : "cable");
#ifdef _WIN32
    char wname[128];
    snprintf(wname, sizeof(wname), "Local\\%s", id);
    mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)sizeof(shm_t), wname);
    if (!mapping) {
        picocalc_sim_log("link: cannot create the shared memory %s", wname);
        return false;
    }
    shm = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(shm_t));
#else
    char pname[128];
    snprintf(pname, sizeof(pname), "/%s", id);
    int fd = shm_open(pname, O_RDWR | O_CREAT, 0600);
    if (fd < 0 || ftruncate(fd, sizeof(shm_t)) != 0) {
        picocalc_sim_log("link: cannot create the shared memory %s", pname);
        if (fd >= 0)
            close(fd);
        return false;
    }
    void *p = mmap(NULL, sizeof(shm_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    shm = p == MAP_FAILED ? NULL : p;
#endif
    if (!shm) {
        detach_mapping();
        return false;
    }
    uint32_t zero = 0;
    atomic_compare_exchange_strong(&shm->magic, &zero, LINK_MAGIC);
    // A free side, or one whose owner stopped without letting go.
    for (int i = 0; i < 2 && me < 0; i++) {
        uint32_t free_ = 0;
        side_t *s = &shm->side[i];
        if (atomic_compare_exchange_strong(&s->used, &free_, 1) || !alive(s))
            me = i;
    }
    if (me < 0) {
        picocalc_sim_log("link: two PBConPC already use the cable \"%s\"", name && *name ? name : "cable");
        detach_mapping();
        return false;
    }
    side_t *s = mine(), *o = theirs();
    atomic_store(&s->used, 1);
    atomic_store(&s->head, 0);
    atomic_store(&s->seen, 0);
    atomic_store(&s->beat, host_ns());
    // Level with the other side's clock if it is there, so neither waits on the other.
    uint64_t start = alive(o) ? atomic_load(&o->pub) : 0;
    offset = (int64_t)start - (int64_t)local_now;
    last_link = start;
    atomic_store(&s->pub, start);
    partner_gen = atomic_load(&o->gen);
    partner_lines = 0;
    uint32_t head = atomic_load(&o->head);
    if (head) {
        partner_lines = o->ring[(head - 1) % RING].lines;
        atomic_store(&s->seen, head);
    }
    my_lines = 0;
    atomic_fetch_add(&s->gen, 1);
    picocalc_sim_log("link: cable \"%s\" plugged in (side %d)%s", name && *name ? name : "cable", me,
                     alive(o) ? ", another PicoBoy is on the other end" : "");
    return true;
}

void link_close(void)
{
    if (!shm)
        return;
    atomic_store(&mine()->used, 0);
    detach_mapping();
    picocalc_sim_log("link: cable unplugged");
}

bool link_is_open(void)
{
    return shm != NULL;
}

int link_side(void)
{
    return shm ? me : -1;
}

bool link_partner_present(void)
{
    return shm && alive(theirs());
}

static uint32_t lines_of(const usb_t *u)
{
    uint32_t d = u->phy_direct, o = u->phy_override, v = 0;
    // Only what the firmware has taken over goes on the wire; the USB controller itself does
    // not talk across the link cable.
    if ((o & OVR_TX_DP_OE) && (d & PHY_TX_DP_OE)) {
        v |= DP_DRV;
        if ((o & OVR_TX_DP) && (d & PHY_TX_DP))
            v |= DP_HI;
    }
    if ((o & OVR_TX_DM_OE) && (d & PHY_TX_DM_OE)) {
        v |= DM_DRV;
        if ((o & OVR_TX_DM) && (d & PHY_TX_DM))
            v |= DM_HI;
    }
    if ((o & OVR_DP_PULLUP) && (d & PHY_DP_PULLUP))
        v |= DP_PULLUP;
    if ((o & OVR_DM_PULLUP) && (d & PHY_DM_PULLUP))
        v |= DM_PULLUP;
    return v;
}

void link_phy_written(struct chip *m)
{
    uint32_t v = lines_of(&m->usb);
    if (v == my_lines)
        return;
    my_lines = v;
    if (!shm)
        return;
    side_t *s = mine(), *o = theirs();
    uint64_t t = to_link(m->access_t);
    uint64_t pub = atomic_load_explicit(&s->pub, memory_order_relaxed);
    if (t < pub)
        t = pub;   // another core's write, or the first after a restart: never into the past
    uint32_t head = atomic_load_explicit(&s->head, memory_order_relaxed);
    // A full ring waits for the other side to read on, unless it has gone.
    while (head - atomic_load_explicit(&o->seen, memory_order_acquire) >= RING && alive(o) &&
           atomic_load(&o->gen) == partner_gen) {
        raise_pub(t);
        picocalc_sim_sleep_ns(20000);
    }
    s->ring[head % RING] = (entry_t){t, v, 0};
    atomic_store_explicit(&s->head, head + 1, memory_order_release);
    raise_pub(t);
}

static int level(uint32_t a, uint32_t b, uint32_t drv, uint32_t hi, uint32_t pullup)
{
    bool da = a & drv, db = b & drv;
    if (da || db)
        return (da && (a & hi)) || (db && (b & hi));
    return (a & pullup) || (b & pullup);
}

// PBConPC measurement (PBCONPC_STATS=1): reads, reads that waited, host ns spent waiting
uint64_t link_stat[4];

// The other side has published nothing new for STALL_NS: it is closed, halted or paused, and the
// wire reads as unplugged until its time moves again.
static bool partner_stalled;
static uint64_t stalled_pub;

// Waits until the other side has run up to t. This side first publishes that it is at t, so the
// other, if it waits for this one, goes on: the one behind never waits.
static void wait_for(side_t *o, uint64_t t)
{
    uint64_t op = atomic_load_explicit(&o->pub, memory_order_acquire);
    if (partner_stalled) {
        if (op == stalled_pub)
            return;
        partner_stalled = false;
    }
    raise_pub(t);
    atomic_store_explicit(&mine()->waiting, 1, memory_order_release);
    uint64_t since = 0, last_pub = op, w0 = host_ns();
    unsigned spins = 0;
    link_stat[1]++;
    while ((op = atomic_load_explicit(&o->pub, memory_order_acquire)) < t) {
        if (op != last_pub) {
            last_pub = op;
            since = 0;
        }
        if (++spins < 2000) {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
            continue;
        }
        spins = 0;
        uint64_t now = host_ns();
        if (!since)
            since = now;
        else if (now - since > STALL_NS || !atomic_load(&o->used)) {
            partner_stalled = true;
            stalled_pub = op;
            break;
        }
        atomic_store_explicit(&mine()->beat, now, memory_order_relaxed);
        picocalc_sim_sleep_ns(0);
    }
    atomic_store_explicit(&mine()->waiting, 0, memory_order_relaxed);
    link_stat[2] += host_ns() - w0;
}

// A polling loop: the same instruction reading the same value again and again.
static uint32_t poll_pc, poll_val;
static unsigned poll_same;
#define POLL_SKIP_MAX_PS 1000000ull   // 1 us

uint32_t link_phy_read(struct chip *m, uint32_t phy_direct)
{
    uint32_t other = 0;
    uint64_t t = to_link(m->access_t), quiet_until = t + POLL_SKIP_MAX_PS;
    link_stat[0]++;
    if (shm) {
        // The fast path, taken millions of times a second while a firmware polls the wire: no
        // clock reads and no stores to the shared memory, which the other side would feel.
        side_t *o = theirs();
        if (atomic_load_explicit(&o->used, memory_order_relaxed)) {
            uint64_t op = atomic_load_explicit(&o->pub, memory_order_acquire);
            if (op < t) {
                wait_for(o, t);
                op = atomic_load_explicit(&o->pub, memory_order_acquire);
            }
            if (!partner_stalled) {
                take_in(t);
                other = partner_lines;
                // The wire cannot change before the other side's next edge, nor past what it
                // has published.
                if (op < quiet_until)
                    quiet_until = op;
                uint32_t seen = atomic_load_explicit(&mine()->seen, memory_order_relaxed);
                if (seen != atomic_load_explicit(&o->head, memory_order_acquire) && o->ring[seen % RING].t < quiet_until)
                    quiet_until = o->ring[seen % RING].t;
            }
        }
    }
    uint32_t v = phy_direct & ~(PHY_RX_DP | PHY_RX_DM);
    if (level(my_lines, other, DP_DRV, DP_HI, DP_PULLUP))
        v |= PHY_RX_DP;
    if (level(my_lines, other, DM_DRV, DM_HI, DM_PULLUP))
        v |= PHY_RX_DM;
    // A loop that only waits for the wire to change (usblink waits by time_us_32, not by
    // counting) spends that time in fewer, longer reads: up to the moment the wire can change at
    // the earliest, at most 1 us. What the firmware sees of time and of the wire stays exact;
    // only the number of times round the loop drops, which costs the host the most.
    cpu_t *c = m->access_core;
    if (c) {
        if (c->r[15] == poll_pc && v == poll_val) {
            if (poll_same < 1000)
                poll_same++;
        } else {
            poll_pc = c->r[15];
            poll_val = v;
            poll_same = 0;
        }
        if (poll_same >= 4 && quiet_until > t && m->sys_ps)
            c->cyc += (uint32_t)((quiet_until - t) / m->sys_ps);
    }
    return v;
}

void link_publish_now(uint64_t local_now)
{
    if (shm)
        raise_pub(to_link(local_now));
}

void link_publish(uint64_t local_now)
{
    if (!shm)
        return;
    uint64_t t = to_link(local_now);
    // Publishing is a store the other side's waiting loop feels; while it does not wait, now and
    // then is enough (it then waits a little more often, and says so).
    if (++publishes % PUB_EVERY == 0 || atomic_load_explicit(&theirs()->waiting, memory_order_acquire))
        raise_pub(t);
    take_in(t);
    if (publishes % BEAT_EVERY == 0)
        atomic_store_explicit(&mine()->beat, host_ns(), memory_order_relaxed);
}
