// PBConPC: the x86-64 instructions rvjit.inc is written in terms of.
//
// rbx holds the core, r15 the chip and ebp the cycles left; rax, rcx and rdx are scratch. Nine
// guest registers live in host registers and are written back around every call into C.

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum { H_T0 = RAX, H_T1 = RCX, H_T2 = RDX };

#ifdef _WIN32
#define X_ARG0 RCX
#define X_ARG1 RDX
#define X_ARG2 R8
#else
#define X_ARG0 RDI
#define X_ARG1 RSI
#define X_ARG2 RDX
#endif

// a0 to a5, sp, s0 and s1
static int8_t h_greg[33];
static const uint8_t h_pinned[] = {2, 8, 9, 10, 11, 12, 13, 14, 15};

static inline void b1(unsigned v)
{
    if (RJ.p < RJ.end)
        *RJ.p++ = (uint8_t)v;
    else
        RJ.overflow = true;
}

static void b4(uint32_t v)
{
    for (int i = 0; i < 4; i++)
        b1(v >> (8 * i));
}

static void b8(uint64_t v)
{
    b4((uint32_t)v);
    b4((uint32_t)(v >> 32));
}

static void x_rex(int w, int reg, int idx, int base, bool force)
{
    unsigned r = 0x40u | (unsigned)(w << 3) | (unsigned)(((reg >> 3) & 1) << 2) | (unsigned)(((idx >> 3) & 1) << 1) |
                 (unsigned)((base >> 3) & 1);
    if (r != 0x40 || force)
        b1(r);
}

// ModRM, SIB and displacement for [base + disp]
static void x_mem(int reg, int base, int32_t disp)
{
    unsigned b = (unsigned)base & 7, r = ((unsigned)reg & 7) << 3;
    if (disp == 0 && b != 5) {
        b1(r | b);
        if (b == 4)
            b1(0x24);
    } else if (disp >= -128 && disp <= 127) {
        b1(0x40 | r | b);
        if (b == 4)
            b1(0x24);
        b1((unsigned)disp);
    } else {
        b1(0x80 | r | b);
        if (b == 4)
            b1(0x24);
        b4((uint32_t)disp);
    }
}

static void x_rr(int w, unsigned op, int reg, int rm)
{
    x_rex(w, reg, 0, rm, false);
    b1(op);
    b1(0xc0 | (((unsigned)reg & 7) << 3) | ((unsigned)rm & 7));
}

static void x_rr0f(unsigned op, int reg, int rm, bool force)
{
    x_rex(0, reg, 0, rm, force);
    b1(0x0f);
    b1(op);
    b1(0xc0 | (((unsigned)reg & 7) << 3) | ((unsigned)rm & 7));
}

static void x_rm(int w, unsigned op, int reg, int base, int32_t disp)
{
    x_rex(w, reg, 0, base, false);
    b1(op);
    x_mem(reg, base, disp);
}

// op r32, imm with the group-1 extension ext
static void x_ri(unsigned ext, int r, uint32_t imm)
{
    x_rex(0, 0, 0, r, false);
    if ((int32_t)imm >= -128 && (int32_t)imm <= 127) {
        b1(0x83);
        b1(0xc0 | (ext << 3) | ((unsigned)r & 7));
        b1(imm);
    } else {
        b1(0x81);
        b1(0xc0 | (ext << 3) | ((unsigned)r & 7));
        b4(imm);
    }
}

static void h_mov_i(int r, uint32_t v)
{
    if (v == 0) {
        x_rr(0, 0x31, r, r);
        return;
    }
    x_rex(0, 0, 0, r, false);
    b1(0xb8 + ((unsigned)r & 7));
    b4(v);
}

static void x_mov_i64(int r, uint64_t v)
{
    x_rex(1, 0, 0, r, false);
    b1(0xb8 + ((unsigned)r & 7));
    b8(v);
}

static void h_mov(int d, int s)
{
    if (d != s)
        x_rr(0, 0x89, s, d);
}

static void h_ldx(int r, unsigned g)
{
    x_rm(0, 0x8b, r, RBX, (int32_t)XOFF(g));
}

