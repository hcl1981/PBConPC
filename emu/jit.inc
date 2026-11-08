// Native code for predecoded blocks on x86-64 hosts. Each block becomes straight-line machine code
// that does what block_run would do for it, so the emulation stays exactly the same as stepping.
//
// Most instructions are written out natively, with whatever can go differently (a load from a
// peripheral, a cache miss, a fault) left to the handler path, which calls the block handler as
// block_run does, with the cycle count, PC and time set up the same way. The guest registers and
// flags stay in cpu_t throughout, so a handler or an exit never has anything to write back. When a
// handler asks for a look, native code does what cpu_run would and, if nothing was taken, goes on.
// The instructions an IT instruction covers run here too, as step would run them.
//
// Time is checked once per block rather than once per instruction: on entry, a block whose fixed
// cycle costs could carry the time past the caller's limit goes instead to a careful translation
// of itself, made when first needed, which checks after every instruction as block_run does.
// Anything whose cost is only known as it runs (a cache miss, a slow memory access, a handler)
// checks again for the rest of the block once it has finished, and leaves for block_run if the
// rest might not fit.
//
// Native code checks its own block is still current when entered, as block_find would, since
// blocks jump straight to one another once linked.

#if defined(__x86_64__) && (defined(__linux__) || defined(_WIN32))
#define JIT_SUPPORTED 1
#endif

#ifdef JIT_SUPPORTED

#include <cpuid.h>

// PBConPC measurement: translations made, careful ones, flushes
uint64_t pbc_jit_stat[4], pbc_jit_why[8];

void *jit_alloc_exec(size_t size, const void *hint);

#ifndef JIT_SIZE
#define JIT_SIZE (64u << 20)
#endif
#define JIT_BLOCK_ROOM (32u << 10)
#define JIT_LOOKUP 262144   // PBConPC: was 32768, see BLK_SETS
#define JIT_PSK 256

enum { JX_DONE, JX_CAREFUL, JX_LINK, JX_LOOKUP, JX_STALE, JX_CAREFUL0 };

// Everything native code reaches through r14.
typedef struct {
    uint64_t psk[JIT_PSK]; // k core clock periods in picoseconds
    uint64_t until;
    uint8_t *site; // what a link or stale exit wants patched
    uint8_t fcond; // PBConPC: a block's last condition, set where its flags were made (K.fuse)
    uint64_t calls[8]; // measurement (PBCONPC_STATS): calls into C by kind, see emit_count_call
    uint32_t ge_mask[16]; // PBConPC: SEL's byte mask for each value of APSR.GE
    uint16_t fx_of_nzcv[16]; // PBConPC: c->fx for APSR's top four bits (fx_mode)
    uint32_t nzc_of_fx[256]; // PBConPC: APSR's N, Z and C for c->fx's low byte
    uint64_t gen[4096];  // measurement: generic calls per instruction (gen_fn, gen_hw: handler, pc)
    // ARM flags from the byte LAHF leaves in AH; nzcc has the carry inverted, for subtraction.
    uint32_t nz[256], nzc[256], nzcc[256];
    struct {
        uint32_t pc, pad;
        void *code;
    } lookup[JIT_LOOKUP];
} jit_ctx_t;

// Kept just before each block's code so a translation can be found again from its address.
typedef struct {
    uint32_t pc, gen;
    uint16_t bytes;
    uint8_t fixed;
    uint8_t code[BLK_MAX * 4];
} jit_hdr_t;

static struct {
    int on;
    uint8_t *start, *end, *p;
    uint32_t epoch;
    uint32_t ps;
    int (*enter)(cpu_t *c, jit_ctx_t *x, void *code);
    uint8_t *exit_done, *exit_careful, *exit_link, *exit_lookup, *exit_stale, *exit_careful0, *exit_notime, *lookup;
} J = {.on = -1};

static jit_ctx_t jx;

// ---------------------------------------------------------------------------------------------
// x86-64 encoding

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G };
enum { ALU_ADD, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP };
enum { SH_SHL = 4, SH_SHR = 5, SH_SAR = 7 };

#ifdef _WIN32
#define ARG0 RCX
#define ARG1 RDX
#define ARG2 R8
#define ARG3 R9
#else
#define ARG0 RDI
#define ARG1 RSI
#define ARG2 RDX
#define ARG3 RCX
#endif

#define XM_RIP (-2)
#define XM_REG (-3)   // PBConPC: not memory but the host register in disp (RG of a register kept in one)

typedef struct {
    int base, index, scale;
    int32_t disp; // the label for XM_RIP
    bool d32;     // always use a 32-bit displacement, so it can be patched
} xm_t;

static inline xm_t xm(int base, int32_t disp)
{
    return (xm_t){base, -1, 1, disp, false};
}

static inline xm_t xmi(int base, int index, int scale, int32_t disp)
{
    return (xm_t){base, index, scale, disp, false};
}

#define JIT_LABELS 1024
#define JIT_FIXUPS 2048

static uint8_t *lab_at[JIT_LABELS];
static int nlab;
static struct {
    uint8_t *site;
    int label, trailing;
} fixups[JIT_FIXUPS];
static int nfix;
static bool jit_overflow;

static int new_label(void)
{
    if (nlab == JIT_LABELS) {
        jit_overflow = true;
        return 0;
    }
    lab_at[nlab] = NULL;
    return nlab++;
}

// PBConPC: where eax was last stored to a guest register (its offset from rbx), and where that
// store ended; a load of the same register right there is left out (mov_rm). Any label bound
// there (another way in) or any other code emitted in between forgets it.
static uint8_t *st_end, *join_at;
static int32_t st_disp, rg_lo, rg_hi;

static inline void bind(int l)
{
    lab_at[l] = J.p;
    join_at = J.p;
}

static void add_fixup(uint8_t *site, int label, int trailing)
{
    if (nfix == JIT_FIXUPS) {
        jit_overflow = true;
        return;
    }
    fixups[nfix].site = site;
    fixups[nfix].label = label;
    fixups[nfix].trailing = trailing;
    nfix++;
}

static inline void e8(unsigned v)
{
    *J.p++ = (uint8_t)v;
}

static inline void e32(uint32_t v)
{
    memcpy(J.p, &v, 4);
    J.p += 4;
}

static inline void e64(uint64_t v)
{
    memcpy(J.p, &v, 8);
    J.p += 8;
}

static inline int32_t rel_to(const void *target, const uint8_t *after)
{
    return (int32_t)((intptr_t)target - (intptr_t)after);
}

static inline bool in_rel32(const void *target, const uint8_t *after)
{
    intptr_t d = (intptr_t)target - (intptr_t)after;
    return d == (int32_t)d;
}

