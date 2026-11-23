// PBConPC: native code for predecoded blocks on AArch64 hosts (Android phones and tablets, Linux on
// ARM). The emulation stays what block_run makes it, cycle for cycle: every instruction either is
// written out natively or calls its block handler as block_run would, and the state digest of a run
// (machine.c state_digest) is the same with and without this file.
//
// It is a second, simpler translator beside the x86-64 one in jit.inc, built on the same blocks:
//
//   - x19 the core, x20 the chip, x22 the context (jx), x23 APSR, w24..w28 the guest's r0..r4;
//     the other guest registers stay in c->r. Before a call into C all of them go back to the core
//     (spill) and are read again after it.
//   - x21 is the time, less the fixed cycles of the block's instructions passed so far. A block is
//     entered only when all its fixed cycles fit before the limit; whatever costs more than was
//     counted (a cache miss, a handler) adds to x21 and checks again, leaving for block_run
//     (JX_CAREFUL) when the rest might not fit.
//   - The guest's N, Z, C and V are ARM64's own, so a flag-setting instruction is the host's, with
//     NZCV copied into x23 (MRS) where a later instruction may read them, and a conditional branch
//     right after a compare uses the host's flags as they stand (hf).
//   - An instruction's handler path (emit_generic) is also where its native form goes when
//     something is not as the fast path wants: an XIP cache miss, memory that is not plain RAM.
//
// Not done here yet, so they run through their handlers: instructions under IT (the block ends at
// the IT instruction and cpu_run steps them), shifts by a register, the 32-bit data-processing
// forms other than those with a modified immediate, floating point.

#include <sys/mman.h>

uint64_t pbc_jit_stat[4], pbc_jit_why[8], pbc_flag_stat[4];

enum { JX_DONE, JX_CAREFUL, JX_LINK, JX_LOOKUP, JX_STALE, JX_LOOK, JX_COUNT };

#ifndef JIT_SIZE
#define JIT_SIZE (64u << 20)
#endif
#define JIT_BLOCK_ROOM (64u << 10)
#define JIT_LOOKUP 262144
#define JIT_PSK 256

// Everything native code reaches through x22.
typedef struct {
    uint64_t psk[JIT_PSK]; // k core clock periods in picoseconds
    uint64_t until;
    uint64_t ps;
    uint32_t *site;     // the branch a link exit wants patched
    const dop_t *cur;   // the instruction whose handler asked for a look
    void *fn_op;        // a64_op
    struct {
        uint32_t tag, pad; // address | 1
        void *code;
    } lookup[JIT_LOOKUP];
} jit_ctx_t;

// Kept just before each block's code so a translation can be found again from its address. The
// instructions are the translation's own copy, which its handler calls are given: the block cache
// may hand the block's place to another block while the translation lives on.
typedef struct {
    uint32_t pc, gen;
    uint16_t bytes;
    uint8_t fixed, pad;
    uint8_t code[BLK_MAX * 4];
    uint32_t pad2;
    dop_t ops[BLK_MAX];
} jit_hdr_t;

static struct {
    int on;
    uint32_t *start, *end, *p, *code_start;
    uint32_t epoch;
    uint32_t ps;
    int (*enter)(cpu_t *c, jit_ctx_t *x, void *code);
    uint32_t *exit[JX_COUNT], *exit_look, *lookup;
} J = {.on = -1};

static jit_ctx_t jx;

void *jit_alloc_exec(size_t size, const void *hint);

// ---------------------------------------------------------------------------------------------
// AArch64 encoding

enum { XC = 19, XM = 20, XT = 21, XJ = 22, WF = 23, IP0 = 16, IP1 = 17, ZR = 31 };
enum { A_EQ, A_NE, A_CS, A_CC, A_MI, A_PL, A_VS, A_VC, A_HI, A_LS, A_GE, A_LT, A_GT, A_LE, A_AL };

static bool jit_overflow;

static inline void e(uint32_t w)
{
    if (J.p < J.end)
        *J.p++ = w;
    else
        jit_overflow = true;
}

#define JIT_LABELS 512
#define JIT_FIXUPS 1024
enum { FX_B26, FX_B19, FX_TB14 };

static uint32_t *lab_at[JIT_LABELS];
static int nlab;
static struct {
    uint32_t *site;
    int label, kind;
} fixups[JIT_FIXUPS];
static int nfix;

static int new_label(void)
{
    if (nlab == JIT_LABELS) {
        jit_overflow = true;
        return 0;
    }
    lab_at[nlab] = NULL;
    return nlab++;
}

static inline void bind(int l)
{
    lab_at[l] = J.p;
}

static void fixup(int label, int kind)
{
    if (nfix == JIT_FIXUPS) {
        jit_overflow = true;
        return;
    }
    fixups[nfix].site = J.p;
    fixups[nfix].label = label;
    fixups[nfix].kind = kind;
    nfix++;
}

static void resolve_fixups(void)
{
    for (int i = 0; i < nfix && !jit_overflow; i++) {
        uint32_t *to = lab_at[fixups[i].label];
        if (!to) {
            jit_overflow = true;
            return;
        }
        int64_t off = to - fixups[i].site;
        uint32_t *w = fixups[i].site;
        switch (fixups[i].kind) {
        case FX_B26: *w |= (uint32_t)off & 0x03ffffffu; break;
        case FX_B19:
            if (off < -(1 << 18) || off >= (1 << 18))
                jit_overflow = true;
            *w |= ((uint32_t)off & 0x7ffffu) << 5;
            break;
        default:
            if (off < -(1 << 13) || off >= (1 << 13))
                jit_overflow = true;
            *w |= ((uint32_t)off & 0x3fffu) << 5;
            break;
        }
    }
}

static void b_l(int l)
{
    fixup(l, FX_B26);
    e(0x14000000u);
}

static void b_a(const uint32_t *t)
{
    e(0x14000000u | ((uint32_t)(t - J.p) & 0x03ffffffu));
}

static void bcond_l(int cc, int l)
{
    fixup(l, FX_B19);
    e(0x54000000u | (uint32_t)cc);
}

static void cbz_l(int sf, int rt, int l)
{
    fixup(l, FX_B19);
    e((sf ? 0xb4000000u : 0x34000000u) | (uint32_t)rt);
}

static void cbnz_l(int sf, int rt, int l)
{
    fixup(l, FX_B19);
    e((sf ? 0xb5000000u : 0x35000000u) | (uint32_t)rt);
}

static void tbz_l(int rt, int bit, int l)
{
    fixup(l, FX_TB14);
    e(0x36000000u | ((uint32_t)(bit >> 5) << 31) | ((uint32_t)(bit & 31) << 19) | (uint32_t)rt);
}

static void tbnz_l(int rt, int bit, int l)
{
    fixup(l, FX_TB14);
    e(0x37000000u | ((uint32_t)(bit >> 5) << 31) | ((uint32_t)(bit & 31) << 19) | (uint32_t)rt);
}

static void mov_wi(int rd, uint32_t v)
{
    if (!(v & 0xffff0000u))
        e(0x52800000u | (v << 5) | (uint32_t)rd);
    else if (!(v & 0xffffu))
        e(0x52a00000u | ((v >> 16) << 5) | (uint32_t)rd);
    else if (!(~v & 0xffff0000u))
        e(0x12800000u | ((~v & 0xffffu) << 5) | (uint32_t)rd);
    else if (!(~v & 0xffffu))
        e(0x12a00000u | ((~v >> 16) << 5) | (uint32_t)rd);
    else {
        e(0x52800000u | ((v & 0xffffu) << 5) | (uint32_t)rd);
        e(0x72a00000u | ((v >> 16) << 5) | (uint32_t)rd);
    }
}

static void mov_xi(int rd, uint64_t v)
{
    bool first = true;
    for (unsigned hw = 0; hw < 4; hw++) {
        uint32_t part = (uint32_t)(v >> (16 * hw)) & 0xffffu;
        if (!part)
            continue;
        e((first ? 0xd2800000u : 0xf2800000u) | (hw << 21) | (part << 5) | (uint32_t)rd);
        first = false;
    }
    if (first)
        e(0xd2800000u | (uint32_t)rd);
}

// Shifted-register forms: op is the opcode with everything but the registers.
#define OP_ADD 0x0b000000u
#define OP_ADDS 0x2b000000u
#define OP_SUB 0x4b000000u
#define OP_SUBS 0x6b000000u
#define OP_AND 0x0a000000u
#define OP_BIC 0x0a200000u
#define OP_ORR 0x2a000000u
#define OP_ORN 0x2a200000u
#define OP_EOR 0x4a000000u
#define OP_ANDS 0x6a000000u
#define OP_BICS 0x6a200000u
#define OP_ADCS 0x3a000000u
#define OP_SBCS 0x7a000000u
#define SF 0x80000000u