static void h_stx(unsigned g, int r)
{
    x_rm(0, 0x89, r, RBX, (int32_t)XOFF(g));
}

static void h_alu(int op, int d, int a, int b)
{
    static const uint8_t opc[] = {[HA_ADD] = 0x01, [HA_SUB] = 0x29, [HA_AND] = 0x21, [HA_OR] = 0x09, [HA_XOR] = 0x31};
    if (d == a) {
        x_rr(0, opc[op], b, d);
    } else if (d == b) {
        if (op == HA_SUB) {
            x_rex(0, 0, 0, d, false);   // neg d
            b1(0xf7);
            b1(0xd8 | ((unsigned)d & 7));
            x_rr(0, 0x01, a, d);
        } else {
            x_rr(0, opc[op], a, d);
        }
    } else {
        h_mov(d, a);
        x_rr(0, opc[op], b, d);
    }
}

static void h_alu_i(int op, int d, int a, uint32_t imm)
{
    static const uint8_t ext[] = {[HA_ADD] = 0, [HA_SUB] = 5, [HA_AND] = 4, [HA_OR] = 1, [HA_XOR] = 6};
    h_mov(d, a);
    if (imm == 0 && op != HA_AND)
        return;
    x_ri(ext[op], d, imm);
}

static void h_shift_i(int op, int d, int a, unsigned n)
{
    static const uint8_t ext[] = {[HS_SLL] = 4, [HS_SRL] = 5, [HS_SRA] = 7};
    h_mov(d, a);
    if (!n)
        return;
    x_rex(0, 0, 0, d, false);
    b1(0xc1);
    b1(0xc0 | (unsigned)(ext[op] << 3) | ((unsigned)d & 7));
    b1(n);
}

// The count goes through cl; neither d nor a is ever rcx.
static void h_shift(int op, int d, int a, int b)
{
    static const uint8_t ext[] = {[HS_SLL] = 4, [HS_SRL] = 5, [HS_SRA] = 7};
    h_mov(RCX, b);
    h_mov(d, a);
    x_rex(0, 0, 0, d, false);
    b1(0xd3);
    b1(0xc0 | (unsigned)(ext[op] << 3) | ((unsigned)d & 7));
}

static void h_mul(int d, int a, int b)
{
    if (d == a) {
        x_rr0f(0xaf, d, b, false);
    } else if (d == b) {
        x_rr0f(0xaf, d, a, false);
    } else {
        h_mov(d, a);
        x_rr0f(0xaf, d, b, false);
    }
}

// d = b + (a << n): lea d, [b + a * 2^n]
static void h_shadd(int d, int a, int b, unsigned n)
{
    x_rex(0, d, a, b, false);
    b1(0x8d);
    bool disp = ((unsigned)b & 7) == 5;
    b1((disp ? 0x40u : 0u) | (((unsigned)d & 7) << 3) | 4);
    b1((n << 6) | (((unsigned)a & 7) << 3) | ((unsigned)b & 7));
    if (disp)
        b1(0);
}

static void h_ext(int kind, int d, int s)
{
    if (kind == HE_SB)
        x_rr0f(0xbe, d, s, s >= 4);
    else
        x_rr0f(kind == HE_SH ? 0xbf : 0xb7, d, s, false);
}

static void h_not(int d, int s)
{
    h_mov(d, s);
    x_rex(0, 0, 0, d, false);
    b1(0xf7);
    b1(0xd0 | ((unsigned)d & 7));
}

static void h_rot(bool left, int d, int a, int b)
{
    h_mov(RCX, b);
    h_mov(d, a);
    x_rex(0, 0, 0, d, false);
    b1(0xd3);
    b1((left ? 0xc0u : 0xc8u) | ((unsigned)d & 7));
}

static void h_rot_i(int d, int a, unsigned n)
{
    h_mov(d, a);
    if (!n)
        return;
    x_rex(0, 0, 0, d, false);
    b1(0xc1);
    b1(0xc8 | ((unsigned)d & 7));
    b1(n);
}

