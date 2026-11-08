#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chip.h"
#include "picocalc-sim/devices.h"
#include "picocalc-sim/net.h"
#include "picocalc-sim/sim.h"
#include "uf2.h"
#include "link.h"
#include "gdb.h"
#include "profile.h"

// The emulation thread. It runs both cores and the peripheral events in short chunks of emulated
// time, paces them against the host clock so the device runs at its real speed, and handles the
// resets, BOOTSEL and power changes that firmware, devices and the user request.

// PBConPC: 4 us while the cores go their own ways, 1 us (upstream's only length) for a while after
// they talk to each other (chip_cores_talk). The end moves in to any event scheduled inside it
// (chip.c ev_schedule), so events fire on time whatever the length. 4 us took a quarter off the
// host time of a game in pbcemu, with the same frames on the LCD.
#define CHUNK_PS (4 * PS_PER_US)
#define CHUNK_FINE_PS (1 * PS_PER_US)
#define SLICE_PS (2 * PS_PER_MS)

static chip_t chip;
static pthread_t thread;
static pthread_mutex_t lock;
static atomic_bool stop_flag;
static _Atomic picocalc_sim_state_t state = PICOCALC_SIM_RUNNING;
static char state_reason[128];
static atomic_bool power_button, bootsel_held, button_reset;
static double speed = 1.0;

static pthread_mutex_t uf2_lock = PTHREAD_MUTEX_INITIALIZER;
static char pending_uf2[4096];
// PICOCALC_SIM_STOP_AT_MS ends the run at an exact emulated time, so that with --trace two builds
// can be compared by the state digest they print.
static uint64_t stop_at_ps;
// PICOCALC_SIM_RESTART_AT_MS restarts the machine once at an exact emulated time, so the digest
// the second run ends with can be checked against a fresh launch's.
static uint64_t restart_at_ps;

// What picocalc_sim_machine_start has set up, for picocalc_sim_machine_stop or a failed start to
// take down.
static bool chip_ready, net_up, running;
static atomic_uint starts;
static atomic_bool restart_flag;
// The flash image this run keeps, once "none" and the default are worked out from
// picocalc_sim_opts.
static char flash_file[4096];
static const char *flash_path;
// With --flash none, each board's flash lasts from one restart to the next, as a power cycle
// would leave it, until picocalc-sim exits.
static uint8_t *kept_flash[8];

// Threads waiting for the lock, which the emulation thread hands over between slices. Running
// behind real time it never sleeps, and would otherwise take the lock back before a waiter woke.
static atomic_int lock_waiters;

void picocalc_sim_machine_lock(void)
{
    atomic_fetch_add(&lock_waiters, 1);
    pthread_mutex_lock(&lock);
    atomic_fetch_sub(&lock_waiters, 1);
}

void picocalc_sim_machine_unlock(void)
{
    pthread_mutex_unlock(&lock);
}

uint64_t picocalc_sim_uptime_us(void)
{
    return chip.now / PS_PER_US;
}

// PBConPC: a script's clock. With a script, the machine waits at hold_ps until the script moves
// it on, so that the script's keys and screenshots fall on exact emulated times whatever the
// host's speed (before, they came whenever the script's thread next looked, and runs of the same
// script differed). held_ps is where the machine waits now.
static bool hold_mode;
static _Atomic uint64_t hold_ps, held_ps;
static __thread bool script_thread_here;

void picocalc_sim_script_hold(void)
{
    hold_mode = true;
    atomic_store(&hold_ps, 1);   // at the start, until the script's first wait
}

void picocalc_sim_script_thread(void)
{
    script_thread_here = true;
}

// The script has ended: no more holds, the machine runs on.
void picocalc_sim_script_release(void)
{
    hold_mode = false;
    atomic_store(&hold_ps, 0);
}

static void script_wait(uint64_t ms)
{
    static uint64_t clock_ps;
    static unsigned starts;
    extern unsigned picocalc_sim_machine_starts(void);
    if (picocalc_sim_machine_starts() != starts) {   // a restart starts the clock from zero
        starts = picocalc_sim_machine_starts();
        clock_ps = 0;
    }
    clock_ps += ms * PS_PER_MS;
    atomic_store(&hold_ps, clock_ps);
    uint64_t idle_ns = 0;
    while (!picocalc_sim_quit_requested(NULL) && atomic_load(&held_ps) != clock_ps) {
        if (picocalc_sim_machine_starts() != starts)
            break;
        // stopped, powered off or in BOOTSEL: emulated time stands, so wait it out in host time
        if (!running || atomic_load(&state) != PICOCALC_SIM_RUNNING) {
            if ((idle_ns += 1000000ull) > ms * 1000000ull)
                break;
            picocalc_sim_sleep_ns(1000000ull);
            continue;
        }
        picocalc_sim_sleep_ns(20000ull);
    }
}

void picocalc_sim_sleep_emulated_ms(uint64_t ms)
{
    if (hold_mode && script_thread_here) {
        script_wait(ms);
        return;
    }
    uint64_t want = ms * 1000, done = 0, last = picocalc_sim_uptime_us();
    while (done < want && !picocalc_sim_quit_requested(NULL)) {
        if (!running || atomic_load(&state) != PICOCALC_SIM_RUNNING) {
            picocalc_sim_sleep_ns(1000000ull);
            done += 1000;
            continue;
        }
        picocalc_sim_sleep_ns(500000ull);
        uint64_t now = picocalc_sim_uptime_us();
        // a restart starts the clock again from zero
        done += now >= last ? now - last : now;
        last = now;
    }
}

