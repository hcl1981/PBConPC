// PBConPC: the ARM64 instructions rvjit.inc is written in terms of.
//
// x19 holds the core, x21 the chip and w20 the cycles left; w9 to w11 are the translator's scratch
// registers and w12, w13, x16 and x17 this file's own. Nine guest registers live in host registers
// (w22 to w28, w14 and w15) and are written back around every call into C that looks at them.

enum { H_T0 = 9, H_T1 = 10, H_T2 = 11, A_S0 = 12, A_S1 = 13, A_IP0 = 16, A_IP1 = 17, A_RC = 19, A_REM = 20, A_RM = 21, A_ZR = 31 };
enum { A_EQ, A_NE, A_HS, A_LO, A_MI, A_PL, A_VS, A_VC, A_HI, A_LS, A_GE, A_LT, A_GT, A_LE };

// a0 to a5, sp, s0 and s1
static int8_t h_greg[33];
static const uint8_t h_pinned[] = {10, 11, 12, 13, 14, 15, 2, 8, 9};

static inline void e32(uint32_t w)
{
    if (RJ.p + 4 <= RJ.end) {
        memcpy(RJ.p, &w, 4);
        RJ.p += 4;
    } else {
        RJ.overflow = true;
    }
}

static void h_mov_i(int rd, uint32_t v)
{
    uint32_t lo = v & 0xffff, hi = v >> 16;
    if (hi == 0) {
        e32(0x52800000u | (lo << 5) | (uint32_t)rd);
    } else if (hi == 0xffff) {
        e32(0x12800000u | ((~lo & 0xffff) << 5) | (uint32_t)rd);
    } else {
        e32(0x52a00000u | (hi << 5) | (uint32_t)rd);
        if (lo)
            e32(0x72800000u | (lo << 5) | (uint32_t)rd);
    }
}

static void a_mov_xi(int rd, uint64_t v)
{
    bool first = true;
    for (unsigned i = 0; i < 4; i++) {
        uint32_t part = (uint32_t)(v >> (16 * i)) & 0xffff;
        if (!part && !(first && i == 3))
            continue;
        e32((first ? 0xd2800000u : 0xf2800000u) | (i << 21) | (part << 5) | (uint32_t)rd);
        first = false;
    }
}