static void rex(int w, int reg, int index, int base, bool force)
{
    unsigned r = (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((index & 8) ? 2 : 0) | ((base & 8) ? 1 : 0);
    if (r || force)
        e8(0x40 | r);
}

static void opcode(unsigned opc)
{
    if (opc > 0xff)
        e8(opc >> 8);
    e8(opc);
}

static void modrm_mem(int reg, xm_t m, int trailing)
{
    reg &= 7;
    if (m.base == XM_REG) {
        e8(0xc0 | reg << 3 | (m.disp & 7));
        return;
    }
    if (m.base == XM_RIP) {
        e8(0x05 | reg << 3);
        add_fixup(J.p, m.disp, trailing);
        e32(0);
        return;
    }
    int b = m.base & 7;
    int mod = m.d32 ? 2 : (m.disp == 0 && b != 5) ? 0 : (m.disp >= -128 && m.disp <= 127) ? 1 : 2;
    if (m.index >= 0 || b == 4) {
        int ss = m.scale == 8 ? 3 : m.scale == 4 ? 2 : m.scale == 2 ? 1 : 0;
        e8(mod << 6 | reg << 3 | 4);
        e8(ss << 6 | (m.index >= 0 ? (m.index & 7) : 4) << 3 | b);
    } else {
        e8(mod << 6 | reg << 3 | b);
    }
    if (mod == 1)
        e8((uint8_t)m.disp);
    else if (mod == 2)
        e32((uint32_t)m.disp);
}

static void op_m(int w, bool p66, unsigned opc, int reg, xm_t m, int trailing, bool force)
{
    if (p66)
        e8(0x66);
    // a register operand always gets a REX prefix, so that byte forms reach SIL, DIL and BPL
    rex(w, reg, m.index >= 0 ? m.index : 0, m.base == XM_RIP ? 0 : m.base == XM_REG ? m.disp : m.base,
        force || m.base == XM_REG);
    opcode(opc);
    modrm_mem(reg, m, trailing);
}

static void op_r(int w, bool p66, unsigned opc, int reg, int rm, bool force)
{
    if (p66)
        e8(0x66);
    rex(w, reg, 0, rm, force);
    opcode(opc);
    e8(0xc0 | (reg & 7) << 3 | (rm & 7));
}

static void mov_rr(int w, int d, int s);

static void mov_rm(int w, int r, xm_t m)
{
    if (!w && st_end == J.p && join_at != J.p && m.base == RBX && m.index < 0 && !m.d32 && m.disp == st_disp) {
        if (r != RAX)
            mov_rr(0, r, RAX);
        return;
    }
    op_m(w, false, 0x8b, r, m, 0, false);
}

static void mov_mr(int w, xm_t m, int r)
{
    op_m(w, false, 0x89, r, m, 0, false);
    if (!w && r == RAX && m.base == RBX && m.index < 0 && !m.d32 && m.disp >= rg_lo && m.disp < rg_hi) {
        st_end = J.p;
        st_disp = m.disp;
    }
}

static void mov_rr(int w, int d, int s)
{
    op_r(w, false, 0x8b, d, s, false);
}

static void mov_mi(xm_t m, uint32_t imm)
{
    op_m(0, false, 0xc7, 0, m, 4, false);
    e32(imm);
}

static void mov_ri(int r, uint32_t imm)
{
    rex(0, 0, 0, r, false);
    e8(0xb8 + (r & 7));
    e32(imm);
}

static void mov_ri64(int r, uint64_t imm)
{
    rex(1, 0, 0, r, false);
    e8(0xb8 + (r & 7));
    e64(imm);
}

static void alu_rm(int w, int op, int r, xm_t m)
{
    op_m(w, false, op * 8 + 3, r, m, 0, false);
}

static void alu_mr(int w, int op, xm_t m, int r)
{
    op_m(w, false, op * 8 + 1, r, m, 0, false);
}

static void alu_rr(int w, int op, int d, int s)
{
    op_r(w, false, op * 8 + 3, d, s, false);
}

static void alu_ri(int w, int op, int r, int32_t imm)
{
    if (imm >= -128 && imm <= 127) {
        op_r(w, false, 0x83, op, r, false);
        e8((uint8_t)imm);
    } else {
        op_r(w, false, 0x81, op, r, false);
        e32((uint32_t)imm);
    }
}

static void alu_mi(int w, int op, xm_t m, int32_t imm)
{
    if (imm >= -128 && imm <= 127) {
        op_m(w, false, 0x83, op, m, 1, false);
        e8((uint8_t)imm);
    } else {
        op_m(w, false, 0x81, op, m, 4, false);
        e32((uint32_t)imm);
    }
}

static void cmp16_mi(xm_t m, int8_t imm)
{
    op_m(0, true, 0x83, 7, m, 1, false);
    e8((uint8_t)imm);
}

static void test_rr(int a, int b)
{
    op_r(0, false, 0x85, b, a, false);
}

// TEST with an immediate, as a byte test where the register and value allow.
static void test_ri(int r, uint32_t imm)
{
    if (imm < 0x100 && r < 4) {
        op_r(0, false, 0xf6, 0, r, false);
        e8(imm);
    } else {
        op_r(0, false, 0xf7, 0, r, false);
        e32(imm);
    }
}

static void test_mi8(xm_t m, uint8_t imm)
{
    op_m(0, false, 0xf6, 0, m, 1, false);
    e8(imm);
}

static void shift_ri(int kind, int r, int n)
{
    if (n == 1) {
        op_r(0, false, 0xd1, kind, r, false);
    } else {
        op_r(0, false, 0xc1, kind, r, false);
        e8(n);
    }
}

static void imul_rm(int w, int r, xm_t m)
{
    op_m(w, false, 0x0faf, r, m, 0, false);
}

static void lea(int w, int r, xm_t m)
{
    op_m(w, false, 0x8d, r, m, 0, false);
}

static void lea_rip_abs(int r, const void *target)
{
    rex(1, r, 0, 0, false);
    e8(0x8d);
    e8(0x05 | (r & 7) << 3);
    e32(rel_to(target, J.p + 4));
}

static void movzx_ah(int d)
{
    op_r(0, false, 0x0fb6, d, 4, false);
}

static void setcc(int cc, int r)
{
    op_r(0, false, 0x0f90 + cc, 0, r, r >= 4);
}

static void bt_mi(xm_t m, int bit)
{
    op_m(0, false, 0x0fba, 4, m, 1, false);
    e8(bit);
}

static void not_r(int r)
{
    op_r(0, false, 0xf7, 2, r, false);
}

static void push_r(int r)
{
    rex(0, 0, 0, r, false);
    e8(0x50 + (r & 7));
}

static void pop_r(int r)
{
    rex(0, 0, 0, r, false);
    e8(0x58 + (r & 7));
}

static void jcc_l(int cc, int l)
{
    e8(0x0f);
    e8(0x80 + cc);
    add_fixup(J.p, l, 0);
    e32(0);
}

static void jmp_l(int l)
{
    e8(0xe9);
    add_fixup(J.p, l, 0);
    e32(0);
}

static void jcc_a(int cc, const void *t)
{
    e8(0x0f);
    e8(0x80 + cc);
    e32(rel_to(t, J.p + 4));
}

static void jmp_a(const void *t)
{
    e8(0xe9);
    e32(rel_to(t, J.p + 4));
}

static void emit_call_done(bool before);

static void call_a(const void *t)
{
    emit_call_done(true);
    if (in_rel32(t, J.p + 5)) {
        e8(0xe8);
        e32(rel_to(t, J.p + 4));
    } else {
        mov_ri64(RAX, (uint64_t)(uintptr_t)t);
        op_r(0, false, 0xff, 2, RAX, false);
    }
    // PBConPC: the call may have scheduled an event inside the chunk, which moves its end in
    // (chip.h until): r13 = min(r13, m->until). Flags are dead after a call; RAX/RDX are kept.
    // jx.until = min(jx.until, m->until), through rcx, which the call has left free
    xm_t until = xm(R15, (int32_t)offsetof(chip_t, until));
    xm_t limit = xm(R14, (int32_t)offsetof(jit_ctx_t, until));
    mov_rm(1, RCX, until);
    alu_rm(1, ALU_CMP, RCX, limit);
    op_m(1, false, 0x0f40 + CC_AE, RCX, limit, 0, false);
    mov_mr(1, limit, RCX);
    emit_call_done(false);
}

// ---------------------------------------------------------------------------------------------
// Layout of the state native code works on

#define CO(f) ((int32_t)offsetof(cpu_t, f))
#define MO(f) ((int32_t)offsetof(chip_t, f))
#define XO(f) ((int32_t)offsetof(jit_ctx_t, f))

// PBConPC: register allocation. Some guest registers live in host registers for as long as native
// code runs (reg_host, the same for every translation, so that blocks go straight on into each
// other): loaded on the way in, stored on the way out (jit_emit_fixed), and stored before and
// loaded after every call into C (emit_spill, call_a), which may read or change them and may
// clobber RSI, RDI, R10 and R11. While a call is being set up, RG gives the memory again: the
// argument registers are among the hosts.
static const uint8_t reg_hosts[] = {RBP, RSI, RDI, R10, R11, R13};
static int8_t reg_host[16];
static uint8_t reg_list[sizeof(reg_hosts)], reg_count;
static bool rg_spilled;

static inline xm_t RGM(unsigned n)
{
    return xm(RBX, CO(r) + 4 * (int32_t)n);
}

static inline xm_t RG(unsigned n)
{
    if (!rg_spilled && reg_host[n & 15] >= 0)
        return (xm_t){XM_REG, -1, 1, reg_host[n & 15], false};
    return RGM(n);
}

static void reg_map_init(void)
{
    memset(reg_host, -1, sizeof(reg_host));
    reg_count = 0;
    const char *e = getenv("PBCONPC_REGMAP");
    const char *list = e ? e : "0,1,2,3,4,13";
    if (!strcmp(list, "none"))
        return;
    for (const char *p = list; *p && reg_count < sizeof(reg_hosts);) {
        char *end;
        long g = strtol(p, &end, 10);
        if (end == p)
            break;
        if (g >= 0 && g < 15 && reg_host[g] < 0) {
            reg_host[g] = (int8_t)reg_hosts[reg_count];
            reg_list[reg_count++] = (uint8_t)g;
        }
        p = *end ? end + 1 : end;
    }
}

static void emit_reg_store_all(void)
{
    for (unsigned i = 0; i < reg_count; i++)
        op_m(0, false, 0x89, reg_host[reg_list[i]], RGM(reg_list[i]), 0, false);
}

static void emit_reg_load_all(void)
{
    for (unsigned i = 0; i < reg_count; i++)
        op_m(0, false, 0x8b, reg_host[reg_list[i]], RGM(reg_list[i]), 0, false);
}

static void emit_count_call(int kind)
{
    static int on = -1;
    if (on < 0)
        on = getenv("PBCONPC_STATS") != NULL;
    if (on)
        alu_mi(1, ALU_ADD, xm(R14, XO(calls) + 8 * kind), 1);
}

// Before setting up a call into C: the host registers into c->r (call_a loads them back after).
static void emit_fx_to_apsr(void);
static void emit_apsr_to_fx(void);
static int fx_mode;

static void emit_spill(void)
{
    if (rg_spilled)
        return;
    emit_reg_store_all();
    if (fx_mode)
        emit_fx_to_apsr();
    rg_spilled = true;
}

// call_a's checks: before the call the registers must have been spilled (a call set up without
// would lose them: the translation is given up, block_run does the block); after it they come back.
static void emit_call_done(bool before)
{
    if (before) {
        if (reg_count && !rg_spilled) {
            static bool told;
            if (!told) {
                told = true;
                emu_log("JIT: a call into C without emit_spill; that block runs in block_run");
            }
            jit_overflow = true;
        }
        return;
    }
    if (rg_spilled) {
        emit_reg_load_all();
        if (fx_mode)
            emit_apsr_to_fx();
        rg_spilled = false;
    }
}

#define APSR xm(RBX, CO(apsr))
#define FX xm(RBX, CO(fx))

// PBConPC: flags in x86 form. While native code runs, N, Z, C and V live in c->fx (fx_mode):
// byte 0 as LAHF leaves AH (SF, ZF, and CF holding NOT C, as x86 subtraction borrows), byte 1 V.
// Setting them is then a SETO, LAHF and a byte store, and testing them SAHF and an x86 condition.
// APSR gets them on the way out and before calls into C, and gives them on the way in and after.
// Only with PBCONPC_FXFLAGS=1: measured no faster.
static int fx_mode = -1;

static void emit_apsr_to_fx(void)   // c->fx from APSR, with rcx (rax untouched)
{
    mov_rm(0, RCX, APSR);
    shift_ri(SH_SHR, RCX, 28);
    op_m(0, false, 0x0fb7, RCX, xmi(R14, RCX, 2, (int32_t)offsetof(jit_ctx_t, fx_of_nzcv)), 0, false);
    mov_mr(0, FX, RCX);
}

static void emit_fx_to_apsr(void)   // APSR's N, Z, C, V from c->fx, with rcx and rdx
{
    op_m(0, false, 0x0fb6, RCX, FX, 0, false);   // movzx ecx, byte fx
    mov_rm(0, RCX, xmi(R14, RCX, 4, (int32_t)offsetof(jit_ctx_t, nzc_of_fx)));
    xm_t v = FX;
    v.disp += 1;
    op_m(0, false, 0x0fb6, RDX, v, 0, false);    // movzx edx, byte fx+1
    shift_ri(SH_SHL, RDX, 28);
    alu_rr(0, ALU_OR, RCX, RDX);
    mov_rm(0, RDX, APSR);
    alu_ri(0, ALU_AND, RDX, 0x0fffffff);
    alu_rr(0, ALU_OR, RDX, RCX);
    mov_mr(0, APSR, RDX);
}
#define PSK(k) xm(R14, XO(psk) + 8 * (int32_t)(k))

// PBConPC: k clock periods as an immediate (the period the translation is made for, J.ps)
// rather than a load from jx.psk: translations are dropped when the clock changes (jit_run,
// jit_run_code, jit_look). Only with PBCONPC_PSIMM=1 (measured no faster).
static int ps_imm = -1;

static bool ps_fits(int k)
{
    return ps_imm && (int64_t)k * J.ps <= INT32_MAX && (int64_t)k * J.ps >= INT32_MIN;
}

static void alu_ps(int op, int r, int k)   // r op= k periods
{
    if (ps_fits(k))
        alu_ri(1, op, r, (int32_t)((int64_t)k * J.ps));
    else
        alu_rm(1, op, r, PSK(k));
}

static void imul_ps(int r)   // r *= one period
{
    if (ps_fits(1)) {
        op_r(1, false, 0x69, r, r, false);
        e32((uint32_t)J.ps);
    } else {
        imul_rm(1, r, PSK(1));
    }
}

static void lea_time(int r, int k)   // r = r12 + k periods
{
    if (ps_fits(k)) {
        lea(1, r, xm(R12, (int32_t)((int64_t)k * J.ps)));
    } else {
        mov_rr(1, r, R12);
        if (k)
            alu_rm(1, ALU_ADD, r, PSK(k));
    }
}

// rbx: the core; r12: the time, less the fixed cycles of the instructions passed since it was last
// brought up to date; r13: the limit; r14: jx; r15: the chip.

// ---------------------------------------------------------------------------------------------
// Fixed code: the way in and out, and the lookup of a block from its address

static void jit_emit_fixed(void)
{
    J.p = J.start;
    J.enter = (int (*)(cpu_t *, jit_ctx_t *, void *))(void *)J.p;
    push_r(RBX);
    push_r(RBP);
#ifdef _WIN32
    push_r(RDI);
    push_r(RSI);
#endif
    push_r(R12);
    push_r(R13);
    push_r(R14);
    push_r(R15);
#ifdef _WIN32
    alu_ri(1, ALU_SUB, RSP, 40); // the callee's shadow space, keeping calls 16-byte aligned
    mov_rr(1, RBX, RCX);
    mov_rr(1, R14, RDX);
#define JIT_CODE_ARG R8
#else
    alu_ri(1, ALU_SUB, RSP, 8);
    mov_rr(1, RBX, RDI);
    mov_rr(1, R14, RSI);
#define JIT_CODE_ARG RDX
#endif
    mov_rm(1, R15, xm(RBX, CO(m)));
    mov_rm(1, R12, xm(RBX, CO(t)));
    mov_rr(1, RAX, JIT_CODE_ARG);   // the code address, before RSI and RDI become guest registers
    emit_reg_load_all();
    if (fx_mode)
        emit_apsr_to_fx();
    op_r(0, false, 0xff, 4, RAX, false);

    uint8_t *epi_time = J.p;
    mov_mr(1, xm(RBX, CO(t)), R12);
    uint8_t *epi = J.p;
    if (fx_mode)
        emit_fx_to_apsr();   // PBConPC: flags in x86 form (fx_mode); eax is kept
    emit_reg_store_all();   // PBConPC: the guest registers kept in host ones (RG)
#ifdef _WIN32
    alu_ri(1, ALU_ADD, RSP, 40);
#else
    alu_ri(1, ALU_ADD, RSP, 8);
#endif
    pop_r(R15);
    pop_r(R14);
    pop_r(R13);
    pop_r(R12);
#ifdef _WIN32
    pop_r(RSI);
    pop_r(RDI);
#endif
    pop_r(RBP);
    pop_r(RBX);
    e8(0xc3);

    static const int codes[] = {JX_DONE, JX_CAREFUL, JX_LINK, JX_LOOKUP, JX_STALE, JX_CAREFUL0};
    uint8_t **exits[] = {&J.exit_done,   &J.exit_careful, &J.exit_link,
                         &J.exit_lookup, &J.exit_stale,   &J.exit_careful0};
    for (int i = 0; i < 6; i++) {
        *exits[i] = J.p;
        mov_ri(RAX, (uint32_t)codes[i]);
        jmp_a(epi_time);
    }
    J.exit_notime = J.p;
    mov_ri(RAX, JX_DONE);
    jmp_a(epi);

    // eax: the address to go to; the hash must match jit_hash.
    J.lookup = J.p;
    mov_rr(0, RCX, RAX);
    shift_ri(SH_SHR, RCX, 17);
    mov_rr(0, RDX, RAX);
    shift_ri(SH_SHR, RDX, 1);
    alu_rr(0, ALU_XOR, RCX, RDX);
    alu_ri(0, ALU_AND, RCX, JIT_LOOKUP - 1);
    shift_ri(SH_SHL, RCX, 4);
    alu_rm(0, ALU_CMP, RAX, xmi(R14, RCX, 1, XO(lookup)));
    e8(0x75); // jne +n to the miss
    uint8_t *jne = J.p;
    e8(0);
    op_m(0, false, 0xff, 4, xmi(R14, RCX, 1, XO(lookup) + 8), 0, false);
    *jne = (uint8_t)(J.p - jne - 1);
    join_at = J.p;
    mov_mr(0, RG(15), RAX);
    jmp_a(J.exit_lookup);

    J.p = (uint8_t *)(((uintptr_t)J.p + 63) & ~(uintptr_t)63);
}

static inline unsigned jit_hash(uint32_t pc)
{
    return ((pc >> 17) ^ (pc >> 1)) & (JIT_LOOKUP - 1);
}

static uint8_t *jit_code_start;

// PBConPC measurement: where each translation starts, for tools/jitprof.py (PBCONPC_HOSTPROF
// writes the buffer and this table beside its samples at exit).
static struct { uint8_t *entry; uint32_t pc; uint16_t n; bool careful; } jit_index[1 << 18];
static unsigned jit_nindex;

static void jit_it_op(cpu_t *c, const dop_t *d);
static const void *gen_fn[4096];
static uint32_t gen_hw[4096], gen_op[4096];

void pbc_jit_gen_report(void)
{
    for (int pass = 0; pass < 20; pass++) {
        int best = -1;
        for (int i = 0; i < 4096; i++)
            if (jx.gen[i] && (best < 0 || jx.gen[i] > jx.gen[best]))
                best = i;
        if (best < 0)
            break;
        emu_log("JIT generic: %.2f M  fn %+ld from pbc_jit_gen_report, pc %08x op %08x it %d", jx.gen[best] / 1e6,
                (long)((const char *)gen_fn[best] - (const char *)(void *)pbc_jit_gen_report), gen_hw[best], gen_op[best], gen_fn[best] == (const void *)jit_it_op);
        jx.gen[best] = 0;
    }
}

void pbc_jit_calls(uint64_t out[8])
{
    memcpy(out, jx.calls, sizeof(jx.calls));
}

void pbc_jit_dump(const char *path)
{
    if (!J.start || J.p - jit_code_start <= 4096)
        return;
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    uint64_t base = (uint64_t)(uintptr_t)J.start, len = (uint64_t)(J.p - J.start);
    fwrite(&base, 8, 1, f);
    fwrite(&len, 8, 1, f);
    fwrite(J.start, 1, (size_t)len, f);
    fclose(f);
    char ip[4200];
    snprintf(ip, sizeof(ip), "%s.index", path);
    if ((f = fopen(ip, "w"))) {
        for (unsigned i = 0; i < jit_nindex; i++)
            fprintf(f, "%lx %08x %u %d\n", (unsigned long)(uintptr_t)jit_index[i].entry, jit_index[i].pc, jit_index[i].n,
                    jit_index[i].careful);
        fclose(f);
    }
}

static void jit_flush(void)
{
    pbc_jit_stat[2]++;
    // measurement: keep what was there for tools/jitprof.py
    static const char *hp = (const char *)1;
    if (hp == (const char *)1)
        hp = getenv("PBCONPC_HOSTPROF");
    if (hp && *hp && J.p - jit_code_start > 4096) {
        char jp[4200];
        snprintf(jp, sizeof(jp), "%s.jit", hp);
        pbc_jit_dump(jp);
    }
    jit_nindex = 0;
    J.p = jit_code_start;
    J.epoch++;
    memset(jx.lookup, 0, sizeof(jx.lookup));
}

static bool jit_init(void)
{
    const char *e = getenv("PICOCALC_SIM_NO_JIT");
    if (e && *e && *e != '0')
        return false;
    // The flags are read with LAHF, which a few of the earliest x86-64 processors lack.
    unsigned a, b, c, d;
    if (!__get_cpuid(0x80000001u, &a, &b, &c, &d) || !(c & 1))
        return false;
    uint8_t *mem = jit_alloc_exec(JIT_SIZE, (const void *)&cpu_run);
    if (!mem)
        return false;
    J.start = mem;
    J.end = mem + JIT_SIZE;
    reg_map_init();
    // off by default: no faster (the table's loads hit the cache), and a clock change drops
    // every translation
    ps_imm = getenv("PBCONPC_PSIMM") != NULL;
    // off by default: measured no faster (Doom 3 % slower; SAHF and the conversions around
    // calls into C cost what the cheaper setting saves)
    fx_mode = getenv("PBCONPC_FXFLAGS") != NULL;
    jit_emit_fixed();
    jit_code_start = J.p;
    for (unsigned ah = 0; ah < 256; ah++) {
        uint32_t nz = ((ah & 0x80) ? F_N : 0) | ((ah & 0x40) ? F_Z : 0);
        jx.nz[ah] = nz;
        jx.nzc[ah] = nz | ((ah & 1) ? F_C : 0);
        jx.nzcc[ah] = nz | ((ah & 1) ? 0 : F_C);
        // fx: SF bit 7, ZF bit 6, CF bit 0 holding NOT C (x86's borrow), V in the next byte
        jx.nzc_of_fx[ah] = ((ah & 0x80) ? F_N : 0) | ((ah & 0x40) ? F_Z : 0) | ((ah & 1) ? 0 : F_C);
        if (ah < 16)
            jx.fx_of_nzcv[ah] = (uint16_t)(((ah & 8) ? 0x80 : 0) | ((ah & 4) ? 0x40 : 0) | ((ah & 2) ? 0 : 1) |
                                           ((ah & 1) ? 0x100 : 0));
        if (ah < 16)
            jx.ge_mask[ah] = ((ah & 1) ? 0xffu : 0) | ((ah & 2) ? 0xff00u : 0) | ((ah & 4) ? 0xff0000u : 0) |
                             ((ah & 8) ? 0xff000000u : 0);
    }
    J.epoch = 1;
    return true;
}

static bool jit_available(void)
{
    if (J.on < 0)
        J.on = jit_init();
    return J.on;
}

static void jit_forget(void)
{
    pbc_jit_stat[3]++;
    if (J.on > 0)
        jit_flush();
}

static void jit_set_ps(uint32_t ps);

// What block_run does when a handler asks for a look: a fault is taken (it is the IT state of the
// instruction), and the time catches up with the handler's cycles at whatever clock it left
// behind. Then what cpu_run would do before running a block again, returning whether native code
// can go straight on at c->r[15].
static bool jit_look(cpu_t *c, uint32_t pc, uint32_t it)
{
    chip_t *m = c->m;
    if (c->fault)
        take_flagged_fault(c, pc, it);
    c->t += (uint64_t)c->cyc * m->sys_ps;
    if (c->t >= jx.until)
        return false;
    if (c->check_exc) {
        check_exceptions(c);
        if (c->state != CPU_RUN)
            sleep_check(c);
    }
    // An exception just taken leaves check_exc set, which block_run answers after one instruction.
    // Non-secure code is stepped, and a flush must wait until no native code is running.
    if (c->state != CPU_RUN || c->itstate || c->check_exc || c->ns || m->code_flush)
        return false;
    if (m->sys_ps != J.ps) {
        if (ps_imm)
            return false;   // native code holds the old period: leave it (jit_run_code drops it)
        jit_set_ps(m->sys_ps);
    }
    return true;
}

// An instruction an IT instruction covers, as step runs it once its first halfword is fetched.
static void jit_it_op(cpu_t *c, const dop_t *d)
{
    uint32_t it = c->itstate;
    c->r[15] = d->pc + (d->lf & LF_LEN);
    if (!cond_pass(c, it >> 4)) {
        it_advance(c);
        return;
    }
    if (d->lf & FETCH_HW2) {
        c->cyc += xip_cost(c->m, d->pc + 2);
        c->fetch_line = (d->pc + 2) & ~7u;
    }
    d->fn(c, d);
    if (!c->fault && c->itstate == it)
        it_advance(c);
}

// ---------------------------------------------------------------------------------------------
// Compiling a block

static struct {
    const block_t *b;
    chip_t *m;
    int n;
    bool fixed;
    bool careful; // checks the time after every instruction
    bool ended;   // the last instruction has sent control on
    uint8_t *pre_site; // the entry's time check, patched to go to the careful translation
    int sb[BLK_MAX + 1]; // fixed cycles before each instruction
    int total;           // fixed cycles of the whole block on its dearest path
    bool generic[BLK_MAX];
    int l_main[BLK_MAX + 1], l_gen1[BLK_MAX], l_gencyc[BLK_MAX], l_fetch1[BLK_MAX], l_fetch2[BLK_MAX];
    int l_careful[BLK_MAX + 1], l_look[BLK_MAX], l_dyn[BLK_MAX], l_dop[BLK_MAX], l_mmio[BLK_MAX];
    // A single load or store that can call the bus itself for a peripheral address.
    struct {
        uint8_t size, reg;
        bool load, sign, keep_on_fault;
    } mm[BLK_MAX];
    int l_fall, l_stale;
    bool fall_bound;
    uint8_t it[BLK_MAX]; // the IT state each instruction an IT instruction covers starts with
    bool nf;             // emitting a 16-bit instruction under IT, which sets no flags
    int l_skip[BLK_MAX]; // where an instruction under IT goes when its condition fails
    int l_post[BLK_MAX]; // just after an instruction, before a careful translation's time check
    // Where a load or store whose page is not plain memory goes, and where a load's value, in
    // eax, carries on from.
    int l_other[BLK_MAX], l_loaded[BLK_MAX];
    struct {
        uint8_t size;
        bool sign;
    } ld[BLK_MAX];
    // Fixed-address XIP accesses that missed their set's last line, to look in both ways for.
    struct {
        int label, hit, miss;
        uint32_t set, tag;
    } xipw[4 * BLK_MAX];
    int nxipw;
    uint8_t *total_sites[8 * BLK_MAX];
    int ntotal;
    struct {
        uint8_t *site;
        uint32_t target;
    } links[2];
    int nlinks;
    // PBConPC: instruction k's flags are overwritten before anything reads them (flag_liveness),
    // so its native code leaves them out; cur is the instruction being emitted.
    bool dead[BLK_MAX];
    int cur;
    // PBConPC: instruction fuse makes the flags the closing conditional branch tests, which it
    // records with SETcc fuse_cc into jx.fcond (fuse_done once emitted); -1 for none.
    int fuse, fuse_cc, fuse_cond;
    bool fuse_done;
    // PBConPC: code after the block whose flags analysis this translation relies on: in SRAM
    // compared on entry like the block's own (a change drops the translation, jit_drop).
    struct {
        uint32_t pc;
        uint8_t len, bytes[16];
    } succ[2];
    int nsucc;
    int l_drop;
    // PBConPC: at the start of instruction k, eax holds the guest register at this offset from
    // rbx (just stored there), or INT32_MIN; code that jumps back to l_main[k] loads it first.
    int32_t eax_disp[BLK_MAX + 1];
    // PBConPC: just after instruction k's fetch, where a fetch that missed comes back (fetch_back)
    int l_fetched[BLK_MAX];
    // PBConPC: the AVX2 entry check's 32-byte pieces of the block's code: where each is compared
    // in the copy after the code (labels set there)
    int cmp_lab[8], ncmp;
    uint16_t cmp_off[8];
    // PBConPC: emit_page's table lookup out of line, per instruction, and the unaligned access
    // in SRAM out of line (ua: its label, ua_back where it goes on, ua_size the access's size)
    struct {
        int tab, back, ua, ua_back, ua_size;
        int32_t table;
        uint32_t span;
    } pg[BLK_MAX];
} K;
static int page_direct = -1;
static int avx2_entry = -1;   // the SRAM entry check 32 bytes at a time (PBCONPC_NO_AVX2ENTRY=1: 8)
// PBConPC: the next single load or store may be unaligned (LDR, LDRH, STR, STRH, which ARMv8-M
// lets be; not LDREX or TBH): set by their emitters, used and cleared by emit_page.
static bool ua_next;
static int ua_opt = -1;   // PBCONPC_NO_UAOPT=1: every unaligned access by its handler
// PBConPC: with PBCONPC_FETCHOPT=1, a fetch that misses the XIP cache charges its cycles and goes
// back to the native code instead of the instruction's handler. Off by default: not exact, as a
// peripheral access in that instruction then sees the miss's cycles already passed (the handler
// and block_run see them after it), and misses are rare (1 million a minute in Sonic 2).
static int fetch_back = -1;   // PBCONPC_NO_PAGEOPT=1: the table for every access (measurements)

static inline int lab(int *slot)
{
    if (*slot < 0)
        *slot = new_label();
    return *slot;
}

static inline uint32_t op_next(const dop_t *d)
{
    return d->pc + (d->lf & LF_LEN);
}

// reg += the block's total cycles in picoseconds, patched in at the end (K.total_sites): an
// immediate (ps_imm) or the jx.psk entry's displacement.
static void emit_add_total(int reg)
{
    if (ps_imm) {
        op_r(1, false, 0x81, ALU_ADD, reg, false);   // add reg, imm32
        e32(0);
    } else {
        xm_t m = PSK(0);
        m.d32 = true;
        op_m(1, false, 0x03, reg, m, 0, false);
    }
}

// Normally r12 + psk[total] < until, or leave at instruction k, for the careful translation at the
// entry and for block_run elsewhere. In a careful translation, stop at instruction k once the time
// has reached the limit.
static void emit_budget_check(int k)
{
    mov_rr(1, RAX, R12);
    if (K.careful) {
        if (K.sb[k])
            alu_ps(ALU_ADD, RAX, K.sb[k]);
    } else {
        emit_add_total(RAX);
        if (K.ntotal < (int)(sizeof(K.total_sites) / sizeof(K.total_sites[0])))
            K.total_sites[K.ntotal++] = J.p - 4;
        else
            jit_overflow = true;
    }
    alu_rm(1, ALU_CMP, RAX, xm(R14, XO(until)));
    jcc_l(CC_AE, lab(&K.l_careful[k]));
}

// In a careful translation, with r12 brought up to date: stop if the time has reached the limit,
// with the PC from target, or from eax when target is 1.
static void emit_end_check(uint32_t target)
{
    if (!K.careful)
        return;
    alu_rm(1, ALU_CMP, R12, xm(R14, XO(until)));
    e8(0x72); // jb over the stop
    uint8_t *jb = J.p;
    e8(0);
    if (target == 1)
        mov_mr(0, RG(15), RAX);
    else
        mov_mi(RG(15), target);
    jmp_a(J.exit_done);
    *jb = (uint8_t)(J.p - jb - 1);
    join_at = J.p;
}

// The time at instruction k, into c->t.
static void emit_store_time(int k)
{
    if (K.sb[k]) {
        lea_time(RAX, K.sb[k]);
        mov_mr(1, xm(RBX, CO(t)), RAX);
    } else {
        mov_mr(1, xm(RBX, CO(t)), R12);
    }
}

static void emit_add_insns(int n)
{
    if (n)
        alu_mi(1, ALU_ADD, xm(RBX, CO(insns)), n);
}

// A jump to another block, patched to go there directly once the block has been found.
static void emit_link(uint32_t target)
{
    emit_end_check(target);
    e8(0xe9);
    K.links[K.nlinks].site = J.p;
    K.links[K.nlinks].target = target;
    K.nlinks++;
    e32(0);
}

// xip_access for a hit, which costs nothing: the line its set used last here, either way out of
// line; anything else goes to miss.
static void emit_xip_hit(uint32_t addr, int miss)
{
    if ((addr >> 24) >= 0x14 || K.nxipw == (int)(sizeof(K.xipw) / sizeof(K.xipw[0]))) {
        jmp_l(miss);
        return;
    }
    uint32_t line = (addr & 0x01ffffffu) >> 3, set = line & 1023, tag = (line >> 10) | 0x80000000u;
    int hit = new_label(), way = new_label();
    test_mi8(xm(R15, MO(xip_ctrl)), 1);
    jcc_l(CC_E, miss);
    alu_mi(0, ALU_CMP, xm(R15, MO(xip_mru) + 4 * (int32_t)set), (int32_t)tag);
    jcc_l(CC_NE, way);
    bind(hit);
    alu_mi(0, ALU_ADD, xm(R15, MO(xip_ctr_acc)), 1);
    alu_mi(0, ALU_ADD, xm(R15, MO(xip_ctr_hit)), 1);
    K.xipw[K.nxipw].label = way;
    K.xipw[K.nxipw].hit = hit;
    K.xipw[K.nxipw].miss = miss;
    K.xipw[K.nxipw].set = set;
    K.xipw[K.nxipw].tag = tag;
    K.nxipw++;
}

// The rest of a hit for emit_xip_hit: the way found becomes the set's most recent.
static void emit_xip_ways(void)
{
    for (int i = 0; i < K.nxipw; i++) {
        int32_t set = (int32_t)K.xipw[i].set, tag = (int32_t)K.xipw[i].tag;
        int way1 = new_label(), done = new_label();
        bind(K.xipw[i].label);
        alu_mi(0, ALU_CMP, xm(R15, MO(xip_tag) + 8 * set), tag);
        jcc_l(CC_NE, way1);
        op_m(0, false, 0xc6, 0, xm(R15, MO(xip_lru) + set), 1, false);
        e8(1);
        jmp_l(done);
        bind(way1);
        alu_mi(0, ALU_CMP, xm(R15, MO(xip_tag) + 8 * set + 4), tag);
        jcc_l(CC_NE, K.xipw[i].miss);
        op_m(0, false, 0xc6, 0, xm(R15, MO(xip_lru) + set), 1, false);
        e8(0);
        bind(done);
        mov_mi(xm(R15, MO(xip_mru) + 4 * set), (uint32_t)tag);
        jmp_l(K.xipw[i].hit);
    }
}

static void emit_fetch(int k, const dop_t *d)
{
    uint32_t pc = d->pc;
    if (d->lf & FETCH_HW1) {
        int same = -1;
        // Later instructions in a block always fetch from a new line when flagged, unless a wide
        // one before them in an IT block might not have fetched its second halfword.
        if (k == 0 || (K.it[k] && (K.b->ops[k - 1].lf & LF_LEN) == 4)) {
            alu_mi(0, ALU_CMP, xm(RBX, CO(fetch_line)), (int32_t)(pc & ~7u));
            same = new_label();
            jcc_l(CC_E, same);
        }
        emit_xip_hit(pc, lab(&K.l_fetch1[k]));
        mov_mi(xm(RBX, CO(fetch_line)), pc & ~7u);
        if (same >= 0)
            bind(same);
    }
    // Under IT, the second halfword is fetched only once the condition passes.
    if ((d->lf & FETCH_HW2) && !K.it[k]) {
        emit_xip_hit(pc + 2, lab(&K.l_fetch2[k]));
        mov_mi(xm(RBX, CO(fetch_line)), (pc + 2) & ~7u);
    }
}

// The handler call, as block_run makes it, with c->cyc already set. st is what the native version
// of the instruction counts as fixed, which r12 must leave out afterwards.
static void emit_cond_jump(unsigned cond, int taken);

// PBConPC: back to instruction k1 of the main flow (or the block's end), with eax as the main
// flow has it there (K.eax_disp, mov_rm).
static void jmp_main(int k1)
{
    if (k1 < K.n && K.eax_disp[k1] != INT32_MIN)
        op_m(0, false, 0x8b, RAX, xm(RBX, K.eax_disp[k1]), 0, false);
    jmp_l(k1 < K.n ? K.l_main[k1] : K.l_fall);
}

static void emit_generic_body(int k, const dop_t *d, int st, bool cold)
{
    uint32_t next = op_next(d);
    emit_spill();
    mov_mi(RG(15), next);
    emit_store_time(k);
    mov_rr(1, ARG0, RBX);
    op_m(1, false, 0x8d, ARG1, (xm_t){XM_RIP, -1, 1, lab(&K.l_dop[k]), false}, 0, false);
    emit_count_call(cold ? 5 : 0);
    static int stats = -1;
    if (stats < 0)
        stats = getenv("PBCONPC_STATS") != NULL;
    if (stats) {
        // which handler, and for the IT path the 16-bit op's first byte
        const void *f = K.it[k] ? (const void *)jit_it_op : (const void *)d->fn;
        // by kind of instruction: the handler and the opcode's top bits (both halfwords)
        uint32_t key = (uint32_t)(d->hw1 & 0xfff0) << 16 | (d->hw2 & 0xf0f0) | (cold ? 1 : 0);
        unsigned slot = (key ^ (key >> 13)) * 2654435761u >> 20;
        for (unsigned tries = 0; tries < 4096 && gen_fn[slot] && (gen_fn[slot] != f || gen_hw[slot] != key); tries++)
            slot = (slot + 1) & 4095;
        if (!gen_fn[slot]) {
            gen_fn[slot] = f;
            gen_hw[slot] = key;
            gen_op[slot] = d->hw1 | (uint32_t)d->hw2 << 16;
        }
        alu_mi(1, ALU_ADD, xm(R14, XO(gen) + 8 * (int32_t)slot), 1);
    }
    call_a(K.it[k] ? (const void *)jit_it_op : (const void *)d->fn);
    if (k == K.fuse) {
        // PBConPC: the handler made the flags (a fetch that missed, say): the closing branch's
        // condition from them, as the native version would have recorded it (emit_fuse_cond)
        int yes = new_label(), done = new_label();
        mov_rm(0, RAX, APSR);
        emit_cond_jump((unsigned)K.fuse_cond, yes);
        op_m(0, false, 0xc6, 0, xm(R14, XO(fcond)), 1, false);
        e8(0);
        jmp_l(done);
        bind(yes);
        op_m(0, false, 0xc6, 0, xm(R14, XO(fcond)), 1, false);
        e8(1);
        bind(done);
    }
    cmp16_mi(xm(RBX, CO(check_exc)), 0);
    jcc_l(CC_NE, lab(&K.l_look[k]));
    mov_rm(0, RAX, xm(RBX, CO(cyc)));
    imul_ps(RAX);
    alu_rr(1, ALU_ADD, R12, RAX);
    if (st)
        alu_ps(ALU_SUB, R12, st);
    alu_mi(0, ALU_CMP, RG(15), (int32_t)next);
    jcc_l(CC_NE, lab(&K.l_dyn[k]));
    emit_budget_check(k + 1);
    if (cold)
        jmp_main(k + 1);
}

// The address of a load or store in eax, its page in rdx and offset in eax, or off to K.l_other
// when the page is not plain memory.
// PBConPC: SRAM, which chip_rebuild_pages maps straight onto chip.sram in whole 64 KB pages, is
// tested by its range here, without the table: the table's load was the dearest part of most
// accesses (a second load the access had to wait for). Anything else looks in the table out of
// line (emit_cold), exactly as before.
static void emit_page(int k, int32_t table, unsigned align_mask, uint32_t span)
{
    uint32_t limit = (K.m->sram_size + 0xffffu) & ~0xffffu;
    if (!page_direct || limit > sizeof(K.m->sram) || K.pg[k].tab >= 0) {
        ua_next = false;
        mov_rr(0, RCX, RAX);
        shift_ri(SH_SHR, RCX, 16);
        mov_rm(1, RDX, xmi(R15, RCX, 8, table));
        test_rr(RDX, RDX);
        jcc_l(CC_E, lab(&K.l_other[k]));
        if (align_mask) {
            test_ri(RAX, align_mask);
            jcc_l(CC_NE, lab(&K.l_gen1[k]));
        }
        op_r(0, false, 0x0fb7, RAX, RAX, false); // movzx eax, ax
        if (span) {
            alu_ri(0, ALU_CMP, RAX, (int32_t)(0x10000 - span));
            jcc_l(CC_A, lab(&K.l_gen1[k]));
        }
        return;
    }
    K.pg[k].tab = new_label();
    K.pg[k].back = new_label();
    K.pg[k].table = table;
    K.pg[k].span = span;
    lea(0, RCX, xm(RAX, -(int32_t)SRAM_BASE));
    alu_ri(0, ALU_CMP, RCX, (int32_t)(span ? limit - span : limit - 1));
    jcc_l(CC_A, K.pg[k].tab);
    mov_rr(0, RAX, RCX);
    lea(1, RDX, xm(R15, MO(sram)));
    bind(K.pg[k].back);
    bool ua = ua_next && !span && align_mask && !K.m->rp2040 && ua_opt;
    ua_next = false;
    if (align_mask && ua) {
        // in SRAM as ld32u/st32u and friends do it: byte by byte, a cycle each
        K.pg[k].ua = new_label();
        K.pg[k].ua_back = new_label();
        K.pg[k].ua_size = (int)align_mask + 1;
        test_ri(RAX, align_mask);
        jcc_l(CC_NE, K.pg[k].ua);
        bind(K.pg[k].ua_back);
    } else if (align_mask) {
        test_ri(RAX, align_mask);
        jcc_l(CC_NE, lab(&K.l_gen1[k]));
    }
}

// PBConPC: an unaligned single access that emit_page found: in SRAM, not over its end and with
// CCR.UNALIGN_TRP clear it is made natively, with the cycles of its bytes; else the handler.
static void emit_page_unaligned(int k)
{
    uint32_t limit = (K.m->sram_size + 0xffffu) & ~0xffffu;
    int size = K.pg[k].ua_size;
    bind(K.pg[k].ua);
    lea(1, RCX, xm(R15, MO(sram)));
    alu_rr(1, ALU_CMP, RDX, RCX);
    jcc_l(CC_NE, lab(&K.l_gen1[k]));
    alu_ri(0, ALU_CMP, RAX, (int32_t)(limit - (uint32_t)size));
    jcc_l(CC_A, lab(&K.l_gen1[k]));
    test_mi8(xm(RBX, CO(ccr)), 8);
    jcc_l(CC_NE, lab(&K.l_gen1[k]));
    // The handler's way checks the time after the instruction (emit_generic_body's
    // emit_budget_check(k + 1)) and stops there if it is up: so where the extra cycles would make
    // that check fail, take that way, for the chunk to end at the same instruction.
    // (in rcx: eax holds the offset the access goes on with)
    mov_rr(1, RCX, R12);
    alu_ps(ALU_ADD, RCX, size - 1);
    if (K.careful) {
        if (K.sb[k + 1])
            alu_ps(ALU_ADD, RCX, K.sb[k + 1]);
    } else {
        emit_add_total(RCX);
        if (K.ntotal < (int)(sizeof(K.total_sites) / sizeof(K.total_sites[0])))
            K.total_sites[K.ntotal++] = J.p - 4;
        else
            jit_overflow = true;
    }
    alu_rm(1, ALU_CMP, RCX, xm(R14, XO(until)));
    jcc_l(CC_AE, lab(&K.l_gen1[k]));
    alu_ps(ALU_ADD, R12, size - 1);
    jmp_l(K.pg[k].ua_back);
}

// The table's way for emit_page, out of line: back with the page in rdx and the offset in eax.
static void emit_page_table(int k)
{
    bind(K.pg[k].tab);
    mov_rr(0, RCX, RAX);
    shift_ri(SH_SHR, RCX, 16);
    mov_rm(1, RDX, xmi(R15, RCX, 8, K.pg[k].table));
    test_rr(RDX, RDX);
    jcc_l(CC_E, lab(&K.l_other[k]));
    op_r(0, false, 0x0fb7, RAX, RAX, false); // movzx eax, ax
    if (K.pg[k].span) {
        alu_ri(0, ALU_CMP, RAX, (int32_t)(0x10000 - K.pg[k].span));
        jcc_l(CC_A, lab(&K.l_gen1[k]));
    }
    jmp_l(K.pg[k].back);
}

static void emit_host_load(int size, bool sign, xm_t at)
{
    if (size == 4)
        mov_rm(0, RAX, at);
    else if (size == 2)
        op_m(0, false, sign ? 0x0fbf : 0x0fb7, RAX, at, 0, false);
    else
        op_m(0, false, sign ? 0x0fbe : 0x0fb6, RAX, at, 0, false);
}

// Loads into eax; size 1 or 2 may sign-extend.
static void emit_load_value(int k, int size, bool sign)
{
    K.ld[k].size = (uint8_t)size;
    K.ld[k].sign = sign;
    emit_page(k, MO(rpage_plain), size == 4 ? 3 : size == 2 ? 1 : 0, 0);
    emit_host_load(size, sign, xmi(RDX, RAX, 1, 0));
    bind(lab(&K.l_loaded[k]));
}

static void emit_store_value(int k, int size, unsigned rt)
{
    emit_page(k, MO(wpage), size == 4 ? 3 : size == 2 ? 1 : 0, 0);
    mov_rm(0, RCX, RG(rt));
    xm_t at = xmi(RDX, RAX, 1, 0);
    if (size == 4)
        mov_mr(0, at, RCX);
    else if (size == 2)
        op_m(0, true, 0x89, RCX, at, 0, false);
    else
        op_m(0, false, 0x88, RCX, at, 0, false);
}

static void emit_addr_imm(unsigned rn, uint32_t imm)
{
    mov_rm(0, RAX, RG(rn));
    if (imm)
        alu_ri(0, ALU_ADD, RAX, (int32_t)imm);
}

// PBConPC: the closing branch's condition from the host flags of instruction K.fuse, as they stand.
uint64_t pbc_flag_stat[4];   // measurement: flag computations emitted, left out, branches fused

static void emit_fuse_cond(void)
{
    if (K.cur != K.fuse || K.fuse_done)
        return;
    op_m(0, false, 0x0f90 + (unsigned)K.fuse_cc, 0, xm(R14, XO(fcond)), 0, false); // setcc byte
    K.fuse_done = true;
    pbc_flag_stat[2]++;
}

// APSR = (APSR & keep) | eax.
static void emit_merge_flags(uint32_t keep)
{
    mov_rm(0, RCX, APSR);
    alu_ri(0, ALU_AND, RCX, (int32_t)keep);
    alu_rr(0, ALU_OR, RCX, RAX);
    mov_mr(0, APSR, RCX);
}

// N and Z from SF and ZF as they stand; C becomes carry when it is 0 or 1, and stays otherwise.
static void emit_flags_nz(int carry)
{
    emit_fuse_cond();
    pbc_flag_stat[K.dead[K.cur] ? 1 : 0]++;
    if (K.dead[K.cur])
        return;
    if (fx_mode) {
        e8(0x9f);                                 // lahf
        op_r(0, false, 0x80, 4, 4, false);        // and ah, 0xc0: SF, ZF
        e8(0xc0);
        if (carry < 0) {
            op_m(0, false, 0x80, 4, FX, 1, false); // and byte fx, 0x3f: keep C
            e8(0x3f);
            op_m(0, false, 0x08, 4, FX, 0, false); // or byte fx, ah
        } else {
            if (!carry) {
                op_r(0, false, 0x80, 1, 4, false); // or ah, 1: C clear is a borrow
                e8(1);
            }
            op_m(0, false, 0x88, 4, FX, 0, false); // mov byte fx, ah
        }
        return;
    }
    e8(0x9f); // lahf
    movzx_ah(RAX);
    mov_rm(0, RAX, xmi(R14, RAX, 4, XO(nz)));
    if (carry == 1)
        alu_ri(0, ALU_OR, RAX, (int32_t)F_C);
    emit_merge_flags(carry < 0 ? ~(F_N | F_Z) : ~(F_N | F_Z | F_C));
}

// N, Z and C from an x86 shift that matches the ARM one.
static void emit_flags_nzc(void)
{
    emit_fuse_cond();
    pbc_flag_stat[K.dead[K.cur] ? 1 : 0]++;
    if (K.dead[K.cur])
        return;
    if (fx_mode) {
        e8(0xf5);                                 // cmc: the shift's carry is C, fx keeps NOT C
        e8(0x9f);                                 // lahf
        op_m(0, false, 0x88, 4, FX, 0, false);    // mov byte fx, ah
        return;
    }
    e8(0x9f);
    movzx_ah(RAX);
    mov_rm(0, RAX, xmi(R14, RAX, 4, XO(nzc)));
    emit_merge_flags(~(F_N | F_Z | F_C));
}

// All four after an x86 add or subtract, with edx zeroed before it; subtraction inverts carry.
static void emit_flags_arith(bool sub)
{
    emit_fuse_cond();
    pbc_flag_stat[K.dead[K.cur] ? 1 : 0]++;
    if (K.dead[K.cur])
        return;
    if (fx_mode) {
        xm_t v = FX;
        v.disp += 1;
        op_m(0, false, 0x0f90 + CC_O, 0, v, 0, false);   // seto byte fx+1
        if (!sub)
            e8(0xf5);                                    // cmc: an addition's carry is C
        e8(0x9f);                                        // lahf
        op_m(0, false, 0x88, 4, FX, 0, false);           // mov byte fx, ah
        return;
    }
    setcc(CC_O, RDX);
    e8(0x9f);
    movzx_ah(RAX);
    mov_rm(0, RAX, xmi(R14, RAX, 4, sub ? XO(nzcc) : XO(nzc)));
    shift_ri(SH_SHL, RDX, 28);
    alu_rr(0, ALU_OR, RAX, RDX);
    emit_merge_flags(0x0fffffffu);
}

// N and Z of a value known now; carry as for emit_flags_nz.
static void emit_flags_const(uint32_t v, int carry)
{
    if (K.dead[K.cur])
        return;
    if (fx_mode) {
        uint8_t nz = (uint8_t)(((v >> 31) ? 0x80 : 0) | (v ? 0 : 0x40));
        if (carry < 0) {
            op_m(0, false, 0x80, 4, FX, 1, false);   // and byte fx, 0x01: keep C
            e8(0x01);
            if (nz) {
                op_m(0, false, 0x80, 1, FX, 1, false);   // or byte fx, nz
                e8(nz);
            }
        } else {
            op_m(0, false, 0xc6, 0, FX, 1, false);   // mov byte fx, nz | NOT C
            e8(nz | (carry ? 0 : 1));
        }
        return;
    }
    uint32_t mask = F_N | F_Z | (carry >= 0 ? F_C : 0);
    uint32_t set = (v & F_N) | (v ? 0 : F_Z) | (carry == 1 ? F_C : 0);
    alu_mi(0, ALU_AND, APSR, (int32_t)~mask);
    if (set)
        alu_mi(0, ALU_OR, APSR, (int32_t)set);
}

// PBConPC: taken if the condition holds for c->fx (fx_mode): SAHF, and V into OF first.
// PBConPC: x86's CF = ARM's C (want_c), or NOT C (for SBB), from wherever the flags are.
static void emit_carry_in(bool want_c)
{
    if (fx_mode) {
        bt_mi(FX, 0);       // CF = NOT C
        if (want_c)
            e8(0xf5);       // cmc
    } else {
        bt_mi(APSR, 29);    // CF = C
        if (!want_c)
            e8(0xf5);
    }
}

static void emit_cond_jump_fx(unsigned cond, int taken)
{
    static const int cc[16] = {CC_E, CC_NE, CC_AE, CC_B, CC_S, CC_NS, CC_O, CC_NO,
                               CC_A, CC_BE, CC_GE, CC_L, CC_G, CC_LE, -1, -1};
    op_m(0, false, 0x0fb7, RAX, FX, 0, false);   // movzx eax, word fx: al flags, ah V
    if (cond == 6 || cond == 7 || (cond >= 0xa && cond <= 0xd)) {
        op_r(0, false, 0x80, 0, 4, false);       // add ah, 0x7f: OF = V
        e8(0x7f);
    }
    op_r(0, false, 0x88, 0, 4, false);           // mov ah, al
    e8(0x9e);                                    // sahf
    if (cc[cond & 15] >= 0)
        jcc_l(cc[cond & 15], taken);
    else
        jmp_l(taken);
}

// The flags as a condition reads them: c->fx in fx_mode, else APSR (into eax).
static void emit_flags_cond_jump(unsigned cond, int taken)
{
    if (fx_mode) {
        emit_cond_jump_fx(cond, taken);
    } else {
        mov_rm(0, RAX, APSR);
        emit_cond_jump(cond, taken);
    }
}

// Taken if the condition holds for the flags in eax.
static void emit_cond_jump(unsigned cond, int taken)
{
    switch (cond) {
    case 0x0: test_ri(RAX, F_Z); jcc_l(CC_NE, taken); break;
    case 0x1: test_ri(RAX, F_Z); jcc_l(CC_E, taken); break;
    case 0x2: test_ri(RAX, F_C); jcc_l(CC_NE, taken); break;
    case 0x3: test_ri(RAX, F_C); jcc_l(CC_E, taken); break;
    case 0x4: test_rr(RAX, RAX); jcc_l(CC_S, taken); break;
    case 0x5: test_rr(RAX, RAX); jcc_l(CC_NS, taken); break;
    case 0x6: test_ri(RAX, F_V); jcc_l(CC_NE, taken); break;
    case 0x7: test_ri(RAX, F_V); jcc_l(CC_E, taken); break;
    case 0x8:
    case 0x9:
        alu_ri(0, ALU_AND, RAX, (int32_t)(F_C | F_Z));
        alu_ri(0, ALU_CMP, RAX, (int32_t)F_C);
        jcc_l(cond == 0x8 ? CC_E : CC_NE, taken);
        break;
    case 0xa:
    case 0xb:
        // N ^ V into bit 31.
        mov_rr(0, RCX, RAX);
        shift_ri(SH_SHL, RCX, 3);
        alu_rr(0, ALU_XOR, RCX, RAX);
        jcc_l(cond == 0xa ? CC_NS : CC_S, taken);
        break;
    default:
        // (N ^ V) | Z into bit 31.
        mov_rr(0, RCX, RAX);
        shift_ri(SH_SHL, RCX, 3);
        alu_rr(0, ALU_XOR, RCX, RAX);
        lea(0, RDX, xmi(RAX, RAX, 1, 0));
        alu_rr(0, ALU_OR, RCX, RDX);
        jcc_l(cond == 0xc ? CC_NS : CC_S, taken);
        break;
    }
}

// The end of a block that may go two ways: fall through with fixed cost st, or be taken with one
// cycle more.
static void emit_two_way(int k, int st, int taken, uint32_t target)
{
    K.l_fall = K.l_fall < 0 ? new_label() : K.l_fall;
    bind(K.l_fall);
    K.fall_bound = true;
    alu_ps(ALU_ADD, R12, K.sb[k] + st);
    emit_add_insns(K.n);
    emit_link(op_next(&K.b->ops[k]));
    bind(taken);
    alu_ps(ALU_ADD, R12, K.sb[k] + st + 1);
    emit_add_insns(K.n);
    emit_link(target);
    K.ended = true;
}

// The end of a block that goes on to an address in eax.
static void emit_to_eax(int k, int st)
{
    alu_ps(ALU_ADD, R12, K.sb[k] + st);
    emit_add_insns(k + 1);
    emit_end_check(1);
    jmp_a(J.lookup);
    K.ended = true;
}

// An interworking branch target in eax that block_run's handler would treat specially (an
// exception return, or a clear Thumb bit) goes to the handler; otherwise the bit is cleared.
static void emit_check_target(int k)
{
    alu_ri(0, ALU_CMP, RAX, (int32_t)0xff000000u);
    jcc_l(CC_AE, lab(&K.l_gen1[k]));
    test_ri(RAX, 1);
    jcc_l(CC_E, lab(&K.l_gen1[k]));
    alu_ri(0, ALU_AND, RAX, -2);
}

// After an instruction of fixed cost st that has asked for a look: what block_run does then.
static void emit_inline_look(int k, int st)
{
    mov_mi(xm(RBX, CO(cyc)), (uint32_t)st);
    mov_mi(RG(15), op_next(&K.b->ops[k]));
    emit_store_time(k);
    jmp_l(lab(&K.l_look[k]));
    K.ended = true;
}

// MRS and MSR of the masks, CPS, barriers and the hints that do nothing.
static int emit_system(int k, const dop_t *d)
{
    if (d->fn == h_exec16 && (d->hw1 & 0xffec) == 0xb660) {
        uint32_t disable = (d->hw1 >> 4) & 1;
        if (d->hw1 & 2)
            mov_mi(xm(RBX, CO(primask)), disable);
        if (d->hw1 & 1)
            mov_mi(xm(RBX, CO(faultmask)), disable);
        op_m(0, false, 0xc6, 0, xm(RBX, CO(check_exc)), 1, false);
        e8(1);
        emit_inline_look(k, 1);
        return 1;
    }
    if (d->fn != h_branch_misc || ((d->hw1 >> 7) & 7) != 7 || (d->hw2 & 0x5000))
        return -1;
    unsigned op = (d->hw1 >> 4) & 0x7f, sysm = d->hw2 & 0xff;
    if (op == 0x3e || op == 0x3f) {
        unsigned rd = (d->hw2 >> 8) & 0xf;
        if (rd == 15)
            return -1;
        if (sysm <= 2 && sysm != 1) {
            if (fx_mode)
                emit_fx_to_apsr();
            mov_rm(0, RAX, APSR);
            alu_ri(0, ALU_AND, RAX, (int32_t)0xf80f0000u);
        } else if (sysm == 16 || sysm == 19) {
            mov_rm(0, RAX, xm(RBX, sysm == 16 ? CO(primask) : CO(faultmask)));
            alu_ri(0, ALU_AND, RAX, 1);
        } else if (sysm == 17 || sysm == 18) {
            mov_rm(0, RAX, xm(RBX, CO(basepri)));
        } else {
            return -1;
        }
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    if ((op == 0x38 || op == 0x39) && sysm == 16 && (d->hw1 & 0xf) != 15) {
        mov_rm(0, RAX, RG(d->hw1 & 0xf));
        alu_ri(0, ALU_AND, RAX, 1);
        mov_mr(0, xm(RBX, CO(primask)), RAX);
        op_m(0, false, 0xc6, 0, xm(RBX, CO(check_exc)), 1, false);
        e8(1);
        emit_inline_look(k, 1);
        return 1;
    }
    if (op == 0x3a && (sysm < 2 || sysm > 4))
        return 1;
    if (op == 0x3b) {
        if (((d->hw2 >> 4) & 0xf) == 2) {
            op_m(0, false, 0xc6, 0, xm(RBX, CO(excl_valid)), 1, false);
            e8(0);
        }
        return 2;
    }
    return -1;
}

// The loads and stores ldst_single handles that decode_ldst_single left to it: sign-extending
// loads, register offsets, 8-bit offsets with writeback, and loads into the PC.
// PBConPC: LDR, LDRH, LDRSH, LDRB, LDRSB (literal), 32-bit: as h_ldr_lit for the 16-bit one, and
// LDR PC, [PC, #x] (a long-branch veneer) as a branch to a known address.
static int lit_opt = -1;   // PBCONPC_NO_LITOPT=1: by the handler

static int emit_ldr_literal(int k, const dop_t *d, unsigned size, bool sign, unsigned rt)
{
    uint16_t hw1 = d->hw1, hw2 = d->hw2;
    uint32_t base = (d->pc + 4) & ~3u, imm = hw2 & 0xfff;
    uint32_t a = (hw1 & 0x80) ? base + imm : base - imm, pc0 = K.b->tag & ~1u;
    unsigned bytes = 1u << size;
    if ((a >> 16) != (pc0 >> 16) || (a & (bytes - 1)) || (a & 0xffff) > 0x10000 - bytes)
        return -1;
    const uint8_t *page = K.m->rpage[a >> 16];
    if (!page)
        return -1;
    // A load into PC sends control on whether or not the block builder ended the block there
    // (it may have gone on into the literal, which then never runs): k + 1 instructions are done,
    // as the handler's way counts them (l_dyn).
    if (rt == 15 && !K.fixed) {
        // from SRAM: the target as it is now, checked as LDR PC does
        if (size != 2)
            return -1;
        mov_rm(0, RAX, xm(R15, MO(sram) + (int32_t)(a - SRAM_BASE)));
        emit_check_target(k);
        emit_to_eax(k, 4);
        return 4;
    }
    if (rt == 15) {
        // from ROM or flash: a known target, if it is Thumb code
        if (size != 2)
            return -1;
        uint32_t v = rd32le(page + (a & 0xffff));
        if (!(v & 1) || v >= 0xff000000u)
            return -1;
        if ((a >> 28) == 1)
            emit_xip_hit(a, lab(&K.l_gen1[k]));
        if (k == K.n - 1) {
            alu_ps(ALU_ADD, R12, K.sb[k] + 4);
            emit_add_insns(k + 1);
            emit_link(v & ~1u);
            K.ended = true;
        } else {
            // mid-block: through the lookup, as a block has room for two links only
            mov_ri(RAX, v & ~1u);
            emit_to_eax(k, 4);
        }
        return 4;
    }
    if (K.fixed) {
        // ROM and flash hold still for as long as the block is current.
        uint32_t v = bytes == 4 ? rd32le(page + (a & 0xffff))
                     : bytes == 2 ? (sign ? (uint32_t)(int32_t)(int16_t)rd16le(page + (a & 0xffff)) : rd16le(page + (a & 0xffff)))
                     : (sign ? (uint32_t)(int32_t)(int8_t)page[a & 0xffff] : page[a & 0xffff]);
        if ((a >> 28) == 1)
            emit_xip_hit(a, lab(&K.l_gen1[k]));
        mov_mi(RG(rt), v);
    } else {
        emit_host_load((int)bytes, sign, xm(R15, MO(sram) + (int32_t)(a - SRAM_BASE)));
        mov_mr(0, RG(rt), RAX);
    }
    return 2;
}

static int emit_ldst_single(int k, const dop_t *d)
{
    uint16_t hw1 = d->hw1, hw2 = d->hw2;
    bool sign = hw1 & 0x100, load = hw1 & 0x10;
    unsigned size = (hw1 >> 5) & 3, rn = hw1 & 0xf, rt = (hw2 >> 12) & 0xf;
    if (rn == 15 && load && size != 3 && lit_opt)
        return emit_ldr_literal(k, d, size, sign, rt);
    if (size == 3 || rn == 15 || (!load && (sign || rt == 15)))
        return -1;
    bool wb = false, index = true;
    int rm = -1;
    unsigned shift = 0;
    uint32_t imm = 0;
    if (hw1 & 0x80) {
        imm = hw2 & 0xfff;
    } else if (hw2 & 0x800) {
        index = hw2 & 0x400;
        wb = hw2 & 0x100;
        imm = (hw2 & 0x200) ? (hw2 & 0xffu) : 0u - (hw2 & 0xffu);
    } else if ((hw2 & 0xfc0) == 0) {
        rm = hw2 & 0xf;
        shift = (hw2 >> 4) & 3;
        if (rm == 15)
            return -1;
    } else {
        return -1;
    }
    // Preloads, which do nothing here.
    if (load && rt == 15 && size != 2)
        return 1;
    if (wb && rn == rt)
        return -1;
    int bytes = 1 << size;
    if (!wb && rt != 15) {
        K.mm[k].size = (uint8_t)bytes;
        K.mm[k].reg = (uint8_t)rt;
        K.mm[k].load = load;
        K.mm[k].sign = sign;
        K.mm[k].keep_on_fault = true;
    }
    mov_rm(0, RAX, RG(rn));
    if (rm >= 0) {
        mov_rm(0, RCX, RG(rm));
        if (shift)
            shift_ri(SH_SHL, RCX, (int)shift);
        alu_rr(0, ALU_ADD, RAX, RCX);
    } else if (index && imm) {
        alu_ri(0, ALU_ADD, RAX, (int32_t)imm);
    }
    ua_next = !(load && rt == 15);
    if (load)
        emit_load_value(k, bytes, sign);
    else
        emit_store_value(k, bytes, rt);
    if (load && rt == 15)
        emit_check_target(k);
    if (wb) {
        mov_rm(0, RCX, RG(rn));
        alu_ri(0, ALU_ADD, RCX, (int32_t)imm);
        mov_mr(0, RG(rn), RCX);
    }
    if (load && rt == 15) {
        emit_to_eax(k, 4);
        return 4;
    }
    if (load)
        mov_mr(0, RG(rt), RAX);
    return 2;
}

// LDM and STM, with PUSH.W and POP.W among them.
static int emit_ldm_stm(int k, const dop_t *d)
{
    unsigned rn = d->rn, n = d->rm;
    uint32_t list = d->imm;
    bool load = d->lf & LF_A, wb = d->lf & LF_B, db = d->lf & LF_C;
    bool pc = load && (list & 0x8000);
    if (rn == 15 || n == 0)
        return -1;
    mov_rm(0, RAX, RG(rn));
    if (db)
        alu_ri(0, ALU_SUB, RAX, (int32_t)(4 * n));
    emit_page(k, load ? MO(rpage_plain) : MO(wpage), 3, 4 * n);
    lea(1, RDX, xmi(RDX, RAX, 1, 0));
    if (pc) {
        mov_rm(0, R9, xm(RDX, 4 * (int32_t)(n - 1)));
        alu_ri(0, ALU_CMP, R9, (int32_t)0xff000000u);
        jcc_l(CC_AE, lab(&K.l_gen1[k]));
        test_ri(R9, 1);
        jcc_l(CC_E, lab(&K.l_gen1[k]));
    }
    int32_t off = 0;
    for (unsigned i = 0; i < 15; i++)
        if (list & (1u << i)) {
            if (load) {
                mov_rm(0, RCX, xm(RDX, off));
                mov_mr(0, RG(i), RCX);
            } else {
                mov_rm(0, RCX, RG(i));
                mov_mr(0, xm(RDX, off), RCX);
            }
            off += 4;
        }
    if (wb && !(load && (list & (1u << rn))))
        alu_mi(0, ALU_ADD, RG(rn), db ? -(int32_t)(4 * n) : (int32_t)(4 * n));
    if (!pc)
        return 1 + (int)n;
    mov_rr(0, RAX, R9);
    alu_ri(0, ALU_AND, RAX, -2);
    emit_to_eax(k, 3 + (int)n);
    return 3 + (int)n;
}

// Clears the other core's exclusive monitor if it covers the address in r8, as excl_clear_others.
static void emit_clear_other_monitor(void)
{
    int done = new_label();
    mov_rm(0, RCX, xm(RBX, CO(id)));
    alu_ri(0, ALU_XOR, RCX, 1);
    op_r(0, false, 0x69, RCX, RCX, false); // imul ecx, ecx, sizeof(cpu_t)
    e32((uint32_t)sizeof(cpu_t));
    lea(1, RCX, xmi(R15, RCX, 1, MO(core)));
    test_mi8(xm(RCX, CO(excl_valid)), 1);
    jcc_l(CC_E, done);
    mov_rm(0, RDX, xm(RCX, CO(excl_addr)));
    alu_rr(0, ALU_XOR, RDX, R8);
    test_ri(RDX, ~7u);
    jcc_l(CC_NE, done);
    op_m(0, false, 0xc6, 0, xm(RCX, CO(excl_valid)), 1, false);
    e8(0);
    bind(done);
}

// The exclusive and acquire/release loads and stores, and TBB and TBH.
static int emit_excl(int k, const dop_t *d)
{
    uint16_t hw1 = d->hw1, hw2 = d->hw2;
    unsigned rn = hw1 & 0xf, rt = (hw2 >> 12) & 0xf;
    unsigned size, rd = 16;
    bool load, excl;
    uint32_t imm = 0;
    if ((hw1 & 0xfff0) == 0xe850 || ((hw1 & 0xfff0) == 0xe840 && (hw2 & 0xf03f) != 0xf000)) {
        load = hw1 & 0x10;
        excl = true;
        size = 4;
        imm = (hw2 & 0xffu) << 2;
        if (!load)
            rd = (hw2 >> 8) & 0xf;
    } else if ((hw1 & 0xfff0) == 0xe8c0 || (hw1 & 0xfff0) == 0xe8d0) {
        load = hw1 & 0x10;
        unsigned op3 = (hw2 >> 4) & 0xf;
        if (load && op3 <= 1) {
            // TBB and TBH.
            unsigned rm = hw2 & 0xf;
            uint32_t pc4 = d->pc + 4;
            // Every read through an uncached alias misses, which the handler charges.
            if (rm == 15 || (rn == 15 && (d->pc >> 28) == 1 && (d->pc >> 24) >= 0x14))
                return -1;
            mov_rm(0, RCX, RG(rm));
            if (op3)
                alu_rr(0, ALU_ADD, RCX, RCX);
            if (rn != 15) {
                alu_rm(0, ALU_ADD, RCX, RG(rn));
                mov_rr(0, RAX, RCX);
                emit_load_value(k, op3 ? 2 : 1, false);
            } else {
                // A table just after the instruction, so in the block's own page.
                alu_ri(0, ALU_ADD, RCX, (int32_t)pc4);
                mov_rr(0, RAX, RCX);
                shift_ri(SH_SHR, RAX, 16);
                alu_ri(0, ALU_CMP, RAX, (int32_t)(d->pc >> 16));
                jcc_l(CC_NE, lab(&K.l_gen1[k]));
                if (op3) {
                    test_ri(RCX, 1);
                    jcc_l(CC_NE, lab(&K.l_gen1[k]));
                }
                if ((d->pc >> 28) == 1) {
                    test_mi8(xm(R15, MO(xip_ctrl)), 1);
                    jcc_l(CC_E, lab(&K.l_gen1[k]));
                    mov_rr(0, RAX, RCX);
                    alu_ri(0, ALU_AND, RAX, 0x01ffffff);
                    shift_ri(SH_SHR, RAX, 3);
                    mov_rr(0, RDX, RAX);
                    alu_ri(0, ALU_AND, RDX, 1023);
                    shift_ri(SH_SHR, RAX, 10);
                    alu_ri(0, ALU_OR, RAX, (int32_t)0x80000000u);
                    alu_rm(0, ALU_CMP, RAX, xmi(R15, RDX, 4, MO(xip_mru)));
                    jcc_l(CC_NE, lab(&K.l_gen1[k]));
                    alu_mi(0, ALU_ADD, xm(R15, MO(xip_ctr_acc)), 1);
                    alu_mi(0, ALU_ADD, xm(R15, MO(xip_ctr_hit)), 1);
                }
                op_r(0, false, 0x0fb7, RCX, RCX, false); // movzx ecx, cx
                mov_ri64(RDX, (uint64_t)(uintptr_t)K.m->rpage[d->pc >> 16]);
                op_m(0, false, op3 ? 0x0fb7 : 0x0fb6, RAX, xmi(RDX, RCX, 1, 0), 0, false);
            }
            lea(0, RAX, xmi(RAX, RAX, 1, (int32_t)pc4));
            emit_to_eax(k, 4);
            return 4;
        }
        size = (op3 & 3) == 0 ? 1 : (op3 & 3) == 1 ? 2 : 4;
        excl = op3 & 4;
        if (!load && excl)
            rd = hw2 & 0xf;
    } else {
        return -1;
    }
    if (rn == 15 || rt == 15 || rd == 15)
        return -1;
    mov_rm(0, RAX, RG(rn));
    if (imm)
        alu_ri(0, ALU_ADD, RAX, (int32_t)imm);
    mov_rr(0, R8, RAX);
    if (load) {
        emit_load_value(k, (int)size, false);
        mov_mr(0, RG(rt), RAX);
        if (excl) {
            op_m(0, false, 0xc6, 0, xm(RBX, CO(excl_valid)), 1, false);
            e8(1);
            mov_mr(0, xm(RBX, CO(excl_addr)), R8);
        }
        return 2;
    }
    int fail = -1, done = -1;
    if (excl) {
        fail = new_label();
        done = new_label();
        test_mi8(xm(RBX, CO(excl_valid)), 1);
        jcc_l(CC_E, fail);
        alu_rm(0, ALU_CMP, R8, xm(RBX, CO(excl_addr)));
        jcc_l(CC_NE, fail);
    }
    emit_store_value(k, (int)size, rt);
    emit_clear_other_monitor();
    if (excl) {
        mov_mi(RG(rd), 0);
        op_m(0, false, 0xc6, 0, xm(RBX, CO(excl_valid)), 1, false);
        e8(0);
        jmp_l(done);
        // A failed store-exclusive stores nothing, so costs a cycle less.
        bind(fail);
        mov_mi(RG(rd), 1);
        op_m(0, false, 0xc6, 0, xm(RBX, CO(excl_valid)), 1, false);
        e8(0);
        alu_ps(ALU_SUB, R12, 1);
        bind(done);
    }
    return 2;
}

// Data processing with a shifted register, where the flags it sets do not need the shifter's carry.
// PBConPC: SSE2 for the byte-wise DSP instructions (xmm0-3, unused elsewhere).
static void sse_rr(unsigned opc, int x, int y)   // opc xmm(x), xmm(y), opc being 0x0fXX after 66
{
    op_r(0, true, opc, x, y, false);
}

static void movd_x_m(int x, xm_t m)
{
    op_m(0, true, 0x0f6e, x, m, 0, false);
}

static void movd_r_x(int r, int x)
{
    op_r(0, true, 0x0f7e, x, r, false);
}

static int dsp_opt = -1;   // PBCONPC_NO_DSPOPT=1: UADD8, USUB8, UQADD8, UQSUB8, SEL, PKH, SMULxy by handler

// UADD8, USUB8 (with APSR.GE as parallel_addsub sets it), UQADD8, UQSUB8, and SEL.
static int emit_dsp_bytes(int k, const dop_t *d)
{
    (void)k;
    uint16_t hw1 = d->hw1, hw2 = d->hw2;
    unsigned op1 = (hw1 >> 4) & 0xf, op2 = (hw2 >> 4) & 0xf;
    unsigned rn = hw1 & 0xf, rd = (hw2 >> 8) & 0xf, rm = hw2 & 0xf;
    if ((hw2 & 0xf000) != 0xf000 || rn == 15 || rd == 15 || rm == 15)
        return -1;
    if (op1 == 0xa && op2 == 8) {   // SEL
        mov_rm(0, RCX, APSR);
        shift_ri(SH_SHR, RCX, 16);
        alu_ri(0, ALU_AND, RCX, 15);
        mov_rm(0, RCX, xmi(R14, RCX, 4, XO(ge_mask)));
        mov_rm(0, RAX, RG(rn));
        alu_rr(0, ALU_AND, RAX, RCX);
        not_r(RCX);
        alu_rm(0, ALU_AND, RCX, RG(rm));
        alu_rr(0, ALU_OR, RAX, RCX);
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    if (op1 < 8 || op2 >= 8)
        return -1;
    unsigned o = op1 & 7, kind = op2 & 3;
    bool uns = op2 & 4;
    if (!uns || (o != 0 && o != 4) || kind > 1)
        return -1;
    bool sub = o == 4;
    movd_x_m(0, RG(rn));
    movd_x_m(1, RG(rm));
    if (kind == 1) {   // saturating, GE untouched
        sse_rr(sub ? 0x0fd8 : 0x0fdc, 0, 1);   // psubusb / paddusb
        movd_r_x(RAX, 0);
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    if (sub) {
        // GE[i]: a[i] >= b[i], that is max(a, b) == a
        op_r(0, true, 0x0f6f, 2, 0, false);   // movdqa xmm2, xmm0
        sse_rr(0x0fde, 2, 1);                 // pmaxub xmm2, xmm1
        sse_rr(0x0f74, 2, 0);                 // pcmpeqb xmm2, xmm0
        sse_rr(0x0ff8, 0, 1);                 // psubb xmm0, xmm1
        op_r(0, true, 0x0fd7, RCX, 2, false); // pmovmskb ecx, xmm2
    } else {
        // GE[i]: the byte carried, so that the saturating sum differs from the plain one
        op_r(0, true, 0x0f6f, 2, 0, false);   // movdqa xmm2, xmm0
        sse_rr(0x0fdc, 2, 1);                 // paddusb xmm2, xmm1
        sse_rr(0x0ffc, 0, 1);                 // paddb xmm0, xmm1
        sse_rr(0x0f74, 2, 0);                 // pcmpeqb xmm2, xmm0
        op_r(0, true, 0x0fd7, RCX, 2, false); // pmovmskb ecx, xmm2
        not_r(RCX);
    }
    movd_r_x(RAX, 0);
    mov_mr(0, RG(rd), RAX);
    alu_ri(0, ALU_AND, RCX, 15);
    shift_ri(SH_SHL, RCX, 16);
    alu_mi(0, ALU_AND, APSR, (int32_t)~0x000f0000u);
    alu_mr(0, ALU_OR, APSR, RCX);
    return 1;
}

static int emit_dp_shifted(int k, const dop_t *d)
{
    uint16_t hw1 = d->hw1, hw2 = d->hw2;
    unsigned op = (hw1 >> 5) & 0xf, rn = hw1 & 0xf, rd = (hw2 >> 8) & 0xf, rm = hw2 & 0xf;
    bool s = hw1 & 0x10;
    unsigned type = (hw2 >> 4) & 3, imm5 = ((hw2 >> 10) & 0x1c) | ((hw2 >> 6) & 3);
    bool logical = op <= 4, arith = op == 8 || op == 0xa || op == 0xb || op == 0xd || op == 0xe;
    if (op == 6 && dsp_opt && !s && rm != 15 && rn != 15 && rd != 15 && (type == 0 || type == 2)) {
        // PKHBT, PKHTB
        mov_rm(0, RCX, RG(rm));
        if (type == 0) {
            if (imm5)
                shift_ri(SH_SHL, RCX, (int)imm5);
            alu_ri(0, ALU_AND, RCX, (int32_t)0xffff0000u);
            mov_rm(0, RAX, RG(rn));
            op_r(0, false, 0x0fb7, RAX, RAX, false); // movzx eax, ax
        } else {
            shift_ri(SH_SAR, RCX, imm5 ? (int)imm5 : 31);
            op_r(0, false, 0x0fb7, RCX, RCX, false); // movzx ecx, cx
            mov_rm(0, RAX, RG(rn));
            alu_ri(0, ALU_AND, RAX, (int32_t)0xffff0000u);
        }
        alu_rr(0, ALU_OR, RAX, RCX);
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    if (dsp_opt && op == 2 && rn == 15 && s && rd != 15 && rm != 15 && imm5 && type <= 2) {
        // MOVS Rd, Rm, LSL/LSR/ASR #1..31: N, Z and C straight from the x86 shift
        mov_rm(0, RAX, RG(rm));
        shift_ri(type == 0 ? SH_SHL : type == 1 ? SH_SHR : SH_SAR, RAX, (int)imm5);
        mov_mr(0, RG(rd), RAX);
        emit_flags_nzc();
        return 1;
    }
    if ((!logical && !arith) || rm == 15)
        return -1;
    if (rn == 15 && op != 2 && op != 3)
        return -1;
    if (rd == 15 && !(s && (op == 0 || op == 4 || op == 8 || op == 0xd)))
        return -1;
    if (s && logical && (type || imm5))
        return -1;
    bool sub = op == 0xb || op == 0xd || op == 0xe;
    if (s && arith)
        alu_rr(0, ALU_XOR, RDX, RDX);
    mov_rm(0, RCX, RG(rm));
    unsigned n = (type == 1 || type == 2) && !imm5 ? 32 : imm5;
    if (type == 0 && n) {
        shift_ri(SH_SHL, RCX, (int)n);
    } else if (type == 1) {
        if (n == 32)
            alu_rr(0, ALU_XOR, RCX, RCX);
        else
            shift_ri(SH_SHR, RCX, (int)n);
    } else if (type == 2) {
        shift_ri(SH_SAR, RCX, n == 32 ? 31 : (int)n);
    } else if (type == 3) {
        if (n) {
            shift_ri(1, RCX, (int)n); // ror
        } else {
            emit_carry_in(true);
            op_r(0, false, 0xd1, 3, RCX, false); // rcr ecx, 1
        }
    }
    if (rn == 15) {
        if (op == 3)
            not_r(RCX);
        mov_rr(0, RAX, RCX);
        if (s)
            test_rr(RAX, RAX);
    } else if (logical) {
        if (op == 1 || op == 3)
            not_r(RCX);
        mov_rm(0, RAX, RG(rn));
        alu_rr(0, op <= 1 ? ALU_AND : op == 4 ? ALU_XOR : ALU_OR, RAX, RCX);
    } else if (op == 0xe) {
        alu_rm(0, ALU_SUB, RCX, RG(rn));
        mov_rr(0, RAX, RCX);
    } else {
        if (op == 0xa || op == 0xb)
            emit_carry_in(op == 0xa);   // ADC takes C, SBC (as SBB) NOT C
        mov_rm(0, RAX, RG(rn));
        alu_rr(0, op == 8 ? ALU_ADD : op == 0xa ? ALU_ADC : op == 0xb ? ALU_SBB : ALU_SUB, RAX, RCX);
    }
    if (rd != 15)
        mov_mr(0, RG(rd), RAX);
    if (s) {
        if (logical)
            emit_flags_nz(-1);
        else
            emit_flags_arith(sub);
    }
    return 1;
}

static void cmov_rr(int cc, int d, int s2)
{
    op_r(0, false, 0x0f40 + cc, d, s2, false);
}

// Shifts by a register without flags, the extends, REV, REV16, REVSH and CLZ.
// PBConPC: rd = rn shifted by rs[7:0] as ARM shifts by a register (kind as shift_ri; 1 is ROR),
// without flags: x86 counts modulo 32, so 32 and more come out by cmov.
static void emit_shift_by_reg(int kind, unsigned rd, unsigned rs, unsigned rn)
{
    mov_rm(0, RCX, RG(rs));
    alu_ri(0, ALU_AND, RCX, 0xff);
    mov_rm(0, RAX, RG(rn));
    if (kind == SH_SAR) {
        mov_ri(RDX, 31);
        alu_ri(0, ALU_CMP, RCX, 31);
        cmov_rr(CC_A, RCX, RDX);
        op_r(0, false, 0xd3, SH_SAR, RAX, false);
    } else if (kind == 1) {
        op_r(0, false, 0xd3, 1, RAX, false); // ror eax, cl
    } else {
        op_r(0, false, 0xd3, kind, RAX, false);
        alu_rr(0, ALU_XOR, RDX, RDX);
        alu_ri(0, ALU_CMP, RCX, 32);
        cmov_rr(CC_AE, RAX, RDX);
    }
    mov_mr(0, RG(rd), RAX);
}

static int emit_dp_register(int k, const dop_t *d)
{
    if (dsp_opt) {
        unsigned op1 = (d->hw1 >> 4) & 0xf, op2 = (d->hw2 >> 4) & 0xf;
        // LSL, LSR, ASR, ROR (.W) by a register, without S or with its flags dead
        if (op2 == 0 && op1 < 8 && (d->hw2 & 0xf000) == 0xf000 && (!(op1 & 1) || K.dead[K.cur])) {
            unsigned rn = d->hw1 & 0xf, rd = (d->hw2 >> 8) & 0xf, rm = d->hw2 & 0xf;
            if (rn == 15 || rd == 15 || rm == 15)
                return -1;
            static const int kinds[4] = {SH_SHL, SH_SHR, SH_SAR, 1};
            emit_shift_by_reg(kinds[op1 >> 1], rd, rm, rn);
            return 1;
        }
        if ((op1 >= 8 && op2 < 8) || (op1 == 0xa && op2 == 8))
            return emit_dsp_bytes(k, d);
    }
    uint16_t hw1 = d->hw1, hw2 = d->hw2;
    unsigned op1 = (hw1 >> 4) & 0xf, op2 = (hw2 >> 4) & 0xf;
    unsigned rn = hw1 & 0xf, rd = (hw2 >> 8) & 0xf, rm = hw2 & 0xf;
    if ((hw2 & 0xf000) != 0xf000 || rd == 15 || rm == 15)
        return -1;
    if (op2 == 0 && op1 < 8) {
        if ((op1 & 1) || rn == 15)
            return -1;
        unsigned type = op1 >> 1;
        mov_rm(0, RCX, RG(rm));
        alu_ri(0, ALU_AND, RCX, 0xff);
        mov_rm(0, RAX, RG(rn));
        if (type <= 1) {
            alu_rr(0, ALU_XOR, RDX, RDX);
            op_r(0, false, 0xd3, type ? SH_SHR : SH_SHL, RAX, false);
            alu_ri(0, ALU_CMP, RCX, 32);
            cmov_rr(CC_AE, RAX, RDX);
        } else if (type == 2) {
            mov_ri(RDX, 31);
            alu_ri(0, ALU_CMP, RCX, 31);
            cmov_rr(CC_A, RCX, RDX);
            op_r(0, false, 0xd3, SH_SAR, RAX, false);
        } else {
            op_r(0, false, 0xd3, 1, RAX, false); // ror eax, cl
        }
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    if (op1 < 8 && (op2 & 8)) {
        if (op1 != 0 && op1 != 1 && op1 != 4 && op1 != 5)
            return -1;
        unsigned rot = (hw2 >> 4) & 3;
        mov_rm(0, RAX, RG(rm));
        if (rot)
            shift_ri(1, RAX, 8 * (int)rot);
        static const unsigned ext[6] = {0x0fbf, 0x0fb7, 0, 0, 0x0fbe, 0x0fb6};
        op_r(0, false, ext[op1], RAX, RAX, false);
        if (rn != 15)
            alu_rm(0, ALU_ADD, RAX, RG(rn));
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    if ((op1 & 0xc) == 8 && (op2 & 0xc) == 8) {
        unsigned o1 = op1 & 3, o2 = op2 & 3;
        if (!(o1 == 3 || (o1 == 1 && o2 != 2)))
            return -1;
        mov_rm(0, RAX, RG(rm));
        if (o1 == 1 && o2 == 0) {
            opcode(0x0fc8 + RAX); // bswap eax, which has no ModRM byte for op_r to add
        } else if (o1 == 1 && o2 == 1) {
            mov_rr(0, RCX, RAX);
            shift_ri(SH_SHR, RAX, 8);
            alu_ri(0, ALU_AND, RAX, 0x00ff00ff);
            shift_ri(SH_SHL, RCX, 8);
            alu_ri(0, ALU_AND, RCX, (int32_t)0xff00ff00u);
            alu_rr(0, ALU_OR, RAX, RCX);
        } else if (o1 == 1 && o2 == 3) {
            op_r(0, true, 0xc1, 0, RAX, false); // rol ax, 8
            e8(8);
            op_r(0, false, 0x0fbf, RAX, RAX, false);
        } else if (o1 == 3) {
            mov_ri(RDX, 0xffffffffu);
            op_r(0, false, 0x0fbd, RCX, RAX, false); // bsr ecx, eax
            cmov_rr(CC_E, RCX, RDX);
            mov_ri(RAX, 31);
            alu_rr(0, ALU_SUB, RAX, RCX);
        } else {
            return -1;
        }
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    return -1;
}

// ADDW, SUBW, MOVW, MOVT, the bitfield extracts, BFI and BFC.
static int emit_dp_plain(int k, const dop_t *d)
{
    uint16_t hw1 = d->hw1, hw2 = d->hw2;
    unsigned op = (hw1 >> 4) & 0x1f, rn = hw1 & 0xf, rd = (hw2 >> 8) & 0xf;
    uint32_t imm12 = ((uint32_t)(hw1 & 0x400) << 1) | ((hw2 >> 4) & 0x700) | (hw2 & 0xff);
    unsigned imm5 = ((hw2 >> 10) & 0x1c) | ((hw2 >> 6) & 3), widthm1 = hw2 & 0x1f;
    if (rd == 15)
        return -1;
    switch (op) {
    case 0x00:
    case 0x0a:
        if (rn == 15) {
            uint32_t base = (d->pc + 4) & ~3u;
            mov_mi(RG(rd), op ? base - imm12 : base + imm12);
            return 1;
        }
        mov_rm(0, RAX, RG(rn));
        alu_ri(0, op ? ALU_SUB : ALU_ADD, RAX, (int32_t)imm12);
        mov_mr(0, RG(rd), RAX);
        return 1;
    case 0x04:
        mov_mi(RG(rd), ((uint32_t)(hw1 & 0xf) << 12) | imm12);
        return 1;
    case 0x0c:
        mov_rm(0, RAX, RG(rd));
        op_r(0, false, 0x0fb7, RAX, RAX, false);
        alu_ri(0, ALU_OR, RAX, (int32_t)((((uint32_t)(hw1 & 0xf) << 12) | imm12) << 16));
        mov_mr(0, RG(rd), RAX);
        return 1;
    case 0x14:
    case 0x1c: {
        if (rn == 15)
            return -1;
        unsigned w = widthm1 + 1;
        mov_rm(0, RAX, RG(rn));
        if (imm5)
            shift_ri(SH_SHR, RAX, (int)imm5);
        if (w < 32) {
            if (op == 0x14) {
                shift_ri(SH_SHL, RAX, 32 - (int)w);
                shift_ri(SH_SAR, RAX, 32 - (int)w);
            } else {
                alu_ri(0, ALU_AND, RAX, (int32_t)((1u << w) - 1));
            }
        }
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    case 0x18: {
        // USAT Rd, #sat, Rn, LSL #imm5 (as sat_unsigned: Q when it saturates)
        if (rn == 15 || !dsp_opt)
            return -1;
        uint32_t hi = widthm1 >= 32 ? 0xffffffffu : (uint32_t)((1ull << widthm1) - 1);
        int neg = new_label(), over = new_label(), done = new_label();
        mov_rm(0, RAX, RG(rn));
        if (imm5)
            shift_ri(SH_SHL, RAX, (int)imm5);
        test_rr(RAX, RAX);
        jcc_l(CC_S, neg);
        alu_ri(0, ALU_CMP, RAX, (int32_t)hi);
        jcc_l(CC_A, over);
        jmp_l(done);
        bind(neg);
        alu_rr(0, ALU_XOR, RAX, RAX);
        alu_mi(0, ALU_OR, APSR, (int32_t)0x08000000u);
        jmp_l(done);
        bind(over);
        mov_ri(RAX, hi);
        alu_mi(0, ALU_OR, APSR, (int32_t)0x08000000u);
        bind(done);
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    case 0x16: {
        unsigned lsb = imm5, msb = widthm1;
        if (msb < lsb)
            return 1;
        uint32_t mask = (uint32_t)((((uint64_t)1 << (msb - lsb + 1)) - 1) << lsb);
        if (rn == 15) {
            alu_mi(0, ALU_AND, RG(rd), (int32_t)~mask);
            return 1;
        }
        mov_rm(0, RAX, RG(rn));
        if (lsb)
            shift_ri(SH_SHL, RAX, (int)lsb);
        alu_ri(0, ALU_AND, RAX, (int32_t)mask);
        mov_rm(0, RCX, RG(rd));
        alu_ri(0, ALU_AND, RCX, (int32_t)~mask);
        alu_rr(0, ALU_OR, RAX, RCX);
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    default:
        return -1;
    }
}

// MUL, MLA and MLS; SMULL, UMULL, SMLAL and UMLAL; SDIV and UDIV.
static int emit_multiply(int k, const dop_t *d, bool lng)
{
    if (dsp_opt && !lng && ((d->hw1 >> 4) & 7) == 1) {
        // SMULxy and SMLAxy: 16 x 16 signed, with Q on an overflowing accumulate
        unsigned rn = d->hw1 & 0xf, ra = (d->hw2 >> 12) & 0xf, rd = (d->hw2 >> 8) & 0xf, rm = d->hw2 & 0xf;
        unsigned op2 = (d->hw2 >> 4) & 3;
        if (rn == 15 || rd == 15 || rm == 15)
            return -1;
        mov_rm(0, RAX, RG(rn));
        if (op2 & 2)
            shift_ri(SH_SAR, RAX, 16);
        else
            op_r(0, false, 0x0fbf, RAX, RAX, false); // movsx eax, ax
        mov_rm(0, RCX, RG(rm));
        if (op2 & 1)
            shift_ri(SH_SAR, RCX, 16);
        else
            op_r(0, false, 0x0fbf, RCX, RCX, false); // movsx ecx, cx
        op_r(0, false, 0x0faf, RAX, RCX, false);     // imul eax, ecx
        if (ra != 15) {
            alu_rm(0, ALU_ADD, RAX, RG(ra));
            e8(0x71); // jno over the Q
            uint8_t *jno = J.p;
            e8(0);
            alu_mi(0, ALU_OR, APSR, (int32_t)0x08000000u);
            *jno = (uint8_t)(J.p - jno - 1);
            join_at = J.p;
        }
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    uint16_t hw1 = d->hw1, hw2 = d->hw2;
    unsigned op1 = (hw1 >> 4) & 7, rn = hw1 & 0xf, rm = hw2 & 0xf;
    if (rn == 15 || rm == 15)
        return -1;
    if (!lng) {
        unsigned op2 = (hw2 >> 4) & 3, ra = (hw2 >> 12) & 0xf, rd = (hw2 >> 8) & 0xf;
        if (op1 || op2 > 1 || rd == 15 || (op2 == 1 && ra == 15))
            return -1;
        mov_rm(0, RAX, RG(rn));
        imul_rm(0, RAX, RG(rm));
        if (op2 == 1) {
            mov_rm(0, RCX, RG(ra));
            alu_rr(0, ALU_SUB, RCX, RAX);
            mov_rr(0, RAX, RCX);
        } else if (ra != 15) {
            alu_rm(0, ALU_ADD, RAX, RG(ra));
        }
        mov_mr(0, RG(rd), RAX);
        return 1;
    }
    unsigned op2 = (hw2 >> 4) & 0xf, rlo = (hw2 >> 12) & 0xf, rhi = (hw2 >> 8) & 0xf;
    if (rhi == 15 || (rlo == 15 && op1 != 1 && op1 != 3))
        return -1;
    if (op1 == 1 || op1 == 3) {
        if (op2 != 0xf)
            return -1;
        // Division by zero, and the one signed overflow, go to the handler.
        mov_rm(0, RCX, RG(rm));
        test_rr(RCX, RCX);
        jcc_l(CC_E, lab(&K.l_gen1[k]));
        mov_rm(0, RAX, RG(rn));
        if (op1 == 1) {
            int ok = new_label();
            alu_ri(0, ALU_CMP, RAX, (int32_t)0x80000000u);
            jcc_l(CC_NE, ok);
            alu_ri(0, ALU_CMP, RCX, -1);
            jcc_l(CC_E, lab(&K.l_gen1[k]));
            bind(ok);
            e8(0x99); // cdq
            op_r(0, false, 0xf7, 7, RCX, false); // idiv ecx
        } else {
            alu_rr(0, ALU_XOR, RDX, RDX);
            op_r(0, false, 0xf7, 6, RCX, false); // div ecx
        }
        mov_mr(0, RG(rhi), RAX);
        return 5;
    }
    if (op2 != 0 || !(op1 == 0 || op1 == 2 || op1 == 4 || op1 == 6))
        return -1;
    mov_rm(0, RAX, RG(rn));
    op_m(0, false, 0xf7, (op1 & 2) ? 4 : 5, RG(rm), 0, false); // mul or imul edx:eax
    if (op1 >= 4) {
        alu_rm(0, ALU_ADD, RAX, RG(rlo));
        alu_rm(0, ALU_ADC, RDX, RG(rhi));
    }
    mov_mr(0, RG(rlo), RAX);
    mov_mr(0, RG(rhi), RDX);
    return 2;
}

static int emit_inline(int k, const dop_t *d, int *taken_extra);

static inline uint32_t it_next(uint32_t it)
{
    return (it & 7) ? (it & 0xe0) | ((it << 1) & 0x1f) : 0;
}

// An instruction under IT: skipped unless its condition holds, otherwise done as outside IT, but
// with the 16-bit forms setting no flags. The IT state moves on first, so that instructions which
// end the block need not; the handler path puts it back.
// PBConPC: the RCP's stack canaries, MRC2/MCR2 p7, #0 without delay (rcp_exec): read the canary
// of the instruction's tag, or check a value against it. Only while this core's RCP has a salt
// and no fault; anything else, and a failed check, goes to the handler.
static int emit_rcp(int k, const dop_t *d)
{
    uint16_t hw1 = d->hw1, hw2 = d->hw2;
    if (K.m->rp2040 || ((hw2 >> 8) & 0xf) != 7 || (hw1 & 0xff00) != 0xfe00 || !(hw2 & 0x10))
        return -1;
    unsigned opc1 = (hw1 >> 5) & 7, opc2 = (hw2 >> 5) & 7, rt = hw2 >> 12;
    if (opc1 != 0 || !(opc2 & 1) || rt == 15)
        return -1;
    unsigned tag = (hw1 & 0xf) << 4 | (hw2 & 0xf);
    mov_rm(0, RCX, xm(RBX, CO(id)));
    op_m(0, false, 0x80, 7, xmi(R15, RCX, 1, MO(rcp.fault)), 1, false);   // cmp byte, 0
    e8(0);
    jcc_l(CC_NE, lab(&K.l_gen1[k]));
    op_m(0, false, 0x80, 7, xmi(R15, RCX, 1, MO(rcp.salt_valid)), 1, false);
    e8(0);
    jcc_l(CC_E, lab(&K.l_gen1[k]));
    shift_ri(SH_SHL, RCX, 10);
    mov_rm(0, RAX, xmi(R15, RCX, 1, MO(rcp.canary) + 4 * (int32_t)tag));
    if (hw1 & 0x10) {
        mov_mr(0, RG(rt), RAX);
    } else {
        alu_rm(0, ALU_CMP, RAX, RG(rt));
        jcc_l(CC_NE, lab(&K.l_gen1[k]));
    }
    return 1;
}


// ---------------------------------------------------------------------------------------------
// PBConPC: single-precision FPU instructions natively (fpu.inc's exec_fp otherwise). Exact as the
// handler only while FPSCR rounds to nearest without flush-to-zero or default NaN, and with
// neither NaN going in nor coming out: those cases leave for the handler (l_gen1) before anything
// is written, so NaN payloads and libm's fmaf never come into it. Results are the same SSE
// operations the handler's C compiles to.

static int fp_opt = -1;    // PBCONPC_NO_FPOPT=1: every FPU instruction by its handler
static int fp_fma = -1;    // the host has FMA3 (for VFMA and friends)

#define SREG(i) xm(RBX, CO(s) + 4 * (int32_t)((i) & 31))

static void movd_x_s(int x, unsigned sreg)   // movd xmm(x), s[sreg]
{
    op_m(0, true, 0x0f6e, x, SREG(sreg), 0, false);
}

static void movd_s_x(unsigned sreg, int x)   // movd s[sreg], xmm(x)
{
    op_m(0, true, 0x0f7e, x, SREG(sreg), 0, false);
}

static void sse_ss(unsigned opc, int x, int y)   // opc xmm(x), xmm(y), F3 0F opc
{
    e8(0xf3);
    rex(0, x, 0, y, false);
    e8(0x0f);
    e8(opc);
    e8(0xc0 | (x & 7) << 3 | (y & 7));
}

static void ucomiss(int x, int y)
{
    rex(0, x, 0, y, false);
    e8(0x0f);
    e8(0x2e);
    e8(0xc0 | (x & 7) << 3 | (y & 7));
}

static void fp_sign_flip(int x)   // xmm(x) ^= sign, through eax
{
    movd_r_x(RAX, x);
    alu_ri(0, ALU_XOR, RAX, (int32_t)0x80000000u);
    op_r(0, true, 0x0f6e, x, RAX, false);   // movd xmm(x), eax
}

// The handler's way unless FPSCR is plain (RMode 0, no FZ, no DN).
static void fp_guard(int k)
{
    xm_t f = xm(RBX, CO(fpscr));
    op_m(0, false, 0xf7, 0, f, 4, false);   // test dword, imm32
    e32(0x03c00000u);
    jcc_l(CC_NE, lab(&K.l_gen1[k]));
}

static void fp_nan_out(int k, int x)   // the result xmm(x) a NaN: the handler's way
{
    ucomiss(x, x);
    jcc_l(CC_P, lab(&K.l_gen1[k]));
}

static void fp_fpca(void)   // exec_fp: CONTROL.FPCA
{
    alu_mi(0, ALU_OR, xm(RBX, CO(control)), 4);
}

static int emit_fp(int k, const dop_t *d)
{
    uint16_t hw1 = d->hw1, hw2 = d->hw2;
    unsigned cp = (hw2 >> 8) & 0xf;
    if ((hw1 & 0xef00) != 0xec00 && (hw1 & 0xef00) != 0xed00 && (hw1 & 0xef00) != 0xee00)
        return -1;
    if (hw1 & 0x1000)   // the "2" forms (VSEL, VMAXNM ...): handler
        return -1;
    if (cp != 10 && cp != 11)
        return -1;
    // VLDR, VSTR (single), offset form
    if ((hw1 & 0x0e00) == 0x0c00 && (hw1 & 0x0fe0) != 0x0c40) {
        bool p = hw1 & 0x100, u = hw1 & 0x80, w = hw1 & 0x20, l = hw1 & 0x10;
        unsigned rn = hw1 & 0xf, imm = (hw2 & 0xff) * 4;
        unsigned sd = (((hw2 >> 12) & 0xf) << 1) | ((hw1 >> 6) & 1);
        if (!p || w || (hw2 & 0x100))
            return -1;
        if (rn == 15) {
            if (!l || !K.fixed)
                return -1;
            uint32_t base = (d->pc + 4) & ~3u, a = u ? base + imm : base - imm, pc0 = K.b->tag & ~1u;
            if ((a >> 16) != (pc0 >> 16) || (a & 0xffff) > 0xfffc)
                return -1;
            const uint8_t *page = K.m->rpage[a >> 16];
            if (!page)
                return -1;
            if ((a >> 28) == 1)
                emit_xip_hit(a, lab(&K.l_gen1[k]));
            fp_fpca();
            mov_mi(SREG(sd), rd32le(page + (a & 0xffff)));
            return 2;
        }
        fp_fpca();
        mov_rm(0, RAX, RG(rn));
        if (imm)
            alu_ri(0, ALU_ADD, RAX, (int32_t)(u ? imm : 0u - imm));
        if (l) {
            K.ld[k].size = 4;
            K.ld[k].sign = false;
            emit_page(k, MO(rpage_plain), 3, 0);
            emit_host_load(4, false, xmi(RDX, RAX, 1, 0));
            bind(lab(&K.l_loaded[k]));
            mov_mr(0, SREG(sd), RAX);
        } else {
            emit_page(k, MO(wpage), 3, 0);
            mov_rm(0, RCX, SREG(sd));
            mov_mr(0, xmi(RDX, RAX, 1, 0), RCX);
        }
        return 2;
    }
    // VMOV Sm, Sm1, Rt, Rt2 and back
    if ((hw1 & 0x0fe0) == 0x0c40) {
        unsigned rt = (hw2 >> 12) & 0xf, rt2 = hw1 & 0xf;
        unsigned mreg = (hw2 & 0x100) ? ((((hw2 >> 5) & 1) << 4) | (hw2 & 0xf)) * 2 : (((hw2 & 0xf) << 1) | ((hw2 >> 5) & 1));
        if (mreg > 30 || rt == 15 || rt2 == 15 || ((hw1 & 0x10) && rt == rt2))
            return -1;
        fp_fpca();
        if (hw1 & 0x10) {
            mov_rm(0, RAX, SREG(mreg));
            mov_mr(0, RG(rt), RAX);
            mov_rm(0, RAX, SREG(mreg + 1));
            mov_mr(0, RG(rt2), RAX);
        } else {
            mov_rm(0, RAX, RG(rt));
            mov_mr(0, SREG(mreg), RAX);
            mov_rm(0, RAX, RG(rt2));
            mov_mr(0, SREG(mreg + 1), RAX);
        }
        return 1;
    }
    if ((hw1 & 0x0f00) != 0x0e00)
        return -1;
    if (hw2 & 0x10) {
        // transfers: VMOV Rt <-> Sn, VMRS
        unsigned rt = (hw2 >> 12) & 0xf, a = (hw1 >> 5) & 7;
        bool l = hw1 & 0x10;
        if (hw2 & 0x100)
            return -1;
        if (a == 0) {
            unsigned n = ((hw1 & 0xf) << 1) | ((hw2 >> 7) & 1);
            if (rt == 15)
                return -1;
            fp_fpca();
            if (l) {
                mov_rm(0, RAX, SREG(n));
                mov_mr(0, RG(rt), RAX);
            } else {
                mov_rm(0, RAX, RG(rt));
                mov_mr(0, SREG(n), RAX);
            }
            return 1;
        }
        if (a == 7 && l && (hw1 & 0xf) == 1) {
            fp_fpca();
            mov_rm(0, RAX, xm(RBX, CO(fpscr)));
            if (rt == 15 && fx_mode) {
                shift_ri(SH_SHR, RAX, 28);
                op_m(0, false, 0x0fb7, RAX, xmi(R14, RAX, 2, XO(fx_of_nzcv)), 0, false);
                mov_mr(0, FX, RAX);
            } else if (rt == 15) {
                alu_ri(0, ALU_AND, RAX, (int32_t)0xf0000000u);
                alu_mi(0, ALU_AND, APSR, 0x0fffffff);
                alu_mr(0, ALU_OR, APSR, RAX);
            } else {
                mov_mr(0, RG(rt), RAX);
            }
            return 1;
        }
        return -1;
    }
    // data processing
    if (hw2 & 0x100)   // double precision: handler (undefined)
        return -1;
    unsigned sd = (((hw2 >> 12) & 0xf) << 1) | ((hw1 >> 6) & 1);
    unsigned sn = ((hw1 & 0xf) << 1) | ((hw2 >> 7) & 1);
    unsigned sm = ((hw2 & 0xf) << 1) | ((hw2 >> 5) & 1);
    bool op = hw2 & 0x40;
    unsigned o1 = (hw1 >> 4) & 0xb;
    if (o1 == 0xb) {
        unsigned opc2 = hw1 & 0xf;
        bool t = hw2 & 0x80;
        if (!op) {   // VMOV immediate
            unsigned imm8 = ((hw1 & 0xf) << 4) | (hw2 & 0xf);
            uint32_t bit6 = (imm8 >> 6) & 1;
            uint32_t exp = ((bit6 ^ 1) << 7) | (bit6 ? 0x7c : 0) | ((imm8 >> 4) & 3);
            fp_fpca();
            mov_mi(SREG(sd), ((imm8 >> 7) << 31) | (exp << 23) | ((imm8 & 0xf) << 19));
            return 1;
        }
        if (opc2 == 0 || (opc2 == 1 && !t)) {   // VMOV, VABS, VNEG
            fp_fpca();
            mov_rm(0, RAX, SREG(sm));
            if (opc2 == 0 && t)
                alu_ri(0, ALU_AND, RAX, 0x7fffffff);
            else if (opc2 == 1)
                alu_ri(0, ALU_XOR, RAX, (int32_t)0x80000000u);
            mov_mr(0, SREG(sd), RAX);
            return 1;
        }
        if (opc2 == 1 && t) {   // VSQRT
            fp_guard(k);
            movd_x_s(1, sm);
            ucomiss(1, 1);
            jcc_l(CC_P, lab(&K.l_gen1[k]));
            sse_ss(0x51, 0, 1);   // sqrtss xmm0, xmm1
            fp_nan_out(k, 0);
            fp_fpca();
            movd_s_x(sd, 0);
            return 14;
        }
        if (opc2 == 4 || opc2 == 5) {   // VCMP, VCMPE (with zero)
            int eq = new_label(), lt = new_label(), done = new_label();
            fp_guard(k);
            movd_x_s(0, sd);
            if (opc2 == 4)
                movd_x_s(1, sm);
            else
                op_r(0, true, 0x0fef, 1, 1, false);   // pxor xmm1, xmm1
            fp_fpca();   // before the comparison, whose flags the branches below use
            ucomiss(0, 1);
            jcc_l(CC_P, lab(&K.l_gen1[k]));
            jcc_l(CC_E, eq);
            jcc_l(CC_B, lt);
            mov_ri(RCX, 0x20000000u);
            jmp_l(done);
            bind(eq);
            mov_ri(RCX, 0x60000000u);
            jmp_l(done);
            bind(lt);
            mov_ri(RCX, 0x80000000u);
            bind(done);
            alu_mi(0, ALU_AND, xm(RBX, CO(fpscr)), 0x0fffffff);
            alu_mr(0, ALU_OR, xm(RBX, CO(fpscr)), RCX);
            return 1;
        }
        if (opc2 == 8) {   // VCVT.F32.S32 / .U32: (float)(int32_t) or (float)(uint32_t)
            fp_guard(k);
            fp_fpca();
            mov_rm(0, RAX, SREG(sm));   // zero-extends into rax, for the unsigned one
            op_r(0, true, 0x0fef, 0, 0, false);   // pxor xmm0, xmm0
            e8(0xf3);
            rex(t ? 0 : 1, 0, 0, RAX, false);     // cvtsi2ss xmm0, eax / rax
            e8(0x0f);
            e8(0x2a);
            e8(0xc0 | (RAX & 7));
            movd_s_x(sd, 0);
            return 1;
        }
        if (opc2 == 0xc || opc2 == 0xd) {   // VCVT(R).S32/U32.F32
            bool is_signed = opc2 & 1;
            fp_guard(k);
            movd_x_s(1, sm);
            ucomiss(1, 1);
            jcc_l(CC_P, lab(&K.l_gen1[k]));
            // cvt(t)ss2si rax, xmm1: exact for |x| < 2^63, else 0x8000... which fails the range test
            e8(0xf3);
            rex(1, RAX, 0, 1, false);
            e8(0x0f);
            e8(t ? 0x2c : 0x2d);
            e8(0xc0 | (RAX & 7) << 3 | 1);
            if (is_signed) {
                op_r(1, false, 0x63, RCX, RAX, false);   // movsxd rcx, eax
                alu_rr(1, ALU_CMP, RCX, RAX);
            } else {
                mov_rr(0, RCX, RAX);                     // zero-extends
                alu_rr(1, ALU_CMP, RCX, RAX);
            }
            jcc_l(CC_NE, lab(&K.l_gen1[k]));             // out of range: saturates, IOC
            fp_fpca();
            mov_mr(0, SREG(sd), RAX);
            return 1;
        }
        return -1;
    }
    // arithmetic
    bool three = o1 == 0x0 || o1 == 0x1 || o1 == 0x9 || o1 == 0xa;
    if (o1 != 0x0 && o1 != 0x1 && o1 != 0x2 && o1 != 0x3 && o1 != 0x8 && !((o1 == 0x9 || o1 == 0xa) && fp_fma))
        return -1;
    fp_guard(k);
    movd_x_s(0, sn);
    movd_x_s(1, sm);
    ucomiss(0, 1);
    jcc_l(CC_P, lab(&K.l_gen1[k]));
    if (three) {
        movd_x_s(2, sd);
        ucomiss(2, 2);
        jcc_l(CC_P, lab(&K.l_gen1[k]));
    }
    int st = 1;
    switch (o1) {
    case 0x0:   // VMLA, VMLS: dd + p, dd - p with p = a * b rounded
    case 0x1:   // VNMLA, VNMLS: -dd - p, -dd + p
        sse_ss(0x59, 0, 1);   // mulss xmm0, xmm1: p
        if (o1 == 0x1)
            fp_sign_flip(2);
        if (op)
            sse_ss(0x5c, 2, 0);   // subss xmm2, xmm0
        else
            sse_ss(0x58, 2, 0);   // addss xmm2, xmm0
        op_r(0, true, 0x0f6f, 0, 2, false);   // movdqa xmm0, xmm2
        break;
    case 0x2:   // VMUL, VNMUL
        sse_ss(0x59, 0, 1);
        if (op)
            fp_sign_flip(0);
        break;
    case 0x3:   // VADD, VSUB
        sse_ss(op ? 0x5c : 0x58, 0, 1);
        break;
    case 0x8: { // VDIV: DZC on a zero divisor, as the handler
        int nz = new_label();
        op_r(0, true, 0x0fef, 3, 3, false);   // pxor xmm3, xmm3
        ucomiss(1, 3);
        jcc_l(CC_NE, nz);
        alu_mi(0, ALU_OR, xm(RBX, CO(fpscr)), 2);
        bind(nz);
        sse_ss(0x5e, 0, 1);   // divss
        st = 14;
        break;
    }
    case 0x9:   // VFNMS, VFNMA: fmaf(a, b, -dd), fmaf(-a, b, -dd)
    case 0xa:   // VFMA, VFMS: fmaf(a, b, dd), fmaf(-a, b, dd)
        if (op)
            fp_sign_flip(0);
        if (o1 == 0x9)
            fp_sign_flip(2);
        // vfmadd231ss xmm2, xmm0, xmm1: xmm2 = xmm0 * xmm1 + xmm2, one rounding
        e8(0xc4); e8(0xe2); e8(0x79); e8(0xb9); e8(0xd1);
        op_r(0, true, 0x0f6f, 0, 2, false);   // movdqa xmm0, xmm2
        break;
    }
    fp_nan_out(k, 0);
    fp_fpca();
    movd_s_x(sd, 0);
    return st;
}

static int it_fetch_opt = -1;   // PBCONPC_NO_ITFETCHOPT=1: wide instructions under IT that fetch, by handler

static int emit_it_op(int k, const dop_t *d, int *taken_extra)
{
    unsigned cond = K.it[k] >> 4;
    bool wide = (d->lf & LF_LEN) == 4;
    if (cond >= 0xe || ((d->lf & FETCH_HW2) && !it_fetch_opt))
        return -1;
    dop_t t = *d;
    if (!wide)
        decode16(&t, d->pc, d->hw1);
    if (t.fn == h_bcond || t.fn == h_cbz)
        return -1;
    uint8_t *mark = J.p;
    int fixmark = nfix, xipwmark = K.nxipw;
    emit_flags_cond_jump(cond ^ 1, lab(&K.l_skip[k]));
    mov_mi(xm(RBX, CO(itstate)), it_next(K.it[k]));
    if (d->lf & FETCH_HW2) {
        // PBConPC: the second halfword, fetched once the condition passes, as jit_it_op does;
        // a miss goes to the handler's way, which puts the IT state back and charges it
        emit_xip_hit(d->pc + 2, lab(&K.l_gen1[k]));
        mov_mi(xm(RBX, CO(fetch_line)), (d->pc + 2) & ~7u);
    }
    K.nf = !wide;
    int st = emit_inline(k, &t, taken_extra);
    K.nf = false;
    if (st < 0) {
        J.p = mark;
        nfix = fixmark;
        K.l_skip[k] = -1;
        K.pg[k].tab = K.pg[k].ua = -1;
        K.nxipw = xipwmark;
        return -1;
    }
    return st;
}

// Native code for instruction k, returning its fixed cycle count, or -1 to use the handler, in
// which case nothing has been emitted. *taken gets the extra cycles of a taken branch.
static int emit_inline(int k, const dop_t *d, int *taken_extra)
{
    dop_fn fn = d->fn;
    bool last = k == K.n - 1;
    *taken_extra = 0;

    if (K.it[k] && d == &K.b->ops[k])
        return emit_it_op(k, d, taken_extra);
    if (fn == h_nop)
        return 1;
    if (fn == h_exec16 && is_it(d->hw1) && !last) {
        mov_mi(xm(RBX, CO(itstate)), d->hw1 & 0xff);
        return 1;
    }
    int sys = emit_system(k, d);
    if (sys >= 0)
        return sys;
    if (fn == h_ldst_single)
        return emit_ldst_single(k, d);
    if (fn == h_ldm_stm)
        return emit_ldm_stm(k, d);
    if (fn == h_ldst_dual_excl)
        return emit_excl(k, d);
    if (fn == h_dp_shifted_reg)
        return emit_dp_shifted(k, d);
    if (fn == h_dp_register)
        return emit_dp_register(k, d);
    if (fn == h_dp_plain_imm)
        return emit_dp_plain(k, d);
    if (fn == h_exec32 && dsp_opt) {
        int st = emit_rcp(k, d);
        if (st >= 0)
            return st;
    }
    if (fn == h_exec32 && fp_opt) {
        uint8_t *mark = J.p;
        int fixmark = nfix;
        int st = emit_fp(k, d);
        if (st >= 0)
            return st;
        J.p = mark;   // a form given up on part-way
        nfix = fixmark;
    }
    if (fn == h_multiply || fn == h_long_multiply)
        return emit_multiply(k, d, fn == h_long_multiply);

    // In an IT block (K.nf), the 16-bit data-processing forms leave the flags alone, apart from
    // the comparisons.
    if (fn == h_movs_reg) {
        mov_rm(0, RAX, RG(d->rm));
        mov_mr(0, RG(d->rd), RAX);
        if (!K.nf) {
            test_rr(RAX, RAX);
            emit_flags_nz(-1);
        }
        return 1;
    }
    if (fn == h_lsl_imm || ((fn == h_lsr_imm || fn == h_asr_imm) && d->imm)) {
        mov_rm(0, RAX, RG(d->rm));
        shift_ri(fn == h_lsl_imm ? SH_SHL : fn == h_lsr_imm ? SH_SHR : SH_SAR, RAX, (int)d->imm);
        mov_mr(0, RG(d->rd), RAX);
        if (!K.nf)
            emit_flags_nzc();
        return 1;
    }
    if (fn == h_add_reg || fn == h_sub_reg || fn == h_add_imm || fn == h_sub_imm || fn == h_cmp_imm ||
        fn == h_cmp_reg) {
        bool sub = fn != h_add_reg && fn != h_add_imm;
        bool reg = fn == h_add_reg || fn == h_sub_reg || fn == h_cmp_reg;
        bool store = fn != h_cmp_imm && fn != h_cmp_reg;
        alu_rr(0, ALU_XOR, RDX, RDX);
        mov_rm(0, RAX, RG(d->rn));
        int op = store ? (sub ? ALU_SUB : ALU_ADD) : ALU_CMP;
        if (reg)
            alu_rm(0, op, RAX, RG(d->rm));
        else
            alu_ri(0, op, RAX, (int32_t)d->imm);
        if (store)
            mov_mr(0, RG(d->rd), RAX);
        if (!K.nf || !store)
            emit_flags_arith(sub);
        return 1;
    }
    if (fn == h_movs_imm) {
        mov_mi(RG(d->rd), d->imm);
        if (!K.nf)
            emit_flags_const(d->imm, -1);
        return 1;
    }
    if (fn == h_alu) {
        unsigned rdn = d->rd, rm = d->rm;
        switch (d->imm) {
        case 0x0:
        case 0x1:
        case 0x8:
        case 0xc:
            mov_rm(0, RAX, RG(rdn));
            alu_rm(0, d->imm == 0x1 ? ALU_XOR : d->imm == 0xc ? ALU_OR : ALU_AND, RAX, RG(rm));
            if (d->imm != 0x8)
                mov_mr(0, RG(rdn), RAX);
            if (!K.nf || d->imm == 0x8)
                emit_flags_nz(-1);
            return 1;
        case 0x2:
        case 0x3:
        case 0x4:
        case 0x7:
            // LSL, LSR, ASR, ROR by a register, with no flags wanted (dead, or under IT)
            if (!dsp_opt || !(K.nf || K.dead[K.cur]))
                return -1;
            emit_shift_by_reg(d->imm == 0x2 ? SH_SHL : d->imm == 0x3 ? SH_SHR : d->imm == 0x4 ? SH_SAR : 1, rdn, rm, rdn);
            return 1;
        case 0x5:
        case 0x6:
            alu_rr(0, ALU_XOR, RDX, RDX);
            emit_carry_in(d->imm == 0x5); // SBC as SBB: x86 borrows where ARM's carry is clear
            mov_rm(0, RAX, RG(rdn));
            alu_rm(0, d->imm == 0x5 ? ALU_ADC : ALU_SBB, RAX, RG(rm));
            mov_mr(0, RG(rdn), RAX);
            if (!K.nf)
                emit_flags_arith(d->imm == 0x6);
            return 1;
        case 0x9:
            alu_rr(0, ALU_XOR, RDX, RDX);
            alu_rr(0, ALU_XOR, RAX, RAX);
            alu_rm(0, ALU_SUB, RAX, RG(rm));
            mov_mr(0, RG(rdn), RAX);
            if (!K.nf)
                emit_flags_arith(true);
            return 1;
        case 0xb:
            alu_rr(0, ALU_XOR, RDX, RDX);
            mov_rm(0, RAX, RG(rdn));
            alu_rm(0, ALU_ADD, RAX, RG(rm));
            emit_flags_arith(false);
            return 1;
        case 0xd:
            mov_rm(0, RAX, RG(rdn));
            imul_rm(0, RAX, RG(rm));
            mov_mr(0, RG(rdn), RAX);
            if (!K.nf) {
                test_rr(RAX, RAX);
                emit_flags_nz(-1);
            }
            return 1;
        case 0xe:
            mov_rm(0, RCX, RG(rm));
            not_r(RCX);
            mov_rm(0, RAX, RG(rdn));
            alu_rr(0, ALU_AND, RAX, RCX);
            mov_mr(0, RG(rdn), RAX);
            if (!K.nf)
                emit_flags_nz(-1);
            return 1;
        case 0xf:
            mov_rm(0, RAX, RG(rm));
            not_r(RAX);
            mov_mr(0, RG(rdn), RAX);
            if (!K.nf) {
                test_rr(RAX, RAX);
                emit_flags_nz(-1);
            }
            return 1;
        default:
            return -1;
        }
    }
    if (fn == h_add_hi) {
        mov_rm(0, RAX, RG(d->rm));
        alu_mr(0, ALU_ADD, RG(d->rd), RAX);
        return 1;
    }
    if (fn == h_mov_hi) {
        mov_rm(0, RAX, RG(d->rm));
        mov_mr(0, RG(d->rd), RAX);
        return 1;
    }
    if (fn == h_set_const) {
        mov_mi(RG(d->rd), d->imm);
        return 1;
    }
    if (fn == h_add_sp_imm) {
        emit_addr_imm(13, d->imm);
        mov_mr(0, RG(d->rd), RAX);
        return 1;
    }
    if (fn == h_adjust_sp) {
        alu_mi(0, ALU_ADD, RG(13), (int32_t)d->imm);
        return 1;
    }
    if (fn == h_extend) {
        static const unsigned opc[4] = {0x0fbf, 0x0fbe, 0x0fb7, 0x0fb6};
        op_m(0, false, opc[d->imm & 3], RAX, RG(d->rm), 0, false);
        mov_mr(0, RG(d->rd), RAX);
        return 1;
    }
    if (fn == h_ldr_lit) {
        uint32_t a = d->imm, pc0 = K.b->tag & ~1u;
        if ((a >> 16) != (pc0 >> 16) || (a & 3))
            return -1;
        const uint8_t *page = K.m->rpage[a >> 16];
        if (!page)
            return -1;
        if (K.fixed) {
            // ROM and flash hold still for as long as the block is current.
            if ((a >> 28) == 1)
                emit_xip_hit(a, lab(&K.l_gen1[k]));
            mov_mi(RG(d->rd), rd32le(page + (a & 0xffff)));
        } else {
            mov_rm(0, RAX, xm(R15, MO(sram) + (int32_t)(a - SRAM_BASE)));
            mov_mr(0, RG(d->rd), RAX);
        }
        return 2;
    }
    if (fn == h_ldr_imm || fn == h_ldrh_imm || fn == h_ldrb_imm || fn == h_ldr_w || fn == h_ldrh_w ||
        fn == h_ldrb_w) {
        int size = (fn == h_ldr_imm || fn == h_ldr_w) ? 4 : (fn == h_ldrh_imm || fn == h_ldrh_w) ? 2 : 1;
        K.mm[k].size = (uint8_t)size;
        K.mm[k].reg = d->rd;
        K.mm[k].load = true;
        K.mm[k].keep_on_fault = fn == h_ldr_w || fn == h_ldrh_w || fn == h_ldrb_w;
        emit_addr_imm(d->rn, d->imm);
        ua_next = true;
        emit_load_value(k, size, false);
        mov_mr(0, RG(d->rd), RAX);
        return 2;
    }
    if (fn == h_str_imm || fn == h_strh_imm || fn == h_strb_imm) {
        int size = fn == h_str_imm ? 4 : fn == h_strh_imm ? 2 : 1;
        K.mm[k].size = (uint8_t)size;
        K.mm[k].reg = d->rd;
        emit_addr_imm(d->rn, d->imm);
        ua_next = true;
        emit_store_value(k, size, d->rd);
        return 2;
    }
    if (fn == h_ldst_reg) {
        static const int8_t size[8] = {4, 2, 1, 1, 4, 2, 1, 2};
        K.mm[k].size = (uint8_t)size[d->imm];
        K.mm[k].reg = d->rd;
        K.mm[k].load = d->imm >= 3;
        K.mm[k].sign = d->imm == 3 || d->imm == 7;
        mov_rm(0, RAX, RG(d->rn));
        alu_rm(0, ALU_ADD, RAX, RG(d->rm));
        ua_next = true;
        if (d->imm < 3) {
            emit_store_value(k, size[d->imm], d->rd);
        } else {
            emit_load_value(k, size[d->imm], d->imm == 3 || d->imm == 7);
            mov_mr(0, RG(d->rd), RAX);
        }
        return 2;
    }
    if (fn == h_ldr_w8 || fn == h_str_w8) {
        bool index = d->lf & LF_A, wb = d->lf & LF_B;
        emit_addr_imm(d->rn, index ? d->imm : 0);
        ua_next = true;
        if (fn == h_ldr_w8)
            emit_load_value(k, 4, false);
        else
            emit_store_value(k, 4, d->rd);
        if (wb) {
            mov_rm(0, RCX, RG(d->rn));
            alu_ri(0, ALU_ADD, RCX, (int32_t)d->imm);
            mov_mr(0, RG(d->rn), RCX);
        }
        if (fn == h_ldr_w8)
            mov_mr(0, RG(d->rd), RAX);
        return 2;
    }
    if (fn == h_ldrd_strd) {
        bool index = d->lf & LF_A, wb = d->lf & LF_B, load = d->lf & LF_C;
        if (d->rd == 15 || d->rm == 15 || (wb && (d->rd == d->rn || d->rm == d->rn)))
            return -1;
        emit_addr_imm(d->rn, index ? d->imm : 0);
        emit_page(k, load ? MO(rpage_plain) : MO(wpage), 3, 8);
        if (load) {
            mov_rm(0, RCX, xmi(RDX, RAX, 1, 0));
            mov_rm(0, RAX, xmi(RDX, RAX, 1, 4));
            mov_mr(0, RG(d->rd), RCX);
            mov_mr(0, RG(d->rm), RAX);
        } else {
            mov_rm(0, RCX, RG(d->rd));
            mov_mr(0, xmi(RDX, RAX, 1, 0), RCX);
            mov_rm(0, RCX, RG(d->rm));
            mov_mr(0, xmi(RDX, RAX, 1, 4), RCX);
        }
        if (wb) {
            mov_rm(0, RCX, RG(d->rn));
            alu_ri(0, ALU_ADD, RCX, (int32_t)d->imm);
            mov_mr(0, RG(d->rn), RCX);
        }
        return 3;
    }
    if (fn == h_push) {
        unsigned n = d->rm;
        mov_rm(0, RAX, RG(13));
        alu_ri(0, ALU_SUB, RAX, (int32_t)(4 * n));
        mov_rr(0, R8, RAX);
        emit_page(k, MO(wpage), 3, 4 * n);
        mov_mr(0, RG(13), R8);
        int32_t off = 0;
        for (unsigned i = 0; i < 15; i++)
            if (d->imm & (1u << i)) {
                mov_rm(0, RCX, RG(i));
                mov_mr(0, xmi(RDX, RAX, 1, off), RCX);
                off += 4;
            }
        return 1 + (int)n;
    }
    if (fn == h_pop) {
        unsigned list = d->imm & 0xff, nl = (unsigned)__builtin_popcount(list);
        bool pc = d->imm & 0x100;
        unsigned n = nl + (pc ? 1 : 0);
        mov_rm(0, RAX, RG(13));
        emit_page(k, MO(rpage_plain), 3, 4 * n);
        if (pc) {
            mov_rm(0, R9, xmi(RDX, RAX, 1, 4 * (int32_t)nl));
            alu_ri(0, ALU_CMP, R9, (int32_t)0xff000000u);
            jcc_l(CC_AE, lab(&K.l_gen1[k]));
            test_ri(R9, 1);
            jcc_l(CC_E, lab(&K.l_gen1[k]));
        }
        int32_t off = 0;
        for (unsigned i = 0; i < 8; i++)
            if (list & (1u << i)) {
                mov_rm(0, RCX, xmi(RDX, RAX, 1, off));
                mov_mr(0, RG(i), RCX);
                off += 4;
            }
        alu_mi(0, ALU_ADD, RG(13), (int32_t)(4 * n));
        if (!pc)
            return 1 + (int)n;
        mov_rr(0, RAX, R9);
        alu_ri(0, ALU_AND, RAX, -2);
        emit_to_eax(k, 3 + (int)n);
        return 3 + (int)n;
    }
    if (fn == h_dp_imm) {
        unsigned op = d->rm, rd = d->rd, rn = d->rn;
        bool s = d->lf & LF_A;
        int carry = (d->lf & LF_B) ? (int)(d->imm >> 31) : -1;
        uint32_t imm = d->imm;
        switch (op) {
        case 0x0:
        case 0x1:
        case 0x2:
        case 0x3:
        case 0x4: {
            bool test_ok = op == 0x0 || op == 0x4;
            if ((rd == 15 && !test_ok) || (rn == 15 && op != 0x2 && op != 0x3))
                return -1;
            if (rn == 15) {
                uint32_t v = op == 0x2 ? imm : ~imm;
                mov_mi(RG(rd), v);
                if (s)
                    emit_flags_const(v, carry);
                return 1;
            }
            mov_rm(0, RAX, RG(rn));
            int aop = (op == 0x0 || op == 0x1) ? ALU_AND : (op == 0x4) ? ALU_XOR : ALU_OR;
            uint32_t b = (op == 0x1 || op == 0x3) ? ~imm : imm;
            alu_ri(0, aop, RAX, (int32_t)b);
            if (rd != 15)
                mov_mr(0, RG(rd), RAX);
            if (s)
                emit_flags_nz(carry);
            return 1;
        }
        case 0x8:
        case 0xd:
        case 0xa:
        case 0xb:
        case 0xe: {
            if (rn == 15 || (rd == 15 && op != 0x8 && op != 0xd))
                return -1;
            bool sub = op == 0xd || op == 0xb || op == 0xe;
            if (s)
                alu_rr(0, ALU_XOR, RDX, RDX);
            if (op == 0xe) {
                mov_ri(RAX, imm);
                alu_rm(0, ALU_SUB, RAX, RG(rn));
            } else {
                if (op == 0xa || op == 0xb)
                    emit_carry_in(op == 0xa);
                mov_rm(0, RAX, RG(rn));
                static const int aops[16] = {[0x8] = ALU_ADD, [0xa] = ALU_ADC, [0xb] = ALU_SBB, [0xd] = ALU_SUB};
                alu_ri(0, aops[op], RAX, (int32_t)imm);
            }
            if (rd != 15)
                mov_mr(0, RG(rd), RAX);
            if (s)
                emit_flags_arith(sub);
            return 1;
        }
        default:
            return -1;
        }
    }

    // Branches, which end the block.
    if (!last)
        return -1;
    if (fn == h_bcond || fn == h_cbz) {
        int taken = new_label();
        if (fn == h_bcond && K.fuse_done && K.fuse == k - 1) {
            op_m(0, false, 0x80, 7, xm(R14, XO(fcond)), 1, false); // cmp byte, 0
            e8(0);
            jcc_l(CC_NE, taken);
        } else if (fn == h_bcond) {
            emit_flags_cond_jump(d->rd, taken);
        } else {
            alu_mi(0, ALU_CMP, RG(d->rn), 0);
            jcc_l(d->rd ? CC_NE : CC_E, taken);
        }
        emit_two_way(k, 1, taken, d->imm);
        *taken_extra = 1;
        return 1;
    }
    if (fn == h_branch || fn == h_bl) {
        int st = fn == h_bl ? 3 : 2;
        if (fn == h_bl)
            mov_mi(RG(14), (d->pc + 4) | 1);
        alu_ps(ALU_ADD, R12, K.sb[k] + st);
        emit_add_insns(K.n);
        emit_link(d->imm);
        K.ended = true;
        return st;
    }
    if (fn == h_bx || fn == h_blx) {
        mov_rm(0, RAX, RG(d->rm));
        emit_check_target(k);
        if (fn == h_blx)
            mov_mi(RG(14), (d->pc + 2) | 1);
        emit_to_eax(k, 2);
        return 2;
    }
    return -1;
}

static void emit_block_entry(void)
{
    const block_t *b = K.b;
    uint32_t pc0 = b->tag & ~1u;
    if (K.fixed) {
        // The first instruction must be at least five bytes, for patching into a jump.
        xm_t g = xm(R15, MO(code_gen));
        g.d32 = true;
        op_m(0, false, 0x81, 7, g, 4, false);
        e32(b->gen);
        jcc_l(CC_NE, K.l_stale);
    } else {
        int32_t base = MO(sram) + (int32_t)(pc0 - SRAM_BASE);
        unsigned n = b->bytes, off = 0;
        if (n >= 32 && avx2_entry) {
            // PBConPC: 32 bytes at a time: load, compare with the copy after the block's code
            // (K.cmp_lab), mask, test; vzeroupper for the SSE code elsewhere
            K.ncmp = 0;
            for (;;) {
                unsigned o = off + 32 <= n ? off : n - 32;
                int l = new_label();
                if (K.ncmp < (int)(sizeof(K.cmp_lab) / sizeof(K.cmp_lab[0]))) {
                    K.cmp_lab[K.ncmp] = l;
                    K.cmp_off[K.ncmp++] = (uint16_t)o;
                } else {
                    jit_overflow = true;
                }
                // vmovdqu ymm0, [r15 + base + o]
                e8(0xc4); e8(0xc1); e8(0x7e); e8(0x6f); e8(0x87);
                e32((uint32_t)(base + (int32_t)o));
                // vpcmpeqb ymm0, ymm0, [rip + copy + o]
                e8(0xc5); e8(0xfd); e8(0x74); e8(0x05);
                add_fixup(J.p, l, 0);
                e32(0);
                // vpmovmskb eax, ymm0
                e8(0xc5); e8(0xfd); e8(0xd7); e8(0xc0);
                e8(0xc5); e8(0xf8); e8(0x77);   // vzeroupper
                alu_ri(0, ALU_CMP, RAX, -1);
                jcc_l(CC_NE, K.l_stale);
                if (off + 32 >= n)
                    break;
                off += 32;
            }
        } else if (n >= 8) {
            for (; off + 8 <= n; off += 8) {
                uint64_t v;
                memcpy(&v, b->code + off, 8);
                mov_ri64(RAX, v);
                alu_rm(1, ALU_CMP, RAX, xm(R15, base + (int32_t)off));
                jcc_l(CC_NE, K.l_stale);
            }
            if (off < n) {
                off = n - 8;
                uint64_t v;
                memcpy(&v, b->code + off, 8);
                mov_ri64(RAX, v);
                alu_rm(1, ALU_CMP, RAX, xm(R15, base + (int32_t)off));
                jcc_l(CC_NE, K.l_stale);
            }
        } else {
            for (; off < n; off += (n - off >= 4) ? 4 : 2) {
                xm_t at = xm(R15, base + (int32_t)off);
                at.d32 = true;
                if (n - off >= 4) {
                    op_m(0, false, 0x81, 7, at, 4, false);
                    e32(rd32le(b->code + off));
                } else {
                    op_m(0, true, 0x81, 7, at, 2, false);
                    e8(b->code[off]);
                    e8(b->code[off + 1]);
                }
                jcc_l(CC_NE, K.l_stale);
            }
        }
    }
    // PBConPC: the code after the block that its flags analysis relied on (flag_liveness)
    for (int i = 0; i < K.nsucc; i++) {
        int32_t base = MO(sram) + (int32_t)(K.succ[i].pc - SRAM_BASE);
        unsigned n = K.succ[i].len, off = 0;
        const uint8_t *bytes = K.succ[i].bytes;
        K.l_drop = K.l_drop < 0 ? new_label() : K.l_drop;
        while (off < n) {
            unsigned rest = n - off;
            if (rest >= 8 || (n >= 8 && rest > 4)) {
                if (rest < 8)
                    off = n - 8;
                uint64_t v;
                memcpy(&v, bytes + off, 8);
                mov_ri64(RAX, v);
                alu_rm(1, ALU_CMP, RAX, xm(R15, base + (int32_t)off));
                off += 8;
            } else if (rest >= 4) {
                xm_t at = xm(R15, base + (int32_t)off);
                at.d32 = true;
                op_m(0, false, 0x81, 7, at, 4, false);
                e32(rd32le(bytes + off));
                off += 4;
            } else {
                xm_t at = xm(R15, base + (int32_t)off);
                at.d32 = true;
                op_m(0, true, 0x81, 7, at, 2, false);
                e8(bytes[off]);
                e8(bytes[off + 1]);
                off += 2;
            }
            jcc_l(CC_NE, K.l_drop);
        }
    }
    if (!K.careful) {
        emit_budget_check(0);
        K.pre_site = J.p - 4;
    }
}

// PBConPC: a fetch at addr that missed: xip_access's cycles onto the time, and the line fetched.
static void emit_fetch_miss(uint32_t addr)
{
    emit_spill();
    mov_rr(1, ARG0, R15);
    mov_ri(ARG1, addr);
    emit_count_call(1);
    call_a((const void *)xip_access);
    mov_rr(0, RAX, RAX); // zero-extend
    imul_ps(RAX);
    alu_rr(1, ALU_ADD, R12, RAX);
    mov_mi(xm(RBX, CO(fetch_line)), addr & ~7u);
}

static void emit_cold(void)
{
    const block_t *b = K.b;
    int n = K.n;

    emit_xip_ways();
    for (int k = 0; k < n; k++) {
        if (K.pg[k].tab >= 0)
            emit_page_table(k);
        if (K.pg[k].ua >= 0)
            emit_page_unaligned(k);
    }

    for (int k = 0; k < n; k++) {
        const dop_t *d = &b->ops[k];
        // PBConPC: an instruction translated natively goes back to its native code after a miss,
        // with the miss's cycles charged and the time checked as its handler's way would have
        bool back = K.l_fetched[k] >= 0 && !K.generic[k];
        if (K.l_fetch1[k] >= 0 && back) {
            bind(K.l_fetch1[k]);
            emit_fetch_miss(d->pc);
            if (d->lf & FETCH_HW2)
                emit_fetch_miss(d->pc + 2);
            emit_budget_check(k);
            jmp_l(K.l_fetched[k]);
        } else if (K.l_fetch1[k] >= 0) {
            bind(K.l_fetch1[k]);
            emit_spill();
            mov_rr(1, ARG0, R15);
            mov_ri(ARG1, d->pc);
            emit_count_call(1);
            call_a((const void *)xip_access);
            alu_ri(0, ALU_ADD, RAX, 1);
            mov_mr(0, xm(RBX, CO(cyc)), RAX);
            mov_mi(xm(RBX, CO(fetch_line)), d->pc & ~7u);
            if ((d->lf & FETCH_HW2) && !K.it[k]) {
                emit_spill();
                mov_rr(1, ARG0, R15);
                mov_ri(ARG1, d->pc + 2);
                emit_count_call(1);
            call_a((const void *)xip_access);
                alu_mr(0, ALU_ADD, xm(RBX, CO(cyc)), RAX);
                mov_mi(xm(RBX, CO(fetch_line)), (d->pc + 2) & ~7u);
            }
            jmp_l(lab(&K.l_gencyc[k]));
        }
        if (K.l_fetch2[k] >= 0 && back) {
            bind(K.l_fetch2[k]);
            emit_fetch_miss(d->pc + 2);
            emit_budget_check(k);
            jmp_l(K.l_fetched[k]);
        } else if (K.l_fetch2[k] >= 0) {
            bind(K.l_fetch2[k]);
            emit_spill();
            mov_rr(1, ARG0, R15);
            mov_ri(ARG1, d->pc + 2);
            emit_count_call(1);
            call_a((const void *)xip_access);
            alu_ri(0, ALU_ADD, RAX, 1);
            mov_mr(0, xm(RBX, CO(cyc)), RAX);
            mov_mi(xm(RBX, CO(fetch_line)), (d->pc + 2) & ~7u);
            jmp_l(lab(&K.l_gencyc[k]));
        }
        if (K.l_other[k] >= 0) {
            bind(K.l_other[k]);
            unsigned lsize = K.ld[k].size;
            if (lsize) {
                // A load through a cached XIP alias that hits the line its set used last, which
                // ld32 and friends charge nothing for.
                int not_xip = new_label();
                mov_rr(0, RCX, RAX);
                shift_ri(SH_SHR, RCX, 26);
                alu_ri(0, ALU_CMP, RCX, 4);
                jcc_l(CC_NE, not_xip);
                if (lsize > 1) {
                    test_ri(RAX, lsize - 1);
                    jcc_l(CC_NE, lab(&K.l_gen1[k]));
                }
                test_mi8(xm(R15, MO(xip_ctrl)), 1);
                jcc_l(CC_E, lab(&K.l_gen1[k]));
                mov_rr(0, RCX, RAX);
                shift_ri(SH_SHR, RCX, 16);
                mov_rm(1, RDX, xmi(R15, RCX, 8, MO(rpage)));
                test_rr(RDX, RDX);
                jcc_l(CC_E, lab(&K.l_gen1[k]));
                mov_rr(0, RCX, RAX);
                alu_ri(0, ALU_AND, RCX, 0x01ffffff);
                shift_ri(SH_SHR, RCX, 3);
                mov_rr(0, R9, RCX);
                alu_ri(0, ALU_AND, R9, 1023);
                shift_ri(SH_SHR, RCX, 10);
                alu_ri(0, ALU_OR, RCX, (int32_t)0x80000000u);
                int hit = new_label(), way1 = new_label(), found = new_label();
                alu_rm(0, ALU_CMP, RCX, xmi(R15, R9, 4, MO(xip_mru)));
                jcc_l(CC_E, hit);
                alu_rm(0, ALU_CMP, RCX, xmi(R15, R9, 8, MO(xip_tag)));
                jcc_l(CC_NE, way1);
                op_m(0, false, 0xc6, 0, xmi(R15, R9, 1, MO(xip_lru)), 1, false);
                e8(1);
                jmp_l(found);
                bind(way1);
                alu_rm(0, ALU_CMP, RCX, xmi(R15, R9, 8, MO(xip_tag) + 4));
                jcc_l(CC_NE, lab(&K.l_gen1[k]));
                op_m(0, false, 0xc6, 0, xmi(R15, R9, 1, MO(xip_lru)), 1, false);
                e8(0);
                bind(found);
                mov_mr(0, xmi(R15, R9, 4, MO(xip_mru)), RCX);
                bind(hit);
                alu_mi(0, ALU_ADD, xm(R15, MO(xip_ctr_acc)), 1);
                alu_mi(0, ALU_ADD, xm(R15, MO(xip_ctr_hit)), 1);
                op_r(0, false, 0x0fb7, RAX, RAX, false);
                emit_host_load((int)lsize, K.ld[k].sign, xmi(RDX, RAX, 1, 0));
                jmp_l(K.l_loaded[k]);
                bind(not_xip);
            }
            if (K.mm[k].size)
                jmp_l(lab(&K.l_mmio[k]));
            else
                jmp_l(lab(&K.l_gen1[k]));
        }
        if (K.l_mmio[k] >= 0) {
            // As ld32, ld16, ld8 and their stores do for an aligned peripheral address.
            bind(K.l_mmio[k]);
            unsigned size = K.mm[k].size;
            if (size > 1) {
                test_ri(RAX, size - 1);
                jcc_l(CC_NE, lab(&K.l_gen1[k]));
            }
            alu_ri(0, ALU_CMP, RAX, 0x40000000);
            jcc_l(CC_B, lab(&K.l_gen1[k]));
            mov_mi(xm(RBX, CO(cyc)), 2);
            emit_spill();
            mov_mi(RG(15), op_next(d));
            mov_rr(0, ARG1, RAX);
            emit_store_time(k);
            mov_rr(1, ARG0, RBX);
            if (K.mm[k].load) {
                mov_ri(ARG2, size);
                emit_count_call(2);
                call_a((const void *)bus_read);
                if (K.mm[k].sign)
                    op_r(0, false, size == 1 ? 0x0fbe : 0x0fbf, RAX, RAX, false);
                int skip = -1;
                if (K.mm[k].keep_on_fault) {
                    skip = new_label();
                    test_mi8(xm(RBX, CO(fault)), 1);
                    jcc_l(CC_NE, skip);
                }
                mov_mr(0, RG(K.mm[k].reg), RAX);
                if (skip >= 0)
                    bind(skip);
            } else {
                mov_rm(0, ARG2, RG(K.mm[k].reg));
                if (size == 2)
                    op_r(0, false, 0x0fb7, ARG2, ARG2, false);
                else if (size == 1)
                    op_r(0, false, 0x0fb6, ARG2, ARG2, ARG2 >= 4);
                mov_ri(ARG3, size);
                emit_count_call(3);
                call_a((const void *)bus_write);
            }
            cmp16_mi(xm(RBX, CO(check_exc)), 0);
            jcc_l(CC_NE, lab(&K.l_look[k]));
            mov_rm(0, RAX, xm(RBX, CO(cyc)));
            imul_ps(RAX);
            alu_rr(1, ALU_ADD, R12, RAX);
            alu_ps(ALU_SUB, R12, K.sb[k + 1] - K.sb[k]);
            emit_budget_check(k + 1);
            jmp_main(k + 1);
        }
        if (K.l_skip[k] >= 0) {
            // The condition failed: one cycle, and the IT state moves on.
            bind(K.l_skip[k]);
            mov_mi(xm(RBX, CO(itstate)), it_next(K.it[k]));
            int st = K.sb[k + 1] - K.sb[k];
            if (st > 1)
                alu_ps(ALU_SUB, R12, st - 1);
            jmp_l(K.l_post[k] >= 0 ? K.l_post[k] : K.l_fall);
        }
        if (!K.generic[k] && (K.l_gen1[k] >= 0 || K.l_gencyc[k] >= 0)) {
            bind(lab(&K.l_gen1[k]));
            if (K.it[k])
                mov_mi(xm(RBX, CO(itstate)), K.it[k]);
            mov_mi(xm(RBX, CO(cyc)), 1);
            bind(lab(&K.l_gencyc[k]));
            emit_generic_body(k, d, K.sb[k + 1] - K.sb[k], true);
        }
    }

    if (K.l_fall >= 0 && !K.fall_bound) {
        bind(K.l_fall);
        if (K.sb[n])
            alu_ps(ALU_ADD, R12, K.sb[n]);
        emit_add_insns(n);
        emit_link(op_next(&b->ops[n - 1]));
    }

    for (int k = 0; k <= n; k++) {
        if (K.l_careful[k] >= 0) {
            bind(K.l_careful[k]);
            mov_mi(RG(15), k < n ? b->ops[k].pc : op_next(&b->ops[n - 1]));
            if (K.sb[k])
                alu_ps(ALU_ADD, R12, K.sb[k]);
            emit_add_insns(k);
            if (K.careful) {
                jmp_a(J.exit_done);
            } else if (k == 0) {
                lea_rip_abs(RAX, K.pre_site);
                mov_mr(1, xm(R14, XO(site)), RAX);
                jmp_a(J.exit_careful0);
            } else {
                jmp_a(J.exit_careful);
            }
        }
        if (k == n)
            break;
        if (K.l_look[k] >= 0) {
            bind(K.l_look[k]);
            emit_add_insns(k + 1);
            emit_spill();
            mov_rr(1, ARG0, RBX);
            mov_ri(ARG1, b->ops[k].pc);
            mov_ri(ARG2, K.it[k]);
            emit_count_call(4);
            call_a((const void *)jit_look);
            op_r(0, false, 0x84, RAX, RAX, false); // test al, al
            jcc_a(CC_E, J.exit_notime);
            mov_rm(1, R12, xm(RBX, CO(t)));
            mov_rm(0, RAX, RG(15));
            alu_ri(0, ALU_CMP, RAX, (int32_t)op_next(&b->ops[k]));
            jcc_a(CC_NE, J.lookup);
            // Nothing was taken: carry on with the next instruction, which the block's own count
            // includes.
            alu_mi(1, ALU_SUB, xm(RBX, CO(insns)), k + 1);
            if (K.sb[k + 1])
                alu_ps(ALU_SUB, R12, K.sb[k + 1]);
            emit_budget_check(k + 1);
            jmp_main(k + 1);
        }
        if (K.l_dyn[k] >= 0) {
            // A handler sent control elsewhere: block_run's chain point.
            bind(K.l_dyn[k]);
            if (K.sb[k + 1])
                alu_ps(ALU_ADD, R12, K.sb[k + 1]);
            emit_add_insns(k + 1);
            alu_rm(1, ALU_CMP, R12, xm(R14, XO(until)));
            jcc_a(CC_AE, J.exit_done);
            alu_mi(0, ALU_CMP, xm(RBX, CO(itstate)), 0);
            jcc_a(CC_NE, J.exit_done);
            alu_mi(0, ALU_CMP, xm(RBX, CO(state)), CPU_RUN);
            jcc_a(CC_NE, J.exit_done);
            mov_rm(0, RAX, RG(15));
            jmp_a(J.lookup);
        }
    }

    for (int i = 0; i < K.nlinks; i++) {
        uint8_t *stub = J.p;
        mov_mi(RG(15), K.links[i].target);
        lea_rip_abs(RAX, K.links[i].site);
        mov_mr(1, xm(R14, XO(site)), RAX);
        jmp_a(J.exit_link);
        int32_t rel = rel_to(stub, K.links[i].site + 4);
        memcpy(K.links[i].site, &rel, 4);
    }
}

// PBConPC: which flags (N 8, Z 4, C 2, V 1) instruction d reads, writes for certain and may write.
// Anything not known here reads them all and writes none, which keeps its flags and every
// earlier instruction's alive.
enum { FL_N = 8, FL_Z = 4, FL_C = 2, FL_V = 1, FL_ALL = 15 };

static void flag_effect(const dop_t *d, uint8_t *rd, uint8_t *wdef, uint8_t *wmay)
{
    static const uint8_t cond_reads[16] = {FL_Z, FL_Z, FL_C, FL_C, FL_N, FL_N, FL_V, FL_V, FL_C | FL_Z, FL_C | FL_Z,
                                           FL_N | FL_V, FL_N | FL_V, FL_N | FL_Z | FL_V, FL_N | FL_Z | FL_V, 0, 0};
    dop_fn fn = d->fn;
    *rd = *wdef = *wmay = 0;
    if (fn == h_nop || fn == h_ldst_single || fn == h_ldm_stm || fn == h_ldst_dual_excl || fn == h_dp_plain_imm ||
        fn == h_multiply || fn == h_long_multiply || fn == h_add_hi || fn == h_mov_hi || fn == h_set_const ||
        fn == h_add_sp_imm || fn == h_adjust_sp || fn == h_extend || fn == h_ldr_lit || fn == h_ldr_imm ||
        fn == h_ldrh_imm || fn == h_ldrb_imm || fn == h_ldr_w || fn == h_ldrh_w || fn == h_ldrb_w ||
        fn == h_str_imm || fn == h_strh_imm || fn == h_strb_imm || fn == h_ldst_reg || fn == h_ldr_w8 ||
        fn == h_str_w8 || fn == h_ldrd_strd || fn == h_push || fn == h_pop || fn == h_branch || fn == h_bl ||
        fn == h_bx || fn == h_blx || fn == h_cbz)
        return;
    if (fn == h_bcond) {
        *rd = cond_reads[d->rd & 15];
        return;
    }
    if (fn == h_movs_reg || fn == h_movs_imm) {
        *wdef = *wmay = FL_N | FL_Z;
        return;
    }
    if (fn == h_lsl_imm || fn == h_lsr_imm || fn == h_asr_imm) {
        *wdef = FL_N | FL_Z;
        *wmay = FL_N | FL_Z | FL_C;
        if (fn != h_lsl_imm || d->imm)
            *wdef = *wmay;
        return;
    }
    if (fn == h_add_reg || fn == h_sub_reg || fn == h_add_imm || fn == h_sub_imm || fn == h_cmp_imm || fn == h_cmp_reg) {
        *wdef = *wmay = FL_ALL;
        return;
    }
    if (fn == h_alu) {
        switch (d->imm) {
        case 0x0: case 0x1: case 0x8: case 0xc: case 0xd: case 0xe: case 0xf:
            *wdef = *wmay = FL_N | FL_Z;
            return;
        case 0x2: case 0x3: case 0x4: case 0x7:
            *wdef = FL_N | FL_Z;
            *wmay = FL_N | FL_Z | FL_C;
            return;
        case 0x5: case 0x6:
            *rd = FL_C;
            *wdef = *wmay = FL_ALL;
            return;
        case 0x9: case 0xa: case 0xb:
            *wdef = *wmay = FL_ALL;
            return;
        }
    }
    if (fn == h_dp_imm) {
        unsigned op = d->rm;
        bool s = d->lf & LF_A;
        if (op <= 4) {
            if (s)
                *wdef = *wmay = FL_N | FL_Z | ((d->lf & LF_B) ? FL_C : 0);
            return;
        }
        if (op == 0x8 || op == 0xd || op == 0xe || op == 0xa || op == 0xb) {
            if (op == 0xa || op == 0xb)
                *rd = FL_C;
            if (s)
                *wdef = *wmay = FL_ALL;
            return;
        }
    }
    if (fn == h_dp_shifted_reg) {
        uint16_t hw1 = d->hw1, hw2 = d->hw2;
        unsigned op = (hw1 >> 5) & 0xf, type = (hw2 >> 4) & 3, imm5 = ((hw2 >> 10) & 0x1c) | ((hw2 >> 6) & 3);
        bool s = hw1 & 0x10, rrx = type == 3 && !imm5;
        bool logical = op <= 4, arith = op == 8 || op == 0xa || op == 0xb || op == 0xd || op == 0xe;
        if (logical || arith) {
            if (rrx || op == 0xa || op == 0xb)
                *rd = FL_C;
            if (s && arith)
                *wdef = *wmay = FL_ALL;
            else if (s) {
                *wdef = FL_N | FL_Z;
                *wmay = (type == 0 && !imm5) ? FL_N | FL_Z : FL_N | FL_Z | FL_C;
                if (*wmay & FL_C && !(type == 0 && !imm5))
                    *wdef = *wmay;   // a shift by a constant other than LSL #0 sets C
            }
            return;
        }
    }
    if (fn == h_dp_register) {
        uint16_t hw1 = d->hw1, hw2 = d->hw2;
        if (!(hw1 & 0x80) && !((hw2 >> 4) & 0xf)) {   // LSL, LSR, ASR, ROR by a register
            if (hw1 & 0x10) {
                *wdef = FL_N | FL_Z;
                *wmay = FL_N | FL_Z | FL_C;
            }
        }
        return;   // the rest (extends, REV, CLZ, SEL, parallel add/sub) leave NZCV alone
    }
    *rd = FL_ALL;
}

// PBCONPC_FLAGOPT: 1 within blocks, 2 the code after them, 4 fused branches, 8 also SRAM code
// after an SRAM block (compared on entry); default 7
static int flagopt = -1;

// PBConPC: the flags code at pc reads before overwriting them for certain, looking at up to six
// instructions (16 bytes) straight from memory; all of them when it cannot tell. The code looked
// at must stay as it is for as long as the translation relies on it: in ROM or flash for a block
// from there (its flash pages are marked as code, so a write there makes the translation stale),
// in SRAM for an SRAM block, which compares it on entry (K.succ) unless the block itself covers it.
static uint8_t succ_live_in(uint32_t pc)
{
    chip_t *m = K.m;
    const uint8_t *page = m->rpage[pc >> 16];
    if (!page || (pc & 1))
        return FL_ALL;
    const uint8_t *src = page + (pc & 0xffff);
    bool in_rom = src >= m->rom && src < m->rom + ROM_SIZE;
    bool in_flash = src >= m->flash && src < m->flash + m->flash_size;
    bool in_sram = src >= m->sram && src < m->sram + m->sram_size;
    if (K.fixed ? !(in_rom || in_flash) : !in_sram)
        return FL_ALL;
    uint8_t live = 0, written = 0;
    uint32_t a = pc;
    for (int i = 0; i < 6 && written != FL_ALL; i++) {
        if ((a & 0xffff) > 0xfffc || a - pc > 12)
            break;
        uint16_t hw1 = rd16le(page + (a & 0xffff));
        bool wide = (hw1 >> 11) >= 0x1d;
        dop_t t;
        memset(&t, 0, sizeof(t));
        t.pc = a;
        t.hw1 = hw1;
        t.hw2 = wide ? rd16le(page + ((a + 2) & 0xffff)) : 0;
        t.lf = wide ? 4 : 2;
        bool last = wide ? decode32(&t, a, hw1, t.hw2) : decode16(&t, a, hw1);
        uint8_t rd, wdef, wmay;
        if (!wide && is_it(hw1))
            rd = FL_ALL, wdef = wmay = 0;
        else
            flag_effect(&t, &rd, &wdef, &wmay);
        live |= rd & ~written;
        written |= wdef;
        a += t.lf & LF_LEN;
        if (last)
            break;
    }
    live |= FL_ALL & ~written;
    if (live == FL_ALL)
        return live;
    // keep what was looked at
    uint32_t pc0 = K.b->tag & ~1u, len = a - pc;
    if (in_flash) {
        uint32_t off = (uint32_t)(src - m->flash), end = off + len - 1;
        for (uint32_t p = off >> 12; p <= end >> 12 && p < sizeof(m->code_page); p++)
            m->code_page[p] = 1;
    } else if (in_sram && !(pc >= pc0 && a <= pc0 + K.b->bytes)) {
        // Comparing it on every entry cost more than the flags saved (Sonic 2 in pbcemu: 2 %
        // slower): only with PBCONPC_FLAGOPT bit 8.
        if (!(flagopt & 8) || K.nsucc == 2 || len > 16)
            return FL_ALL;
        K.succ[K.nsucc].pc = pc;
        K.succ[K.nsucc].len = (uint8_t)len;
        memcpy(K.succ[K.nsucc].bytes, src, len);
        K.nsucc++;
    }
    return live;
}

// PBConPC: how instruction d's host flags give the ARM condition cond, as an x86 condition code,
// or -1. d's own native code must make them with emit_flags_nz/nzc/arith, and set every flag
// cond reads.
static int fuse_cc_for(const dop_t *d, unsigned cond)
{
    enum { NONE, SUB, ADD, NZ, NZC } kind = NONE;
    dop_fn fn = d->fn;
    if (fn == h_sub_reg || fn == h_sub_imm || fn == h_cmp_imm || fn == h_cmp_reg)
        kind = SUB;
    else if (fn == h_add_reg || fn == h_add_imm)
        kind = ADD;
    else if (fn == h_movs_reg)
        kind = NZ;
    else if (fn == h_lsl_imm || fn == h_lsr_imm || fn == h_asr_imm)
        kind = d->imm ? NZC : NONE;
    else if (fn == h_alu) {
        switch (d->imm) {
        case 0x0: case 0x1: case 0x8: case 0xc: case 0xd: case 0xe: case 0xf: kind = NZ; break;
        case 0x6: case 0x9: kind = SUB; break;
        case 0x5: case 0xb: kind = ADD; break;
        }
    } else if (fn == h_dp_imm && (d->lf & LF_A)) {
        unsigned op = d->rm;
        if (op <= 4 && d->rn != 15)
            kind = NZ;
        else if (op == 0xd || op == 0xb || op == 0xe)
            kind = SUB;
        else if (op == 0x8 || op == 0xa)
            kind = ADD;
    } else if (fn == h_dp_shifted_reg && (d->hw1 & 0x10)) {
        unsigned op = (d->hw1 >> 5) & 0xf, type = (d->hw2 >> 4) & 3;
        unsigned imm5 = ((d->hw2 >> 10) & 0x1c) | ((d->hw2 >> 6) & 3);
        if (op <= 4 && !type && !imm5)
            kind = NZ;
        else if (op == 0xd || op == 0xb || op == 0xe)
            kind = SUB;
        else if (op == 0x8 || op == 0xa)
            kind = ADD;
    }
    static const int8_t common[16] = {CC_E, CC_NE, -1, -1, CC_S, CC_NS, CC_O, CC_NO, -1, -1, CC_GE, CC_L, CC_G, CC_LE, -1, -1};
    switch (kind) {
    case SUB:
        if (cond == 2) return CC_AE;
        if (cond == 3) return CC_B;
        if (cond == 8) return CC_A;
        if (cond == 9) return CC_BE;
        return common[cond];
    case ADD:
        if (cond == 2) return CC_B;
        if (cond == 3) return CC_AE;
        return common[cond];
    case NZC:
        if (cond == 2) return CC_B;
        if (cond == 3) return CC_AE;
        // fall through
    case NZ:
        return cond <= 5 && cond != 2 && cond != 3 ? common[cond] : -1;
    default:
        return -1;
    }
}

// Backwards through the block: an instruction whose flags are all overwritten for certain before
// anything reads them computes none. Instructions under IT neither die nor kill (they may not
// run). Where the block ends, the flags are alive that the code it goes on to may read
// (succ_live_in), and a closing conditional branch may take its condition straight from the
// instruction before it (K.fuse). Leaving a block early (a peripheral access, the time limit, an
// exception) is safe: what runs after it still overwrites those flags before reading any; only a
// debugger halted in between, or a handler looking at the stacked xPSR, could see the old ones.
static void flag_liveness(void)
{
    int n = K.n;
    const dop_t *last = &K.b->ops[n - 1];
    uint8_t live = FL_ALL;
    if (!K.it[n - 1] && (flagopt & 2)) {
        if (last->fn == h_bcond || last->fn == h_cbz)
            live = succ_live_in(op_next(last)) | succ_live_in(last->imm);
        else if (last->fn == h_branch || last->fn == h_bl)
            live = succ_live_in(last->imm);
    }
    // the closing branch tested from the host flags of the instruction before it
    bool fused_read = false;
    if ((flagopt & 4) && last->fn == h_bcond && n >= 2 && !K.it[n - 1] && !K.it[n - 2]) {
        const dop_t *p = &K.b->ops[n - 2];
        uint8_t rd, wdef, wmay;
        flag_effect(p, &rd, &wdef, &wmay);
        static const uint8_t cond_reads[16] = {FL_Z, FL_Z, FL_C, FL_C, FL_N, FL_N, FL_V, FL_V, FL_C | FL_Z,
                                               FL_C | FL_Z, FL_N | FL_V, FL_N | FL_V, FL_N | FL_Z | FL_V,
                                               FL_N | FL_Z | FL_V, 0, 0};
        unsigned cond = last->rd & 15;
        int cc = fuse_cc_for(p, cond);
        if (cc >= 0 && cond < 14 && (cond_reads[cond] & ~wdef) == 0) {
            K.fuse = n - 2;
            K.fuse_cc = cc;
            K.fuse_cond = (int)cond;
            // A fetch that misses runs the branch by its handler, which reads APSR: then the
            // flags must still be written there.
            fused_read = !(last->lf & (FETCH_HW1 | FETCH_HW2));
        }
    }
    for (int k = n - 1; k >= 0; k--) {
        const dop_t *d = &K.b->ops[k];
        uint8_t rd, wdef, wmay;
        if (K.it[k] || (d->fn == h_exec16 && is_it(d->hw1))) {
            rd = FL_ALL;
            wdef = wmay = 0;
        } else {
            flag_effect(d, &rd, &wdef, &wmay);
        }
        if (k == n - 1 && fused_read)
            rd = 0;
        K.dead[k] = wmay && !(wmay & live);
        live = (uint8_t)((live & ~wdef) | rd);
    }
}

static void jit_entries_forget(uint32_t pc, const void *code);

// PBConPC: an SRAM translation found code after it changed that its flags analysis relied on:
// forget it everywhere, so that the block is translated again.
static void jit_drop(uint8_t *entry)
{
    pbc_flag_stat[3]++;
    jit_hdr_t *h = (jit_hdr_t *)(entry - sizeof(jit_hdr_t));
    uint32_t pc = h->pc;
    h->bytes = 0xffff;
    unsigned i = jit_hash(pc);
    if (jx.lookup[i].code == entry)
        jx.lookup[i].code = NULL;
    unsigned set = ((pc >> 1) ^ (pc >> 13) ^ (pc >> 25)) & (BLK_SETS - 1);
    for (unsigned w = 0; w < 2; w++)
        if (blocks[set][w].jit == entry)
            blocks[set][w].jit = NULL;
    jit_entries_forget(pc, entry);
}

static void *jit_compile(chip_t *m, block_t *b, bool careful)
{
    pbc_jit_stat[careful ? 1 : 0]++;
    uint32_t pc0 = b->tag & ~1u;
    if (!b->fixed && pc0 - SRAM_BASE >= 0x90000u)
        return NULL;
    if ((size_t)(J.end - J.p) < JIT_BLOCK_ROOM)
        jit_flush();

    uint8_t *begin = J.p;
    memset(&K, 0, sizeof(K));
    static int reload_opt = -1;   // PBCONPC_NO_RELOADOPT=1 keeps every load (measurements)
    if (reload_opt < 0)
        reload_opt = !getenv("PBCONPC_NO_RELOADOPT");
    if (page_direct < 0)
        page_direct = !getenv("PBCONPC_NO_PAGEOPT");
    for (int i = 0; i < BLK_MAX; i++)
        K.pg[i].tab = K.pg[i].ua = -1;
    ua_next = false;
    if (ua_opt < 0)
        ua_opt = !getenv("PBCONPC_NO_UAOPT");
    if (lit_opt < 0)
        lit_opt = !getenv("PBCONPC_NO_LITOPT");
    if (dsp_opt < 0)
        dsp_opt = !getenv("PBCONPC_NO_DSPOPT");
    if (it_fetch_opt < 0)
        it_fetch_opt = !getenv("PBCONPC_NO_ITFETCHOPT");
    if (fp_opt < 0)
        fp_opt = !getenv("PBCONPC_NO_FPOPT");
    if (avx2_entry < 0)
        avx2_entry = __builtin_cpu_supports("avx2") && getenv("PBCONPC_AVX2ENTRY");   // measured no faster
    if (fp_fma < 0)
        fp_fma = __builtin_cpu_supports("fma") && !getenv("PBCONPC_NO_FMA");
    memset(K.l_fetched, -1, sizeof(K.l_fetched));
    if (fetch_back < 0)
        fetch_back = getenv("PBCONPC_FETCHOPT") != NULL;
    rg_lo = reload_opt ? CO(r) : 0;
    rg_hi = reload_opt ? CO(r) + 16 * 4 : 0;
    st_end = NULL;
    rg_spilled = false;
    K.b = b;
    K.m = m;
    K.n = b->n;
    K.fixed = b->fixed;
    K.careful = careful;
    memset(K.l_main, -1, sizeof(K.l_main));
    memset(K.l_gen1, -1, sizeof(K.l_gen1));
    memset(K.l_gencyc, -1, sizeof(K.l_gencyc));
    memset(K.l_fetch1, -1, sizeof(K.l_fetch1));
    memset(K.l_fetch2, -1, sizeof(K.l_fetch2));
    memset(K.l_careful, -1, sizeof(K.l_careful));
    memset(K.l_look, -1, sizeof(K.l_look));
    memset(K.l_dyn, -1, sizeof(K.l_dyn));
    memset(K.l_dop, -1, sizeof(K.l_dop));
    memset(K.l_mmio, -1, sizeof(K.l_mmio));
    memset(K.l_skip, -1, sizeof(K.l_skip));
    memset(K.l_post, -1, sizeof(K.l_post));
    memset(K.l_other, -1, sizeof(K.l_other));
    memset(K.l_loaded, -1, sizeof(K.l_loaded));
    nlab = nfix = 0;
    jit_overflow = false;
    for (int k = 0; k <= K.n; k++)
        K.l_main[k] = new_label();
    if (b->nrun < b->n) {
        uint32_t it = b->ops[b->nrun - 1].hw1 & 0xff;
        for (int k = b->nrun; k < K.n; k++) {
            K.it[k] = (uint8_t)it;
            it = (it & 7) ? (it & 0xe0) | ((it << 1) & 0x1f) : 0;
        }
    }
    K.l_fall = new_label();
    K.l_stale = new_label();
    K.fuse = -1;
    K.l_drop = -1;
    if (flagopt < 0) {
        const char *e = getenv("PBCONPC_FLAGOPT");
        flagopt = getenv("PBCONPC_NO_FLAGOPT") ? 0 : e ? atoi(e) : 7;
    }
    if (!careful && flagopt)
        flag_liveness();

    {   // PBCONPC_JITPAD=n bytes between translations, to see whether layout matters
        static int pad = -1;
        if (pad < 0)
            pad = getenv("PBCONPC_JITPAD") ? atoi(getenv("PBCONPC_JITPAD")) : 0;
        J.p += pad;
    }
    J.p = (uint8_t *)(((uintptr_t)J.p + sizeof(jit_hdr_t) + 15) & ~(uintptr_t)15);
    jit_hdr_t *h = (jit_hdr_t *)(J.p - sizeof(jit_hdr_t));
    h->pc = pc0;
    h->gen = b->gen;
    h->fixed = b->fixed;
    h->bytes = b->bytes;
    memcpy(h->code, b->code, b->bytes);
    uint8_t *entry = J.p;
    if (jit_nindex < sizeof(jit_index) / sizeof(jit_index[0]))
        jit_index[jit_nindex++] = (typeof(jit_index[0])){entry, pc0, (uint16_t)b->n, careful};

    emit_block_entry();
    int max_extra = 0;
    for (int k = 0; k < K.n; k++) {
        const dop_t *d = &b->ops[k];
        // not a join for mov_rm: the code that comes back here restores eax (jmp_main)
        lab_at[K.l_main[k]] = J.p;
        K.eax_disp[k] = st_end == J.p ? st_disp : INT32_MIN;
        emit_fetch(k, d);
        if (fetch_back && (d->lf & (FETCH_HW1 | FETCH_HW2)) && !K.it[k])
            bind(lab(&K.l_fetched[k]));
        int extra = 0;
        K.ended = false;
        K.cur = k;
        int st = emit_inline(k, d, &extra);
        if (st < 0) {
            K.generic[k] = true;
            st = 0;
            K.sb[k + 1] = K.sb[k];
            mov_mi(xm(RBX, CO(cyc)), 1);
            bind(lab(&K.l_gencyc[k]));
            emit_generic_body(k, d, 0, false);
        }
        K.sb[k + 1] = K.sb[k] + st;
        if (!K.ended)
            bind(lab(&K.l_post[k]));
        if (K.careful && !K.generic[k] && !K.ended)
            emit_budget_check(k + 1);
        if (k == K.n - 1)
            max_extra = extra;
    }
    bind(K.l_main[K.n]);
    const dop_t *lastd = &b->ops[K.n - 1];
    if (!(!K.generic[K.n - 1] && K.ended)) {
        bind(K.l_fall);
        K.fall_bound = true;
        if (K.sb[K.n])
            alu_ps(ALU_ADD, R12, K.sb[K.n]);
        emit_add_insns(K.n);
        if (K.generic[K.n - 1]) {
            // A handler may have started an IT block or stopped the core.
            alu_mi(0, ALU_CMP, xm(RBX, CO(itstate)), 0);
            jcc_a(CC_NE, J.exit_done);
            alu_mi(0, ALU_CMP, xm(RBX, CO(state)), CPU_RUN);
            jcc_a(CC_NE, J.exit_done);
        }
        emit_link(op_next(lastd));
    }
    K.total = K.sb[K.n] + max_extra;

    emit_cold();
    if (K.l_drop >= 0) {
        bind(K.l_drop);
        emit_spill();
        mov_ri64(ARG0, (uint64_t)(uintptr_t)entry);
        call_a((const void *)jit_drop);
    }
    bind(K.l_stale);
    mov_mi(RG(15), pc0);
    lea_rip_abs(RAX, entry);
    mov_mr(1, xm(R14, XO(site)), RAX);
    jmp_a(J.exit_stale);
    uint8_t *code_end = J.p;

    if (K.ncmp) {
        // the block's code again, for the AVX2 entry check to compare with
        J.p = (uint8_t *)(((uintptr_t)J.p + 31) & ~(uintptr_t)31);
        memcpy(J.p, b->code, b->bytes);
        for (int i = 0; i < K.ncmp; i++)
            lab_at[K.cmp_lab[i]] = J.p + K.cmp_off[i];
        J.p += b->bytes;
    }
    J.p = (uint8_t *)(((uintptr_t)J.p + 7) & ~(uintptr_t)7);
    for (int k = 0; k < K.n; k++)
        if (K.l_dop[k] >= 0) {
            bind(K.l_dop[k]);
            memcpy(J.p, &b->ops[k], sizeof(dop_t));
            J.p += sizeof(dop_t);
        }

    if (jit_overflow || K.total >= JIT_PSK || (size_t)(J.p - begin) > JIT_BLOCK_ROOM) {
        J.p = begin;
        return NULL;
    }
    for (int i = 0; i < K.ntotal; i++) {
        int32_t disp = XO(psk) + 8 * K.total;
        if (ps_imm) {
            if (!ps_fits(K.total))
                jit_overflow = true;
            disp = (int32_t)((int64_t)K.total * J.ps);
        }
        memcpy(K.total_sites[i], &disp, 4);
    }
    for (int i = 0; i < nfix; i++) {
        uint8_t *t = lab_at[fixups[i].label];
        if (!t) {
            J.p = begin;
            return NULL;
        }
        int32_t rel = rel_to(t, fixups[i].site + 4 + fixups[i].trailing);
        memcpy(fixups[i].site, &rel, 4);
    }
    {
        // PBConPC measurement: PBCONPC_JITDUMP=hexpc writes that block's translation to
        // jitdump_<pc>_<n>.bin (x86-64 code from entry to the end of the block's code)
        static const char *want;
        static int checked;
        if (!checked) {
            want = getenv("PBCONPC_JITDUMP");
            checked = 1;
        }
        if (want && strtoul(want, NULL, 16) == pc0 && !careful) {
            static int n;
            char name[64];
            snprintf(name, sizeof(name), "jitdump_%08x_%d.bin", pc0, n++);
            FILE *f = fopen(name, "wb");
            if (f) {
                fwrite(entry, 1, (size_t)(code_end - entry), f);
                fclose(f);
            }
        }
    }
    return entry;
}

// ---------------------------------------------------------------------------------------------
// Running

static void *jit_code_for(chip_t *m, block_t *b)
{
    if (b->jit && b->jit_epoch == J.epoch)
        return b->jit;
    uint32_t pc = b->tag & ~1u;
    void *code = jx.lookup[jit_hash(pc)].code;
    if (code && jx.lookup[jit_hash(pc)].pc == pc) {
        // A translation the block cache lost track of serves again if made from the same code.
        const jit_hdr_t *h = (const jit_hdr_t *)((uint8_t *)code - sizeof(jit_hdr_t));
        if (h->fixed == b->fixed &&
            (b->fixed ? h->gen == b->gen : h->bytes == b->bytes && !memcmp(h->code, b->code, b->bytes))) {
            b->jit = code;
            b->jit_epoch = J.epoch;
            return code;
        }
    }
    code = jit_compile(m, b, false);
    if (code) {
        b->jit = code;
        b->jit_epoch = J.epoch;
        jx.lookup[jit_hash(pc)].pc = pc;
        jx.lookup[jit_hash(pc)].code = code;
    }
    return code;
}

// PBConPC: block_find for a block about to run as native code. An SRAM block with a current
// translation skips comparing its code here, since that translation compares it again on entry
// (emit_block_entry) and leaves as stale if it changed; block_find then builds it anew. This
// comparison cost several per cent of the host's time, once per core and chunk.
static inline block_t *block_find_for_jit(chip_t *m, uint32_t pc)
{
    const uint8_t *page = m->rpage[pc >> 16];
    if (likely(page)) {
        unsigned set = ((pc >> 1) ^ (pc >> 13) ^ (pc >> 25)) & (BLK_SETS - 1);
        block_t *w = blocks[set];
        for (unsigned i = 0; i < 2; i++)
            if (w[i].tag == (pc | 1) && !w[i].fixed && w[i].jit && w[i].jit_epoch == J.epoch) {
                block_mru[set] = (uint8_t)i;
                return &w[i];
            }
    }
    return block_find(m, pc);
}

static void jit_set_ps(uint32_t ps)
{
    J.ps = ps;
    for (unsigned k = 0; k < JIT_PSK; k++)
        jx.psk[k] = (uint64_t)k * ps;
}

// Like block_run from block b, returning true when it stopped early for block_run to finish the
// time to the exact instruction.
// PBConPC: where each core last entered native code, by address, so that the next chunk can go
// straight back in without the block cache, whose large table mostly misses the host's caches.
// Translations check on entry that they are still current (code_gen, or the code bytes for
// SRAM), so the entry needs only the epoch that frees them.
typedef struct {
    uint32_t pc, epoch;
    void *code;
} jit_entry_t;
static jit_entry_t jit_entries[2][4096];

static void jit_entries_forget(uint32_t pc, const void *code)
{
    for (unsigned c = 0; c < 2; c++)
        if (jit_entries[c][(pc >> 1) & 4095].code == code)
            jit_entries[c][(pc >> 1) & 4095].code = NULL;
}

static inline void *jit_entry_for(cpu_t *c, uint32_t pc)
{
    jit_entry_t *e = &jit_entries[c->id & 1][(pc >> 1) & 4095];
    return e->pc == pc && e->epoch == J.epoch && e->code ? e->code : NULL;
}

static bool jit_loop(cpu_t *c, void *code, uint64_t until);

static bool jit_run_code(cpu_t *c, void *code, uint64_t until)
{
    chip_t *m = c->m;
    if (m->sys_ps != J.ps) {
        jit_set_ps(m->sys_ps);
        if (ps_imm) {   // translations hold the old period: drop them, cpu_run comes back
            jit_flush();
            return false;
        }
    }
    if (m->until < until)
        until = m->until;
    jx.until = until;
    return jit_loop(c, code, until);
}

static bool jit_run(cpu_t *c, block_t *b, uint64_t until)
{
    chip_t *m = c->m;
    if (m->sys_ps != J.ps) {
        jit_set_ps(m->sys_ps);
        if (ps_imm)
            jit_flush();
    }
    if (m->until < until)
        until = m->until;
    jx.until = until;
    void *code = jit_code_for(m, b);
    if (!code) {
        block_run(c, b, until);
        return false;
    }
    uint32_t pc0 = b->tag & ~1u;
    jit_entry_t *e = &jit_entries[c->id & 1][(pc0 >> 1) & 4095];
    e->pc = pc0;
    e->epoch = J.epoch;
    e->code = code;
    return jit_loop(c, code, until);
}

static bool jit_loop(cpu_t *c, void *code, uint64_t until)
{
    chip_t *m = c->m;
    for (;;) {
        int why = J.enter(c, &jx, code);
        pbc_jit_why[why < 8 ? why : 7]++;
        if (why == JX_DONE)
            return false;
        if (why == JX_CAREFUL)
            return true;
        if (why == JX_CAREFUL0) {
            // Near the limit: the block's careful translation, made now and kept for next time.
            uint8_t *site = jx.site;
            uint32_t epoch = J.epoch;
            block_t *cb = block_find(m, c->r[15]);
            code = cb ? jit_compile(m, cb, true) : NULL;
            if (!code)
                return true;
            if (epoch == J.epoch) {
                int32_t rel = rel_to(code, site + 4);
                memcpy(site, &rel, 4);
            }
            continue;
        }
        uint8_t *site = jx.site;
        uint32_t epoch = J.epoch, npc = c->r[15];
        if (c->ns || m->code_flush)
            return false;
        // a stale block must be looked at again; any other's translation checks itself
        block_t *nb = why == JX_STALE ? block_find(m, npc) : block_find_for_jit(m, npc);
        if (!nb)
            return false;
        code = jit_code_for(m, nb);
        if (!code) {
            block_run(c, nb, until);
            return false;
        }
        if (epoch == J.epoch) {
            if (why == JX_LINK) {
                int32_t rel = rel_to(code, site + 4);
                memcpy(site, &rel, 4);
            } else if (why == JX_STALE && site != code) {
                site[0] = 0xe9;
                int32_t rel = rel_to(code, site + 5);
                memcpy(site + 1, &rel, 4);
            }
        }
        jx.lookup[jit_hash(npc)].pc = npc;
        jx.lookup[jit_hash(npc)].code = code;
    }
}

#else

static inline void *jit_entry_for(cpu_t *c, uint32_t pc)
{
    (void)c;
    (void)pc;
    return NULL;
}

static bool jit_run_code(cpu_t *c, void *code, uint64_t until)
{
    (void)c;
    (void)code;
    (void)until;
    return false;
}

static inline block_t *block_find_for_jit(chip_t *m, uint32_t pc)
{
    return block_find(m, pc);
}

static bool jit_available(void)
{
    return false;
}

static void jit_forget(void)
{
}

static bool jit_run(cpu_t *c, block_t *b, uint64_t until)
{
    block_run(c, b, until);
    return false;
}

#endif