// Raspberry Pi's ROM running its USB boot, which is in Non-secure code in the ROM.
static atomic_bool usb_boot;

picocalc_sim_state_t picocalc_sim_state(void)
{
    picocalc_sim_state_t s = atomic_load(&state);
    return s == PICOCALC_SIM_RUNNING && atomic_load(&usb_boot) ? PICOCALC_SIM_BOOTSEL : s;
}

const char *picocalc_sim_state_reason(void)
{
    return state_reason;
}

const char *picocalc_sim_board_name(void)
{
    return chip.board ? chip.board->name : "?";
}

bool picocalc_sim_board_desc(int i, picocalc_sim_board_desc_t *d)
{
    for (int n = 0; n <= i; n++)
        if (!board_table[n].name)
            return false;
    const board_info_t *b = &board_table[i];
    *d = (picocalc_sim_board_desc_t){b->name, b->rp2040, b->wifi, b->flash_size >> 20, b->psram_size >> 20};
    return true;
}

double picocalc_sim_speed(void)
{
    return speed;
}

static void set_state(picocalc_sim_state_t s, const char *reason)
{
    snprintf(state_reason, sizeof(state_reason), "%s", reason);
    atomic_store(&state, s);
}

void picocalc_sim_reboot(const char *reason)
{
    atomic_store(&button_reset, true);
    chip_request(&chip, REQ_RESET, reason);
}

void picocalc_sim_stop(picocalc_sim_state_t s, const char *reason)
{
    if (s == PICOCALC_SIM_POWERED_OFF)
        chip_request(&chip, REQ_POWEROFF, reason);
    else if (s == PICOCALC_SIM_BOOTSEL)
        chip_request(&chip, REQ_BOOTSEL, reason);
}

void picocalc_sim_power_button(void)
{
    atomic_store(&power_button, true);
}

void picocalc_sim_bootsel_button(bool held)
{
    atomic_store(&bootsel_held, held);
}

void picocalc_sim_usb_plug(bool plugged)
{
    picocalc_sim_machine_lock();
    if (chip_ready && plugged != (chip.usb.host != NULL)) {
        if (plugged)
            usbhost_init(&chip);
        else
            usbhost_free(&chip);
        gpio_refresh_mask(&chip, 1ull << 24);
        picocalc_sim_log("usb: cable %s", plugged ? "plugged into the emulated host" : "unplugged");
    }
    picocalc_sim_machine_unlock();
}

size_t picocalc_sim_usb_serial_send(const uint8_t *data, size_t len)
{
    picocalc_sim_machine_lock();
    size_t n = chip_ready ? usbhost_serial_send(&chip, data, len) : 0;
    picocalc_sim_machine_unlock();
    return n;
}

static atomic_uint serial_terminals;

static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

void picocalc_sim_usb_serial_attach(unsigned terminal, bool on)
{
    picocalc_sim_machine_lock();
    unsigned was = atomic_load(&serial_terminals);
    unsigned now = on ? was | terminal : was & ~terminal;
    atomic_store(&serial_terminals, now);
    if (chip_ready && !was != !now)
        usbhost_serial_wanted_changed(&chip);
    picocalc_sim_machine_unlock();
}

bool picocalc_sim_usb_serial_attached(unsigned terminal)
{
    return (atomic_load(&serial_terminals) & terminal) != 0;
}

bool picocalc_sim_usb_serial_wanted(void)
{
    return atomic_load(&serial_terminals) != 0;
}

int picocalc_sim_usb_serial_state(char *product, size_t cap)
{
    picocalc_sim_machine_lock();
    int s = chip_ready ? usbhost_serial_state(&chip, product, cap) : PICOCALC_SIM_USB_SERIAL_UNPLUGGED;
    if (!chip_ready && cap)
        *product = 0;
    picocalc_sim_machine_unlock();
    return s;
}

bool picocalc_sim_usb_drive_copy(const char *path, char *err, size_t errlen)
{
    size_t len;
    uint8_t *data = uf2_read_raw(path, &len, err, errlen);
    if (!data)
        return false;
    picocalc_sim_machine_lock();
    bool plugged = chip_ready && chip.usb.host;
    bool ok = plugged && usbhost_drive_copy(&chip, data, len, base_name(path));
    picocalc_sim_machine_unlock();
    if (!plugged) {
        free(data);
        snprintf(err, errlen, "the USB cable is unplugged");
    } else if (!ok) {
        snprintf(err, errlen, "the drive is busy copying another UF2");
    }
    return ok;
}

bool picocalc_sim_usb_serial_is_open(void)
{
    picocalc_sim_machine_lock();
    bool open = chip_ready && usbhost_serial_is_open(&chip);
    picocalc_sim_machine_unlock();
    return open;
}

// ---------------------------------------------------------------------------------------------
// Flash persistence

static void flash_save(void)
{
    if (!flash_path || !chip.flash || !chip.flash_dirty)
        return;
    FILE *f = fopen(flash_path, "r+b");
    if (!f)
        f = fopen(flash_path, "w+b");
    if (!f) {
        picocalc_sim_log("cannot write flash image %s: %s", flash_path, strerror(errno));
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size != (long)chip.flash_size) {
        fseek(f, 0, SEEK_SET);
        fwrite(chip.flash, 1, chip.flash_size, f);
    } else {
        fseek(f, (long)chip.flash_dirty_lo, SEEK_SET);
        fwrite(chip.flash + chip.flash_dirty_lo, 1, chip.flash_dirty_hi - chip.flash_dirty_lo, f);
    }
    fclose(f);
    chip.flash_dirty = false;
}