// The upper half of the product: 0 signed, 1 signed by unsigned, 2 unsigned. Through rax and rcx
// as 64-bit values; a is never rcx and b never rax.
static void h_mulh(int kind, int d, int a, int b)
{
    if (kind == 0)
        x_rr(1, 0x63, RCX, b);   // movsxd rcx, b
    else
        h_mov(RCX, b);
    if (kind == 2)
        h_mov(RAX, a);
    else
        x_rr(1, 0x63, RAX, a);
    b1(0x48);   // imul rax, rcx
    b1(0x0f);
    b1(0xaf);
    b1(0xc1);
    b1(0x48);   // shr rax, 32
    b1(0xc1);
    b1(0xe8);
    b1(32);
    h_mov(d, RAX);
}

// 0 min, 1 minu, 2 max, 3 maxu
static void h_minmax(int kind, int d, int a, int b)
{
    static const uint8_t cmov[] = {0x4f, 0x47, 0x4c, 0x42};
    h_mov(RDX, a);
    x_rr(0, 0x39, b, RDX);
    x_rr0f(cmov[kind], RDX, b, false);
    h_mov(d, RDX);
}

static void h_clz(int d, int a)
{
    x_rr0f(0xbd, RDX, a, false);   // bsr edx, a
    h_mov_i(RCX, 63);
    x_rr0f(0x44, RDX, RCX, false);   // cmovz edx, ecx
    x_ri(6, RDX, 31);
    h_mov(d, RDX);
}

static void h_ctz(int d, int a)
{
    x_rr0f(0xbc, RDX, a, false);   // bsf edx, a
    h_mov_i(RCX, 32);
    x_rr0f(0x44, RDX, RCX, false);
    h_mov(d, RDX);
}

static void h_rev8(int d, int a)
{
    h_mov(d, a);
    x_rex(0, 0, 0, d, false);
    b1(0x0f);
    b1(0xc8 | ((unsigned)d & 7));
}

static void x_cmp_i(int a, uint32_t imm)
{
    x_ri(7, a, imm);
}

static void x_set(int cond, int d)
{
    b1(0x0f);
    b1(cond == HC_LT ? 0x9c : 0x92);
    b1(0xc2);   // dl
    x_rr0f(0xb6, d, RDX, false);
}

static void h_setcmp(int cond, int d, int a, int b)
{
    x_rr(0, 0x39, b, a);
    x_set(cond, d);
}

static void h_setcmp_i(int cond, int d, int a, int32_t imm)
{
    x_cmp_i(a, (uint32_t)imm);
    x_set(cond, d);
}

static void x_jcc(unsigned cc, int l)
{
    b1(0x0f);
    b1(0x80 | cc);
    fix_add(l, 0);
    b4(0);
}

static void x_jcc_abs(unsigned cc, const uint8_t *target)
{
    b1(0x0f);
    b1(0x80 | cc);
    b4((uint32_t)(target - (RJ.p + 4)));
}

static const uint8_t x_cc[] = {[HC_EQ] = 4, [HC_NE] = 5, [HC_LT] = 0xc, [HC_GE] = 0xd, [HC_LTU] = 2, [HC_GEU] = 3};

static void h_br(int cond, int a, int b, int l)
{
    x_rr(0, 0x39, b, a);
    x_jcc(x_cc[cond], l);
}

static void h_br_i(int cond, int a, uint32_t imm, int l)
{
    if (imm == 0 && (cond == HC_EQ || cond == HC_NE))
        x_rr(0, 0x85, a, a);
    else
        x_cmp_i(a, imm);
    x_jcc(x_cc[cond], l);
}

static void h_br_nz(int r, int l)
{
    x_rr(0, 0x85, r, r);
    x_jcc(5, l);
}

static void h_br_nz_abs(int r, const uint8_t *target)
{
    x_rr(0, 0x85, r, r);
    x_jcc_abs(5, target);
}

// Unsigned division as RISC-V defines it: by zero gives all ones, and the dividend as remainder.
static void x_jcc(unsigned cc, int l);
static void h_jmp(int l);
static void h_divu(bool rem, int d, int a, int b)
{
    int zero = lab_new(), done = lab_new();
    h_mov(RCX, b);
    h_mov(RAX, a);
    x_rr(0, 0x85, RCX, RCX);
    x_jcc(4, zero);
    x_rr(0, 0x31, RDX, RDX);
    b1(0xf7);   // div ecx
    b1(0xf1);
    h_jmp(done);
    h_bind(zero);
    if (rem)
        h_mov(RDX, RAX);
    else
        h_mov_i(RAX, 0xffffffffu);
    h_bind(done);
    h_mov(d, rem ? RDX : RAX);
}

