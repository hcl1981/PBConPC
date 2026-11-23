// PBConPC: native code for the Hazard3 cores, included by riscv.c. A run of instructions up to the
// next jump is translated once into host code (x86-64 or ARM64) and chained to the blocks it leads
// to. The translation keeps the interpreter's timing to the cycle, so a run gives the same state
// either way (PICOCALC_SIM_NO_JIT=1 turns it off for comparing):
//
// - The cycles left of the run are held in a host register. A block subtracts what its instructions
//   cost when it is left, and is only entered while enough are left for all of it; otherwise the
//   interpreter finishes the run.
// - What cannot be known beforehand is charged as it happens: a miss in the flash cache, the bus, a
//   mispredicted branch, the penalty for using a value a load has just produced or for jumping to a
//   32-bit instruction that straddles two words (h->ld_rd and h->jumped between blocks).
// - Anything but the common instructions is done by rv_exec, the interpreter's own code, called
//   with the registers written back; if it has touched what the run loop looks at, or the cycles no
//   longer reach the end of the block, the block is left there.
//
// This file holds what both hosts share; rvjit_x64.inc and rvjit_a64.inc write the host's
// instructions (the h_ functions).

#if (defined(__x86_64__) && (defined(__linux__) || defined(_WIN32))) || (defined(__aarch64__) && defined(__linux__))

#include "gdb.h"

void *jit_alloc_exec(size_t size, const void *hint);

#define RVJ_MAX 24
#define RVJ_SIZE (32u << 20)
#define RVJ_ROOM (24u << 10)
#define RVJ_LOOKUP 65536
#define RVJ_MAXBYTES 88

enum { RJX_DONE, RJX_CAREFUL, RJX_LINK, RJX_LOOKUP, RJX_STALE, RJX_COUNT };

// Kept before each block's code: its instructions as decoded, where each lies, the cycles those
// before it cost when nothing unforeseen happens, and the flash cache line last fetched once it
// has been.
typedef struct {
    uint32_t pc0;
    uint16_t n, p_last;
    bool xip, verify;
    rvd_t d[RVJ_MAX];
    uint32_t pc[RVJ_MAX + 1];
    uint16_t p[RVJ_MAX + 1];
    uint32_t line[RVJ_MAX];
    // Cycles beyond the first that are part of p because the instruction always takes them.
    uint8_t ex[RVJ_MAX];
} rvj_hdr_t;

typedef struct {
    uint32_t tag, pad;   // address | 1
    void *code;
} rvj_slot_t;

static struct {
    int on;
    uint8_t *start, *end, *p, *code_start;
    uint32_t gen, epoch;
    bool overflow;
    int (*enter)(cpu_t *c, void *code);
    uint8_t *x_exit[RJX_COUNT], *x_asis, *x_spill_asis, *x_lookup;
    uint64_t blocks, flushes;
} RJ = {.on = -1};

static rvj_slot_t rvj_lookup[RVJ_LOOKUP];
static bool rvj_user_off, rvj_need_flush;

// The PMP's window changed on some core: translations assume it as it was (rvj_compile).
static void rvj_guard_changed(void)
{
    rvj_need_flush = true;
}

#define CO(f) ((uint32_t)offsetof(cpu_t, f))
#define HO(f) ((uint32_t)offsetof(cpu_t, h3.f))
#define MO(f) ((uint32_t)offsetof(chip_t, f))
#define XOFF(g) (HO(x) + 4u * (g))
#define FLAGS_OFF HO(ld_rd)
#define FLAG_JUMPED 0x100u

_Static_assert(offsetof(rv_t, jumped) == offsetof(rv_t, ld_rd) + 1, "the two are read as one 16-bit value");

// ---------------------------------------------------------------------------------------------
// Labels within a block being written

#define RVJ_LABELS 256
#define RVJ_FIXUPS 512
static uint8_t *lab_at[RVJ_LABELS];
static int nlab;
static struct {
    uint8_t *at;
    int label, kind;
} fixups[RVJ_FIXUPS];
static int nfix;

static int lab_new(void)
{
    if (nlab >= RVJ_LABELS) {
        RJ.overflow = true;
        return 0;
    }
    lab_at[nlab] = NULL;
    return nlab++;
}

static void fix_add(int label, int kind)
{
    if (nfix >= RVJ_FIXUPS) {
        RJ.overflow = true;
        return;
    }
    fixups[nfix].at = RJ.p;
    fixups[nfix].label = label;
    fixups[nfix].kind = kind;
    nfix++;
}

static inline void h_bind(int l)
{
    lab_at[l] = RJ.p;
}

enum { HA_ADD, HA_SUB, HA_AND, HA_OR, HA_XOR };
enum { HS_SLL, HS_SRL, HS_SRA };
enum { HC_EQ, HC_NE, HC_LT, HC_GE, HC_LTU, HC_GEU };
enum { HE_SB, HE_SH, HE_ZH };

static uint32_t rvj_step(cpu_t *c, const rvj_hdr_t *hd, uint32_t j);
static uint32_t rvj_xip(cpu_t *c, const rvj_hdr_t *hd, uint32_t jw);

#if defined(__x86_64__)
#include "rvjit_x64.inc"
#else
#include "rvjit_a64.inc"
#endif

// ---------------------------------------------------------------------------------------------
// What native code calls

static inline bool rvj_is_load(unsigned op)
{
    return op == OP_LB || op == OP_LBU || op == OP_LH || op == OP_LHU || op == OP_LW;
}