// PBConPC: one PBConPC per flash image. The lock lasts until the process ends.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static bool claim_flash(const char *path)
{
    static HANDLE held = INVALID_HANDLE_VALUE;
    static char held_path[4096];
    if (held != INVALID_HANDLE_VALUE && !strcmp(held_path, path))
        return true;
    char lock[4200];
    snprintf(lock, sizeof(lock), "%s.lock", path);
    HANDLE h = CreateFileA(lock, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    if (held != INVALID_HANDLE_VALUE)
        CloseHandle(held);
    held = h;
    snprintf(held_path, sizeof(held_path), "%s", path);
    return true;
}
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
static bool claim_flash(const char *path)
{
    static int held = -1;
    static char held_path[4096];
    if (held >= 0 && !strcmp(held_path, path))
        return true;
    char lock[4200];
    snprintf(lock, sizeof(lock), "%s.lock", path);
    int fd = open(lock, O_RDWR | O_CREAT, 0600);
    if (fd < 0)
        return true;   // no lock possible: go on as upstream does
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return false;
    }
    if (held >= 0)
        close(held);
    held = fd;
    snprintf(held_path, sizeof(held_path), "%s", path);
    return true;
}
#endif

static void copy_if_missing(const char *from, const char *to)
{
    FILE *t = fopen(to, "rb");
    if (t) {
        fclose(t);
        return;
    }
    FILE *f = fopen(from, "rb");
    if (!f)
        return;
    t = fopen(to, "wb");
    if (t) {
        static uint8_t buf[1 << 16];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            fwrite(buf, 1, n, t);
        fclose(t);
        picocalc_sim_log("second PicoBoy: %s, a copy of %s", to, from);
    }
    fclose(f);
}

static void flash_load(void)
{
    uint8_t **kept = &kept_flash[chip.board - board_table];
    if (picocalc_sim_opts.erase_flash) {
        free(*kept);
        *kept = NULL;
        return;
    }
    if (!flash_path) {
        if (*kept) {
            free(chip.flash);
            chip.flash = *kept;
            *kept = NULL;
        }
        return;
    }
    FILE *f = fopen(flash_path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0 && size <= (long)chip.flash_size && fread(chip.flash, 1, (size_t)size, f) != (size_t)size)
        picocalc_sim_log("short read from flash image %s", flash_path);
    fclose(f);
}

// ---------------------------------------------------------------------------------------------
// UF2 download

static bool program_uf2_locked(const char *path, char *err, size_t errlen)
{
    uf2_file_t f;
    if (!uf2_load(path, &f, err, errlen))
        return false;
    bool ok = uf2_program(&chip, &f, err, errlen);
    bool ram = f.ram;
    uf2_free(&f);
    if (ok && ram) {
        picocalc_sim_log("loaded %s into %s RAM", path, chip.board->name);
    } else if (ok) {
        flash_save();
        picocalc_sim_log("wrote %s to %s flash", path, chip.board->name);
    }
    return ok;
}