static void h_jmp(int l)
{
    b1(0xe9);
    fix_add(l, 0);
    b4(0);
}

static void h_jmp_abs(const uint8_t *target)
{
    b1(0xe9);
    b4((uint32_t)(target - (RJ.p + 4)));
}

static void h_resolve(uint8_t *at, const uint8_t *target, int kind)
{
    (void)kind;
    uint32_t rel = (uint32_t)(target - (at + 4));
    memcpy(at, &rel, 4);
}

// A jump the dispatcher redirects once its target is translated; the address of its displacement
// identifies it.
static uint8_t *h_link(int l)
{
    b1(0xe9);
    uint8_t *site = RJ.p;
    fix_add(l, 0);
    b4(0);
    return site;
}

static void h_patch(void *site, const void *dest)
{
    h_resolve(site, dest, 0);
}

static void h_ldc(int size, int r, uint32_t off)
{
    if (size == 4) {
        x_rm(0, 0x8b, r, RBX, (int32_t)off);
    } else {
        x_rex(0, r, 0, RBX, false);
        b1(0x0f);
        b1(size == 2 ? 0xb7 : 0xb6);
        x_mem(r, RBX, (int32_t)off);
    }
}

static void h_stc_i(int size, uint32_t off, uint32_t v)
{
    if (size == 2)
        b1(0x66);
    b1(0xc7);
    x_mem(0, RBX, (int32_t)off);
    if (size == 2) {
        b1(v);
        b1(v >> 8);
    } else {
        b4(v);
    }
}

static void h_stc_ptr(uint32_t off, const void *ptr)
{
    x_mov_i64(RAX, (uint64_t)(uintptr_t)ptr);
    x_rm(1, 0x89, RAX, RBX, (int32_t)off);
}

static void h_brc_i(int cond, uint32_t off, uint32_t v, int l)
{
    b1(0x81);
    x_mem(7, RBX, (int32_t)off);
    b4(v);
    x_jcc(x_cc[cond], l);
}

static void h_addc64_i(uint32_t off, uint32_t v)
{
    b1(0x48);
    b1(0x83);
    x_mem(0, RBX, (int32_t)off);
    b1(v);
}

static void h_rem_sub(uint32_t n)
{
    if (n)
        x_ri(5, RBP, n);
}

static void h_rem_br_le(uint32_t n, int l)
{
    x_cmp_i(RBP, n);
    x_jcc(0xe, l);
}

static void h_rem_store(uint32_t off)
{
    x_rm(0, 0x89, RBP, RBX, (int32_t)off);
}

static void h_rem_load(void)
{
    x_rm(0, 0x8b, RBP, RBX, (int32_t)HO(jrem));
}

// From the guest address in eax to the host page in rcx and the offset within it in eax; to slow
// where there is no plain memory or the address is not aligned. Loads leave flash to the slow
// path, which keeps its cache.
static void h_page(bool load, unsigned align, bool guard, int slow)
{
    x_rr(0, 0x89, RAX, RCX);
    if (guard) {
        // within the PMP's window: sub ecx, g_lo; cmp ecx, g_len; jb
        x_rm(0, 0x2b, RCX, RBX, (int32_t)HO(g_lo));
        x_rm(0, 0x3b, RCX, RBX, (int32_t)HO(g_len));
        x_jcc(2, slow);
        x_rr(0, 0x89, RAX, RCX);
    }
    b1(0xc1);   // shr ecx, 16
    b1(0xe9);
    b1(16);
    b1(0x49);   // mov rcx, [r15 + rcx * 8 + table]
    b1(0x8b);
    b1(0x8c);
    b1(0xcf);
    b4(load ? MO(rpage_plain) : MO(wpage));
    x_rr(1, 0x85, RCX, RCX);
    x_jcc(4, slow);
    if (align) {
        b1(0xa8);   // test al, align
        b1(align);
        x_jcc(5, slow);
    }
    b1(0x0f);   // movzx eax, ax
    b1(0xb7);
    b1(0xc0);
}