static inline void a_rrr(uint32_t op, int rd, int rn, int rm)
{
    e32(op | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

static void h_mov(int d, int s)
{
    if (d != s)
        a_rrr(0x2a0003e0u, d, 0, s);
}

static inline void a_mov_x(int d, int s)
{
    a_rrr(0xaa0003e0u, d, 0, s);
}

// Loads and stores at [rn + off]: size 1, 2, 4 or 8 bytes, through x16 where the offset does not
// fit the instruction.
static void a_mem(bool load, int size, int rt, int rn, uint32_t off)
{
    static const uint32_t imm_op[2][4] = {{0x39000000u, 0x79000000u, 0xb9000000u, 0xf9000000u},
                                          {0x39400000u, 0x79400000u, 0xb9400000u, 0xf9400000u}};
    static const uint32_t reg_op[2][4] = {{0x38206800u, 0x78206800u, 0xb8206800u, 0xf8206800u},
                                          {0x38606800u, 0x78606800u, 0xb8606800u, 0xf8606800u}};
    int k = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
    if (off % (uint32_t)size == 0 && off / (uint32_t)size < 4096) {
        e32(imm_op[load][k] | ((off / (uint32_t)size) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
    } else {
        a_mov_xi(A_IP0, off);
        a_rrr(reg_op[load][k], rt, rn, A_IP0);
    }
}

static void h_ldx(int r, unsigned g)
{
    a_mem(true, 4, r, A_RC, XOFF(g));
}

static void h_stx(unsigned g, int r)
{
    a_mem(false, 4, r, A_RC, XOFF(g));
}

static void h_alu(int op, int d, int a, int b)
{
    static const uint32_t opc[] = {[HA_ADD] = 0x0b000000u, [HA_SUB] = 0x4b000000u, [HA_AND] = 0x0a000000u,
                                   [HA_OR] = 0x2a000000u,  [HA_XOR] = 0x4a000000u};
    a_rrr(opc[op], d, a, b);
}

// The encoding of v as a logical immediate (immr << 6 | imms), or -1 when it has none.
static int a_logic_imm(uint32_t v)
{
    if (v == 0 || v == 0xffffffffu)
        return -1;
    for (unsigned size = 2; size <= 32; size <<= 1) {
        uint32_t mask = size == 32 ? 0xffffffffu : (1u << size) - 1, elt = v & mask;
        bool rep = true;
        for (unsigned i = size; i < 32; i += size)
            if (((v >> i) & mask) != elt)
                rep = false;
        if (!rep)
            continue;
        unsigned ones = (unsigned)__builtin_popcount(elt);
        if (ones == 0 || ones == size)
            continue;
        for (unsigned r = 0; r < size; r++) {
            uint32_t rot = r ? ((elt >> r) | (elt << (size - r))) & mask : elt;
            if (rot == (1u << ones) - 1)
                return (int)((((size - r) & (size - 1)) << 6) | ((~(size * 2 - 1)) & 0x3f) | (ones - 1));
        }
    }
    return -1;
}

static void h_alu_i(int op, int d, int a, uint32_t imm)
{
    if (op == HA_SUB) {
        op = HA_ADD;
        imm = 0u - imm;
    }
    if (op == HA_ADD) {
        if (imm == 0) {
            h_mov(d, a);
        } else if (imm < 4096) {
            e32(0x11000000u | (imm << 10) | ((uint32_t)a << 5) | (uint32_t)d);
        } else if (0u - imm < 4096) {
            e32(0x51000000u | ((0u - imm) << 10) | ((uint32_t)a << 5) | (uint32_t)d);
        } else {
            h_mov_i(A_S0, imm);
            a_rrr(0x0b000000u, d, a, A_S0);
        }
        return;
    }
    static const uint32_t imm_op[] = {[HA_AND] = 0x12000000u, [HA_OR] = 0x32000000u, [HA_XOR] = 0x52000000u};
    int enc = a_logic_imm(imm);
    if (enc >= 0) {
        e32(imm_op[op] | ((uint32_t)enc << 10) | ((uint32_t)a << 5) | (uint32_t)d);
    } else if (imm == 0) {
        if (op == HA_AND)
            h_mov_i(d, 0);
        else
            h_mov(d, a);
    } else {
        h_mov_i(A_S0, imm);
        h_alu(op, d, a, A_S0);
    }
}

static void h_shift_i(int op, int d, int a, unsigned n)
{
    if (!n) {
        h_mov(d, a);
        return;
    }
    if (op == HS_SLL)
        e32(0x53000000u | (((32 - n) & 31) << 16) | ((31 - n) << 10) | ((uint32_t)a << 5) | (uint32_t)d);
    else
        e32((op == HS_SRL ? 0x53000000u : 0x13000000u) | (n << 16) | (31u << 10) | ((uint32_t)a << 5) | (uint32_t)d);
}

static void h_shift(int op, int d, int a, int b)
{
    static const uint32_t opc[] = {[HS_SLL] = 0x1ac02000u, [HS_SRL] = 0x1ac02400u, [HS_SRA] = 0x1ac02800u};
    a_rrr(opc[op], d, a, b);
}

static void h_mul(int d, int a, int b)
{
    a_rrr(0x1b007c00u, d, a, b);
}

// d = b + (a << n)
static void h_shadd(int d, int a, int b, unsigned n)
{
    e32(0x0b000000u | ((uint32_t)a << 16) | (n << 10) | ((uint32_t)b << 5) | (uint32_t)d);
}

static void h_ext(int kind, int d, int s)
{
    e32((kind == HE_SB ? 0x13001c00u : kind == HE_SH ? 0x13003c00u : 0x53003c00u) | ((uint32_t)s << 5) | (uint32_t)d);
}

static void h_not(int d, int s)
{
    a_rrr(0x2a2003e0u, d, 0, s);
}

static void h_rot(bool left, int d, int a, int b)
{
    if (left) {
        a_rrr(0x4b000000u, A_S0, A_ZR, b);   // neg
        b = A_S0;
    }
    a_rrr(0x1ac02c00u, d, a, b);
}

static void h_rot_i(int d, int a, unsigned n)
{
    if (!n)
        h_mov(d, a);
    else
        e32(0x13800000u | ((uint32_t)a << 16) | (n << 10) | ((uint32_t)a << 5) | (uint32_t)d);
}

// The upper half of the product: 0 signed, 1 signed by unsigned, 2 unsigned.
static void h_mulh(int kind, int d, int a, int b)
{
    if (kind == 1) {
        e32(0x93407c00u | ((uint32_t)a << 5) | A_S0);   // sxtw x12, a
        h_mov(A_S1, b);
        a_rrr(0x2a0003e0u, A_S1, 0, b);
        a_rrr(0x9b007c00u, A_S0, A_S0, A_S1);
    } else {
        a_rrr(kind == 0 ? 0x9b207c00u : 0x9ba07c00u, A_S0, a, b);
    }
    e32(0xd340fc00u | (32u << 16) | ((uint32_t)A_S0 << 5) | A_S0);   // lsr x12, x12, #32
    h_mov(d, A_S0);
}

static inline void a_cmp(int a, int b)
{
    a_rrr(0x6b00001fu, 0, a, b);
}

static void a_cmp_i(int a, uint32_t imm)
{
    if (imm < 4096) {
        e32(0x7100001fu | (imm << 10) | ((uint32_t)a << 5));
    } else if (0u - imm < 4096) {
        e32(0x3100001fu | ((0u - imm) << 10) | ((uint32_t)a << 5));
    } else {
        h_mov_i(A_S1, imm);
        a_cmp(a, A_S1);
    }
}

// 0 min, 1 minu, 2 max, 3 maxu
static void h_minmax(int kind, int d, int a, int b)
{
    static const uint8_t cond[] = {A_LT, A_LO, A_GT, A_HI};
    a_cmp(a, b);
    e32(0x1a800000u | ((uint32_t)b << 16) | ((uint32_t)cond[kind] << 12) | ((uint32_t)a << 5) | (uint32_t)d);
}

static void h_clz(int d, int a)
{
    e32(0x5ac01000u | ((uint32_t)a << 5) | (uint32_t)d);
}

static void h_ctz(int d, int a)
{
    e32(0x5ac00000u | ((uint32_t)a << 5) | A_S0);   // rbit
    e32(0x5ac01000u | ((uint32_t)A_S0 << 5) | (uint32_t)d);
}

static void h_rev8(int d, int a)
{
    e32(0x5ac00800u | ((uint32_t)a << 5) | (uint32_t)d);
}

static void a_cset(int d, int cond)
{
    e32(0x1a9f07e0u | ((uint32_t)(cond ^ 1) << 12) | (uint32_t)d);
}

static void h_setcmp(int cond, int d, int a, int b)
{
    a_cmp(a, b);
    a_cset(d, cond == HC_LT ? A_LT : A_LO);
}

static void h_setcmp_i(int cond, int d, int a, int32_t imm)
{
    a_cmp_i(a, (uint32_t)imm);
    a_cset(d, cond == HC_LT ? A_LT : A_LO);
}

static const uint8_t a_cc[] = {[HC_EQ] = A_EQ, [HC_NE] = A_NE, [HC_LT] = A_LT, [HC_GE] = A_GE, [HC_LTU] = A_LO, [HC_GEU] = A_HS};

static void a_bcond(int cond, int l)
{
    fix_add(l, 1);
    e32(0x54000000u | (uint32_t)cond);
}

static void h_br(int cond, int a, int b, int l)
{
    a_cmp(a, b);
    a_bcond(a_cc[cond], l);
}

static void h_br_i(int cond, int a, uint32_t imm, int l)
{
    if (imm == 0 && (cond == HC_EQ || cond == HC_NE)) {
        fix_add(l, 1);
        e32((cond == HC_EQ ? 0x34000000u : 0x35000000u) | (uint32_t)a);
        return;
    }
    a_cmp_i(a, imm);
    a_bcond(a_cc[cond], l);
}

static void h_br_nz(int r, int l)
{
    fix_add(l, 1);
    e32(0x35000000u | (uint32_t)r);
}

static void h_jmp(int l)
{
    fix_add(l, 0);
    e32(0x14000000u);
}

static void h_jmp_abs(const uint8_t *target)
{
    e32(0x14000000u | ((uint32_t)((target - RJ.p) >> 2) & 0x03ffffffu));
}

static void h_br_nz_abs(int r, const uint8_t *target)
{
    e32(0x34000000u | (2u << 5) | (uint32_t)r);   // cbz r, over the branch
    h_jmp_abs(target);
}

// kind 0: b and bl's 26-bit offset; 1: the 19-bit one of b.cond, cbz and cbnz
static void h_resolve(uint8_t *at, const uint8_t *target, int kind)
{
    uint32_t w, off = (uint32_t)((target - at) >> 2);
    memcpy(&w, at, 4);
    if (kind == 0)
        w = (w & 0xfc000000u) | (off & 0x03ffffffu);
    else
        w = (w & 0xff00001fu) | ((off & 0x7ffffu) << 5);
    memcpy(at, &w, 4);
}

static uint8_t *h_link(int l)
{
    uint8_t *site = RJ.p;
    h_jmp(l);
    return site;
}

static void h_patch(void *site, const void *dest)
{
    h_resolve(site, dest, 0);
    __builtin___clear_cache((char *)site, (char *)site + 4);
}

// Unsigned division as RISC-V defines it: by zero gives all ones, and the dividend as remainder.
static void h_divu(bool rem, int d, int a, int b)
{
    a_rrr(0x1ac00800u, A_S0, a, b);
    if (rem) {
        e32(0x1b008000u | ((uint32_t)b << 16) | ((uint32_t)a << 10) | ((uint32_t)A_S0 << 5) | A_S1);   // msub
        h_mov(d, A_S1);
    } else {
        a_cmp_i(b, 0);
        e32(0x5a800000u | ((uint32_t)A_ZR << 16) | ((uint32_t)A_NE << 12) | ((uint32_t)A_S0 << 5) | (uint32_t)d);   // csinv
    }
}

static void h_ldc(int size, int r, uint32_t off)
{
    a_mem(true, size, r, A_RC, off);
}

static void h_stc_i(int size, uint32_t off, uint32_t v)
{
    int r = A_ZR;
    if (v) {
        h_mov_i(A_S0, v);
        r = A_S0;
    }
    a_mem(false, size, r, A_RC, off);
}

static void h_stc_ptr(uint32_t off, const void *ptr)
{
    a_mov_xi(A_S0, (uint64_t)(uintptr_t)ptr);
    a_mem(false, 8, A_S0, A_RC, off);
}

static void h_brc_i(int cond, uint32_t off, uint32_t v, int l)
{
    a_mem(true, 4, A_S0, A_RC, off);
    a_cmp_i(A_S0, v);
    a_bcond(a_cc[cond], l);
}

static void h_addc64_i(uint32_t off, uint32_t v)
{
    a_mem(true, 8, A_S0, A_RC, off);
    e32(0x91000000u | (v << 10) | ((uint32_t)A_S0 << 5) | A_S0);
    a_mem(false, 8, A_S0, A_RC, off);
}

static void h_rem_sub(uint32_t n)
{
    if (n)
        h_alu_i(HA_SUB, A_REM, A_REM, n);
}

static void h_rem_br_le(uint32_t n, int l)
{
    a_cmp_i(A_REM, n);
    a_bcond(A_LE, l);
}

static void h_rem_store(uint32_t off)
{
    a_mem(false, 4, A_REM, A_RC, off);
}

static void h_rem_load(void)
{
    a_mem(true, 4, A_REM, A_RC, HO(jrem));
}

static void a_tst_i(int r, uint32_t imm)
{
    e32(0x7200001fu | ((uint32_t)a_logic_imm(imm) << 10) | ((uint32_t)r << 5));
}

// Within the PMP's window: to slow
static void a_guard(int slow)
{
    a_mem(true, 4, A_S0, A_RC, HO(g_lo));
    a_rrr(0x4b000000u, A_S0, H_T0, A_S0);
    a_mem(true, 4, A_S1, A_RC, HO(g_len));
    a_cmp(A_S0, A_S1);
    a_bcond(A_LO, slow);
}

// x10 = table[w9 >> 16], the table being at off in the chip; to slow where the entry is empty
static void a_page(uint32_t off, int slow)
{
    e32(0x53000000u | (16u << 16) | (31u << 10) | ((uint32_t)H_T0 << 5) | H_T1);   // lsr w10, w9, #16
    a_mov_xi(A_IP0, off);
    a_rrr(0x8b000000u, A_IP0, A_RM, A_IP0);
    e32(0xf8607800u | ((uint32_t)H_T1 << 16) | ((uint32_t)A_IP0 << 5) | H_T1);   // ldr x10, [x16, x10, lsl #3]
    fix_add(slow, 1);
    e32(0xb4000000u | H_T1);   // cbz x10
}

// From the guest address in w9 to the host page in x10 and the offset within it in w9; to slow,
// with the address still in w9, where there is no plain memory or the address is not aligned.
// Loads leave flash to the slow path, which keeps its cache.
static void h_page(bool load, unsigned align, bool guard, int slow)
{
    if (guard)
        a_guard(slow);
    a_page(load ? MO(rpage_plain) : MO(wpage), slow);
    if (align) {
        a_tst_i(H_T0, align);
        a_bcond(A_NE, slow);
    }
    e32(0x53003c00u | ((uint32_t)H_T0 << 5) | H_T0);   // uxth
}

// For a run of words on the stack: from the lowest address in w9 to its host address in x10; to
// slow unless all nbytes are aligned, in one page of RAM and outside the PMP's window.
static void h_page_stack(unsigned nbytes, bool guard, int slow)
{
    a_tst_i(H_T0, 3);
    a_bcond(A_NE, slow);
    if (guard) {
        a_guard(slow);
        a_mem(true, 4, A_S0, A_RC, HO(g_lo));
        a_rrr(0x4b000000u, A_S0, A_S0, H_T0);
        a_cmp_i(A_S0, nbytes);
        a_bcond(A_LO, slow);
    }
    a_page(MO(wpage), slow);
    e32(0x53003c00u | ((uint32_t)H_T0 << 5) | A_S0);   // uxth w12, w9
    a_cmp_i(A_S0, 0x10000 - nbytes);
    a_bcond(A_HI, slow);
    a_rrr(0x8b000000u, H_T1, H_T1, A_S0);
}

static void h_ld_at(int d, uint32_t disp)
{
    a_mem(true, 4, d, H_T1, disp);
}

static void h_st_at(int s, uint32_t disp)
{
    a_mem(false, 4, s, H_T1, disp);
}

// [x10 + x9]
static void h_ld_host(int size, bool sign, int d)
{
    uint32_t op = size == 4 ? 0xb8606800u : size == 2 ? (sign ? 0x78e06800u : 0x78606800u) : (sign ? 0x38e06800u : 0x38606800u);
    a_rrr(op, d, H_T1, H_T0);
}

static void h_st_host(int size, int s)
{
    a_rrr(size == 4 ? 0xb8206800u : size == 2 ? 0x78206800u : 0x38206800u, s, H_T1, H_T0);
}

static void a_count_hit(chip_t *m)
{
    (void)m;
    a_mem(true, 4, A_S0, A_RM, MO(xip_ctr_acc));
    e32(0x11000400u | ((uint32_t)A_S0 << 5) | A_S0);
    a_mem(false, 4, A_S0, A_RM, MO(xip_ctr_acc));
    a_mem(true, 4, A_S0, A_RM, MO(xip_ctr_hit));
    e32(0x11000400u | ((uint32_t)A_S0 << 5) | A_S0);
    a_mem(false, 4, A_S0, A_RM, MO(xip_ctr_hit));
}

// xip_access for a fetch: a hit in either way of the set, with the bookkeeping that keeps; anything
// else goes to miss.
static void h_xip_hit(chip_t *m, uint32_t addr, int miss)
{
    if ((addr >> 24) >= m->xip_cached_top) {
        h_jmp(miss);
        return;
    }
    uint32_t line = (addr & 0x01ffffffu) >> 3, set = line & 1023, tag = (line >> 10) | 0x80000000u;
    int count = lab_new(), way1 = lab_new(), mru = lab_new();
    a_mem(true, 4, A_S0, A_RM, MO(xip_ctrl));
    a_tst_i(A_S0, 1);
    a_bcond(A_EQ, miss);
    h_mov_i(A_S1, tag);
    a_mem(true, 4, A_S0, A_RM, MO(xip_mru) + 4 * set);
    a_cmp(A_S0, A_S1);
    a_bcond(A_EQ, count);
    a_mem(true, 4, A_S0, A_RM, MO(xip_tag) + 8 * set);
    a_cmp(A_S0, A_S1);
    a_bcond(A_NE, way1);
    h_mov_i(A_S0, 1);
    a_mem(false, 1, A_S0, A_RM, MO(xip_lru) + set);
    h_jmp(mru);
    h_bind(way1);
    a_mem(true, 4, A_S0, A_RM, MO(xip_tag) + 8 * set + 4);
    a_cmp(A_S0, A_S1);
    a_bcond(A_NE, miss);
    a_mem(false, 1, A_ZR, A_RM, MO(xip_lru) + set);
    h_bind(mru);
    a_mem(false, 4, A_S1, A_RM, MO(xip_mru) + 4 * set);
    h_bind(count);
    a_count_hit(m);
}

// A load from flash, the address still in w9: when its cache line is the one its set used last
// (xip_cost's quick case), the page ends up in x10 and the offset in w9 as after h_page.
static void h_flash_page(chip_t *m, unsigned align, bool guard, int slow)
{
    e32(0x53000000u | (24u << 16) | (31u << 10) | ((uint32_t)H_T0 << 5) | A_S0);   // lsr w12, w9, #24
    h_alu_i(HA_SUB, A_S0, A_S0, 0x10);
    a_cmp_i(A_S0, m->xip_cached_top - 0x10u);
    a_bcond(A_HS, slow);
    if (guard)
        a_guard(slow);
    if (align) {
        a_tst_i(H_T0, align);
        a_bcond(A_NE, slow);
    }
    a_mem(true, 4, A_S0, A_RM, MO(xip_ctrl));
    a_tst_i(A_S0, 1);
    a_bcond(A_EQ, slow);
    a_page(MO(rpage), slow);
    // the set's tag used last against this address's
    e32(0x53000000u | (3u << 16) | (12u << 10) | ((uint32_t)H_T0 << 5) | A_S0);   // ubfx w12, w9, #3, #10
    a_mov_xi(A_IP0, MO(xip_mru));
    a_rrr(0x8b000000u, A_IP0, A_RM, A_IP0);
    e32(0xb8607800u | ((uint32_t)A_S0 << 16) | ((uint32_t)A_IP0 << 5) | A_S0);   // ldr w12, [x16, x12, lsl #2]
    e32(0x53000000u | (13u << 16) | (24u << 10) | ((uint32_t)H_T0 << 5) | A_S1);   // ubfx w13, w9, #13, #12
    e32(0x32000000u | (1u << 16) | ((uint32_t)A_S1 << 5) | A_S1);   // orr w13, w13, #0x80000000
    a_cmp(A_S0, A_S1);
    a_bcond(A_NE, slow);
    a_count_hit(m);
    e32(0x53003c00u | ((uint32_t)H_T0 << 5) | H_T0);   // uxth
}

static void h_spill_all(void)
{
    for (unsigned i = 0; i < sizeof(h_pinned); i++)
        h_stx(h_pinned[i], h_greg[h_pinned[i]]);
}

static void h_reload_all(void)
{
    for (unsigned i = 0; i < sizeof(h_pinned); i++)
        h_ldx(h_greg[h_pinned[i]], h_pinned[i]);
}

// fn(core, ptr, arg), with the result in w9
static void h_call(void *fn, const void *ptr, uint32_t arg)
{
    a_mov_x(0, A_RC);
    a_mov_xi(1, (uint64_t)(uintptr_t)ptr);
    h_mov_i(2, arg);
    a_mov_xi(A_IP0, (uint64_t)(uintptr_t)fn);
    e32(0xd63f0000u | ((uint32_t)A_IP0 << 5));
    h_mov(H_T0, 0);
}

// The same for a function that leaves the guest registers alone: only the two the calling
// convention lets it use are kept, on the stack.
static void h_call_light(void *fn, const void *ptr, uint32_t arg)
{
    e32(0xa9bf3feeu);   // stp x14, x15, [sp, #-16]!
    h_call(fn, ptr, arg);
    e32(0xa8c13feeu);   // ldp x14, x15, [sp], #16
}

// Compares the len bytes at host with what they are now.
static void h_verify(const uint8_t *host, unsigned len, int stale)
{
    a_mov_xi(A_IP1, (uint64_t)(uintptr_t)host);
    unsigned o = 0;
    while (len - o >= 8) {
        uint64_t v;
        memcpy(&v, host + o, 8);
        a_mem(true, 8, A_S0, A_IP1, o);
        a_mov_xi(A_S1, v);
        a_rrr(0xeb00001fu, 0, A_S0, A_S1);
        a_bcond(A_NE, stale);
        o += 8;
    }
    if (len - o >= 4) {
        uint32_t v;
        memcpy(&v, host + o, 4);
        a_mem(true, 4, A_S0, A_IP1, o);
        a_cmp_i(A_S0, v);
        a_bcond(A_NE, stale);
        o += 4;
    }
    if (len - o >= 2) {
        a_mem(true, 2, A_S0, A_IP1, o);
        a_cmp_i(A_S0, (uint32_t)host[o] | ((uint32_t)host[o + 1] << 8));
        a_bcond(A_NE, stale);
        o += 2;
    }
}

static void h_code_done(uint8_t *from, uint8_t *to)
{
    __builtin___clear_cache((char *)from, (char *)to);
}

// The way in and out, and the lookup of a block from its address.
static void h_fixed(void)
{
    static const int8_t host[] = {22, 23, 24, 25, 26, 27, 28, 14, 15};
    memset(h_greg, -1, sizeof(h_greg));
    for (unsigned i = 0; i < sizeof(h_pinned); i++)
        h_greg[h_pinned[i]] = host[i];

    RJ.enter = (int (*)(cpu_t *, void *))(void *)RJ.p;
    e32(0xa9ba7bfdu);   // stp x29, x30, [sp, #-96]!
    e32(0xa90153f3u);   // stp x19, x20, [sp, #16]
    e32(0xa9025bf5u);   // stp x21, x22, [sp, #32]
    e32(0xa90363f7u);   // stp x23, x24, [sp, #48]
    e32(0xa9046bf9u);   // stp x25, x26, [sp, #64]
    e32(0xa90573fbu);   // stp x27, x28, [sp, #80]
    a_mov_x(A_RC, 0);
    a_mov_x(A_IP1, 1);
    a_mem(true, 8, A_RM, A_RC, CO(m));
    h_rem_load();
    h_reload_all();
    e32(0xd61f0000u | ((uint32_t)A_IP1 << 5));

    uint8_t *epi = RJ.p;
    e32(0xa94153f3u);   // ldp x19, x20, [sp, #16]
    e32(0xa9425bf5u);
    e32(0xa94363f7u);
    e32(0xa9446bf9u);
    e32(0xa94573fbu);
    e32(0xa8c67bfdu);   // ldp x29, x30, [sp], #96
    e32(0xd65f03c0u);   // ret
    for (int i = 0; i < RJX_COUNT; i++) {
        RJ.x_exit[i] = RJ.p;
        h_spill_all();
        h_rem_store(HO(jrem));
        h_mov_i(0, (uint32_t)i);
        h_jmp_abs(epi);
    }
    // After rvj_step or rvj_xip left the block: the core holds everything.
    RJ.x_asis = RJ.p;
    h_mov_i(0, RJX_DONE);
    h_jmp_abs(epi);
    RJ.x_spill_asis = RJ.p;
    h_spill_all();
    h_jmp_abs(RJ.x_asis);

    // w9: the address to go to; the hash is rvj_hash
    RJ.x_lookup = RJ.p;
    _Static_assert(RVJ_LOOKUP == 65536 && sizeof(rvj_slot_t) == 16, "the hash below");
    e32(0x53000000u | (1u << 16) | (15u << 10) | ((uint32_t)H_T0 << 5) | A_S0);   // ubfx w12, w9, #1, #16
    a_mov_xi(A_IP0, (uint64_t)(uintptr_t)rvj_lookup);
    e32(0x8b000000u | ((uint32_t)A_S0 << 16) | (4u << 10) | ((uint32_t)A_IP0 << 5) | A_IP0);   // add x16, x16, x12, lsl #4
    a_mem(true, 4, A_S0, A_IP0, 0);
    e32(0x32000000u | ((uint32_t)H_T0 << 5) | A_S1);   // orr w13, w9, #1
    a_cmp(A_S0, A_S1);
    e32(0x54000000u | (3u << 5) | A_NE);   // b.ne miss
    a_mem(true, 8, A_IP0, A_IP0, 8);
    e32(0xd61f0000u | ((uint32_t)A_IP0 << 5));
    a_mem(false, 4, H_T0, A_RC, CO(r) + 60);
    h_jmp_abs(RJ.x_exit[RJX_LOOKUP]);
}