bool picocalc_sim_flash_uf2(const char *path, char *err, size_t errlen)
{
    uf2_file_t f;
    if (!uf2_load(path, &f, err, errlen))
        return false;
    uf2_free(&f);
    pthread_mutex_lock(&uf2_lock);
    snprintf(pending_uf2, sizeof(pending_uf2), "%s", path);
    pthread_mutex_unlock(&uf2_lock);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Run loop

static void handle_requests(void)
{
    pthread_mutex_lock(&chip.power_lock);
    bool reset = chip.req_reset, bootsel = chip.req_bootsel, poweroff = chip.req_poweroff;
    char reason[96];
    memcpy(reason, chip.req_reason, sizeof(reason));
    chip.req_reset = chip.req_bootsel = chip.req_poweroff = false;
    atomic_store(&chip.req_any, false);
    pthread_mutex_unlock(&chip.power_lock);

    char path[4096] = "";
    pthread_mutex_lock(&uf2_lock);
    if (pending_uf2[0]) {
        memcpy(path, pending_uf2, sizeof(path));
        pending_uf2[0] = 0;
    }
    pthread_mutex_unlock(&uf2_lock);
    if (path[0]) {
        char err[256];
        // With a boot ROM's drive mounted, the file goes onto the drive and the ROM writes it.
        if (usbhost_drive_mounted(&chip)) {
            size_t len;
            uint8_t *data = uf2_read_raw(path, &len, err, sizeof(err));
            if (!data)
                picocalc_sim_log("%s", err);
            else if (!usbhost_drive_copy(&chip, data, len, base_name(path)))
                picocalc_sim_log("the drive is busy copying another UF2");
            return;
        }
        if (program_uf2_locked(path, err, sizeof(err))) {
            set_state(PICOCALC_SIM_RUNNING, "");
            chip_reset(&chip, true);
        } else {
            picocalc_sim_log("%s", err);
        }
        return;
    }

    if (poweroff) {
        picocalc_sim_log("power off: %s", reason);
        flash_save();
        set_state(PICOCALC_SIM_POWERED_OFF, reason);
        for (int i = 0; i < 2; i++)
            chip.core[i].state = CPU_OFF;
        return;
    }
    if (bootsel) {
        picocalc_sim_log("BOOTSEL: %s", reason);
        flash_save();
        set_state(PICOCALC_SIM_BOOTSEL, reason);
        for (int i = 0; i < 2; i++)
            chip.core[i].state = CPU_OFF;
        return;
    }
    if (reset) {
        if (chip.trace)
            picocalc_sim_log("reset: %s", reason);
        flash_save();
        set_state(PICOCALC_SIM_RUNNING, "");
        bool button = atomic_exchange(&button_reset, false), held = atomic_load(&bootsel_held);
        // The reset button pulls RUN low, which resets the watchdog too, unlike its own reboots.
        if (button) {
            memset(chip.wd_scratch, 0, sizeof(chip.wd_scratch));
            chip.wd_reason = 0;
            chip.bootram_write_once[0] = chip.bootram_write_once[1] = 0;
            chip.boot_type = held ? 2 : 0;
        }
        chip_reset(&chip, true);
        // A person holds BOOTSEL through the reset button and lets go once the board has started,
        // long after the ROM has looked at it, and before any reset the ROM itself then makes.
        if (button)
            chip.bootsel_until = held && chip.real_rom ? chip.now + 100 * PS_PER_MS : 0;
    }
    if (atomic_exchange(&power_button, false)) {
        picocalc_sim_state_t s = atomic_load(&state);
        if (s == PICOCALC_SIM_POWERED_OFF) {
            set_state(PICOCALC_SIM_RUNNING, "");
            chip.boot_type = 0;
            chip_power_on(&chip);
        }
    }
}

// PBConPC measurement: host time per core (PBCONPC_STATS=1 prints it at the end)
static uint64_t stat_host[2], stat_insns0[2], stat_t0;
static int stats_on = -1;

static inline uint64_t host_tsc(void)
{
#if defined(__x86_64__)
    return __builtin_ia32_rdtsc();
#else
    return picocalc_sim_mono_ns();
#endif
}

static uint64_t stat_wall0;

void machine_print_stats(void)
{
    if (stats_on != 1)
        return;
    double emu_s = (double)(chip.now - stat_t0) / 1e12;
    if (stat_wall0)
        picocalc_sim_log("stats: %.2f s emulated in %.2f s host time: speed %.3f", emu_s,
                         (picocalc_sim_mono_ns() - stat_wall0) / 1e9, emu_s / ((picocalc_sim_mono_ns() - stat_wall0) / 1e9));
    extern uint64_t pbc_lcd_pixels, pbc_lcd_ramwr;
    picocalc_sim_log("stats: LCD: %.2f M pixels, %llu RAMWR", pbc_lcd_pixels / 1e6, (unsigned long long)pbc_lcd_ramwr);
    extern uint64_t pbc_stat[2][4];
    extern uint64_t pbc_bulk_stat[4], pbc_spi_frames, pbc_spi_dr[3];
    picocalc_sim_log("stats: SPI data writes: DMA %.1f M, core 0 %.1f M, core 1 %.1f M", pbc_spi_dr[0] / 1e6, pbc_spi_dr[1] / 1e6, pbc_spi_dr[2] / 1e6);
    picocalc_sim_log("stats: SPI bulk: %llu runs, %.1f M frames (of %.1f M SPI-TX channel looks), %.1f M frames the ordinary way",
                     (unsigned long long)pbc_bulk_stat[0], pbc_bulk_stat[1] / 1e6, pbc_bulk_stat[2] / 1e6, pbc_spi_frames / 1e6);
    extern uint64_t link_stat[4];
    picocalc_sim_log("stats: link: %.1f M PHY reads, %.2f M waited, %.2f s waiting", link_stat[0] / 1e6, link_stat[1] / 1e6,
                     link_stat[2] / 1e9);
    extern void pbc_jit_calls(uint64_t out[8]);
    uint64_t jc[8];
    pbc_jit_calls(jc);
    picocalc_sim_log("stats: JIT calls into C: generic %.1f M (+%.1f M from native code's cold paths), xip %.1f M, bus_read %.1f M, bus_write %.1f M, look %.1f M",
                     jc[0] / 1e6, jc[5] / 1e6, jc[1] / 1e6, jc[2] / 1e6, jc[3] / 1e6, jc[4] / 1e6);
    extern void pbc_jit_gen_report(void);
    pbc_jit_gen_report();
    extern uint64_t pbc_flag_stat[4];
    picocalc_sim_log("stats: JIT flags (translations so far): %llu computed, %llu left out, %llu branches fused, %llu translations dropped for changed code after them",
                     (unsigned long long)pbc_flag_stat[0], (unsigned long long)pbc_flag_stat[1], (unsigned long long)pbc_flag_stat[2], (unsigned long long)pbc_flag_stat[3]);
    extern uint64_t pbc_jit_stat[4], pbc_jit_why[8];
    picocalc_sim_log("stats: JIT exits: done %.1f M, careful %.1f M, link %.1f M, lookup %.1f M, stale %.1f M, careful0 %.1f M",
                     pbc_jit_why[0] / 1e6, pbc_jit_why[1] / 1e6, pbc_jit_why[2] / 1e6, pbc_jit_why[3] / 1e6, pbc_jit_why[4] / 1e6, pbc_jit_why[5] / 1e6);
    picocalc_sim_log("stats: JIT translations %.1f M normal, %.1f M careful, %llu flushes (%llu asked for by cpu_flush_code)", pbc_jit_stat[0] / 1e6,
                     pbc_jit_stat[1] / 1e6, (unsigned long long)pbc_jit_stat[2], (unsigned long long)pbc_jit_stat[3]);
    for (int i = 0; i < 2; i++) {
        picocalc_sim_log("stats: core %d: %.2f G host cycles (%.2f per emulated s), %.0f M instructions",
                         i, stat_host[i] / 1e9, stat_host[i] / 1e9 / emu_s,
                         (double)(chip.core[i].insns - stat_insns0[i]) / 1e6);
        picocalc_sim_log("stats: core %d: native %.0f M, block_run %.0f M, stepped %.0f M (%.0f M outside flash/SRAM)",
                         i, pbc_stat[i][0] / 1e6, pbc_stat[i][1] / 1e6, pbc_stat[i][2] / 1e6, pbc_stat[i][3] / 1e6);
    }
}

// From the script's "stats" command: print and start counting again (machine locked).
void machine_stats_mark(void)
{
    extern uint64_t pbc_stat[2][4], link_stat[4];
    if (stats_on == 1)
        machine_print_stats();
    stats_on = 1;
    stat_t0 = chip.now;
    stat_wall0 = picocalc_sim_mono_ns();
    memset(stat_host, 0, sizeof(stat_host));
    for (int i = 0; i < 2; i++)
        stat_insns0[i] = chip.core[i].insns;
    memset(pbc_stat, 0, sizeof(pbc_stat));
    memset(link_stat, 0, sizeof(link_stat));
}

static void run_chunk(uint64_t target)
{
    chip_t *m = &chip;
    if (stats_on < 0) {
        const char *e = getenv("PBCONPC_STATS");
        stats_on = e && *e == '1';
        stat_t0 = m->now;
    }
    m->until = target;   // may move in while the cores run (see chip.h)
    for (int i = 0; i < 2; i++) {
        cpu_t *c = &m->core[i];
        if (c->state == CPU_OFF || c->state == CPU_LOCKUP || c->state == CPU_STALL) {
            if (c->t < m->until)
                c->t = m->until;
            continue;
        }
        uint64_t h0 = stats_on == 1 ? host_tsc() : 0;
        if (c->t < m->until)
            cpu_run(c, m->until);
        if (stats_on == 1)
            stat_host[i] += host_tsc() - h0;
        if (atomic_load_explicit(&gdb_halt_req, memory_order_relaxed)) {
            m->until = 0;
            return;   // PBConPC: a debugger stops the machine where the core stopped
        }
        if (c->state != CPU_RUN && c->t < m->until)
            c->t = m->until;
        if (c->t > m->now)
            m->now = c->t;
    }
    if (m->now < m->until)
        m->now = m->until;
    m->until = 0;
}

static bool cores_idle(void)
{
    for (int i = 0; i < 2; i++)
        if (chip.core[i].state == CPU_RUN)
            return false;
    return true;
}

static void fire_events(void)
{
    chip_t *m = &chip;
    while (m->events && m->events->when <= m->now) {
        emu_event_t *ev = m->events;
        m->events = ev->next;
        ev->queued = false;
        ev->next = NULL;
        ev->fn(m, ev);
    }
}

static void run_slice(void)
{
    chip_t *m = &chip;
    uint64_t end = m->now + SLICE_PS;
    if (stop_at_ps && end > stop_at_ps)
        end = stop_at_ps;
    uint64_t hold = atomic_load(&hold_ps);   // a script's clock (script_wait)
    if (hold && end > hold)
        end = hold;
    while (m->now < end && !atomic_load(&stop_flag) && !atomic_load_explicit(&gdb_halt_req, memory_order_relaxed)) {
        static uint64_t chunk_ps;   // PBCONPC_CHUNK_NS: a fixed length, for measurements
        if (!chunk_ps) {
            const char *e = getenv("PBCONPC_CHUNK_NS");
            chunk_ps = e && atoi(e) > 0 ? (uint64_t)atoi(e) * 1000 : 1;
        }
        uint64_t target = m->now + (chunk_ps > 1 ? chunk_ps : m->now < m->fine_until ? CHUNK_FINE_PS : CHUNK_PS);
        if (m->events && m->events->when < target)
            target = m->events->when > m->now ? m->events->when : m->now + 1;
        if ((stop_at_ps || hold) && target > end)
            target = end;
        run_chunk(target);
        link_publish(m->now);
        profile_sample(m);
        fire_events();
        if (cores_idle()) {
            // Nothing runs until the next event, so emulated time can jump straight to it.
            uint64_t next = m->events ? m->events->when : end;
            if (next > end)
                next = end;
            if (next > m->now) {
                m->now = next;
                for (int i = 0; i < 2; i++)
                    if (m->core[i].t < next)
                        m->core[i].t = next;
            }
            fire_events();
            for (int i = 0; i < 2; i++)
                if (m->core[i].state == CPU_WFI || m->core[i].state == CPU_WFE)
                    cpu_poke(&m->core[i]);
            for (int i = 0; i < 2; i++) {
                cpu_t *c = &m->core[i];
                if ((c->state == CPU_WFI || c->state == CPU_WFE) && cpu_irq_wakes(c))
                    break;
            }
        }
        if (atomic_load(&m->req_any))
            break;
    }
}

static void *machine_main(void *arg)
{
    (void)arg;
    uint64_t host0 = picocalc_sim_mono_ns(), emu0 = chip.now;
    uint64_t last_save = host0, speed_host = host0, speed_emu = chip.now;
    pthread_mutex_lock(&lock);
    while (!atomic_load(&stop_flag)) {
        handle_requests();
        picocalc_sim_state_t s = atomic_load(&state);
        if (atomic_load(&gdb_halt_req)) {
            // PBConPC: held for a debugger, which looks at the chip meanwhile
            atomic_store(&gdb_held, true);
            pthread_mutex_unlock(&lock);
            picocalc_sim_sleep_ns(1000000ull);
            pthread_mutex_lock(&lock);
            host0 = picocalc_sim_mono_ns();
            emu0 = chip.now;
            continue;
        }
        if (s != PICOCALC_SIM_RUNNING) {
            pthread_mutex_unlock(&lock);
            picocalc_sim_sleep_ns(10 * 1000000ull);
            pthread_mutex_lock(&lock);
            host0 = picocalc_sim_mono_ns();
            emu0 = chip.now;
            continue;
        }
        uint64_t hold = atomic_load(&hold_ps);
        if (hold && chip.now >= hold) {
            // PBConPC: at the script's clock: wait for its next command (script_wait)
            atomic_store(&held_ps, hold);
            pthread_mutex_unlock(&lock);
            link_publish_now(chip.now);
            picocalc_sim_sleep_ns(20000ull);
            pthread_mutex_lock(&lock);
            host0 = picocalc_sim_mono_ns();
            emu0 = chip.now;
            continue;
        }
        run_slice();
        atomic_store(&usb_boot, chip.real_rom && chip.core[0].ns && chip.core[0].r[15] < 0x8000u);
        pinwave_advance(&chip);
        if (restart_at_ps && chip.now >= restart_at_ps) {
            restart_at_ps = 0;
            picocalc_sim_request_restart();
        }
        if (stop_at_ps && chip.now >= stop_at_ps) {
            atomic_store(&stop_flag, true);
            picocalc_sim_request_quit(0);
            break;
        }
        pthread_mutex_unlock(&lock);

        uint64_t host = picocalc_sim_mono_ns();
        // PBConPC: the link cable's other end must not wait out this side's pause
        link_publish_now(chip.now);
        if (!picocalc_sim_opts.unthrottled) {
            uint64_t want = host0 + (chip.now - emu0) / PS_PER_NS;
            if (want > host + 200000)
                picocalc_sim_sleep_ns(want - host);
            else if (host > want + 100000000ull) {
                host0 = host;
                emu0 = chip.now;
            }
        }
        if (host - speed_host > 1000000000ull) {
            speed = (double)(chip.now - speed_emu) / PS_PER_NS / (double)(host - speed_host);
            speed_host = host;
            speed_emu = chip.now;
        }
        for (int spins = 0; atomic_load(&lock_waiters) && spins < 1000; spins++)
            sched_yield();
        pthread_mutex_lock(&lock);
        if (host - last_save > 2000000000ull) {
            flash_save();
            last_save = host;
        }
    }
    flash_save();
    machine_print_stats();
    pthread_mutex_unlock(&lock);
    return NULL;
}

// The RP2040's boot ROM can't be shipped with the emulator, so it comes from a file: --rom, or
// rp2040-rom.elf (or .bin) in the data directory.
static bool load_rp2040_rom(void)
{
    char path[4096], err[512];
    const char *p = picocalc_sim_opts.rom_path;
    if (!p) {
        picocalc_sim_data_path(path, sizeof(path), "rp2040-rom.elf", false);
        FILE *fp = fopen(path, "rb");
        if (!fp)
            picocalc_sim_data_path(path, sizeof(path), "rp2040-rom.bin", false);
        else
            fclose(fp);
        p = path;
    }
    if (rp2040_load_rom(&chip, p, err, sizeof(err)))
        return true;
    picocalc_sim_log("%s", err);
    picocalc_sim_log("the RP2040 boards run the chip's own boot ROM: download b2.elf from "
              "https://github.com/raspberrypi/pico-bootrom-rp2040/releases and pass it with --rom, or save it as %s",
              picocalc_sim_opts.rom_path ? "rp2040-rom.elf in the data directory" : p);
    return false;
}

static const board_info_t *pick_board(const uf2_file_t *f)
{
    if (picocalc_sim_opts.board) {
        const board_info_t *b = board_find(picocalc_sim_opts.board);
        if (!b)
            picocalc_sim_log("unknown board %s", picocalc_sim_opts.board);
        return b;
    }
    const char *hint = f ? uf2_board_hint(f) : NULL;
    bool rp2040 = f && f->family == UF2_FAMILY_RP2040;
    // PBConPC: an RP2350 image runs on the PicoBoy unless --board says otherwise; pbc-plus
    // firmwares are built for the pico2 board definition and say so.
    if (!rp2040 && !picocalc_sim_opts.default_board)
        return board_find("pbcplus");
    if (hint) {
        const board_info_t *b = board_find(hint);
        if (b && b->rp2040 == rp2040)
            return b;
    }
    const board_info_t *b = picocalc_sim_opts.default_board ? board_find(picocalc_sim_opts.default_board) : NULL;
    if (!b || (f && b->rp2040 != rp2040))
        b = board_find(rp2040 ? "pico" : "pbcplus");
    if (hint)
        picocalc_sim_log("image was built for board %s; running it on a %s", hint, b->name);
    return b;
}

// Takes down whatever picocalc_sim_machine_start set up, in the order nothing freed is used again.
static void teardown(void)
{
    // The frontend's other threads look at picocalc_sim_picocalc with the machine locked.
    picocalc_sim_machine_lock();
    picocalc_board_free();
    picocalc_sim_machine_unlock();
    if (chip_ready) {
        cyw43_free(&chip);
        usbhost_free(&chip);
    }
    devices_detach_all();
    if (net_up)
        picocalc_sim_net_shutdown();
    net_up = false;
    // Also after a failed start, which would otherwise lose what flash_load took out.
    if (chip_ready && !flash_path) {
        uint8_t **kept = &kept_flash[chip.board - board_table];
        free(*kept);
        *kept = chip.flash;
        chip.flash = NULL;
    }
    if (chip_ready)
        chip_free(&chip);
    chip_ready = false;
    pthread_mutex_lock(&uf2_lock);
    pending_uf2[0] = 0;
    pthread_mutex_unlock(&uf2_lock);
}

static bool start(uf2_file_t *f)
{
    const char *stop_ms = getenv("PICOCALC_SIM_STOP_AT_MS");
    stop_at_ps = stop_ms && *stop_ms ? (uint64_t)(atof(stop_ms) * PS_PER_MS) : 0;
    static bool restart_at_read;
    if (!restart_at_read) {
        restart_at_read = true;
        const char *restart_ms = getenv("PICOCALC_SIM_RESTART_AT_MS");
        if (restart_ms && *restart_ms)
            restart_at_ps = (uint64_t)(atof(restart_ms) * PS_PER_MS);
    }
    if (hold_mode) {
        atomic_store(&hold_ps, 1);
        atomic_store(&held_ps, 0);
    }
    atomic_store(&stop_flag, false);
    atomic_store(&power_button, false);
    set_state(PICOCALC_SIM_RUNNING, "");
    speed = 1.0;

    char err[256];
    if (picocalc_sim_opts.uf2_path && !uf2_load(picocalc_sim_opts.uf2_path, f, err, sizeof(err))) {
        picocalc_sim_log("%s", err);
        return false;
    }
    bool have_uf2 = f->blocks != NULL;
    const board_info_t *b = pick_board(have_uf2 ? f : NULL);
    if (!b)
        return false;
    chip_init(&chip, b);
    chip_ready = true;
    if (picocalc_sim_opts.usb)
        usbhost_init(&chip);
    chip.trace = picocalc_sim_opts.trace;
    g_fast_bus = picocalc_sim_opts.fast;
    if (!picocalc_sim_opts.flash_path) {
        // PBConPC: a second PBConPC (two PicoBoys on the link cable) takes BOARD-2.flash, made
        // the first time as a copy of the first one's, so both have the same firmware and games.
        char file[64], first[4096];
        snprintf(file, sizeof(file), "%s.flash", b->name);
        picocalc_sim_data_path(first, sizeof(first), file, true);
        snprintf(flash_file, sizeof(flash_file), "%s", first);
        for (int n = 2; !claim_flash(flash_file) && n <= 9; n++) {
            snprintf(file, sizeof(file), "%s-%d.flash", b->name, n);
            picocalc_sim_data_path(flash_file, sizeof(flash_file), file, true);
            copy_if_missing(first, flash_file);
        }
        flash_path = flash_file;
    } else if (!strcmp(picocalc_sim_opts.flash_path, "none")) {
        flash_path = NULL;
    } else {
        snprintf(flash_file, sizeof(flash_file), "%s", picocalc_sim_opts.flash_path);
        flash_path = flash_file;
    }
    flash_load();
    if (b->rp2040 && !load_rp2040_rom())
        return false;
    // Raspberry Pi's A4 ROM unless another is named, or picocalc-sim's own stand-in.
    const char *rp2350_rom = picocalc_sim_opts.rp2350_rom_path ? picocalc_sim_opts.rp2350_rom_path : "A4";
    if (!b->rp2040 && strcmp(rp2350_rom, "stand-in") != 0) {
        char err[512];
        if (!rp2350_load_rom(&chip, rp2350_rom, err, sizeof(err))) {
            picocalc_sim_log("%s", err);
            return false;
        }
        if (rp2350_builtin_rom(rp2350_rom))
            picocalc_sim_log("boot ROM: Raspberry Pi's RP2350-A%c, built in", rp2350_rom[1]);
        else
            picocalc_sim_log("boot ROM: %s", rp2350_rom);
    } else if (!b->rp2040) {
        picocalc_sim_log("boot ROM: picocalc-sim's stand-in");
    }
    picocalc_sim_machine_lock();
    bool board = picocalc_board_init();
    picocalc_sim_machine_unlock();
    if (!board)
        return false;
    // The card is in the slot when the PicoCalc is switched on, as a fresh launch has it.
    if (picocalc_sim_opts.card_path && !sdcard_insert(picocalc_sim_picocalc.sd, picocalc_sim_opts.card_path))
        picocalc_sim_log("continuing with the card slot empty");
    if (b->wifi && !picocalc_sim_opts.no_wifi) {
        net_up = true;
        if (!picocalc_sim_net_init())
            return false;
        cyw43_init(&chip);
    }
    if (have_uf2 && !f->ram) {
        if (!uf2_program(&chip, f, err, sizeof(err))) {
            picocalc_sim_log("%s", err);
            return false;
        }
        picocalc_sim_log("wrote %s to %s flash", picocalc_sim_opts.uf2_path, b->name);
        flash_save();
    }
    picocalc_sim_log("%s: %s, %u MB flash%s%s", b->name, b->rp2040 ? "RP2040" : "RP2350", b->flash_size >> 20,
              b->psram_size ? ", 8 MB PSRAM" : "",
              !b->wifi ? "" : picocalc_sim_opts.no_wifi ? ", CYW43439 left out (--no-wifi)" : ", CYW43439 WiFi");
    if (flash_path)
        picocalc_sim_log("flash image: %s", flash_path);
    uint32_t boot_type = chip.boot_type;
    uint32_t p0 = chip.boot_param[0], p1 = chip.boot_param[1];
    chip_power_on(&chip);
    chip.boot_type = boot_type;
    chip.boot_param[0] = p0;
    chip.boot_param[1] = p1;
    // A RAM image goes in once power-on has given SRAM its random contents, as BOOTSEL's drive
    // would put it there, and the board then resets without losing it.
    if (have_uf2 && f->ram) {
        if (!uf2_program(&chip, f, err, sizeof(err))) {
            picocalc_sim_log("%s", err);
            return false;
        }
        picocalc_sim_log("loaded %s into %s RAM", picocalc_sim_opts.uf2_path, b->name);
        chip_reset(&chip, true);
    }
    if (pthread_create(&thread, NULL, machine_main, NULL) != 0) {
        picocalc_sim_log("cannot start the emulation thread");
        return false;
    }
    return true;
}

bool picocalc_sim_machine_running(void)
{
    return running;
}

bool picocalc_sim_machine_start(void)
{
    if (running)
        return true;
    static bool lock_ready;
    if (!lock_ready) {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&lock, &attr);
        pthread_mutexattr_destroy(&attr);
        lock_ready = true;
    }
    uf2_file_t f = {0};
    bool ok = start(&f);
    uf2_free(&f);
    if (!ok) {
        teardown();
        return false;
    }
    running = true;
    atomic_fetch_add(&starts, 1);
    return true;
}

