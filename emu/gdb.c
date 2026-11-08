#include "gdb.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../sim/net/sock.h"
#ifndef _WIN32
#include <netinet/tcp.h>
#endif
#include "chip.h"
#include "picocalc-sim/sim.h"

// See gdb.h. The protocol: https://sourceware.org/gdb/current/onlinedocs/gdb.html/Remote-Protocol.html

atomic_bool gdb_halt_req, gdb_held;
volatile bool gdb_slow;

#define MAX_BP 64
static uint32_t bp[MAX_BP];
static int nbp;
static volatile int step_core = -1;
static uint32_t skip_pc[2] = {~0u, ~0u};
static volatile int stop_core;

static sock_t listen_fd = SOCK_NONE, client = SOCK_NONE;
static pthread_t thread;
static atomic_bool quit;
static bool noack;
static int reg_core;   // the core 'g', 'p' and 'm' look at (Hg)
static int run_core;   // the core 's' steps (Hc)

static void update_slow(void)
{
    gdb_slow = nbp > 0 || step_core >= 0;
}

static void request_stop(int core)
{
    stop_core = core;
    atomic_store(&gdb_halt_req, true);
}

bool gdb_before_step(struct cpu *c)
{
    if (atomic_load_explicit(&gdb_halt_req, memory_order_relaxed))
        return true;
    uint32_t pc = c->r[15] & ~1u;
    if (pc == skip_pc[c->id]) {
        skip_pc[c->id] = ~0u;
        return false;
    }
    for (int i = 0; i < nbp; i++)
        if (bp[i] == pc) {
            request_stop(c->id);
            return true;
        }
    return false;
}