static inline void rrr(uint32_t op, int rd, int rn, int rm)
{
    e(op | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

// rd = rn op (rm shifted): type 0 LSL, 1 LSR, 2 ASR, 3 ROR
static inline void rrr_sh(uint32_t op, int rd, int rn, int rm, int type, int amount)
{
    e(op | ((uint32_t)type << 22) | ((uint32_t)rm << 16) | ((uint32_t)amount << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

static inline void mov_rr(int sf, int rd, int rm)
{
    if (rd != rm)
        rrr(OP_ORR | (sf ? SF : 0), rd, ZR, rm);
}

// Immediate forms: op 0 ADD, 1 ADDS, 2 SUB, 3 SUBS, with imm up to 4095 (or that times 4096).
static inline void addsub_i(int sf, int op, int rd, int rn, uint32_t imm, bool lsl12)
{
    e(0x11000000u | (sf ? SF : 0) | ((uint32_t)op << 29) | (lsl12 ? 1u << 22 : 0) | (imm << 10) | ((uint32_t)rn << 5) |
      (uint32_t)rd);
}

// rd = rn + imm for any 32-bit imm, through IP1 when it does not fit (flags untouched).
static void add_wi(int rd, int rn, uint32_t imm)
{
    uint32_t neg = 0u - imm;
    if (!imm)
        mov_rr(0, rd, rn);
    else if (imm < 4096)
        addsub_i(0, 0, rd, rn, imm, false);
    else if (neg < 4096)
        addsub_i(0, 2, rd, rn, neg, false);
    else if (!(imm & 0xfffu) && imm < (1u << 24))
        addsub_i(0, 0, rd, rn, imm >> 12, true);
    else if (!(neg & 0xfffu) && neg < (1u << 24))
        addsub_i(0, 2, rd, rn, neg >> 12, true);
    else {
        mov_wi(IP1, imm);
        rrr(OP_ADD, rd, rn, IP1);
    }
}

static inline void ubfm(int sf, int rd, int rn, int immr, int imms)
{
    e((sf ? 0xd3400000u : 0x53000000u) | ((uint32_t)immr << 16) | ((uint32_t)imms << 10) | ((uint32_t)rn << 5) |
      (uint32_t)rd);
}

static inline void sbfm(int rd, int rn, int immr, int imms)
{
    e(0x13000000u | ((uint32_t)immr << 16) | ((uint32_t)imms << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

static inline void lsl_i(int rd, int rn, int n)
{
    ubfm(0, rd, rn, (32 - n) & 31, 31 - n);
}

static inline void lsr_i(int sf, int rd, int rn, int n)
{
    ubfm(sf, rd, rn, n, sf ? 63 : 31);
}

static inline void asr_i(int rd, int rn, int n)
{
    sbfm(rd, rn, n, 31);
}

static inline void ubfx(int rd, int rn, int lsb, int width)
{
    ubfm(0, rd, rn, lsb, lsb + width - 1);
}

// rd's bits lsb.. take rn's low `width` bits
static inline void bfi(int rd, int rn, int lsb, int width)
{
    e(0x33000000u | ((uint32_t)((32 - lsb) & 31) << 16) | ((uint32_t)(width - 1) << 10) | ((uint32_t)rn << 5) |
      (uint32_t)rd);
}

// The N:immr:imms of a 32-bit logical immediate, or -1 if v is not one.
static int logic_imm(uint32_t v)
{
    if (v == 0 || v == 0xffffffffu)
        return -1;
    for (unsigned size = 2; size <= 32; size *= 2) {
        uint32_t mask = size == 32 ? 0xffffffffu : (1u << size) - 1, elem = v & mask;
        bool repeats = true;
        for (unsigned i = size; i < 32; i += size)
            if (((v >> i) & mask) != elem)
                repeats = false;
        if (!repeats)
            continue;
        // elem must be a rotated run of ones
        for (unsigned rot = 0; rot < size; rot++) {
            uint32_t r = rot ? ((elem >> rot) | (elem << (size - rot))) & mask : elem;
            unsigned ones = (unsigned)__builtin_popcount(r);
            if (ones && ones < size && r == (1u << ones) - 1) {
                // r = ROR(elem, rot), so elem = ROR(r, size - rot)
                unsigned immr = (size - rot) % size;
                unsigned imms = ((~(size * 2 - 1)) & 0x3f) | (ones - 1);
                return (int)((immr << 6) | (imms & 0x3f));
            }
        }
        return -1;
    }
    return -1;
}

// op: 0 AND, 1 ORR, 2 EOR, 3 ANDS; false if imm cannot be encoded
static bool logic_i(int op, int rd, int rn, uint32_t imm)
{
    int enc = logic_imm(imm);
    if (enc < 0)
        return false;
    e(0x12000000u | ((uint32_t)op << 29) | ((uint32_t)enc << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
    return true;
}

// Loads and stores with an unsigned offset. size: 0 byte, 1 half, 2 word, 3 doubleword.
static inline void ldr_o(int size, int rt, int rn, uint32_t off)
{
    e(0x39400000u | ((uint32_t)size << 30) | ((off >> size) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}

static inline void str_o(int size, int rt, int rn, uint32_t off)
{
    e(0x39000000u | ((uint32_t)size << 30) | ((off >> size) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}

// ... with a register offset; sign: sign-extend into the 32-bit register (bytes and halves)
static inline void ldr_r(int size, bool sign, int rt, int rn, int rm)
{
    e(0x38606800u | ((uint32_t)size << 30) | (sign ? 0x00800000u : 0) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) |
      (uint32_t)rt);
}

static inline void str_r(int size, int rt, int rn, int rm)
{
    e(0x38206800u | ((uint32_t)size << 30) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rt);
}

// An offset of any size from a base: through IP0 when the unsigned form cannot hold it.
static void ldr_big(int size, int rt, int rn, uint32_t off)
{
    if (!(off & ((1u << size) - 1)) && (off >> size) < 4096)
        ldr_o(size, rt, rn, off);
    else {
        mov_wi(IP0, off);
        ldr_r(size, false, rt, rn, IP0);
    }
}

static void str_big(int size, int rt, int rn, uint32_t off)
{
    if (!(off & ((1u << size) - 1)) && (off >> size) < 4096)
        str_o(size, rt, rn, off);
    else {
        mov_wi(IP0, off);
        str_r(size, rt, rn, IP0);
    }
}

static inline void mrs_nzcv(int rt)
{
    e(0xd53b4200u | (uint32_t)rt);
}

static inline void msr_nzcv(int rt)
{
    e(0xd51b4200u | (uint32_t)rt);
}

static inline void br(int rn)
{
    e(0xd61f0000u | ((uint32_t)rn << 5));
}

static inline void blr(int rn)
{
    e(0xd63f0000u | ((uint32_t)rn << 5));
}

// ---------------------------------------------------------------------------------------------
// The state native code works on

#define CO(f) ((uint32_t)offsetof(cpu_t, f))
#define MO(f) ((uint32_t)offsetof(chip_t, f))
#define XO(f) ((uint32_t)offsetof(jit_ctx_t, f))

// The guest registers kept in host registers, the same for every translation.
#define NMAPPED 5
static inline int reg_host(unsigned n)
{
    return n < NMAPPED ? 24 + (int)n : -1;
}

// The host register holding guest register n: its own, or tmp after loading it from the core.
static int rs(unsigned n, int tmp)
{
    int h = reg_host(n);
    if (h >= 0)
        return h;
    ldr_o(2, tmp, XC, CO(r) + 4 * n);
    return tmp;
}

// Where to compute guest register n: its own host register, or tmp (then wr puts it into the core).
static inline int rd_(unsigned n, int tmp)
{
    int h = reg_host(n);
    return h >= 0 ? h : tmp;
}

static void wr(unsigned n, int reg)
{
    int h = reg_host(n);
    if (h < 0)
        str_o(2, reg, XC, CO(r) + 4 * n);
    else
        mov_rr(0, h, reg);
}

// Before a call into C, which reads and changes the core: the host registers into it.
static void emit_spill(void)
{
    _Static_assert(NMAPPED == 5, "the pairs below");
    e(0x29000000u | ((CO(r) / 4) << 15) | (25u << 10) | (XC << 5) | 24);       // stp w24, w25, [x19, #r0]
    e(0x29000000u | ((CO(r) / 4 + 2) << 15) | (27u << 10) | (XC << 5) | 26);   // stp w26, w27, [x19, #r2]
    str_o(2, 28, XC, CO(r) + 16);
    str_o(2, WF, XC, CO(apsr));
}

static void emit_reload(void)
{
    e(0x29400000u | ((CO(r) / 4) << 15) | (25u << 10) | (XC << 5) | 24);
    e(0x29400000u | ((CO(r) / 4 + 2) << 15) | (27u << 10) | (XC << 5) | 26);
    ldr_o(2, 28, XC, CO(r) + 16);
    ldr_o(2, WF, XC, CO(apsr));
}

// ---------------------------------------------------------------------------------------------
// Fixed code: the way in and out, and the lookup of a block from its address

static inline unsigned jit_hash(uint32_t pc)
{
    return ((pc >> 17) ^ (pc >> 1)) & (JIT_LOOKUP - 1);
}

static void jit_emit_fixed(void)
{
    J.p = J.start;
    J.enter = (int (*)(cpu_t *, jit_ctx_t *, void *))(void *)J.p;
    e(0xa9ba7bfdu);   // stp x29, x30, [sp, #-96]!
    e(0xa90153f3u);   // stp x19, x20, [sp, #16]
    e(0xa9025bf5u);   // stp x21, x22, [sp, #32]
    e(0xa90363f7u);   // stp x23, x24, [sp, #48]
    e(0xa9046bf9u);   // stp x25, x26, [sp, #64]
    e(0xa90573fbu);   // stp x27, x28, [sp, #80]
    mov_rr(1, XC, 0);
    mov_rr(1, XJ, 1);
    mov_rr(1, IP0, 2);
    ldr_o(3, XM, XC, CO(m));
    ldr_o(3, XT, XC, CO(t));
    emit_reload();
    br(IP0);

    uint32_t *epi_time = J.p;
    str_o(3, XT, XC, CO(t));
    emit_spill();
    uint32_t *epi = J.p;
    e(0xa94153f3u);   // ldp x19, x20, [sp, #16]
    e(0xa9425bf5u);   // ldp x21, x22, [sp, #32]
    e(0xa94363f7u);   // ldp x23, x24, [sp, #48]
    e(0xa9446bf9u);   // ldp x25, x26, [sp, #64]
    e(0xa94573fbu);   // ldp x27, x28, [sp, #80]
    e(0xa8c67bfdu);   // ldp x29, x30, [sp], #96
    e(0xd65f03c0u);   // ret

    for (int i = 0; i < JX_COUNT; i++) {
        J.exit[i] = J.p;
        mov_wi(0, (uint32_t)i);
        b_a(epi_time);
    }
    // after a handler asked for a look: the core holds everything, the time included
    J.exit_look = J.p;
    mov_wi(0, JX_LOOK);
    b_a(epi);

    // w0: the address to go to; the hash must match jit_hash
    J.lookup = J.p;
    lsr_i(0, 9, 0, 17);
    rrr_sh(OP_EOR, 9, 9, 0, 1, 1);          // eor w9, w9, w0, lsr #1
    ubfx(9, 9, 0, 18);
    _Static_assert(JIT_LOOKUP == 1 << 18, "the mask above");
    mov_wi(IP0, XO(lookup));
    rrr(OP_ADD | SF, 10, XJ, IP0);
    rrr_sh(OP_ADD | SF, 10, 10, 9, 0, 4);   // add x10, x10, x9, lsl #4
    ldr_o(2, 11, 10, 0);
    logic_i(1, 12, 0, 1);                   // orr w12, w0, #1
    rrr(OP_EOR, 11, 11, 12);
    e(0x35000000u | (3u << 5) | 11);        // cbnz w11, miss
    ldr_o(3, 11, 10, 8);
    br(11);
    str_o(2, 0, XC, CO(r) + 60);            // miss: c->r[15] = w0
    b_a(J.exit[JX_LOOKUP]);
}

static void jit_flush(void)
{
    pbc_jit_stat[2]++;
    J.p = J.code_start;
    J.epoch++;
    memset(jx.lookup, 0, sizeof(jx.lookup));
}

// The block handlers by name, for the report of those still called most (PBCONPC_STATS=1).
static struct {
    dop_fn fn;
    uint64_t n;
} gen_count[96];
static int gen_stats = -1;

void pbc_jit_gen_report(void);
void pbc_jit_gen_report(void)
{
#define H(f) {f, #f}
    static const struct {
        dop_fn fn;
        const char *name;
    } names[] = {H(h_exec16), H(h_exec32), H(h_ldst_dual_excl), H(h_dp_shifted_reg), H(h_branch_misc),
                 H(h_dp_plain_imm), H(h_ldst_single), H(h_dp_register), H(h_multiply), H(h_long_multiply),
                 H(h_nop), H(h_movs_reg), H(h_lsl_imm), H(h_lsr_imm), H(h_asr_imm), H(h_add_reg), H(h_sub_reg),
                 H(h_add_imm), H(h_sub_imm), H(h_movs_imm), H(h_cmp_imm), H(h_cmp_reg), H(h_alu), H(h_add_hi),
                 H(h_mov_hi), H(h_ldr_lit), H(h_ldst_reg), H(h_str_imm), H(h_ldr_imm), H(h_strb_imm),
                 H(h_ldrb_imm), H(h_strh_imm), H(h_ldrh_imm), H(h_set_const), H(h_add_sp_imm), H(h_adjust_sp),
                 H(h_extend), H(h_cbz), H(h_push), H(h_pop), H(h_bx), H(h_blx), H(h_bl), H(h_dp_imm),
                 H(h_ldm_stm), H(h_ldr_w), H(h_ldrb_w), H(h_ldrh_w), H(h_ldr_w8), H(h_str_w8), H(h_ldrd_strd),
                 H(h_bcond), H(h_branch)};
#undef H
    for (int pass = 0; pass < 24; pass++) {
        int best = -1;
        for (int i = 0; i < 96; i++)
            if (gen_count[i].n && (best < 0 || gen_count[i].n > gen_count[best].n))
                best = i;
        if (best < 0)
            break;
        const char *name = "?";
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            if (names[i].fn == gen_count[best].fn)
                name = names[i].name;
        emu_log("JIT handler calls: %9.2f M  %s", (double)gen_count[best].n / 1e6, name);
        gen_count[best].n = 0;
    }
}

void pbc_jit_calls(uint64_t out[8]);
void pbc_jit_calls(uint64_t out[8])
{
    for (int i = 0; i < 8; i++)
        out[i] = 0;
}

void pbc_jit_dump(const char *path);
void pbc_jit_dump(const char *path)
{
    if (!J.start)
        return;
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(J.start, 4, (size_t)(J.p - J.start), f);
    fclose(f);
}

// For finding a wrong translation: PBCONPC_A64_MAX=n lets only the first n blocks be translated
// (the rest run in block_run), and PBCONPC_A64_DUMP=n writes the n-th one's code to a64-block.bin
// and names its instructions.
// PBCONPC_A64_OFF=bits leaves parts out, to see which one a difference comes from: 1 dead flags,
// 2 the host's flags for conditions, 4 loads and stores, 8 the multiple ones, 16 literals,
// 32 the branches, 64 the 32-bit immediates, 128 everything (handlers only).
static long dbg_max = -2, dbg_dump = -1, dbg_count, dbg_off;

// One instruction as block_run runs it, for native code: mode 0 accounts for its fetches, 1 only
// for its second halfword's (the first was done natively), 2 for neither. Returns its cycles, or
// bit 31 set when the core must be looked at.
static uint32_t a64_op(cpu_t *c, const dop_t *d, uint32_t mode)
{
    chip_t *m = c->m;
    uint32_t pc = d->pc, cyc = 1;
    if (mode < 2 && (d->lf & (FETCH_HW1 | FETCH_HW2))) {
        if (mode == 0 && (d->lf & FETCH_HW1) && (pc & ~7u) != c->fetch_line) {
            cyc += xip_cost(m, pc);
            c->fetch_line = pc & ~7u;
        }
        if (d->lf & FETCH_HW2) {
            cyc += xip_cost(m, pc + 2);
            c->fetch_line = (pc + 2) & ~7u;
        }
    }
    c->cyc = cyc;
    c->r[15] = pc + (d->lf & LF_LEN);
    d->fn(c, d);
    if (unlikely(gen_stats)) {
        for (int i = 0; i < 96; i++)
            if (gen_count[i].fn == d->fn || !gen_count[i].fn) {
                gen_count[i].fn = d->fn;
                gen_count[i].n++;
                break;
            }
    }
    // The handler may have scheduled an event inside the chunk, which moves its end in, as
    // block_run sees it too.
    if (m->until < jx.until)
        jx.until = m->until;
    if (unlikely(needs_look(c))) {
        jx.cur = d;
        return 0x80000000u;
    }
    return c->cyc;
}

static bool jit_init(void)
{
    const char *env = getenv("PICOCALC_SIM_NO_JIT");
    if (env && *env && *env != '0')
        return false;
    uint32_t *mem = jit_alloc_exec(JIT_SIZE, (const void *)&cpu_run);
    if (!mem)
        return false;
    J.start = mem;
    J.end = mem + JIT_SIZE / 4;
    jit_emit_fixed();
    J.code_start = J.p;
    __builtin___clear_cache((char *)J.start, (char *)J.p);
    jx.fn_op = (void *)a64_op;
    gen_stats = getenv("PBCONPC_STATS") != NULL;
    const char *e1 = getenv("PBCONPC_A64_MAX"), *e2 = getenv("PBCONPC_A64_DUMP"), *e3 = getenv("PBCONPC_A64_OFF");
    dbg_max = e1 ? atol(e1) : -1;
    dbg_dump = e2 ? atol(e2) : -1;
    dbg_off = e3 ? atol(e3) : 0;
    J.epoch = 1;
    return !jit_overflow;
}

static bool jit_user_off;

static bool jit_available(void)
{
    if (jit_user_off)
        return false;
    if (J.on < 0)
        J.on = jit_init();
    return J.on;
}

void cpu_jit_set(bool on)
{
    jit_user_off = !on;
    rv_jit_set(on);
}

int cpu_jit_state(void)
{
    return jit_user_off ? 0 : J.on < 0 ? 1 : J.on;
}

static void jit_forget(void)
{
    pbc_jit_stat[3]++;
    if (J.on > 0)
        jit_flush();
}

// ---------------------------------------------------------------------------------------------
// Compiling a block


// How an instruction is written out.
enum {
    KD_GENERIC,   // its handler
    KD_PLAIN,     // registers and flags only
    KD_MEM,       // one load or store
    KD_LIT,       // a literal load whose value is known
    KD_MULTI,     // push, pop, LDM, STM
    KD_BCOND,
    KD_BRANCH,
    KD_BL,
    KD_CBZ,
    KD_BX,
};

enum { FL_N = 8, FL_Z = 4, FL_C = 2, FL_V = 1, FL_NZ = 12, FL_NZC = 14, FL_ALL = 15 };
enum { HF_NONE, HF_NZ, HF_FULL };

static struct {
    const block_t *b;
    chip_t *m;
    int n;        // the instructions written out: up to an IT instruction
    bool fixed;
    uint8_t kind[BLK_MAX];
    uint8_t fixc[BLK_MAX];    // the cycles counted as fixed for each
    uint8_t fl_w[BLK_MAX];    // the flags it always writes
    bool fl_r[BLK_MAX];       // whether it (or its handler) may read flags, or leave some as they were
    bool live[BLK_MAX];       // whether the flags it writes may be read before they are written again
    int sb[BLK_MAX + 1];      // fixed cycles before each instruction
    int total;                // fixed cycles of the whole block on its dearest path
    int l_main[BLK_MAX + 1];
    int l_careful[BLK_MAX + 1];
    int l_stale;
    int hf;                   // what the host's NZCV hold of the guest's flags
    const dop_t *ops;         // the translation's copy of the instructions
    // code put after the block's main line
    struct {
        int kind, k, mode, label, back;
        uint32_t target;
        uint32_t *site;
    } cold[BLK_MAX * 6];
    int ncold;
} K;

enum { COLD_GEN, COLD_LINK };

static void logic_wi(int op, int rd, int rn, uint32_t imm);

static int cold_add(int kind, int k, int mode, int back)
{
    if (K.ncold == (int)(sizeof(K.cold) / sizeof(K.cold[0]))) {
        jit_overflow = true;
        return 0;
    }
    int l = new_label();
    K.cold[K.ncold].kind = kind;
    K.cold[K.ncold].k = k;
    K.cold[K.ncold].mode = mode;
    K.cold[K.ncold].label = l;
    K.cold[K.ncold].back = back;
    K.ncold++;
    return l;
}

static inline uint32_t op_next(const dop_t *d)
{
    return d->pc + (d->lf & LF_LEN);
}

static inline int lab(int *slot)
{
    if (*slot < 0)
        *slot = new_label();
    return *slot;
}

// x21 += k periods
static void emit_time(int k)
{
    if (!k)
        return;
    if (k >= JIT_PSK) {
        jit_overflow = true;
        return;
    }
    ldr_o(3, 9, XJ, XO(psk) + 8 * (uint32_t)k);
    rrr(OP_ADD | SF, XT, XT, 9);
}

static void emit_insns(int n)
{
    ldr_o(3, 9, XC, CO(insns));
    addsub_i(1, 0, 9, 9, (uint32_t)n, false);
    str_o(3, 9, XC, CO(insns));
}

// Leaves when the block's fixed cycles from here might not fit before the limit. Clobbers the
// host's flags.
static void emit_budget(int to)
{
    if (K.total >= JIT_PSK) {
        jit_overflow = true;
        return;
    }
    ldr_o(3, 9, XJ, XO(psk) + 8 * (uint32_t)K.total);
    rrr(OP_ADD | SF, 9, XT, 9);
    ldr_o(3, 10, XJ, XO(until));
    rrr(OP_SUBS | SF, ZR, 9, 10);
    bcond_l(A_CS, to);
}

// A way out to the block at a known address: a branch that the first time through goes to a stub
// asking for the block, and is then patched to go straight there.
static void emit_link(uint32_t target)
{
    int l = cold_add(COLD_LINK, 0, 0, -1);
    K.cold[K.ncold - 1].target = target;
    K.cold[K.ncold - 1].site = J.p;
    b_l(l);
}

// The host's flags (N, Z, C, V as the guest's) into APSR.
static void flags_full(int k)
{
    if (K.live[k]) {
        mrs_nzcv(9);
        lsr_i(1, 9, 9, 28);
        bfi(WF, 9, 28, 4);
    }
    K.hf = HF_FULL;
}

// N and Z of the host's flags into APSR; C and V stay.
static void flags_nz(int k)
{
    if (K.live[k]) {
        mrs_nzcv(9);
        lsr_i(1, 9, 9, 30);
        bfi(WF, 9, 30, 2);
    }
    K.hf = HF_NZ;
}

// N and Z from the value in a register.
static void flags_nz_of(int k, int reg)
{
    rrr(OP_ANDS, ZR, reg, reg);
    flags_nz(k);
}

// The guest's C from bit 0 of a register.
static void flag_c_from(int k, int reg)
{
    if (K.live[k])
        bfi(WF, reg, 29, 1);
}

// N, Z (and C, if carry >= 0) for a value known now.
static void flags_const(int k, uint32_t v, int carry)
{
    if (K.live[k]) {
        uint32_t clear = FL_NZ | (carry >= 0 ? FL_C : 0), set = (v >> 31 ? FL_N : 0) | (v ? 0 : FL_Z) | (carry > 0 ? FL_C : 0);
        logic_wi(0, WF, WF, ~(clear << 28));
        if (set)
            logic_wi(1, WF, WF, set << 28);
    }
    K.hf = HF_NONE;
}

// Makes the host's flags the guest's, for a condition or a carry in.
static void host_flags(void)
{
    if (K.hf != HF_FULL || (dbg_off & 2)) {
        msr_nzcv(WF);
        K.hf = HF_FULL;
    }
}

// Whether the host's flags as they stand answer the condition.
static bool cond_ready(unsigned cond)
{
    if (dbg_off & 2)
        return false;
    return K.hf == HF_FULL || (K.hf == HF_NZ && cond < 2) || (K.hf == HF_NZ && (cond == A_MI || cond == A_PL));
}

// The XIP cache's answer for the line at addr when it is the one used last in its set, which
// leaves the cache as it was apart from the counters (xip_cost); anything else goes to miss.
static void emit_xip_hit(uint32_t addr, int miss)
{
    if ((addr >> 24) >= K.m->xip_cached_top) {
        b_l(miss);
        return;
    }
    uint32_t line = (addr & 0x01ffffffu) >> 3, set = line & 1023, tag = (line >> 10) | 0x80000000u;
    ldr_big(2, 9, XM, MO(xip_ctrl));
    tbz_l(9, 0, miss);
    ldr_big(2, 9, XM, MO(xip_mru) + 4 * set);
    mov_wi(10, tag);
    rrr(OP_EOR, 9, 9, 10);
    cbnz_l(0, 9, miss);
    ldr_big(2, 9, XM, MO(xip_ctr_acc));
    addsub_i(0, 0, 9, 9, 1, false);
    str_big(2, 9, XM, MO(xip_ctr_acc));
    ldr_big(2, 9, XM, MO(xip_ctr_hit));
    addsub_i(0, 0, 9, 9, 1, false);
    str_big(2, 9, XM, MO(xip_ctr_hit));
}

// The fetch of instruction k, as block_run accounts for it: nothing unless a new cache line is
// needed, and then nothing more than counting when the line is there. A miss runs the whole
// instruction through its handler (the cold code), which then rejoins after it.
static void emit_fetch(int k, const dop_t *d)
{
    uint32_t pc = d->pc;
    if (!(d->lf & (FETCH_HW1 | FETCH_HW2)))
        return;
    int back = lab(&K.l_main[k + 1]);
    if (d->lf & FETCH_HW1) {
        int same = -1;
        // later instructions of a block always fetch from a new line when flagged
        if (k == 0) {
            ldr_o(2, 9, XC, CO(fetch_line));
            mov_wi(10, pc & ~7u);
            rrr(OP_EOR, 9, 9, 10);
            same = new_label();
            cbz_l(0, 9, same);
        }
        emit_xip_hit(pc, cold_add(COLD_GEN, k, 0, back));
        mov_wi(10, pc & ~7u);
        str_o(2, 10, XC, CO(fetch_line));
        if (same >= 0)
            bind(same);
    }
    if (d->lf & FETCH_HW2) {
        emit_xip_hit(pc + 2, cold_add(COLD_GEN, k, 1, back));
        mov_wi(10, (pc + 2) & ~7u);
        str_o(2, 10, XC, CO(fetch_line));
    }
}

static void emit_end_dynamic_c(int k);

// Instruction k through its handler, as block_run calls it. In the main line (cold false) this is
// the instruction; as cold code it is where the native form goes when it cannot go on, and control
// returns to the main line after the instruction.
static void emit_generic(int k, int mode, bool cold)
{
    const dop_t *d = &K.b->ops[k];
    bool last = k == K.n - 1;
    emit_spill();
    if (K.sb[k]) {
        ldr_o(3, 9, XJ, XO(psk) + 8 * (uint32_t)K.sb[k]);
        rrr(OP_ADD | SF, 9, XT, 9);
        str_o(3, 9, XC, CO(t));
    } else {
        str_o(3, XT, XC, CO(t));
    }
    mov_rr(1, 0, XC);
    mov_xi(1, (uint64_t)(uintptr_t)&K.ops[k]);
    mov_wi(2, (uint32_t)mode);
    ldr_o(3, IP0, XJ, XO(fn_op));
    blr(IP0);
    // a look: the handler's state is the core's; the instructions run so far are counted
    int look = new_label(), go = new_label();
    tbz_l(0, 31, go);
    bind(look);
    emit_insns(k + 1);
    b_a(J.exit_look);
    bind(go);
    // what it cost beyond the cycles counted as fixed
    if (K.fixc[k])
        addsub_i(0, 2, 0, 0, K.fixc[k], false);
    ldr_o(3, 9, XJ, XO(ps));
    e(0x9b200000u | (9u << 16) | ((uint32_t)XT << 10) | (0u << 5) | XT);   // smaddl x21, w0, w9, x21
    emit_reload();
    if (last) {
        emit_end_dynamic_c(k);
        return;
    }
    // a handler may send control elsewhere in the middle of a block (a table branch, a load of
    // the PC): then the block ends here, as it does for block_run
    int on = new_label();
    ldr_o(2, 9, XC, CO(r) + 60);
    mov_wi(10, op_next(d));
    rrr(OP_EOR, 9, 9, 10);
    cbz_l(0, 9, on);
    emit_end_dynamic_c(k);
    bind(on);
    emit_budget(lab(&K.l_careful[k + 1]));
    msr_nzcv(WF);
    if (cold)
        b_l(lab(&K.l_main[k + 1]));
    else
        K.hf = HF_FULL;
}

// The end of the block after a handler ran its last instruction: C looks at the core, as block_run
// does at a block's end, and finds where to go on.
static void emit_end_dynamic_c(int k)
{
    emit_time(K.sb[k] + K.fixc[k]);
    emit_insns(k + 1);
    b_a(J.exit[JX_LOOKUP]);
}

// The end of the block at an address in w0, which native code made itself: the lookup.
static void emit_end_lookup(int k, int extra)
{
    emit_time(K.sb[k] + K.fixc[k] + extra);
    emit_insns(k + 1);
    b_a(J.lookup);
}

// The end of the block at a known address.
static void emit_end_link(int k, int extra, uint32_t target)
{
    emit_time(K.sb[k] + K.fixc[k] + extra);
    emit_insns(k + 1);
    emit_link(target);
}

// What an instruction does to the flags: those it always writes, and whether it may read any or
// leave any of those it writes unchanged.
static void flag_effect(const dop_t *d, uint8_t *w, bool *r)
{
    dop_fn f = d->fn;
    *w = 0;
    *r = true;
    if (f == h_nop || f == h_add_hi || f == h_mov_hi || f == h_set_const || f == h_add_sp_imm || f == h_adjust_sp ||
        f == h_extend || f == h_ldr_lit || f == h_ldst_reg || f == h_str_imm || f == h_ldr_imm || f == h_strb_imm ||
        f == h_ldrb_imm || f == h_strh_imm || f == h_ldrh_imm || f == h_ldr_w || f == h_ldrb_w || f == h_ldrh_w ||
        f == h_push || f == h_ldr_w8 || f == h_str_w8 || f == h_ldrd_strd) {
        *r = false;   // neither reads nor writes
    } else if (f == h_movs_reg || f == h_movs_imm) {
        *w = FL_NZ, *r = false;
    } else if (f == h_lsl_imm || f == h_lsr_imm || f == h_asr_imm) {
        *w = FL_NZC, *r = false;
    } else if (f == h_add_reg || f == h_sub_reg || f == h_add_imm || f == h_sub_imm || f == h_cmp_imm ||
               f == h_cmp_reg) {
        *w = FL_ALL, *r = false;
    } else if (f == h_alu) {
        switch (d->imm) {
        case 0x0: case 0x1: case 0x8: case 0xc: case 0xd: case 0xe: case 0xf: *w = FL_NZ, *r = false; break;
        case 0x9: case 0xa: case 0xb: *w = FL_ALL, *r = false; break;
        case 0x5: case 0x6: *w = FL_ALL; break;   // ADC and SBC read the carry
        default: *w = FL_NZ; break;               // shifts by a register may leave the carry
        }
    }
}

// Decides how each instruction is written out, and from that the cycles counted as fixed.
static void plan(void)
{
    const block_t *b = K.b;
    for (int k = 0; k < K.n; k++) {
        const dop_t *d = &b->ops[k];
        dop_fn f = d->fn;
        bool last = k == K.n - 1;
        int kind = KD_GENERIC, fix = 0;
        if (f == h_nop || f == h_movs_reg || f == h_lsl_imm || f == h_lsr_imm || f == h_asr_imm || f == h_add_reg ||
            f == h_sub_reg || f == h_add_imm || f == h_sub_imm || f == h_movs_imm || f == h_cmp_imm ||
            f == h_cmp_reg || f == h_add_hi || f == h_mov_hi || f == h_set_const || f == h_add_sp_imm ||
            f == h_adjust_sp || f == h_extend) {
            kind = KD_PLAIN, fix = 1;
        } else if (f == h_alu) {
            if (d->imm != 2 && d->imm != 3 && d->imm != 4 && d->imm != 7)
                kind = KD_PLAIN, fix = 1;
        } else if (f == h_dp_imm) {
            unsigned op = d->rm;
            bool s = d->lf & LF_A;
            bool ok = false;
            if (op == 0 || op == 4 || op == 8 || op == 0xd)
                ok = d->rn != 15 && (d->rd != 15 || s);
            else if (op == 1 || op == 0xa || op == 0xb || op == 0xe)
                ok = d->rn != 15 && d->rd != 15;
            else if (op == 2 || op == 3)
                ok = d->rd != 15;
            if (ok)
                kind = KD_PLAIN, fix = 1;
        } else if (f == h_str_imm || f == h_ldr_imm || f == h_strb_imm || f == h_ldrb_imm || f == h_strh_imm ||
                   f == h_ldrh_imm || f == h_ldst_reg || f == h_ldr_w || f == h_ldrb_w || f == h_ldrh_w) {
            kind = KD_MEM, fix = 2;
        } else if (f == h_ldr_lit) {
            // from ROM or flash in a block from there: the value cannot change under the block
            uint32_t a = d->imm;
            const uint8_t *p = K.m->rpage[a >> 16];
            const uint8_t *src = p ? p + (a & 0xffff) : NULL;
            bool rom = src && src >= K.m->rom && src + 4 <= K.m->rom + ROM_SIZE;
            bool flash = src && src >= K.m->flash && src + 4 <= K.m->flash + K.m->flash_size;
            if (K.fixed && !(a & 3) && (rom || flash) && ((a >> 28) == 1 || (a >> 28) == 0))
                kind = KD_LIT, fix = 2;
            else
                kind = KD_MEM, fix = 2;
        } else if (f == h_push) {
            kind = KD_MULTI, fix = 1 + d->rm;
        } else if (f == h_pop) {
            kind = KD_MULTI, fix = 1 + __builtin_popcount(d->imm & 0x1ff);
        } else if (f == h_ldm_stm) {
            // the base must stay a register that is neither the PC nor loaded over while still needed
            if (d->rn != 15 && d->rm)
                kind = KD_MULTI, fix = 1 + d->rm;
        } else if (last && f == h_bcond) {
            kind = KD_BCOND, fix = 1;
        } else if (last && f == h_branch) {
            kind = KD_BRANCH, fix = 1;
        } else if (last && f == h_bl) {
            kind = KD_BL, fix = 1;
        } else if (last && f == h_cbz) {
            kind = KD_CBZ, fix = 1;
        } else if (last && (f == h_bx || f == h_blx)) {
            kind = KD_BX, fix = 1;
        }
        // an instruction that sends control on must be the last one written out
        if (!last && (kind == KD_MULTI) &&
            ((f == h_pop && (d->imm & 0x100)) || (f == h_ldm_stm && (d->lf & LF_A) && (d->imm & 0x8000))))
            kind = KD_GENERIC, fix = 0;
        if (dbg_off) {
            if (((dbg_off & 4) && kind == KD_MEM) || ((dbg_off & 8) && kind == KD_MULTI) ||
                ((dbg_off & 16) && kind == KD_LIT) || ((dbg_off & 32) && kind >= KD_BCOND) ||
                ((dbg_off & 64) && f == h_dp_imm) || (dbg_off & 128))
                kind = KD_GENERIC, fix = 0;
        }
        K.kind[k] = (uint8_t)kind;
        K.fixc[k] = (uint8_t)fix;
        flag_effect(d, &K.fl_w[k], &K.fl_r[k]);
        if (f == h_dp_imm) {
            // set here rather than in flag_effect: only the forms written out natively are known
            unsigned op = d->rm;
            bool s = d->lf & LF_A;
            K.fl_r[k] = op == 0xa || op == 0xb || kind == KD_GENERIC;
            K.fl_w[k] = 0;
            if (s && kind != KD_GENERIC) {
                if (op == 8 || op == 0xd || op == 0xe || op == 0xa || op == 0xb)
                    K.fl_w[k] = FL_ALL;
                else
                    K.fl_w[k] = (d->lf & LF_B) ? FL_NZC : FL_NZ;
            }
        }
    }
    K.sb[0] = 0;
    for (int k = 0; k < K.n; k++)
        K.sb[k + 1] = K.sb[k] + K.fixc[k];
    // the dearest way out of the last instruction: a branch taken, a call, a return
    int extra = 0;
    switch (K.kind[K.n - 1]) {
    case KD_BCOND: case KD_BRANCH: case KD_CBZ: case KD_BX: extra = 1; break;
    case KD_BL: case KD_MULTI: extra = 2; break;
    default: break;
    }
    K.total = K.sb[K.n] + extra;
    // Flags are dead where the next instruction writes them all again without reading any, and
    // nothing between can leave: the next one's handler path, should it be taken, writes them too.
    for (int k = 0; k < K.n; k++) {
        K.live[k] = true;
        if (k + 1 < K.n && K.fl_w[k] && !K.fl_r[k + 1] && (K.fl_w[k] & ~K.fl_w[k + 1]) == 0 && !(dbg_off & 1))
            K.live[k] = false;
    }
}

// rd = rn op imm with the flags set (op 1 ADDS, 3 SUBS), through IP1 when imm does not fit
static void addsubs_wi(int op, int rd, int rn, uint32_t imm)
{
    if (imm < 4096)
        addsub_i(0, op, rd, rn, imm, false);
    else if (!(imm & 0xfffu) && imm < (1u << 24))
        addsub_i(0, op, rd, rn, imm >> 12, true);
    else {
        mov_wi(IP1, imm);
        rrr(op == 1 ? OP_ADDS : OP_SUBS, rd, rn, IP1);
    }
}

// rd = rn op imm for a logical op (0 AND, 1 ORR, 2 EOR, 3 ANDS), through IP1 when not encodable
static void logic_wi(int op, int rd, int rn, uint32_t imm)
{
    static const uint32_t reg_op[4] = {OP_AND, OP_ORR, OP_EOR, OP_ANDS};
    if (!logic_i(op, rd, rn, imm)) {
        mov_wi(IP1, imm);
        rrr(reg_op[op], rd, rn, IP1);
    }
}

// The registers-only instructions.
static void emit_plain(int k, const dop_t *d)
{
    dop_fn f = d->fn;
    if (f == h_nop)
        return;
    if (f == h_movs_reg) {
        int s = rs(d->rm, 0), t = rd_(d->rd, 1);
        mov_rr(0, t, s);
        wr(d->rd, t);
        if (K.live[k])
            flags_nz_of(k, t);
        else
            K.hf = HF_NONE;
        return;
    }
    if (f == h_lsl_imm || f == h_lsr_imm || f == h_asr_imm) {
        int s = rs(d->rm, 0), t = rd_(d->rd, 1);
        unsigned n = d->imm;
        // the carry out first: the result may take the source's register
        if (f == h_lsl_imm) {
            if (K.live[k])
                ubfx(10, s, (int)(32 - n), 1);
            lsl_i(t, s, (int)n);
        } else if (n == 0) {
            // by 32: the carry is the top bit, the result nothing but zeros or sign bits
            if (K.live[k])
                lsr_i(0, 10, s, 31);
            if (f == h_lsr_imm)
                mov_wi(t, 0);
            else
                asr_i(t, s, 31);
        } else {
            if (K.live[k])
                ubfx(10, s, (int)(n - 1), 1);
            if (f == h_lsr_imm)
                lsr_i(0, t, s, (int)n);
            else
                asr_i(t, s, (int)n);
        }
        wr(d->rd, t);
        if (K.live[k]) {
            flags_nz_of(k, t);
            flag_c_from(k, 10);
        } else {
            K.hf = HF_NONE;
        }
        return;
    }
    if (f == h_add_reg || f == h_sub_reg) {
        int a = rs(d->rn, 0), b = rs(d->rm, 1), t = rd_(d->rd, 2);
        rrr(f == h_add_reg ? OP_ADDS : OP_SUBS, t, a, b);
        wr(d->rd, t);
        flags_full(k);
        return;
    }
    if (f == h_add_imm || f == h_sub_imm) {
        int a = rs(d->rn, 0), t = rd_(d->rd, 2);
        addsubs_wi(f == h_add_imm ? 1 : 3, t, a, d->imm);
        wr(d->rd, t);
        flags_full(k);
        return;
    }
    if (f == h_cmp_imm) {
        addsubs_wi(3, ZR, rs(d->rn, 0), d->imm);
        flags_full(k);
        return;
    }
    if (f == h_cmp_reg) {
        int a = rs(d->rn, 0), b = rs(d->rm, 1);
        rrr(OP_SUBS, ZR, a, b);
        flags_full(k);
        return;
    }
    if (f == h_movs_imm) {
        int t = rd_(d->rd, 0);
        mov_wi(t, d->imm);
        wr(d->rd, t);
        flags_const(k, d->imm, -1);
        return;
    }
    if (f == h_alu) {
        int a = rs(d->rd, 0), b = rs(d->rm, 1), t = rd_(d->rd, 2);
        switch (d->imm) {
        case 0x0: rrr(OP_ANDS, t, a, b); wr(d->rd, t); flags_nz(k); break;
        case 0x1: rrr(OP_EOR, t, a, b); wr(d->rd, t); flags_nz_of(k, t); break;
        case 0x5:
        case 0x6:
            host_flags();
            rrr(d->imm == 5 ? OP_ADCS : OP_SBCS, t, a, b);
            wr(d->rd, t);
            flags_full(k);
            break;
        case 0x8: rrr(OP_ANDS, ZR, a, b); flags_nz(k); break;
        case 0x9: rrr(OP_SUBS, t, ZR, b); wr(d->rd, t); flags_full(k); break;
        case 0xa: rrr(OP_SUBS, ZR, a, b); flags_full(k); break;
        case 0xb: rrr(OP_ADDS, ZR, a, b); flags_full(k); break;
        case 0xc: rrr(OP_ORR, t, a, b); wr(d->rd, t); flags_nz_of(k, t); break;
        case 0xd:
            e(0x1b007c00u | ((uint32_t)b << 16) | ((uint32_t)a << 5) | (uint32_t)t);   // mul
            wr(d->rd, t);
            flags_nz_of(k, t);
            break;
        case 0xe: rrr(OP_BICS, t, a, b); wr(d->rd, t); flags_nz(k); break;
        default: rrr(OP_ORN, t, ZR, b); wr(d->rd, t); flags_nz_of(k, t); break;
        }
        return;
    }
    if (f == h_add_hi) {
        int a = rs(d->rd, 0), b = rs(d->rm, 1), t = rd_(d->rd, 2);
        rrr(OP_ADD, t, a, b);
        wr(d->rd, t);
        return;
    }
    if (f == h_mov_hi) {
        int s = rs(d->rm, 0);
        wr(d->rd, s);
        return;
    }
    if (f == h_set_const) {
        int t = rd_(d->rd, 0);
        mov_wi(t, d->imm);
        wr(d->rd, t);
        return;
    }
    if (f == h_add_sp_imm) {
        int a = rs(13, 0), t = rd_(d->rd, 1);
        add_wi(t, a, d->imm);
        wr(d->rd, t);
        return;
    }
    if (f == h_adjust_sp) {
        int a = rs(13, 0);
        add_wi(0, a, d->imm);
        wr(13, 0);
        return;
    }
    if (f == h_extend) {
        int s = rs(d->rm, 0), t = rd_(d->rd, 1);
        switch (d->imm) {
        case 0: sbfm(t, s, 0, 15); break;
        case 1: sbfm(t, s, 0, 7); break;
        case 2: ubfx(t, s, 0, 16); break;
        default: ubfx(t, s, 0, 8); break;
        }
        wr(d->rd, t);
        return;
    }
    // h_dp_imm: the forms plan() let through
    unsigned op = d->rm;
    bool s = d->lf & LF_A;
    uint32_t imm = d->imm;
    int carry = (d->lf & LF_B) ? (int)(imm >> 31) : -1;
    if ((op == 2 || op == 3) && d->rn == 15) {
        // MOV and MVN of a constant
        uint32_t v = op == 2 ? imm : ~imm;
        int t = rd_(d->rd, 0);
        mov_wi(t, v);
        wr(d->rd, t);
        if (s)
            flags_const(k, v, carry);
        return;
    }
    int a = rs(d->rn, 0);
    int t = d->rd == 15 ? ZR : rd_(d->rd, 2);
    switch (op) {
    case 0x0: case 0x1: case 0x2: case 0x3: case 0x4: {
        // AND (TST), BIC, ORR, ORN, EOR (TEQ) with a constant
        uint32_t v = (op == 1 || op == 3) ? ~imm : imm;
        int lop = (op == 0 || op == 1) ? (s ? 3 : 0) : (op == 2 || op == 3) ? 1 : 2;
        int dst = t == ZR && lop != 3 ? 2 : t;   // TEQ has no register to drop the result in
        logic_wi(lop, dst, a, v);
        if (d->rd != 15)
            wr(d->rd, dst);
        if (s) {
            if (lop == 3)
                flags_nz(k);
            else
                flags_nz_of(k, dst);
            if (carry >= 0 && K.live[k])
                logic_wi(carry ? 1 : 0, WF, WF, carry ? 0x20000000u : 0xdfffffffu);
        }
        return;
    }
    case 0x8: case 0xd:
        if (s) {
            addsubs_wi(op == 8 ? 1 : 3, t, a, imm);
            flags_full(k);
        } else {
            add_wi(t, a, op == 8 ? imm : 0u - imm);
        }
        if (d->rd != 15)
            wr(d->rd, t);
        return;
    case 0xe:
        // RSB: imm - rn
        mov_wi(IP1, imm);
        rrr(s ? OP_SUBS : OP_SUB, t, IP1, a);
        wr(d->rd, t);
        if (s)
            flags_full(k);
        return;
    default:
        // ADC and SBC
        host_flags();
        mov_wi(IP1, imm);
        if (s) {
            rrr(op == 0xa ? OP_ADCS : OP_SBCS, t, a, IP1);
            flags_full(k);
        } else {
            rrr(op == 0xa ? 0x1a000000u : 0x5a000000u, t, a, IP1);
        }
        wr(d->rd, t);
        return;
    }
}

// The host address of the guest's memory at the address in w0, for an access of `bytes` bytes
// (`span` of them in a row, for the multiple loads and stores) in x9; anything that is not plain
// memory at an aligned address, or XIP flash for a load (which costs a cache access), goes to slow.
static void emit_host_addr(bool load, unsigned bytes, unsigned span, int slow)
{
    ldr_o(3, 9, XC, load ? CO(rpage) : CO(wpage));
    lsr_i(0, 10, 0, 16);
    e(0xf8607800u | (10u << 16) | (9u << 5) | 9);   // ldr x9, [x9, x10, lsl #3]
    cbz_l(1, 9, slow);
    if (bytes == 4) {
        ubfx(11, 0, 0, 2);
        cbnz_l(0, 11, slow);
    } else if (bytes == 2) {
        tbnz_l(0, 0, slow);
    }
    if (load) {
        lsr_i(0, 11, 0, 28);
        addsub_i(0, 2, 11, 11, 1, false);
        cbz_l(0, 11, slow);
    }
    ubfx(10, 0, 0, 16);
    if (span > bytes) {
        // all of it within this page
        addsub_i(0, 0, 11, 10, span - 1, false);
        lsr_i(0, 11, 11, 16);
        cbnz_l(0, 11, slow);
    }
    rrr(OP_ADD | SF, 9, 9, 10);
}

static void emit_mem(int k, const dop_t *d)
{
    dop_fn f = d->fn;
    bool load, sign = false;
    unsigned size;   // log2 of the bytes
    bool reg_off = false;
    uint32_t imm = d->imm;
    if (f == h_ldst_reg) {
        static const uint8_t sizes[8] = {2, 1, 0, 0, 2, 1, 0, 1};
        reg_off = true;
        load = d->imm >= 3;
        size = sizes[d->imm];
        sign = d->imm == 3 || d->imm == 7;
    } else if (f == h_ldr_lit) {
        load = true, size = 2;
    } else {
        load = f == h_ldr_imm || f == h_ldrb_imm || f == h_ldrh_imm || f == h_ldr_w || f == h_ldrb_w || f == h_ldrh_w;
        size = (f == h_str_imm || f == h_ldr_imm || f == h_ldr_w) ? 2 : (f == h_strh_imm || f == h_ldrh_imm || f == h_ldrh_w) ? 1 : 0;
    }
    int slow = cold_add(COLD_GEN, k, 2, lab(&K.l_main[k + 1]));
    if (f == h_ldr_lit) {
        mov_wi(0, imm);
    } else if (reg_off) {
        int a = rs(d->rn, 0), b = rs(d->rm, 1);
        rrr(OP_ADD, 0, a, b);
    } else {
        int a = rs(d->rn, 0);
        add_wi(0, a, imm);
    }
    emit_host_addr(load, 1u << size, 1u << size, slow);
    if (load) {
        int t = rd_(d->rd, 2);
        if (sign)
            e(0x39c00000u | ((uint32_t)size << 30) | (9u << 5) | (uint32_t)t);   // ldrsb/ldrsh w, [x9]
        else
            ldr_o((int)size, t, 9, 0);
        wr(d->rd, t);
    } else {
        int v = rs(d->rd, 2);
        str_o((int)size, v, 9, 0);
    }
}

// A literal from ROM or flash, whose value the block knows: only the XIP cache's part remains.
static void emit_lit(int k, const dop_t *d)
{
    uint32_t a = d->imm;
    if ((a >> 28) == 1)
        emit_xip_hit(a, cold_add(COLD_GEN, k, 2, lab(&K.l_main[k + 1])));
    const uint8_t *p = K.m->rpage[a >> 16];
    int t = rd_(d->rd, 0);
    mov_wi(t, rd32le(p + (a & 0xffff)));
    wr(d->rd, t);
}

// Leaves to slow unless the value in w0 is an ordinary Thumb address to branch to.
static void emit_interwork_check(int slow)
{
    tbz_l(0, 0, slow);
    lsr_i(0, 11, 0, 25);
    addsub_i(0, 2, 11, 11, 0x7f, false);
    cbz_l(0, 11, slow);
}

// PUSH, POP, LDM and STM: all the words in one piece of plain memory, or the handler.
static void emit_multi(int k, const dop_t *d)
{
    dop_fn f = d->fn;
    unsigned rn, n;
    uint32_t list;
    bool load, wb, db, pc = false;
    if (f == h_push) {
        rn = 13, list = d->imm, n = d->rm, load = false, wb = true, db = true;
    } else if (f == h_pop) {
        rn = 13, list = d->imm & 0xff, pc = d->imm & 0x100, load = true, wb = true, db = false;
        n = (unsigned)__builtin_popcount(d->imm & 0x1ff);
    } else {
        rn = d->rn, list = d->imm & 0x7fff, n = d->rm;
        load = d->lf & LF_A, wb = d->lf & LF_B, db = d->lf & LF_C;
        pc = load && (d->imm & 0x8000);
    }
    int slow = cold_add(COLD_GEN, k, 2, lab(&K.l_main[k + 1]));
    int base = rs(rn, 12);
    // w0: the lowest address; w12: the base
    mov_rr(0, 12, base);
    if (db)
        add_wi(0, 12, 0u - 4 * n);
    else
        mov_rr(0, 0, 12);
    emit_host_addr(load, 4, 4 * n, slow);
    if (load) {
        if (pc) {
            // the address to return to first: an unusual one leaves everything to the handler
            ldr_o(2, 0, 9, 4 * (n - 1));
            emit_interwork_check(slow);
        }
        unsigned j = 0;
        for (unsigned i = 0; i < 15; i++)
            if (list & (1u << i)) {
                int t = rd_(i, 1);
                ldr_o(2, t, 9, 4 * j++);
                wr(i, t);
            }
        // POP always moves the stack pointer; LDM writes back unless the base was loaded
        if (f == h_pop || (wb && !(list & (1u << rn)))) {
            add_wi(1, 12, db ? 0u - 4 * n : 4 * n);
            wr(rn, 1);
        }
        if (pc) {
            logic_i(0, 0, 0, 0xfffffffeu);
            emit_end_lookup(k, 2);
        }
    } else {
        unsigned j = 0;
        for (unsigned i = 0; i < 15; i++)
            if (list & (1u << i)) {
                // the base's own value is the one from before the instruction
                int v = i == rn ? 12 : rs(i, 1);
                str_o(2, v, 9, 4 * j++);
            }
        if (wb) {
            add_wi(1, 12, db ? 0u - 4 * n : 4 * n);
            wr(rn, 1);
        }
    }
}

// Jumps to taken when the guest's condition holds.
static void emit_cond(unsigned cond, int taken)
{
    if (!cond_ready(cond))
        host_flags();
    bcond_l((int)cond, taken);
}

static void emit_block_end(int k, const dop_t *d)
{
    switch (K.kind[k]) {
    case KD_BCOND: {
        int taken = new_label();
        emit_cond(d->rd, taken);
        emit_end_link(k, 0, op_next(d));
        bind(taken);
        emit_end_link(k, 1, d->imm);
        break;
    }
    case KD_CBZ: {
        int taken = new_label();
        int v = rs(d->rn, 0);
        if (d->rd)
            cbnz_l(0, v, taken);
        else
            cbz_l(0, v, taken);
        emit_end_link(k, 0, op_next(d));
        bind(taken);
        emit_end_link(k, 1, d->imm);
        break;
    }
    case KD_BRANCH:
        emit_end_link(k, 1, d->imm);
        break;
    case KD_BL:
        mov_wi(0, (d->pc + 4) | 1);
        wr(14, 0);
        emit_end_link(k, 2, d->imm);
        break;
    default: {
        // BX and BLX
        int slow = cold_add(COLD_GEN, k, 2, -1);
        int v = rs(d->rm, 0);
        mov_rr(0, 0, v);
        emit_interwork_check(slow);
        if (d->fn == h_blx) {
            mov_wi(1, (d->pc + 2) | 1);
            wr(14, 1);
        }
        logic_i(0, 0, 0, 0xfffffffeu);
        emit_end_lookup(k, 1);
        break;
    }
    }
}

static void emit_cold(void)
{
    for (int i = 0; i < K.ncold; i++) {
        bind(K.cold[i].label);
        if (K.cold[i].kind == COLD_GEN) {
            emit_generic(K.cold[i].k, K.cold[i].mode, true);
        } else {
            // a link's first time: the address for the core, and the branch for C to patch
            mov_wi(9, K.cold[i].target);
            str_o(2, 9, XC, CO(r) + 60);
            int64_t off = (K.cold[i].site - J.p) * 4;   // in bytes
            e(0x10000000u | (((uint32_t)off & 3) << 29) | (((uint32_t)(off >> 2) & 0x7ffffu) << 5) | 9);   // adr x9, site
            str_o(3, 9, XJ, XO(site));
            b_a(J.exit[JX_LINK]);
        }
    }
    // leaving before instruction k for block_run, the core as that expects it
    for (int k = 0; k <= K.n; k++) {
        if (K.l_careful[k] < 0)
            continue;
        bind(K.l_careful[k]);
        const block_t *b = K.b;
        mov_wi(9, k < b->n ? b->ops[k].pc : op_next(&b->ops[k - 1]));
        str_o(2, 9, XC, CO(r) + 60);
        emit_time(K.sb[k]);
        if (k)
            emit_insns(k);
        b_a(J.exit[JX_CAREFUL]);
    }
    if (K.l_stale >= 0) {
        bind(K.l_stale);
        mov_wi(9, K.b->tag & ~1u);
        str_o(2, 9, XC, CO(r) + 60);
        b_a(J.exit[JX_STALE]);
    }
}

// The block is used only while it is what it was made from: the chip's code generation for ROM
// and flash, the bytes themselves for SRAM.
static void emit_block_entry(void)
{
    const block_t *b = K.b;
    uint32_t pc0 = b->tag & ~1u;
    K.l_stale = new_label();
    if (K.fixed) {
        ldr_big(2, 9, XM, MO(code_gen));
        mov_wi(10, b->gen);
        rrr(OP_EOR, 9, 9, 10);
        cbnz_l(0, 9, K.l_stale);
    } else {
        uint32_t base = MO(sram) + (pc0 - SRAM_BASE);
        mov_wi(IP0, base);
        rrr(OP_ADD | SF, 11, XM, IP0);
        unsigned n = b->bytes;
        for (unsigned off = 0; off < n;) {
            unsigned rest = n - off;
            if (rest >= 8 || n >= 8) {
                if (rest < 8)
                    off = n - 8;
                uint64_t v;
                memcpy(&v, b->code + off, 8);
                e(0xf8400000u | ((off & 0x1ff) << 12) | (11u << 5) | 9);   // ldur x9, [x11, #off]
                mov_xi(10, v);
                rrr(OP_EOR | SF, 9, 9, 10);
                cbnz_l(1, 9, K.l_stale);
                off += 8;
            } else if (rest >= 4) {
                e(0xb8400000u | ((off & 0x1ff) << 12) | (11u << 5) | 9);   // ldur w9, [x11, #off]
                mov_wi(10, rd32le(b->code + off));
                rrr(OP_EOR, 9, 9, 10);
                cbnz_l(0, 9, K.l_stale);
                off += 4;
            } else {
                e(0x78400000u | ((off & 0x1ff) << 12) | (11u << 5) | 9);   // ldurh w9, [x11, #off]
                mov_wi(10, rd16le(b->code + off));
                rrr(OP_EOR, 9, 9, 10);
                cbnz_l(0, 9, K.l_stale);
                off += 2;
            }
        }
    }
    emit_budget(lab(&K.l_careful[0]));
    K.hf = HF_NONE;
}

static void *jit_compile(chip_t *m, block_t *b)
{
    if (dbg_max >= 0 && dbg_count >= dbg_max)
        return NULL;
    dbg_count++;
    pbc_jit_stat[0]++;
    uint32_t pc0 = b->tag & ~1u;
    if (!b->fixed && (pc0 - SRAM_BASE >= sizeof(m->sram) || pc0 - SRAM_BASE + b->bytes > sizeof(m->sram)))
        return NULL;
    if ((size_t)(J.end - J.p) < JIT_BLOCK_ROOM / 4)
        jit_flush();

    uint32_t *begin = J.p;
    memset(&K, 0, sizeof(K));
    K.b = b;
    K.m = m;
    K.n = b->nrun;
    K.fixed = b->fixed;
    memset(K.l_main, -1, sizeof(K.l_main));
    memset(K.l_careful, -1, sizeof(K.l_careful));
    K.l_stale = -1;
    nlab = nfix = 0;
    jit_overflow = false;

    jit_hdr_t *h = (jit_hdr_t *)(void *)J.p;
    J.p += (sizeof(jit_hdr_t) + 3) / 4;
    h->pc = pc0;
    h->gen = b->gen;
    h->bytes = b->bytes;
    h->fixed = b->fixed;
    memcpy(h->code, b->code, b->bytes);
    memcpy(h->ops, b->ops, sizeof(h->ops));
    K.ops = h->ops;
    _Static_assert(sizeof(jit_hdr_t) % 8 == 0, "code follows the header");

    uint32_t *entry = J.p;
    plan();
    emit_block_entry();
    for (int k = 0; k < K.n; k++) {
        const dop_t *d = &b->ops[k];
        if (K.l_main[k] >= 0)
            bind(K.l_main[k]);
        emit_fetch(k, d);
        switch (K.kind[k]) {
        case KD_GENERIC: emit_generic(k, 2, false); break;
        case KD_PLAIN: emit_plain(k, d); break;
        case KD_MEM: emit_mem(k, d); break;
        case KD_LIT: emit_lit(k, d); break;
        case KD_MULTI: emit_multi(k, d); break;
        default: emit_block_end(k, d); break;
        }
    }
    // where a block ends without sending control on itself: the next address follows
    if (K.l_main[K.n] >= 0)
        bind(K.l_main[K.n]);
    int last = K.kind[K.n - 1];
    const dop_t *ld = &b->ops[K.n - 1];
    bool ended = last == KD_GENERIC || last == KD_BCOND || last == KD_BRANCH || last == KD_BL || last == KD_CBZ ||
                 last == KD_BX ||
                 (last == KD_MULTI && ((ld->fn == h_pop && (ld->imm & 0x100)) ||
                                       (ld->fn == h_ldm_stm && (ld->lf & LF_A) && (ld->imm & 0x8000))));
    if (K.l_main[K.n] >= 0 && ended) {
        // cold code came back here after a handler ran the last instruction, which went elsewhere:
        // that case never returns (emit_generic leaves through C), so nothing can arrive here
        e(0xd4200000u);   // brk
    } else if (!ended) {
        if (K.n < b->n) {
            // an IT instruction was the last: cpu_run steps what it covers
            emit_time(K.sb[K.n]);
            emit_insns(K.n);
            mov_wi(9, op_next(ld));
            str_o(2, 9, XC, CO(r) + 60);
            b_a(J.exit[JX_DONE]);
        } else {
            emit_time(K.sb[K.n]);
            emit_insns(K.n);
            emit_link(op_next(ld));
        }
    }
    emit_cold();
    resolve_fixups();
    if (jit_overflow) {
        J.p = begin;
        return NULL;
    }
    __builtin___clear_cache((char *)begin, (char *)J.p);
    if (dbg_count == dbg_dump || (dbg_dump < -1 && (uint32_t)-dbg_dump == pc0)) {   // DUMP=-address: the block there
        emu_log("A64 block %ld at %08x: %d of %d instructions, %s, code at %p", dbg_count, pc0, K.n, b->n,
                b->fixed ? "fixed" : "SRAM", (void *)entry);
        for (int k = 0; k < K.n; k++)
            emu_log("  %08x %04x %04x kind %d fix %d live %d rd %u rn %u rm %u imm %08x lf %02x", b->ops[k].pc,
                    b->ops[k].hw1, b->ops[k].hw2, K.kind[k], K.fixc[k], K.live[k], b->ops[k].rd, b->ops[k].rn,
                    b->ops[k].rm, b->ops[k].imm, b->ops[k].lf);
        FILE *f = fopen("a64-block.bin", "wb");
        if (f) {
            fwrite(entry, 4, (size_t)(J.p - entry), f);
            fclose(f);
        }
    }
    return entry;
}

static void *jit_code_for(chip_t *m, block_t *b)
{
    if (b->jit && b->jit_epoch == J.epoch)
        return b->jit;
    uint32_t pc = b->tag & ~1u;
    void *code = jx.lookup[jit_hash(pc)].code;
    if (code && jx.lookup[jit_hash(pc)].tag == (pc | 1)) {
        // a translation the block cache lost track of serves again if made from the same code
        const jit_hdr_t *h = (const jit_hdr_t *)((uint8_t *)code - sizeof(jit_hdr_t));
        if (h->fixed == b->fixed &&
            (b->fixed ? h->gen == b->gen : h->bytes == b->bytes && !memcmp(h->code, b->code, b->bytes))) {
            b->jit = code;
            b->jit_epoch = J.epoch;
            return code;
        }
    }
    code = jit_compile(m, b);
    if (code) {
        b->jit = code;
        b->jit_epoch = J.epoch;
        jx.lookup[jit_hash(pc)].tag = pc | 1;
        jx.lookup[jit_hash(pc)].code = code;
    }
    return code;
}

static inline block_t *block_find_for_jit(chip_t *m, uint32_t pc)
{
    return block_find(m, pc);
}

static void jit_set_ps(uint32_t ps)
{
    J.ps = ps;
    jx.ps = ps;
    for (unsigned k = 0; k < JIT_PSK; k++)
        jx.psk[k] = (uint64_t)k * ps;
}

// Where each core last entered native code, so that the next chunk can go straight back in.
typedef struct {
    uint32_t pc, epoch;
    void *code;
} jit_entry_t;
static jit_entry_t jit_entries[2][4096];

static inline void *jit_entry_for(cpu_t *c, uint32_t pc)
{
    jit_entry_t *e_ = &jit_entries[c->id & 1][(pc >> 1) & 4095];
    return e_->pc == pc && e_->epoch == J.epoch && e_->code ? e_->code : NULL;
}

// Like block_run, returning true when it stopped early for block_run to finish the time to the
// exact instruction.
static bool jit_loop(cpu_t *c, void *code)
{
    chip_t *m = c->m;
    for (;;) {
        int why = J.enter(c, &jx, code);
        pbc_jit_why[why < 8 ? why : 7]++;
        if (why == JX_DONE)
            return false;
        if (why == JX_CAREFUL) {
            // near the limit: block_run takes it from here to the exact instruction, as it would
            // have had it run all along (going back to cpu_run first would end the chunk at an
            // event scheduled meanwhile, which block_run does not look for)
            if (c->t >= jx.until)
                return false;   // what the last instruction cost has used the time up
            block_t *cb = block_find(m, c->r[15]);
            if (!cb)
                return true;
            block_run(c, cb, jx.until);
            return false;
        }
        if (why == JX_LOOK) {
            // as block_run when a handler asks for a look
            if (c->fault)
                take_flagged_fault(c, jx.cur->pc, 0);
            c->t += (uint64_t)c->cyc * m->sys_ps;
            return false;
        }
        uint32_t *site = jx.site;
        uint32_t epoch = J.epoch, npc = c->r[15];
        // what block_run looks at where a block ends
        if (c->t >= jx.until || c->itstate || c->state != CPU_RUN || c->ns || m->code_flush || c->check_exc)
            return false;
        if (m->sys_ps != J.ps)
            return false;
        if (why == JX_STALE) {
            // the code under a translation changed: all of them go, as they may be linked to it
            jit_flush();
        }
        block_t *nb = block_find(m, npc);
        if (!nb)
            return false;
        code = jit_code_for(m, nb);
        if (!code) {
            block_run(c, nb, jx.until);
            return false;
        }
        if (why == JX_LINK && epoch == J.epoch) {
            *site = 0x14000000u | ((uint32_t)((uint32_t *)code - site) & 0x03ffffffu);
            __builtin___clear_cache((char *)site, (char *)(site + 1));
        }
    }
}

static bool jit_run_code(cpu_t *c, void *code, uint64_t until)
{
    chip_t *m = c->m;
    if (m->sys_ps != J.ps) {
        // translations do not hold the period, but the table they read it from changes
        jit_set_ps(m->sys_ps);
    }
    if (m->until < until)
        until = m->until;
    jx.until = until;
    return jit_loop(c, code);
}

static bool jit_run(cpu_t *c, block_t *b, uint64_t until)
{
    chip_t *m = c->m;
    if (m->sys_ps != J.ps)
        jit_set_ps(m->sys_ps);
    if (m->until < until)
        until = m->until;
    jx.until = until;
    void *code = jit_code_for(m, b);
    if (!code) {
        block_run(c, b, until);
        return false;
    }
    uint32_t pc0 = b->tag & ~1u;
    jit_entry_t *e_ = &jit_entries[c->id & 1][(pc0 >> 1) & 4095];
    e_->pc = pc0;
    e_->epoch = J.epoch;
    e_->code = code;
    return jit_loop(c, code);
}