static uint64_t fnv1a(uint64_t h, const void *data, size_t len)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// Everything a change to the CPU core could disturb: registers, time and instruction counts on
// both cores, and the whole of SRAM.
static uint64_t state_digest(void)
{
    uint64_t h = 0xcbf29ce484222325ull;
    h = fnv1a(h, &chip.now, sizeof(chip.now));
    for (int i = 0; i < 2; i++) {
        const cpu_t *c = &chip.core[i];
        h = fnv1a(h, c->r, sizeof(c->r));
        h = fnv1a(h, &c->apsr, sizeof(c->apsr));
        h = fnv1a(h, &c->ipsr, sizeof(c->ipsr));
        h = fnv1a(h, &c->itstate, sizeof(c->itstate));
        h = fnv1a(h, &c->state, sizeof(c->state));
        h = fnv1a(h, &c->t, sizeof(c->t));
        h = fnv1a(h, &c->insns, sizeof(c->insns));
        h = fnv1a(h, c->s, sizeof(c->s));
        h = fnv1a(h, &c->fpscr, sizeof(c->fpscr));
    }
    return fnv1a(h, chip.sram, sizeof(chip.sram));
}

void picocalc_sim_machine_stop(void)
{
    if (!running)
        return;
    atomic_store(&stop_flag, true);
    pthread_join(thread, NULL);
    running = false;
    if (chip.trace) {
        picocalc_sim_log("stopped at %.3f s emulated, clk_sys %llu Hz", (double)chip.now / PS_PER_S,
                  (unsigned long long)chip.sys_hz);
        for (int i = 0; i < 2; i++) {
            picocalc_sim_log("core %d: %llu instructions", i, (unsigned long long)chip.core[i].insns);
            cpu_dump(&chip.core[i]);
        }
        picocalc_sim_log("state digest %016llx", (unsigned long long)state_digest());
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++) {
                pio_sm_t *sm = &chip.pio[i].sm[j];
                if (chip.pio[i].ctrl & (1u << j))
                    picocalc_sim_log("pio%d sm%d pc=%u x=%08x y=%08x osr=%d isr=%d tx=%d rx=%d%s", i, j, sm->pc, sm->x, sm->y,
                              sm->osr_count, sm->isr_count, sm->txn, sm->rxn, sm->stalled ? " stalled" : "");
            }
        for (int i = 0; i < 16; i++) {
            dma_ch_t *c = &chip.dma.ch[i];
            if (c->ctrl & (1u << 26))
                picocalc_sim_log("dma%d busy ctrl=%08x read=%08x write=%08x count=%08x", i, c->ctrl, c->read_addr,
                          c->write_addr, c->trans_count);
        }
    }
    teardown();
}

bool picocalc_sim_machine_restart(void)
{
    picocalc_sim_machine_stop();
    return picocalc_sim_machine_start();
}

void picocalc_sim_request_restart(void)
{
    atomic_store(&restart_flag, true);
}

bool picocalc_sim_restart_requested(void)
{
    return atomic_exchange(&restart_flag, false);
}

unsigned picocalc_sim_machine_starts(void)
{
    return atomic_load(&starts);
}