// The cycles instruction j has been charged since it began, before this call: a penalty at the
// block's start and misses in the flash cache when it was fetched. The cycle count identifies
// both together with the instruction, since it only ever falls from block to block during a run.
static int32_t rvj_charged(const rv_t *h, int32_t lazy, uint32_t j)
{
    int32_t add = 0;
    if (j == 0 && h->jpen_rem == lazy) {
        add += h->jpen_before - lazy;
        lazy = h->jpen_before;
    }
    if (h->jx_j == j && h->jx_rem == lazy)
        add += (int32_t)h->jx_e;
    return add;
}

// PBCONPC_RVJ_STATS=1 prints, at the end, how often rv_exec was called for which instruction.
static uint64_t rvj_steps[256], rvj_xips, rvj_why[5];

static void rvj_report(void)
{
    fprintf(stderr, "[pbconpc] RISC-V JIT: %llu blocks, %llu flushes, %llu flash cache calls; rv_exec calls by instruction:",
            (unsigned long long)RJ.blocks, (unsigned long long)RJ.flushes, (unsigned long long)rvj_xips);
    for (int n = 0; n < 12; n++) {
        int best = 0;
        for (int i = 1; i < 256; i++)
            if (rvj_steps[i] > rvj_steps[best])
                best = i;
        if (!rvj_steps[best])
            break;
        fprintf(stderr, " op%d %.2fM", best, (double)rvj_steps[best] / 1e6);
        rvj_steps[best] = 0;
    }
    fprintf(stderr, "\n[pbconpc] RISC-V JIT runs: %llu untranslatable, %llu left before a charged instruction, %llu ran, %llu too "
                    "few cycles (%llu cycles in all)\n",
            (unsigned long long)rvj_why[0], (unsigned long long)rvj_why[1], (unsigned long long)rvj_why[2],
            (unsigned long long)rvj_why[3], (unsigned long long)rvj_why[4]);
}

// Runs instruction j of a block as the interpreter does, with the registers in memory. Returns 1
// when the block must be left after it, everything being accounted for, and 0 to go on.
static uint32_t rvj_step(cpu_t *c, const rvj_hdr_t *hd, uint32_t j)
{
    rv_t *h = &c->h3;
    rvj_steps[hd->d[j].op]++;
    int32_t lazy = h->jrem;
    // The bus must see the time at which the instruction began.
    c->t = h->jlimv - (uint64_t)((int64_t)(lazy + rvj_charged(h, lazy, j) - hd->p[j]) * (int64_t)h->jps);
    uint32_t pc = hd->pc[j], npc = hd->pc[j + 1], xc;
    c->r[15] = pc;
    c->insns += j;
    unsigned r = rv_exec(c, &hd->d[j], pc, &npc, &xc);
    xc -= hd->ex[j];
    lazy -= (int32_t)xc;
    if (r || j + 1 == hd->n || (xc && lazy <= hd->p_last)) {
        if (!(r & RX_TRAP))
            c->r[15] = npc;
        c->insns++;
        h->jrem = lazy - hd->p[j + 1];
        if (hd->xip)
            c->fetch_line = hd->line[j];
        return 1;
    }
    c->insns -= j;
    h->ld_rd = 0;
    h->jumped = false;
    h->jrem = lazy;
    return 0;
}

