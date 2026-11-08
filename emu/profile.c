#include "profile.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chip.h"
#include "picocalc-sim/sim.h"

// See profile.h.

#define TABLE (1u << 16)
#define SAMPLE_EVERY 4   // us of emulated time

typedef struct {
    uint32_t pc, count;
} slot_t;

typedef struct {
    slot_t table[TABLE];
    uint64_t samples, sleeping, stopped, other;
} core_prof_t;

static core_prof_t prof[2];
static atomic_bool running;
static uint64_t next_sample;
static uint64_t t_start, t_end;

typedef struct {
    uint32_t lo, hi;
    char *name;
    const char *elf;
} sym_t;

static sym_t *syms;
static size_t nsyms, capsyms;

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

void profile_add_elf(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        picocalc_sim_log("profile: cannot open %s", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *e = malloc((size_t)size);
    if (!e || fread(e, 1, (size_t)size, f) != (size_t)size || size < 0x34 || memcmp(e, "\x7f" "ELF\x01\x01", 6)) {
        picocalc_sim_log("profile: %s is not a 32-bit little-endian ELF", path);
        free(e);
        fclose(f);
        return;
    }
    fclose(f);
    uint32_t shoff = rd32(e + 0x20);
    uint16_t shentsize = rd16(e + 0x2e), shnum = rd16(e + 0x30);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char *elf_name = strdup(base);
    size_t before = nsyms;
    for (unsigned i = 0; i < shnum; i++) {
        const uint8_t *sh = e + shoff + i * shentsize;
        if ((size_t)(sh - e) + 40 > (size_t)size || rd32(sh + 4) != 2)   // SHT_SYMTAB
            continue;
        uint32_t off = rd32(sh + 0x10), sz = rd32(sh + 0x14), link = rd32(sh + 0x18);
        const uint8_t *str_sh = e + shoff + link * shentsize;
        uint32_t stroff = rd32(str_sh + 0x10);
        for (uint32_t k = 0; k + 16 <= sz; k += 16) {
            const uint8_t *s = e + off + k;
            uint32_t value = rd32(s + 4) & ~1u, ssize = rd32(s + 8);
            if ((s[12] & 0xf) != 2 || !ssize || !value)   // STT_FUNC with a size
                continue;
            if (nsyms == capsyms) {
                capsyms = capsyms ? capsyms * 2 : 4096;
                syms = realloc(syms, capsyms * sizeof(*syms));
            }
            syms[nsyms++] = (sym_t){value, value + ssize, strdup((const char *)e + stroff + rd32(s)), elf_name};
        }
    }
    free(e);
    picocalc_sim_log("profile: %zu functions from %s", nsyms - before, path);
}

static int sym_cmp(const void *a, const void *b)
{
    const sym_t *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

// The ELF that ran most in flash during the profile: RAM functions are looked up in it first,
// since pbcemu's launcher and emulators all put theirs at the same SRAM addresses.
static const char *main_elf;

static const sym_t *find_sym(uint32_t pc)
{
    size_t lo = 0, hi = nsyms;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (syms[mid].lo <= pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    // the last function starting at or before pc that holds it, from main_elf if pc is in RAM
    bool ram = (pc >> 28) == 2;
    const sym_t *any = NULL;
    for (size_t i = lo; i-- > 0 && lo - i < 256;)
        if (pc < syms[i].hi) {
            if (!ram || !main_elf || syms[i].elf == main_elf)
                return &syms[i];
            if (!any)
                any = &syms[i];
        }
    return any;
}

void profile_start(void)
{
    memset(prof, 0, sizeof(prof));
    t_start = g_chip ? g_chip->now : 0;
    next_sample = 0;
    atomic_store(&running, true);
    picocalc_sim_log("profile: started");
}

bool profile_running(void)
{
    return atomic_load(&running);
}

static void add(core_prof_t *p, uint32_t pc)
{
    uint32_t h = (pc >> 1) * 2654435761u;
    for (uint32_t i = 0; i < TABLE; i++) {
        slot_t *s = &p->table[(h + i) & (TABLE - 1)];
        if (s->count && s->pc == pc) {
            s->count++;
            return;
        }
        if (!s->count) {
            s->pc = pc;
            s->count = 1;
            return;
        }
    }
    p->other++;   // table full
}

void profile_sample(struct chip *m)
{
    if (!atomic_load_explicit(&running, memory_order_relaxed) || m->now < next_sample)
        return;
    // by emulated time, as chunks vary in length (machine.c)
    next_sample = m->now + SAMPLE_EVERY * PS_PER_US;
    t_end = m->now;
    for (int i = 0; i < 2; i++) {
        cpu_t *c = &m->core[i];
        core_prof_t *p = &prof[i];
        p->samples++;
        if (c->state == CPU_WFI || c->state == CPU_WFE)
            p->sleeping++;
        else if (c->state != CPU_RUN)
            p->stopped++;
        else
            add(p, c->r[15] & ~1u);
    }
}

typedef struct {
    const char *name, *elf;
    uint32_t lo;
    uint64_t count;
} row_t;

static int row_cmp(const void *a, const void *b)
{
    const row_t *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

static int row_addr_cmp(const void *a, const void *b)
{
    const row_t *x = a, *y = b;
    if (x->lo != y->lo)
        return x->lo < y->lo ? -1 : 1;
    return (x->name > y->name) - (x->name < y->name);
}

static void report_core(FILE *out, int core, int log_lines)
{
    core_prof_t *p = &prof[core];
    if (!p->samples)
        return;
    // functions (or, without a symbol, 256-byte stretches of code) and their samples
    row_t *rows = calloc(TABLE + 1, sizeof(*rows));
    size_t nrows = 0;
    for (uint32_t i = 0; i < TABLE; i++) {
        slot_t *s = &p->table[i];
        if (!s->count)
            continue;
        const sym_t *sym = find_sym(s->pc);
        rows[nrows++] = (row_t){sym ? sym->name : NULL, sym ? sym->elf : NULL, sym ? sym->lo : s->pc & ~0xffu, s->count};
    }
    // merge the samples of one function: sort by address, add up neighbours
    qsort(rows, nrows, sizeof(*rows), row_addr_cmp);
    size_t w = 0;
    for (size_t i = 0; i < nrows; i++) {
        if (w && rows[w - 1].lo == rows[i].lo && rows[w - 1].name == rows[i].name)
            rows[w - 1].count += rows[i].count;
        else
            rows[w++] = rows[i];
    }
    nrows = w;
    qsort(rows, nrows, sizeof(*rows), row_cmp);
    double total = (double)p->samples;
    double busy = (double)(p->samples - p->sleeping - p->stopped);
    if (out) {
        fprintf(out, "\ncore %d: busy %.1f %%, sleeping (WFI/WFE) %.1f %%, off %.1f %%\n", core,
                100.0 * busy / total, 100.0 * p->sleeping / total, 100.0 * p->stopped / total);
        fprintf(out, "  %%time  %%busy  address   function\n");
    }
    for (size_t i = 0; i < nrows; i++) {
        char label[160];
        if (rows[i].name)
            snprintf(label, sizeof(label), "%s%s%s%s", rows[i].name, rows[i].elf ? "  [" : "",
                     rows[i].elf ? rows[i].elf : "", rows[i].elf ? "]" : "");
        else
            snprintf(label, sizeof(label), "(no symbol, %08x..%08x)", rows[i].lo, rows[i].lo + 0xff);
        double pt = 100.0 * rows[i].count / total, pb = busy > 0 ? 100.0 * rows[i].count / busy : 0;
        if (out && (pt >= 0.05 || i < 40))
            fprintf(out, " %6.2f %6.2f  %08x  %s\n", pt, pb, rows[i].lo, label);
        if ((int)i < log_lines)
            picocalc_sim_log("profile: core %d %5.1f %%  %s", core, pt, label);
    }
    if (getenv("PBCONPC_PROFILE_PCS")) {
        // the single hottest instruction addresses, for looking at loops
        for (int r = 0; r < 12; r++) {
            uint32_t best = 0, bi = 0;
            for (uint32_t i = 0; i < TABLE; i++)
                if (p->table[i].count > best) {
                    best = p->table[i].count;
                    bi = i;
                }
            if (!best)
                break;
            picocalc_sim_log("profile: core %d pc %08x %5.2f %%", core, p->table[bi].pc, 100.0 * best / total);
            p->table[bi].count = 0;
        }
    }
    picocalc_sim_log("profile: core %d busy %.1f %%, sleeping %.1f %%", core, 100.0 * busy / total,
                     100.0 * p->sleeping / total);
    free(rows);
}

void profile_stop(const char *path)
{
    if (!atomic_exchange(&running, false))
        return;
    if (nsyms)
        qsort(syms, nsyms, sizeof(*syms), sym_cmp);
    // which ELF ran most from flash
    main_elf = NULL;
    {
        const char *elfs[16];
        uint64_t counts[16] = {0};
        int nelf = 0;
        for (int c = 0; c < 2; c++)
            for (uint32_t i = 0; i < TABLE; i++) {
                slot_t *sl = &prof[c].table[i];
                if (!sl->count || (sl->pc >> 28) != 1)
                    continue;
                const sym_t *sy = find_sym(sl->pc);
                if (!sy)
                    continue;
                int k = 0;
                while (k < nelf && elfs[k] != sy->elf)
                    k++;
                if (k == nelf && nelf < 16)
                    elfs[nelf++] = sy->elf;
                if (k < 16)
                    counts[k] += sl->count;
            }
        uint64_t best = 0;
        for (int k = 0; k < nelf; k++)
            if (counts[k] > best) {
                best = counts[k];
                main_elf = elfs[k];
            }
    }
    FILE *out = path ? fopen(path, "w") : NULL;
    if (path && !out)
        picocalc_sim_log("profile: cannot write %s", path);
    double secs = (double)(t_end - t_start) / 1e12;
    if (out) {
        fprintf(out, "PBConPC profile: %.3f s of emulated time, a sample every %d us per core\n", secs,
                SAMPLE_EVERY);
        fprintf(out, "%%time: share of the core's time; %%busy: share of the time it was not sleeping\n");
    }
    for (int i = 0; i < 2; i++)
        report_core(out, i, 8);
    if (out) {
        fclose(out);
        picocalc_sim_log("profile: report in %s", path);
    }
}