// For a run of words on the stack: from the lowest address in eax to its host address in rcx; to
// slow unless all nbytes are aligned, in one page of RAM and outside the PMP's window.
static void h_page_stack(unsigned nbytes, bool guard, int slow)
{
    b1(0xa8);   // test al, 3
    b1(3);
    x_jcc(5, slow);
    if (guard) {
        x_rr(0, 0x89, RAX, RCX);
        x_rm(0, 0x2b, RCX, RBX, (int32_t)HO(g_lo));
        x_rm(0, 0x3b, RCX, RBX, (int32_t)HO(g_len));
        x_jcc(2, slow);
        x_rm(0, 0x8b, RCX, RBX, (int32_t)HO(g_lo));
        x_rr(0, 0x29, RAX, RCX);
        x_ri(7, RCX, nbytes);
        x_jcc(2, slow);
    }
    x_rr(0, 0x89, RAX, RCX);
    b1(0xc1);   // shr ecx, 16
    b1(0xe9);
    b1(16);
    b1(0x49);   // mov rcx, [r15 + rcx * 8 + wpage]
    b1(0x8b);
    b1(0x8c);
    b1(0xcf);
    b4(MO(wpage));
    x_rr(1, 0x85, RCX, RCX);
    x_jcc(4, slow);
    b1(0x0f);   // movzx eax, ax
    b1(0xb7);
    b1(0xc0);
    x_ri(7, RAX, 0x10000 - nbytes);
    x_jcc(7, slow);
    x_rr(1, 0x01, RAX, RCX);
}

static void h_ld_at(int d, uint32_t disp)
{
    x_rm(0, 0x8b, d, RCX, (int32_t)disp);
}

static void h_st_at(int s, uint32_t disp)
{
    x_rm(0, 0x89, s, RCX, (int32_t)disp);
}

// [rcx + rax]
static void h_ld_host(int size, bool sign, int d)
{
    x_rex(0, d, RAX, RCX, false);
    if (size == 4) {
        b1(0x8b);
    } else {
        b1(0x0f);
        b1(size == 2 ? (sign ? 0xbf : 0xb7) : (sign ? 0xbe : 0xb6));
    }
    b1((((unsigned)d & 7) << 3) | 4);
    b1(0x01);
}

static void h_st_host(int size, int s)
{
    if (size == 2)
        b1(0x66);
    x_rex(0, s, RAX, RCX, size == 1 && s >= 4);
    b1(size == 1 ? 0x88 : 0x89);
    b1((((unsigned)s & 7) << 3) | 4);
    b1(0x01);
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
    b1(0x41);   // test byte [r15 + xip_ctrl], 1
    b1(0xf6);
    x_mem(0, R15, (int32_t)MO(xip_ctrl));
    b1(1);
    x_jcc(4, miss);
    b1(0x41);   // cmp dword [r15 + xip_mru[set]], tag
    b1(0x81);
    x_mem(7, R15, (int32_t)(MO(xip_mru) + 4 * set));
    b4(tag);
    x_jcc(4, count);
    b1(0x41);   // cmp dword [r15 + xip_tag[set][0]], tag
    b1(0x81);
    x_mem(7, R15, (int32_t)(MO(xip_tag) + 8 * set));
    b4(tag);
    x_jcc(5, way1);
    b1(0x41);   // mov byte [r15 + xip_lru[set]], 1
    b1(0xc6);
    x_mem(0, R15, (int32_t)(MO(xip_lru) + set));
    b1(1);
    h_jmp(mru);
    h_bind(way1);
    b1(0x41);
    b1(0x81);
    x_mem(7, R15, (int32_t)(MO(xip_tag) + 8 * set + 4));
    b4(tag);
    x_jcc(5, miss);
    b1(0x41);
    b1(0xc6);
    x_mem(0, R15, (int32_t)(MO(xip_lru) + set));
    b1(0);
    h_bind(mru);
    b1(0x41);   // mov dword [r15 + xip_mru[set]], tag
    b1(0xc7);
    x_mem(0, R15, (int32_t)(MO(xip_mru) + 4 * set));
    b4(tag);
    h_bind(count);
    b1(0x41);   // inc dword [r15 + xip_ctr_acc]
    b1(0xff);
    x_mem(0, R15, (int32_t)MO(xip_ctr_acc));
    b1(0x41);
    b1(0xff);
    x_mem(0, R15, (int32_t)MO(xip_ctr_hit));
}