// A fetch from a flash cache line that was not the one used last in its set: jw is the instruction
// and, in bit 8, whether its second half is meant. When the miss leaves too few cycles for the
// rest of the block, the block is left before the instruction, which the interpreter then runs
// with these cycles already on its account (returns 1).
static uint32_t rvj_xip(cpu_t *c, const rvj_hdr_t *hd, uint32_t jw)
{
    rv_t *h = &c->h3;
    uint32_t j = jw & 0xff, addr = hd->pc[j] + ((jw >> 8) ? 2 : 0);
    rvj_xips++;
    int32_t before = h->jrem;
    uint32_t e = xip_cost(c->m, addr);
    c->fetch_line = addr & ~7u;
    if (!e)
        return 0;
    uint32_t earlier = h->jx_j == j && h->jx_rem == before ? h->jx_e : 0;
    if (before - (int32_t)e <= hd->p_last) {
        c->r[15] = hd->pc[j];
        c->insns += j;
        h->jrem = before + (int32_t)earlier - hd->p[j];
        h->jpre = e + earlier;
        if (j > 0) {
            h->ld_rd = rvj_is_load(hd->d[j - 1].op) ? hd->d[j - 1].rd : 0;
            h->jumped = false;
        }
        h->jforce = true;
        return 1;
    }
    h->jx_e = earlier + e;
    h->jx_j = j;
    h->jx_rem = before - (int32_t)e;
    h->jrem = before - (int32_t)e;
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Translating a block

enum { CK_CAREFUL, CK_STALE, CK_PEN, CK_XIP, CK_SLOW, CK_FLASH, CK_LINK };

static struct {
    chip_t *m;
    rvj_hdr_t *hd;
    // Some core has a PMP window, so loads and stores must look whether they fall into it.
    bool guard;
    struct {
        int kind, label, back, mid;
        uint32_t a;
        uint8_t *site;
    } cold[RVJ_MAX * 4 + 16];
    int ncold;
} K;

static int cold_add(int kind, uint32_t a, int back)
{
    int l = lab_new();
    if (K.ncold >= (int)(sizeof(K.cold) / sizeof(K.cold[0]))) {
        RJ.overflow = true;
        return l;
    }
    K.cold[K.ncold].kind = kind;
    K.cold[K.ncold].label = l;
    K.cold[K.ncold].back = back;
    K.cold[K.ncold].a = a;
    K.cold[K.ncold].site = NULL;
    K.ncold++;
    return l;
}

// The host register holding guest register g: its own, or tmp loaded with it.
static int gs(unsigned g, int tmp)
{
    if (g == 0) {
        h_mov_i(tmp, 0);
        return tmp;
    }
    if (h_greg[g] >= 0)
        return h_greg[g];
    h_ldx(tmp, g);
    return tmp;
}

// Where the result for guest register g is computed, and what stores it afterwards.
static inline int gd(unsigned g, int tmp)
{
    return g < 32 && h_greg[g] >= 0 ? h_greg[g] : tmp;
}

static inline void gw(unsigned g, int r)
{
    if (g < 32 && h_greg[g] < 0)
        h_stx(g, r);
}

// Calls rv_exec for instruction j. Where the registers are in memory afterwards, the fixed exit
// takes over; otherwise they are read back.
static void emit_step(uint32_t j, bool always_leaves)
{
    h_spill_all();
    h_rem_store(HO(jrem));
    h_call((void *)rvj_step, K.hd, j);
    if (always_leaves) {
        h_jmp_abs(RJ.x_asis);
        return;
    }
    h_br_nz_abs(H_T0, RJ.x_asis);
    h_rem_load();
    h_reload_all();
}

// The fetch of one half of instruction j from flash: nothing but two counters when the line is
// the one its set used last.
static void emit_xip(uint32_t j, uint32_t which, bool compare)
{
    uint32_t addr = K.hd->pc[j] + (which ? 2 : 0);
    int back = lab_new();
    if (compare)
        h_brc_i(HC_EQ, CO(fetch_line), addr & ~7u, back);
    h_xip_hit(K.m, addr, cold_add(CK_XIP, j | (which << 8), back));
    h_bind(back);
}

// Leaving the block after all n instructions: P cycles, the count of instructions, the line
// fetched last and what the next instruction must know about this one.
static void emit_leave(uint32_t P, uint32_t flags)
{
    const rvj_hdr_t *hd = K.hd;
    h_rem_sub(P);
    h_addc64_i(CO(insns), hd->n);
    if (hd->xip)
        h_stc_i(4, CO(fetch_line), hd->line[hd->n - 1]);
    if (flags)
        h_stc_i(2, FLAGS_OFF, flags);
}

// A jump to the block at target, through the dispatcher until that block exists.
static void emit_link(uint32_t target)
{
    int l = cold_add(CK_LINK, target, 0);
    K.cold[K.ncold - 1].site = h_link(l);
}

static void emit_load(uint32_t j, const rvd_t *d, int size, bool sign)
{
    int back = lab_new(), mid = lab_new();
    int a = gs(d->rs1, H_T0);
    h_alu_i(HA_ADD, H_T0, a, (uint32_t)d->imm);
    // What is not plain memory may still be flash whose cache line was the last used in its set.
    h_page(true, (unsigned)size - 1, K.guard, cold_add(CK_FLASH, j | ((uint32_t)size << 8), back));
    K.cold[K.ncold - 1].mid = mid;
    h_bind(mid);
    int r = gd(d->rd, H_T2);
    h_ld_host(size, sign, r);
    gw(d->rd, r);
    h_bind(back);
}

static void emit_store(uint32_t j, const rvd_t *d, int size)
{
    int back = lab_new();
    int a = gs(d->rs1, H_T0);
    h_alu_i(HA_ADD, H_T0, a, (uint32_t)d->imm);
    h_page(false, (unsigned)size - 1, K.guard, cold_add(CK_SLOW, j, back));
    int s = gs(d->rs2, H_T2);
    h_st_host(size, s);
    h_bind(back);
}

static void emit_push(uint32_t j, const rvd_t *d);
static void emit_pop(uint32_t j, const rvd_t *d, int back);

// The instructions written out natively; false for those rv_exec does.
static bool emit_plain(uint32_t j, const rvd_t *d)
{
    uint32_t pc = K.hd->pc[j];
    bool sink = d->rd == 32;
    int a, b, r;
    switch (d->op) {
    case OP_NOP: return true;
    case OP_LUI:
    case OP_AUIPC:
        if (!sink) {
            r = gd(d->rd, H_T0);
            h_mov_i(r, d->op == OP_LUI ? (uint32_t)d->imm : pc + (uint32_t)d->imm);
            gw(d->rd, r);
        }
        return true;
    case OP_ADDI:
    case OP_XORI:
    case OP_ORI:
    case OP_ANDI: {
        static const uint8_t ops[] = {[OP_ADDI] = HA_ADD, [OP_XORI] = HA_XOR, [OP_ORI] = HA_OR, [OP_ANDI] = HA_AND};
        if (sink)
            return true;
        r = gd(d->rd, H_T0);
        if (d->rs1 == 0) {
            h_mov_i(r, d->op == OP_ANDI ? 0 : (uint32_t)d->imm);
        } else {
            a = gs(d->rs1, H_T0);
            h_alu_i(ops[d->op], r, a, (uint32_t)d->imm);
        }
        gw(d->rd, r);
        return true;
    }
    case OP_SLLI:
    case OP_SRLI:
    case OP_SRAI:
        if (sink)
            return true;
        a = gs(d->rs1, H_T0);
        r = gd(d->rd, H_T0);
        h_shift_i(d->op == OP_SLLI ? HS_SLL : d->op == OP_SRLI ? HS_SRL : HS_SRA, r, a, (unsigned)d->imm & 31);
        gw(d->rd, r);
        return true;
    case OP_SLTI:
    case OP_SLTIU:
        if (sink)
            return true;
        a = gs(d->rs1, H_T0);
        r = gd(d->rd, H_T0);
        h_setcmp_i(d->op == OP_SLTI ? HC_LT : HC_LTU, r, a, d->imm);
        gw(d->rd, r);
        return true;
    case OP_ADD:
    case OP_SUB:
    case OP_AND:
    case OP_OR:
    case OP_XOR: {
        static const uint8_t ops[] = {[OP_ADD] = HA_ADD, [OP_SUB] = HA_SUB, [OP_AND] = HA_AND, [OP_OR] = HA_OR, [OP_XOR] = HA_XOR};
        if (sink)
            return true;
        r = gd(d->rd, H_T0);
        if (d->op == OP_ADD && d->rs1 == 0) {
            // mv
            b = gs(d->rs2, H_T1);
            h_mov(r, b);
        } else {
            a = gs(d->rs1, H_T0);
            b = gs(d->rs2, H_T1);
            h_alu(ops[d->op], r, a, b);
        }
        gw(d->rd, r);
        return true;
    }
    case OP_SLL:
    case OP_SRL:
    case OP_SRA:
        if (sink)
            return true;
        a = gs(d->rs1, H_T0);
        b = gs(d->rs2, H_T1);
        r = gd(d->rd, H_T0);
        h_shift(d->op == OP_SLL ? HS_SLL : d->op == OP_SRL ? HS_SRL : HS_SRA, r, a, b);
        gw(d->rd, r);
        return true;
    case OP_SLT:
    case OP_SLTU:
        if (sink)
            return true;
        a = gs(d->rs1, H_T0);
        b = gs(d->rs2, H_T1);
        r = gd(d->rd, H_T0);
        h_setcmp(d->op == OP_SLT ? HC_LT : HC_LTU, r, a, b);
        gw(d->rd, r);
        return true;
    case OP_MUL:
        if (sink)
            return true;
        a = gs(d->rs1, H_T0);
        b = gs(d->rs2, H_T1);
        r = gd(d->rd, H_T0);
        h_mul(r, a, b);
        gw(d->rd, r);
        return true;
    case OP_SH1ADD:
    case OP_SH2ADD:
    case OP_SH3ADD:
        if (sink)
            return true;
        a = gs(d->rs1, H_T0);
        b = gs(d->rs2, H_T1);
        r = gd(d->rd, H_T0);
        h_shadd(r, a, b, d->op == OP_SH1ADD ? 1 : d->op == OP_SH2ADD ? 2 : 3);
        gw(d->rd, r);
        return true;
    case OP_SEXTB:
    case OP_SEXTH:
        if (sink)
            return true;
        a = gs(d->rs1, H_T0);
        r = gd(d->rd, H_T0);
        h_ext(d->op == OP_SEXTB ? HE_SB : HE_SH, r, a);
        gw(d->rd, r);
        return true;
    case OP_PACK:
        // zext.h is pack with x0
        if (d->rs2 != 0)
            return false;
        if (sink)
            return true;
        a = gs(d->rs1, H_T0);
        r = gd(d->rd, H_T0);
        h_ext(HE_ZH, r, a);
        gw(d->rd, r);
        return true;
    case OP_PACKH:
        if (sink)
            return true;
        a = gs(d->rs1, H_T0);
        h_alu_i(HA_AND, H_T0, a, 0xff);
        b = gs(d->rs2, H_T1);
        h_alu_i(HA_AND, H_T1, b, 0xff);
        h_shift_i(HS_SLL, H_T1, H_T1, 8);
        r = gd(d->rd, H_T0);
        h_alu(HA_OR, r, H_T0, H_T1);
        gw(d->rd, r);
        return true;
    case OP_ANDN:
    case OP_ORN:
    case OP_XNOR:
        if (sink)
            return true;
        b = gs(d->rs2, H_T1);
        h_not(H_T1, b);
        a = gs(d->rs1, H_T0);
        r = gd(d->rd, H_T0);
        h_alu(d->op == OP_ANDN ? HA_AND : d->op == OP_ORN ? HA_OR : HA_XOR, r, a, H_T1);
        gw(d->rd, r);
        return true;
    case OP_BEXTI:
    case OP_BEXTMI:
        if (sink)
            return true;
        a = gs(d->rs1, H_T0);
        r = gd(d->rd, H_T0);
        h_shift_i(HS_SRL, r, a, (unsigned)d->imm & 31);
        h_alu_i(HA_AND, r, r, d->op == OP_BEXTI ? 1 : (1u << (d->imm >> 8)) - 1);
        gw(d->rd, r);
        return true;
    case OP_BSETI:
    case OP_BCLRI:
    case OP_BINVI:
        if (sink)
            return true;
        a = gs(d->rs1, H_T0);
        r = gd(d->rd, H_T0);
        if (d->op == OP_BSETI)
            h_alu_i(HA_OR, r, a, 1u << d->imm);
        else if (d->op == OP_BCLRI)
            h_alu_i(HA_AND, r, a, ~(1u << d->imm));
        else
            h_alu_i(HA_XOR, r, a, 1u << d->imm);
        gw(d->rd, r);
        return true;
    case OP_BEXT:
    case OP_BEXTM:
        if (sink)
            return true;
        a = gs(d->rs1, H_T0);
        b = gs(d->rs2, H_T1);
        r = gd(d->rd, H_T0);
        h_shift(HS_SRL, r, a, b);
        h_alu_i(HA_AND, r, r, d->op == OP_BEXT ? 1 : (1u << d->imm) - 1);
        gw(d->rd, r);
        return true;
    case OP_BSET:
    case OP_BCLR:
    case OP_BINV:
        if (sink)
            return true;
        b = gs(d->rs2, H_T1);
        h_mov_i(H_T2, 1);
        h_shift(HS_SLL, H_T2, H_T2, b);
        if (d->op == OP_BCLR)
            h_not(H_T2, H_T2);
        a = gs(d->rs1, H_T0);
        r = gd(d->rd, H_T0);
        h_alu(d->op == OP_BSET ? HA_OR : d->op == OP_BCLR ? HA_AND : HA_XOR, r, a, H_T2);
        gw(d->rd, r);
        return true;
    case OP_MIN:
    case OP_MINU:
    case OP_MAX:
    case OP_MAXU:
        if (sink)
            return true;
        a = gs(d->rs1, H_T0);
        b = gs(d->rs2, H_T1);
        r = gd(d->rd, H_T0);
        h_minmax(d->op == OP_MIN ? 0 : d->op == OP_MINU ? 1 : d->op == OP_MAX ? 2 : 3, r, a, b);
        gw(d->rd, r);
        return true;
    case OP_MULH:
    case OP_MULHSU:
    case OP_MULHU:
        if (sink)
            return true;
        a = gs(d->rs1, H_T0);
        b = gs(d->rs2, H_T1);
        r = gd(d->rd, H_T0);
        h_mulh(d->op == OP_MULH ? 0 : d->op == OP_MULHSU ? 1 : 2, r, a, b);
        gw(d->rd, r);
        return true;
    case OP_DIVU:
    case OP_REMU:
        a = gs(d->rs1, H_T0);
        b = gs(d->rs2, H_T1);
        r = gd(d->rd, H_T0);
        h_divu(d->op == OP_REMU, r, a, b);
        gw(d->rd, r);
        return true;
    case OP_ROL:
    case OP_ROR:
        if (sink)
            return true;
        a = gs(d->rs1, H_T0);
        b = gs(d->rs2, H_T1);
        r = gd(d->rd, H_T0);
        h_rot(d->op == OP_ROL, r, a, b);
        gw(d->rd, r);
        return true;
    case OP_RORI:
    case OP_CLZ:
    case OP_CTZ:
    case OP_REV8:
        if (sink)
            return true;
        a = gs(d->rs1, H_T0);
        r = gd(d->rd, H_T0);
        if (d->op == OP_RORI)
            h_rot_i(r, a, (unsigned)d->imm & 31);
        else if (d->op == OP_CLZ)
            h_clz(r, a);
        else if (d->op == OP_CTZ)
            h_ctz(r, a);
        else
            h_rev8(r, a);
        gw(d->rd, r);
        return true;
    case OP_CMMVA01S:
        // both are read before either is written
        h_mov(H_T0, gs(d->rs1, H_T0));
        h_mov(H_T1, gs(d->rs2, H_T1));
        r = gd(10, H_T0);
        h_mov(r, H_T0);
        gw(10, r);
        r = gd(11, H_T1);
        h_mov(r, H_T1);
        gw(11, r);
        return true;
    case OP_CMMVSA01: {
        unsigned r1 = (unsigned)d->imm & 0xff, r2 = ((unsigned)d->imm >> 8) & 0xff;
        a = gs(10, H_T0);
        r = gd(r1, H_T0);
        h_mov(r, a);
        gw(r1, r);
        a = gs(11, H_T0);
        r = gd(r2, H_T0);
        h_mov(r, a);
        gw(r2, r);
        return true;
    }
    case OP_CMPUSH: emit_push(j, d); return true;
    case OP_CMPOP: {
        int back = lab_new();
        emit_pop(j, d, back);
        h_bind(back);
        return true;
    }
    case OP_LB: emit_load(j, d, 1, true); return true;
    case OP_LBU: emit_load(j, d, 1, false); return true;
    case OP_LH: emit_load(j, d, 2, true); return true;
    case OP_LHU: emit_load(j, d, 2, false); return true;
    case OP_LW: emit_load(j, d, 4, false); return true;
    case OP_SB: emit_store(j, d, 1); return true;
    case OP_SH: emit_store(j, d, 2); return true;
    case OP_SW: emit_store(j, d, 4); return true;
    default: return false;
    }
}

// The registers Zcmp's rlist names and the stack it moves: see rv_exec.
static void emit_push(uint32_t j, const rvd_t *d)
{
    unsigned n = zcmp_count((uint32_t)d->imm & 15);
    int back = lab_new();
    int sp = gs(2, H_T0);
    h_alu_i(HA_SUB, H_T0, sp, 4 * n);
    h_page_stack(4 * n, K.guard, cold_add(CK_SLOW, j, back));
    for (unsigned i = 0; i < n; i++)
        h_st_at(gs(zcmp_regs[i], H_T2), 4 * i);
    sp = gs(2, H_T0);
    int r = gd(2, H_T0);
    h_alu_i(HA_SUB, r, sp, zcmp_adj((uint32_t)d->imm));
    gw(2, r);
    h_bind(back);
}

// cm.pop, and cm.popret and cm.popretz up to the return address in H_T0
static void emit_pop(uint32_t j, const rvd_t *d, int back)
{
    unsigned n = zcmp_count((uint32_t)d->imm & 15);
    uint32_t adj = zcmp_adj((uint32_t)d->imm);
    int sp = gs(2, H_T0);
    h_alu_i(HA_ADD, H_T0, sp, adj - 4 * n);
    h_page_stack(4 * n, K.guard, cold_add(CK_SLOW, j, back));
    for (unsigned i = 0; i < n; i++) {
        int r = gd(zcmp_regs[i], H_T2);
        h_ld_at(r, 4 * i);
        gw(zcmp_regs[i], r);
    }
    sp = gs(2, H_T0);
    int r = gd(2, H_T0);
    h_alu_i(HA_ADD, r, sp, adj);
    gw(2, r);
    if (d->op != OP_CMPOP) {
        if (d->op == OP_CMPOPRETZ) {
            r = gd(10, H_T0);
            h_mov_i(r, 0);
            gw(10, r);
        }
        int ra = gs(1, H_T0);
        h_alu_i(HA_AND, H_T0, ra, ~1u);
    }
}

// The cycles beyond its first that an instruction written out natively always takes.
static unsigned rvj_extra(const rvd_t *d)
{
    unsigned n;
    switch (d->op) {
    case OP_JAL:
    case OP_JALR:
    case OP_CMMVSA01:
    case OP_CMMVA01S: return 1;
    case OP_DIVU:
    case OP_REMU: return 17;
    case OP_CMPUSH:
    case OP_CMPOP: return zcmp_count((uint32_t)d->imm & 15);
    case OP_CMPOPRET:
        n = zcmp_count((uint32_t)d->imm & 15);
        return n + (n == 1 ? 2 : 1);
    case OP_CMPOPRETZ: return zcmp_count((uint32_t)d->imm & 15) + 2;
    default: return 0;
    }
}

static inline bool rvj_is_branch(unsigned op)
{
    return op >= OP_BEQ && op <= OP_BGEU;
}

// Instructions after which a block cannot go on: those that jump, and those rv_exec always leaves
// native code for.
static bool rvj_ends(unsigned op)
{
    switch (op) {
    case OP_JAL:
    case OP_JALR:
    case OP_BEQ:
    case OP_BNE:
    case OP_BLT:
    case OP_BGE:
    case OP_BLTU:
    case OP_BGEU:
    case OP_NONE:
    case OP_ILL:
    case OP_ECALL:
    case OP_EBREAK:
    case OP_MRET:
    case OP_WFI:
    case OP_BLOCK:
    case OP_UNBLOCK:
    case OP_CSRRW:
    case OP_CSRRS:
    case OP_CSRRC:
    case OP_CSRRWI:
    case OP_CSRRSI:
    case OP_CSRRCI:
    case OP_LR:
    case OP_SC:
    case OP_AMOSWAP:
    case OP_AMOADD:
    case OP_AMOXOR:
    case OP_AMOAND:
    case OP_AMOOR:
    case OP_AMOMIN:
    case OP_AMOMAX:
    case OP_AMOMINU:
    case OP_AMOMAXU:
    case OP_CMPOPRET:
    case OP_CMPOPRETZ: return true;
    default: return false;
    }
}

// Decodes the block at pc into hd. False where the interpreter must do the fetching.
static bool rvj_plan(chip_t *m, uint32_t pc, rvj_hdr_t *hd)
{
    uint8_t *page = m->rpage[pc >> 16];
    if (!page || (pc & 1))
        return false;
    memset(hd, 0, sizeof(*hd));
    hd->pc0 = pc;
    hd->xip = (pc >> 28) == 1;
    hd->verify = m->wpage[pc >> 16] != NULL;
    uint32_t a = pc, prefix = 0, n = 0;
    unsigned prev_ld = 0;
    while (n < RVJ_MAX && (a & 0xffff) <= 0xfffc && (a >> 16) == (pc >> 16) && a - pc < RVJ_MAXBYTES) {
        uint32_t w = rd32le(page + (a & 0xffff));
        const rvd_t *d = rv_decode(w);
        uint32_t len = (w & 3) == 3 ? 4 : 2;
        hd->d[n] = *d;
        hd->pc[n] = a;
        hd->p[n] = (uint16_t)prefix;
        hd->line[n] = (a + len - 2) & ~7u;
        hd->ex[n] = (uint8_t)rvj_extra(d);
        prefix += 1 + (prev_ld && (d->rs1 == prev_ld || d->rs2 == prev_ld)) + hd->ex[n];
        prev_ld = rvj_is_load(d->op) ? d->rd : 0;
        a += len;
        n++;
        if (rvj_ends(d->op))
            break;
    }
    if (!n)
        return false;
    // Code inside a PMP window is the interpreter's, which asks the PMP at every fetch.
    for (int i = 0; i < 2; i++) {
        const rv_t *h = &m->core[i].h3;
        if (m->core[i].rv && (pc - h->g_lo < h->g_len || h->g_lo - pc < a - pc))
            return false;
    }
    hd->n = (uint16_t)n;
    hd->pc[n] = a;
    hd->p[n] = (uint16_t)prefix;
    hd->p_last = hd->p[n - 1];
    return true;
}

static void emit_cold(void)
{
    const rvj_hdr_t *hd = K.hd;
    for (int i = 0; i < K.ncold; i++) {
        uint32_t a = K.cold[i].a;
        h_bind(K.cold[i].label);
        switch (K.cold[i].kind) {
        case CK_CAREFUL:
        case CK_STALE:
            h_stc_i(4, CO(r) + 60, hd->pc0);
            h_jmp_abs(RJ.x_exit[K.cold[i].kind == CK_CAREFUL ? RJX_CAREFUL : RJX_STALE]);
            break;
        case CK_PEN: {
            // H_T0 holds ld_rd in its low byte and jumped above it.
            const rvd_t *d = &hd->d[0];
            h_rem_store(HO(jpen_before));
            if ((hd->pc0 & 2) && hd->pc[1] == hd->pc0 + 4) {
                int skip = lab_new();
                h_br_i(HC_LTU, H_T0, FLAG_JUMPED, skip);
                h_rem_sub(1);
                h_bind(skip);
            }
            if (d->rs1 || d->rs2) {
                int skip = lab_new(), pen = lab_new();
                h_alu_i(HA_AND, H_T0, H_T0, 0xff);
                if (d->rs1)
                    h_br_i(HC_EQ, H_T0, d->rs1, pen);
                if (d->rs2)
                    h_br_i(HC_NE, H_T0, d->rs2, skip);
                else
                    h_jmp(skip);
                h_bind(pen);
                h_rem_sub(1);
                h_bind(skip);
            }
            h_rem_store(HO(jpen_rem));
            h_stc_i(2, FLAGS_OFF, 0);
            h_jmp(K.cold[i].back);
            break;
        }
        case CK_XIP:
            // rvj_xip leaves the registers alone, so they stay where they are.
            h_rem_store(HO(jrem));
            h_call_light((void *)rvj_xip, hd, a);
            h_br_nz_abs(H_T0, RJ.x_spill_asis);
            h_rem_load();
            h_jmp(K.cold[i].back);
            break;
        case CK_SLOW:
            emit_step(a, false);
            h_jmp(K.cold[i].back);
            break;
        case CK_FLASH: {
            int slow = lab_new();
            h_flash_page(K.m, (a >> 8) - 1, K.guard, slow);
            h_jmp(K.cold[i].mid);
            h_bind(slow);
            emit_step(a & 0xff, false);
            h_jmp(K.cold[i].back);
            break;
        }
        case CK_LINK:
            h_stc_i(4, CO(r) + 60, a);
            h_stc_ptr(HO(jsite), K.cold[i].site);
            h_jmp_abs(RJ.x_exit[RJX_LINK]);
            break;
        default: break;
        }
    }
}

static void rvj_flush(void)
{
    RJ.p = RJ.code_start;
    RJ.epoch++;
    RJ.flushes++;
    memset(rvj_lookup, 0, sizeof(rvj_lookup));
}

static inline unsigned rvj_hash(uint32_t pc)
{
    return (pc >> 1) & (RVJ_LOOKUP - 1);
}

static void *rvj_compile(chip_t *m, uint32_t pc)
{
    if ((size_t)(RJ.end - RJ.p) < RVJ_ROOM)
        rvj_flush();
    RJ.p = (uint8_t *)(((uintptr_t)RJ.p + 15) & ~(uintptr_t)15);
    rvj_hdr_t *hd = (rvj_hdr_t *)(void *)RJ.p;
    if (!rvj_plan(m, pc, hd))
        return NULL;
    uint8_t *begin = RJ.p;
    RJ.p += (sizeof(*hd) + 15) & ~(size_t)15;
    uint8_t *code = RJ.p;
    K.m = m;
    K.hd = hd;
    K.guard = (m->core[0].rv && m->core[0].h3.g_len) || (m->core[1].rv && m->core[1].h3.g_len);
    K.ncold = 0;
    nlab = nfix = 0;
    RJ.overflow = false;

    uint32_t n = hd->n;
    h_rem_br_le(hd->p_last + 2u, cold_add(CK_CAREFUL, 0, 0));
    if (hd->verify) {
        // Code in memory that can be written is compared with what was translated.
        h_verify(m->rpage[pc >> 16] + (pc & 0xffff), hd->pc[n] - pc, cold_add(CK_STALE, 0, 0));
    }
    if (hd->xip) {
        emit_xip(0, 0, true);
        if ((pc & 7) == 6 && hd->pc[1] == pc + 4)
            emit_xip(0, 1, false);
    }
    {
        int back = lab_new();
        h_ldc(2, H_T0, FLAGS_OFF);
        h_br_nz(H_T0, cold_add(CK_PEN, 0, back));
        h_bind(back);
    }
    for (uint32_t j = 0; j < n; j++) {
        const rvd_t *d = &hd->d[j];
        uint32_t ipc = hd->pc[j], len = hd->pc[j + 1] - ipc;
        if (j > 0 && hd->xip) {
            if ((ipc & ~7u) != hd->line[j - 1])
                emit_xip(j, 0, false);
            if ((ipc & 7) == 6 && len == 4)
                emit_xip(j, 1, false);
        }
        bool last = j + 1 == n;
        uint32_t P = hd->p[n];
        if (d->op == OP_JAL) {
            if (d->rd != 32) {
                int r = gd(d->rd, H_T0);
                h_mov_i(r, ipc + len);
                gw(d->rd, r);
            }
            emit_leave(P, FLAG_JUMPED);
            emit_link(ipc + (uint32_t)d->imm);
        } else if (d->op == OP_CMPOPRET || d->op == OP_CMPOPRETZ) {
            int back = lab_new();
            emit_pop(j, d, back);
            emit_leave(P, FLAG_JUMPED);
            h_jmp_abs(RJ.x_lookup);
            h_bind(back);
        } else if (d->op == OP_JALR) {
            int a = gs(d->rs1, H_T0);
            h_alu_i(HA_ADD, H_T0, a, (uint32_t)d->imm);
            h_alu_i(HA_AND, H_T0, H_T0, ~1u);
            if (d->rd != 32) {
                int r = gd(d->rd, H_T1);
                h_mov_i(r, ipc + len);
                gw(d->rd, r);
            }
            emit_leave(P, FLAG_JUMPED);
            h_jmp_abs(RJ.x_lookup);
        } else if (rvj_is_branch(d->op)) {
            static const uint8_t conds[] = {[OP_BEQ] = HC_EQ, [OP_BNE] = HC_NE,   [OP_BLT] = HC_LT,
                                            [OP_BGE] = HC_GE, [OP_BLTU] = HC_LTU, [OP_BGEU] = HC_GEU};
            int taken = lab_new(), nt = lab_new(), pred = lab_new();
            int a = gs(d->rs1, H_T0);
            if (d->rs2 == 0) {
                h_br_i(conds[d->op], a, 0, taken);
            } else {
                int b = gs(d->rs2, H_T1);
                h_br(conds[d->op], a, b, taken);
            }
            // Not taken: a cycle more if the frontend had predicted it taken.
            h_brc_i(HC_NE, HO(bp_pc), ipc, nt);
            h_rem_sub(1);
            h_stc_i(4, HO(bp_pc), 0);
            h_bind(nt);
            emit_leave(P, 0);
            emit_link(ipc + len);
            // Taken: a cycle more unless predicted, and a backward branch is remembered.
            h_bind(taken);
            h_brc_i(HC_EQ, HO(bp_pc), ipc, pred);
            if (d->imm < 0)
                h_stc_i(4, HO(bp_pc), ipc);
            emit_leave(P + 1, FLAG_JUMPED);
            emit_link(ipc + (uint32_t)d->imm);
            h_bind(pred);
            emit_leave(P, 0);
            emit_link(ipc + (uint32_t)d->imm);
        } else if (emit_plain(j, d)) {
            if (last) {
                emit_leave(P, rvj_is_load(d->op) ? d->rd : 0);
                emit_link(hd->pc[n]);
            }
        } else {
            emit_step(j, last);
        }
    }
    emit_cold();
    for (int i = 0; i < nfix; i++)
        h_resolve(fixups[i].at, lab_at[fixups[i].label], fixups[i].kind);
    if (RJ.overflow || RJ.p > RJ.end - 256) {
        RJ.p = begin;
        rvj_flush();
        return NULL;
    }
    h_code_done(begin, RJ.p);
    RJ.blocks++;
    return code;
}

static void *rvj_code(chip_t *m, uint32_t pc)
{
    rvj_slot_t *s = &rvj_lookup[rvj_hash(pc)];
    if (s->tag == (pc | 1))
        return s->code;
    void *code = rvj_compile(m, pc);
    if (code) {
        s = &rvj_lookup[rvj_hash(pc)];
        s->tag = pc | 1;
        s->code = code;
    }
    return code;
}

static bool rvj_init(void)
{
    // PBCONPC_RVJ_OFF=1 turns off only this translator, leaving the Arm cores theirs.
    const char *env = getenv("PICOCALC_SIM_NO_JIT"), *env2 = getenv("PBCONPC_RVJ_OFF");
    if ((env && *env && *env != '0') || (env2 && *env2 && *env2 != '0'))
        return false;
    uint8_t *mem = jit_alloc_exec(RVJ_SIZE, (const void *)&rv_reset);
    if (!mem)
        return false;
    RJ.start = RJ.p = mem;
    RJ.end = mem + RVJ_SIZE;
    RJ.overflow = false;
    h_fixed();
    RJ.code_start = RJ.p;
    h_code_done(RJ.start, RJ.p);
    if (getenv("PBCONPC_RVJ_STATS"))
        atexit(rvj_report);
    return !RJ.overflow;
}

static inline bool rvj_on(void)
{
    if (unlikely(RJ.on < 0))
        RJ.on = rvj_init();
    return RJ.on && !rvj_user_off;
}

void rv_jit_set(bool on)
{
    rvj_user_off = !on;
}

// Runs native code from the core's program counter for as long as it can within the cycles up to
// lim. False when the interpreter must take the next instruction: nothing is translated there,
// too few cycles are left for the block, or a block was left before an instruction it had already
// charged for.
static bool rvj_run(cpu_t *c, uint64_t lim)
{
    chip_t *m = c->m;
    rv_t *h = &c->h3;
    if (unlikely(gdb_slow))
        return false;
    if (unlikely(m->code_gen != RJ.gen || rvj_need_flush)) {
        rvj_flush();
        RJ.gen = m->code_gen;
        rvj_need_flush = false;
    }
    void *code = rvj_code(m, c->r[15]);
    if (!code) {
        rvj_why[0]++;
        return false;
    }
    uint64_t ps = m->sys_ps, B = (lim - c->t + ps - 1) / ps;
    if (B > (1u << 30))
        B = 1u << 30;
    h->jrem = (int32_t)B;
    h->jlimv = c->t + B * ps;
    h->jps = ps;
    h->jpen_rem = h->jx_rem = INT32_MIN;
    h->jforce = false;
    uint64_t n0 = c->insns;
    for (;;) {
        int why = RJ.enter(c, code);
        if (why == RJX_LINK) {
            uint32_t epoch = RJ.epoch;
            code = rvj_code(m, c->r[15]);
            if (!code)
                break;
            if (epoch == RJ.epoch)
                h_patch(h->jsite, code);
            continue;
        }
        if (why == RJX_LOOKUP) {
            code = rvj_code(m, c->r[15]);
            if (!code)
                break;
            continue;
        }
        if (why == RJX_STALE)
            rvj_flush();
        break;
    }
    c->t = h->jlimv - (uint64_t)((int64_t)h->jrem * (int64_t)ps);
    // rv_run counts every instruction as interpreted.
    pbc_stat[c->id][0] += c->insns - n0;
    pbc_stat[c->id][2] -= c->insns - n0;
    rvj_why[h->jforce ? 1 : c->insns != n0 ? 2 : 3]++;
    if (c->insns == n0 && !h->jforce)
        rvj_why[4] += (uint64_t)h->jrem;
    return !h->jforce && c->insns != n0;
}

#else

static inline bool rvj_on(void)
{
    return false;
}

static inline bool rvj_run(cpu_t *c, uint64_t lim)
{
    (void)c;
    (void)lim;
    return false;
}

void rv_jit_set(bool on)
{
    (void)on;
}

static void rvj_guard_changed(void)
{
}

#endif