bool gdb_after_step(struct cpu *c)
{
    if (step_core == c->id) {
        step_core = -1;
        update_slow();
        request_stop(c->id);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- chip access (machine held)

static chip_t *M(void)
{
    return g_chip;
}

static bool mem_read8(uint32_t a, uint8_t *v)
{
    chip_t *m = M();
    uint8_t *p = m->rpage[a >> 16];
    if (p) {
        *v = p[a & 0xffff];
        return true;
    }
    if (a >= 0x10000000u && a < 0x14000000u && (a & 0x00ffffffu) < m->flash_size) {
        *v = m->flash[a & 0x00ffffffu];
        return true;
    }
    if ((a >> 28) == 4 || (a >> 28) == 5 || (a >> 28) == 0xd || (a >> 28) == 0xe) {
        // registers: one bus read of the word, as a probe would make
        uint32_t w = bus_read(&m->core[reg_core], a & ~3u, 4);
        *v = (uint8_t)(w >> (8 * (a & 3)));
        return true;
    }
    return false;
}

static bool mem_write8(uint32_t a, uint8_t v)
{
    chip_t *m = M();
    if (a >= 0x10000000u && a < 0x14000000u && (a & 0x00ffffffu) < m->flash_size) {
        uint32_t o = a & 0x00ffffffu;
        m->flash[o] = v;
        if (!m->flash_dirty) {
            m->flash_dirty = true;
            m->flash_dirty_lo = o;
            m->flash_dirty_hi = o + 1;
        } else {
            if (o < m->flash_dirty_lo)
                m->flash_dirty_lo = o;
            if (o + 1 > m->flash_dirty_hi)
                m->flash_dirty_hi = o + 1;
        }
        return true;
    }
    uint8_t *p = m->wpage[a >> 16];
    if (p) {
        p[a & 0xffff] = v;
        return true;
    }
    return false;
}

static void code_changed(void)
{
    chip_t *m = M();
    m->code_flush = true;
    for (int i = 0; i < 2; i++)
        cpu_poke(&m->core[i]);
}

static bool psp_active(const cpu_t *c)
{
    return c->ipsr == 0 && (c->control & 2);
}

// The register list target.xml announces, in its order.
enum { R_XPSR = 16, R_MSP, R_PSP, R_PRIMASK, R_BASEPRI, R_FAULTMASK, R_CONTROL, R_D0, R_FPSCR = R_D0 + 16, R_COUNT };

static int reg_size(int n)
{
    return n >= R_D0 && n < R_FPSCR ? 8 : 4;
}

static uint64_t reg_get(cpu_t *c, int n)
{
    if (n < 16)
        return c->r[n];
    switch (n) {
    case R_XPSR: return (c->apsr & 0xf80f0000u) | (c->ipsr & 0x1ffu) | (1u << 24);
    case R_MSP: return psp_active(c) ? c->msp : c->r[13];
    case R_PSP: return psp_active(c) ? c->r[13] : c->psp;
    case R_PRIMASK: return c->primask;
    case R_BASEPRI: return c->basepri;
    case R_FAULTMASK: return c->faultmask;
    case R_CONTROL: return c->control;
    case R_FPSCR: return c->fpscr;
    default:
        if (n >= R_D0 && n < R_FPSCR) {
            int d = n - R_D0;
            return c->s[2 * d] | (uint64_t)c->s[2 * d + 1] << 32;
        }
        return 0;
    }
}

static void reg_set(cpu_t *c, int n, uint64_t v)
{
    uint32_t w = (uint32_t)v;
    if (n < 16) {
        c->r[n] = n == 15 ? w & ~1u : w;
        return;
    }
    switch (n) {
    case R_XPSR: c->apsr = (c->apsr & ~0xf80f0000u) | (w & 0xf80f0000u); break;
    case R_MSP: if (psp_active(c)) c->msp = w; else c->r[13] = w; break;
    case R_PSP: if (psp_active(c)) c->r[13] = w; else c->psp = w; break;
    case R_PRIMASK: c->primask = w & 1; break;
    case R_BASEPRI: c->basepri = w & 0xff; break;
    case R_FAULTMASK: c->faultmask = w & 1; break;
    case R_CONTROL: c->control = w; break;
    case R_FPSCR: c->fpscr = w; break;
    default:
        if (n >= R_D0 && n < R_FPSCR) {
            int d = n - R_D0;
            c->s[2 * d] = w;
            c->s[2 * d + 1] = (uint32_t)(v >> 32);
        }
        break;
    }
    cpu_poke(c);
}

static const char target_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\"><target version=\"1.0\">"
    "<architecture>arm</architecture>"
    "<feature name=\"org.gnu.gdb.arm.m-profile\">"
    "<reg name=\"r0\" bitsize=\"32\"/><reg name=\"r1\" bitsize=\"32\"/><reg name=\"r2\" bitsize=\"32\"/>"
    "<reg name=\"r3\" bitsize=\"32\"/><reg name=\"r4\" bitsize=\"32\"/><reg name=\"r5\" bitsize=\"32\"/>"
    "<reg name=\"r6\" bitsize=\"32\"/><reg name=\"r7\" bitsize=\"32\"/><reg name=\"r8\" bitsize=\"32\"/>"
    "<reg name=\"r9\" bitsize=\"32\"/><reg name=\"r10\" bitsize=\"32\"/><reg name=\"r11\" bitsize=\"32\"/>"
    "<reg name=\"r12\" bitsize=\"32\"/><reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"lr\" bitsize=\"32\"/><reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>"
    "<reg name=\"xpsr\" bitsize=\"32\"/></feature>"
    "<feature name=\"org.gnu.gdb.arm.m-system\">"
    "<reg name=\"msp\" bitsize=\"32\" type=\"data_ptr\"/><reg name=\"psp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"primask\" bitsize=\"32\"/><reg name=\"basepri\" bitsize=\"32\"/>"
    "<reg name=\"faultmask\" bitsize=\"32\"/><reg name=\"control\" bitsize=\"32\"/></feature>"
    "<feature name=\"org.gnu.gdb.arm.vfp\">"
    "<reg name=\"d0\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"d1\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d2\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"d3\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d4\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"d5\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d6\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"d7\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d8\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"d9\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d10\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"d11\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d12\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"d13\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"d14\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"d15\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"fpscr\" bitsize=\"32\" type=\"int\" group=\"float\"/></feature>"
    "</target>";

// ---------------------------------------------------------------- packets

static const char hexd[] = "0123456789abcdef";

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

static uint64_t parse_hex(const char **p)
{
    uint64_t v = 0;
    int d;
    while ((d = hexval(**p)) >= 0) {
        v = v << 4 | (uint64_t)d;
        (*p)++;
    }
    return v;
}

static bool send_raw(const char *s, size_t n)
{
    while (n) {
        int k = (int)send(client, s, n, MSG_NOSIGNAL);
        if (k <= 0)
            return false;
        s += k;
        n -= (size_t)k;
    }
    return true;
}

static bool reply(const char *body)
{
    size_t n = strlen(body);
    char *buf = malloc(n + 5);
    unsigned sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += (unsigned char)body[i];
    buf[0] = '$';
    memcpy(buf + 1, body, n);
    buf[n + 1] = '#';
    buf[n + 2] = hexd[(sum >> 4) & 15];
    buf[n + 3] = hexd[sum & 15];
    bool ok = send_raw(buf, n + 4);
    free(buf);
    return ok;
}

// Reads one packet into buf; returns its length, -1 when the connection ended, -2 for a Ctrl-C.
static int read_packet(char *buf, int cap)
{
    char ch;
    for (;;) {
        int k = (int)recv(client, &ch, 1, 0);
        if (k <= 0)
            return -1;
        if (ch == 3)
            return -2;
        if (ch == '$')
            break;
    }
    int n = 0;
    for (;;) {
        int k = (int)recv(client, &ch, 1, 0);
        if (k <= 0)
            return -1;
        if (ch == '#')
            break;
        if (n < cap - 1)
            buf[n++] = ch;
    }
    char cs[2];
    if (recv(client, cs, 1, 0) <= 0 || recv(client, cs + 1, 1, 0) <= 0)
        return -1;
    if (!noack)
        send_raw("+", 1);
    buf[n] = 0;
    return n;
}

static void hex_bytes(char *out, const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++) {
        out[2 * i] = hexd[p[i] >> 4];
        out[2 * i + 1] = hexd[p[i] & 15];
    }
    out[2 * n] = 0;
}

static void put_reg(char *out, cpu_t *c, int n)
{
    uint64_t v = reg_get(c, n);
    uint8_t b[8];
    for (int i = 0; i < 8; i++)
        b[i] = (uint8_t)(v >> (8 * i));
    hex_bytes(out, b, reg_size(n));
}

static uint64_t get_le(const char **p, int size)
{
    uint64_t v = 0;
    for (int i = 0; i < size; i++) {
        int hi = hexval((*p)[0]), lo = hexval((*p)[1]);
        if (hi < 0 || lo < 0)
            break;
        v |= (uint64_t)(hi << 4 | lo) << (8 * i);
        *p += 2;
    }
    return v;
}

// Waits until the machine thread holds, or the machine is not running at all.
static void wait_held(void)
{
    for (int i = 0; i < 2000 && !atomic_load(&gdb_held) && picocalc_sim_machine_running(); i++)
        picocalc_sim_sleep_ns(1000000ull);
}

static void stop_reply(void)
{
    char s[32];
    snprintf(s, sizeof(s), "T05thread:%02x;", stop_core + 1);
    reply(s);
}

static void resume(void)
{
    chip_t *m = M();
    if (m)
        for (int i = 0; i < 2; i++)
            skip_pc[i] = m->core[i].r[15] & ~1u;
    atomic_store(&gdb_held, false);
    atomic_store(&gdb_halt_req, false);
}

// Runs until something stops the machine, watching for Ctrl-C meanwhile.
static bool run_until_stop(void)
{
    resume();
    for (;;) {
        if (atomic_load(&gdb_halt_req)) {
            wait_held();
            stop_reply();
            return true;
        }
        struct pollfd pfd = {.fd = client, .events = POLLIN};
        if (sock_poll(&pfd, 1, 20) > 0) {
            char ch;
            int k = (int)recv(client, &ch, 1, 0);
            if (k <= 0)
                return false;
            if (ch == 3)
                request_stop(stop_core);
        }
    }
}

static void handle(char *pkt, int n)
{
    static char out[0x4000 * 2 + 64];
    chip_t *m = M();
    const char *p = pkt + 1;
    switch (pkt[0]) {
    case '?':
        stop_reply();
        return;
    case 'H': {
        int t = (int)strtol(pkt + 2, NULL, 16);
        int core = t > 0 ? t - 1 : stop_core;
        if (core < 0 || core > 1)
            core = 0;
        if (pkt[1] == 'g')
            reg_core = core;
        else
            run_core = core;
        reply("OK");
        return;
    }
    case 'T':
        reply("OK");
        return;
    case 'g': {
        picocalc_sim_machine_lock();
        char *o = out;
        for (int r = 0; r < R_COUNT; r++) {
            put_reg(o, &m->core[reg_core], r);
            o += 2 * reg_size(r);
        }
        picocalc_sim_machine_unlock();
        reply(out);
        return;
    }
    case 'G': {
        picocalc_sim_machine_lock();
        for (int r = 0; r < R_COUNT && *p; r++)
            reg_set(&m->core[reg_core], r, get_le(&p, reg_size(r)));
        picocalc_sim_machine_unlock();
        reply("OK");
        return;
    }
    case 'p': {
        int r = (int)parse_hex(&p);
        if (r >= R_COUNT) {
            reply("E01");
            return;
        }
        picocalc_sim_machine_lock();
        put_reg(out, &m->core[reg_core], r);
        picocalc_sim_machine_unlock();
        reply(out);
        return;
    }
    case 'P': {
        int r = (int)parse_hex(&p);
        if (*p == '=')
            p++;
        if (r >= R_COUNT) {
            reply("E01");
            return;
        }
        picocalc_sim_machine_lock();
        reg_set(&m->core[reg_core], r, get_le(&p, reg_size(r)));
        picocalc_sim_machine_unlock();
        reply("OK");
        return;
    }
    case 'm': {
        uint32_t a = (uint32_t)parse_hex(&p);
        p++;
        uint32_t len = (uint32_t)parse_hex(&p);
        if (len > 0x4000)
            len = 0x4000;
        picocalc_sim_machine_lock();
        uint32_t i = 0;
        for (; i < len; i++) {
            uint8_t v;
            if (!mem_read8(a + i, &v))
                break;
            out[2 * i] = hexd[v >> 4];
            out[2 * i + 1] = hexd[v & 15];
        }
        picocalc_sim_machine_unlock();
        out[2 * i] = 0;
        reply(i ? out : "E01");
        return;
    }
    case 'M':
    case 'X': {
        uint32_t a = (uint32_t)parse_hex(&p);
        p++;
        uint32_t len = (uint32_t)parse_hex(&p);
        p++;
        bool ok = true;
        picocalc_sim_machine_lock();
        for (uint32_t i = 0; i < len && ok; i++) {
            uint8_t v;
            if (pkt[0] == 'M') {
                v = (uint8_t)(hexval(p[0]) << 4 | hexval(p[1]));
                p += 2;
            } else {
                v = (uint8_t)*p++;
                if (v == 0x7d)
                    v = (uint8_t)(*p++ ^ 0x20);
            }
            ok = mem_write8(a + i, v);
        }
        code_changed();
        picocalc_sim_machine_unlock();
        reply(ok ? "OK" : "E01");
        return;
    }
    case 'Z':
    case 'z': {
        if (pkt[1] != '0' && pkt[1] != '1') {
            reply("");   // watchpoints are not modelled
            return;
        }
        p = pkt + 3;
        uint32_t a = (uint32_t)parse_hex(&p) & ~1u;
        picocalc_sim_machine_lock();
        int i = 0;
        while (i < nbp && bp[i] != a)
            i++;
        if (pkt[0] == 'Z' && i == nbp && nbp < MAX_BP)
            bp[nbp++] = a;
        else if (pkt[0] == 'z' && i < nbp)
            bp[i] = bp[--nbp];
        update_slow();
        picocalc_sim_machine_unlock();
        reply(pkt[0] == 'Z' && i == MAX_BP ? "E01" : "OK");
        return;
    }
    case 'c':
    case 'C':
        run_until_stop();
        return;
    case 's':
    case 'S':
        step_core = run_core;
        update_slow();
        run_until_stop();
        return;
    case 'k':
        picocalc_sim_reboot("debugger kill");
        return;
    case 'D':
        reply("OK");
        nbp = 0;
        update_slow();
        resume();
        return;
    case 'R':
    case 'r':
        picocalc_sim_reboot("debugger reset");
        reply("OK");
        return;
    case 'v':
        if (!strncmp(pkt, "vMustReplyEmpty", 15) || !strncmp(pkt, "vCont?", 6)) {
            reply("");
            return;
        }
        if (!strncmp(pkt, "vKill", 5)) {
            picocalc_sim_reboot("debugger kill");
            reply("OK");
            return;
        }
        reply("");
        return;
    case 'q':
        if (!strncmp(pkt, "qSupported", 10)) {
            reply("PacketSize=4000;qXfer:features:read+;QStartNoAckMode+");
        } else if (!strncmp(pkt, "qXfer:features:read:target.xml:", 31)) {
            p = pkt + 31;
            size_t off = (size_t)parse_hex(&p);
            p++;
            size_t len = (size_t)parse_hex(&p), total = sizeof(target_xml) - 1;
            if (off >= total) {
                reply("l");
                return;
            }
            size_t k = total - off < len ? total - off : len;
            out[0] = k + off >= total ? 'l' : 'm';
            memcpy(out + 1, target_xml + off, k);
            out[k + 1] = 0;
            reply(out);
        } else if (!strcmp(pkt, "qfThreadInfo")) {
            reply("m1,2");
        } else if (!strcmp(pkt, "qsThreadInfo")) {
            reply("l");
        } else if (!strcmp(pkt, "qC")) {
            snprintf(out, sizeof(out), "QC%x", stop_core + 1);
            reply(out);
        } else if (!strcmp(pkt, "qAttached")) {
            reply("1");
        } else if (!strncmp(pkt, "qThreadExtraInfo,", 17)) {
            p = pkt + 17;
            int t = (int)parse_hex(&p);
            char text[32];
            snprintf(text, sizeof(text), "core %d", t - 1);
            hex_bytes(out, (const uint8_t *)text, (int)strlen(text));
            reply(out);
        } else if (!strncmp(pkt, "qRcmd,", 6)) {
            char cmd[128];
            p = pkt + 6;
            int k = 0;
            while (p[0] && p[1] && k < (int)sizeof(cmd) - 1) {
                cmd[k++] = (char)(hexval(p[0]) << 4 | hexval(p[1]));
                p += 2;
            }
            cmd[k] = 0;
            if (!strncmp(cmd, "reset", 5)) {
                picocalc_sim_reboot("debugger reset");
                const char *msg = "PicoBoy reset\n";
                char h[64] = "O";
                hex_bytes(h + 1, (const uint8_t *)msg, (int)strlen(msg));
                reply(h);
                reply("OK");
            } else {
                reply("");
            }
        } else {
            reply("");
        }
        return;
    case 'Q':
        if (!strcmp(pkt, "QStartNoAckMode")) {
            reply("OK");
            noack = true;
            return;
        }
        reply("");
        return;
    default:
        (void)n;
        reply("");
        return;
    }
}

static void serve_client(void)
{
    static char pkt[0x4000 * 2 + 256];
    noack = false;
    reg_core = run_core = 0;
    // A debugger expects the target stopped when it attaches.
    request_stop(0);
    wait_held();
    picocalc_sim_log("gdb: debugger attached; the machine holds");
    for (;;) {
        int n = read_packet(pkt, sizeof(pkt));
        if (n == -1)
            break;
        if (n == -2) {
            request_stop(stop_core);
            wait_held();
            stop_reply();
            continue;
        }
        handle(pkt, n);
    }
    nbp = 0;
    step_core = -1;
    update_slow();
    resume();
    picocalc_sim_log("gdb: debugger gone; the machine runs on");
}

static void *gdb_thread(void *arg)
{
    (void)arg;
    while (!atomic_load(&quit)) {
        struct pollfd pfd = {.fd = listen_fd, .events = POLLIN};
        if (sock_poll(&pfd, 1, 200) <= 0)
            continue;
        client = accept(listen_fd, NULL, NULL);
        if (client == SOCK_NONE)
            continue;
        int one = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
        serve_client();
        sock_close(client);
        client = SOCK_NONE;
    }
    return NULL;
}

bool gdb_start(const char *spec)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", spec && *spec ? spec : "3333");
    uint32_t ip = INADDR_LOOPBACK;
    char *port_s = buf, *colon = strrchr(buf, ':');
    if (colon) {
        *colon = 0;
        port_s = colon + 1;
        struct in_addr a;
        if (!strcmp(buf, "*"))
            ip = INADDR_ANY;
        else if (inet_pton(AF_INET, buf, &a) == 1)
            ip = ntohl(a.s_addr);
    }
    int port = atoi(port_s);
    sock_startup();
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == SOCK_NONE)
        return false;
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(ip)};
    if (bind(listen_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(listen_fd, 1) != 0) {
        picocalc_sim_log("gdb: cannot listen on port %d", port);
        sock_close(listen_fd);
        listen_fd = SOCK_NONE;
        return false;
    }
    atomic_store(&quit, false);
    pthread_create(&thread, NULL, gdb_thread, NULL);
    picocalc_sim_log("gdb: waiting on port %d: gdb-multiarch FIRMWARE.elf -ex \"target extended-remote :%d\"", port, port);
    return true;
}

void gdb_stop(void)
{
    if (listen_fd == SOCK_NONE)
        return;
    atomic_store(&quit, true);
    if (client != SOCK_NONE)
        shutdown(client, SHUT_WR);
    pthread_join(thread, NULL);
    sock_close(listen_fd);
    listen_fd = SOCK_NONE;
    resume();
}