// A load from flash, the address still in eax: when its cache line is the one its set used last
// (xip_cost's quick case), the page ends up in rcx and the offset in eax as after h_page.
static void h_flash_page(chip_t *m, unsigned align, bool guard, int slow)
{
    x_rr(0, 0x89, RAX, RCX);
    b1(0xc1);   // shr ecx, 24
    b1(0xe9);
    b1(24);
    x_ri(5, RCX, 0x10);
    x_ri(7, RCX, m->xip_cached_top - 0x10u);
    x_jcc(3, slow);
    if (guard) {
        x_rr(0, 0x89, RAX, RCX);
        x_rm(0, 0x2b, RCX, RBX, (int32_t)HO(g_lo));
        x_rm(0, 0x3b, RCX, RBX, (int32_t)HO(g_len));
        x_jcc(2, slow);
    }
    if (align) {
        b1(0xa8);
        b1(align);
        x_jcc(5, slow);
    }
    b1(0x41);   // test byte [r15 + xip_ctrl], 1
    b1(0xf6);
    x_mem(0, R15, (int32_t)MO(xip_ctrl));
    b1(1);
    x_jcc(4, slow);
    x_rr(0, 0x89, RAX, RCX);
    b1(0xc1);
    b1(0xe9);
    b1(16);
    b1(0x49);   // mov rcx, [r15 + rcx * 8 + rpage]
    b1(0x8b);
    b1(0x8c);
    b1(0xcf);
    b4(MO(rpage));
    x_rr(1, 0x85, RCX, RCX);
    x_jcc(4, slow);
    x_rr(0, 0x89, RAX, RDX);
    b1(0xc1);   // shr edx, 3
    b1(0xea);
    b1(3);
    x_rex(0, 0, 0, RDX, false);   // and edx, 1023
    b1(0x81);
    b1(0xe2);
    b4(1023);
    b1(0x41);   // mov edx, [r15 + rdx * 4 + xip_mru]
    b1(0x8b);
    b1(0x94);
    b1(0x97);
    b4(MO(xip_mru));
    b1(0x50);   // push rax
    b1(0x25);   // and eax, 0x01ffffff
    b4(0x01ffffffu);
    b1(0xc1);   // shr eax, 13
    b1(0xe8);
    b1(13);
    b1(0x0d);   // or eax, 0x80000000
    b4(0x80000000u);
    x_rr(0, 0x39, RDX, RAX);
    b1(0x58);   // pop rax
    x_jcc(5, slow);
    b1(0x41);
    b1(0xff);
    x_mem(0, R15, (int32_t)MO(xip_ctr_acc));
    b1(0x41);
    b1(0xff);
    x_mem(0, R15, (int32_t)MO(xip_ctr_hit));
    b1(0x0f);   // movzx eax, ax
    b1(0xb7);
    b1(0xc0);
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

// fn(core, ptr, arg), with the result in eax
static void h_call(void *fn, const void *ptr, uint32_t arg)
{
    x_rr(1, 0x89, RBX, X_ARG0);
    x_mov_i64(X_ARG1, (uint64_t)(uintptr_t)ptr);
    h_mov_i(X_ARG2, arg);
    x_mov_i64(RAX, (uint64_t)(uintptr_t)fn);
    b1(0xff);
    b1(0xd0);
}

// The same for a function that leaves the guest registers alone: only those the calling
// convention lets it use are kept, on the stack.
static void h_call_light(void *fn, const void *ptr, uint32_t arg)
{
    static const uint8_t push[] = {0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53};
    static const uint8_t pop[] = {0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e};
    for (unsigned i = 0; i < sizeof(push); i++)
        b1(push[i]);
    h_call(fn, ptr, arg);
    for (unsigned i = 0; i < sizeof(pop); i++)
        b1(pop[i]);
}

// Compares the len bytes at host with what they are now.
static void h_verify(const uint8_t *host, unsigned len, int stale)
{
    x_mov_i64(RAX, (uint64_t)(uintptr_t)host);
    unsigned o = 0;
    while (len - o >= 8) {
        uint64_t v;
        memcpy(&v, host + o, 8);
        x_mov_i64(RCX, v);
        x_rm(1, 0x39, RCX, RAX, (int32_t)o);
        x_jcc(5, stale);
        o += 8;
    }
    if (len - o >= 4) {
        uint32_t v;
        memcpy(&v, host + o, 4);
        b1(0x81);
        x_mem(7, RAX, (int32_t)o);
        b4(v);
        x_jcc(5, stale);
        o += 4;
    }
    if (len - o >= 2) {
        b1(0x66);
        b1(0x81);
        x_mem(7, RAX, (int32_t)o);
        b1(host[o]);
        b1(host[o + 1]);
        x_jcc(5, stale);
        o += 2;
    }
}

static void h_code_done(uint8_t *from, uint8_t *to)
{
    (void)from;
    (void)to;
}

// The way in and out, and the lookup of a block from its address.
static void h_fixed(void)
{
    static const int8_t host[] = {R14, RSI, RDI, R8, R9, R10, R11, R12, R13};
    memset(h_greg, -1, sizeof(h_greg));
    for (unsigned i = 0; i < sizeof(h_pinned); i++)
        h_greg[h_pinned[i]] = host[i];
    static const uint8_t push[] = {0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x28};
    static const uint8_t pop[] = {0x48, 0x83, 0xc4, 0x28, 0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5f, 0x5e, 0x5d, 0x5b, 0xc3};
    RJ.enter = (int (*)(cpu_t *, void *))(void *)RJ.p;
    for (unsigned i = 0; i < sizeof(push); i++)
        b1(push[i]);
    x_rr(1, 0x89, X_ARG1, RAX);
    x_rr(1, 0x89, X_ARG0, RBX);
    x_rm(1, 0x8b, R15, RBX, (int32_t)CO(m));
    h_rem_load();
    h_reload_all();
    b1(0xff);   // jmp rax
    b1(0xe0);

    uint8_t *epi = RJ.p;
    for (unsigned i = 0; i < sizeof(pop); i++)
        b1(pop[i]);
    for (int i = 0; i < RJX_COUNT; i++) {
        RJ.x_exit[i] = RJ.p;
        h_spill_all();
        h_rem_store(HO(jrem));
        b1(0xb8);
        b4((uint32_t)i);
        h_jmp_abs(epi);
    }
    // After rvj_step or rvj_xip left the block: the core holds everything.
    RJ.x_asis = RJ.p;
    b1(0xb8);
    b4(RJX_DONE);
    h_jmp_abs(epi);

    RJ.x_spill_asis = RJ.p;
    h_spill_all();
    h_jmp_abs(RJ.x_asis);

    // eax: the address to go to; the hash is rvj_hash
    RJ.x_lookup = RJ.p;
    static const uint8_t hash[] = {0x89, 0xc1, 0xd1, 0xe9, 0x0f, 0xb7, 0xc9, 0xc1, 0xe1, 0x04};
    _Static_assert(RVJ_LOOKUP == 65536 && sizeof(rvj_slot_t) == 16, "the hash above");
    for (unsigned i = 0; i < sizeof(hash); i++)
        b1(hash[i]);
    x_mov_i64(RDX, (uint64_t)(uintptr_t)rvj_lookup);
    static const uint8_t find[] = {0x48, 0x01, 0xca, 0x89, 0xc1, 0x83, 0xc9, 0x01, 0x39, 0x0a, 0x75, 0x03, 0xff, 0x62, 0x08};
    for (unsigned i = 0; i < sizeof(find); i++)
        b1(find[i]);
    x_rm(0, 0x89, RAX, RBX, (int32_t)(CO(r) + 60));
    h_jmp_abs(RJ.x_exit[RJX_LOOKUP]);
}
