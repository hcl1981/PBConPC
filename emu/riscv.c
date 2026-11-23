#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chip.h"

// PBConPC: Hazard3 as the RP2350 has it, for the cores OTP's ARCHSEL hands to RISC-V: RV32IMAC with
// Zicsr, Zifencei, Zba, Zbb, Zbs, Zbkb, Zcb and Zcmp, M and U modes, the PMP, and the custom
// Xh3irq, Xh3pmpm, Xh3power and Xh3bextm. Raspberry Pi's boot ROM brings the core up, as on the
// chip. The program counter is c->r[15]; everything else is in c->h3.
//
// Instructions are decoded once: a table holds every 16-bit encoding, and a hashed one the 32-bit
// encodings seen so far. Both are keyed by the instruction bits alone, so code that changes in RAM
// needs no care.

extern uint64_t pbc_stat[2][4];

enum {
    OP_NONE,
    OP_ILL,
    OP_NOP,
    OP_LUI,
    OP_AUIPC,
    OP_JAL,
    OP_JALR,
    OP_BEQ,
    OP_BNE,
    OP_BLT,
    OP_BGE,
    OP_BLTU,
    OP_BGEU,
    OP_LB,
    OP_LH,
    OP_LW,
    OP_LBU,
    OP_LHU,
    OP_SB,
    OP_SH,
    OP_SW,
    OP_ADDI,
    OP_SLTI,
    OP_SLTIU,
    OP_XORI,
    OP_ORI,
    OP_ANDI,
    OP_SLLI,
    OP_SRLI,
    OP_SRAI,
    OP_ADD,
    OP_SUB,
    OP_SLL,
    OP_SLT,
    OP_SLTU,
    OP_XOR,
    OP_SRL,
    OP_SRA,
    OP_OR,
    OP_AND,
    OP_MUL,
    OP_MULH,
    OP_MULHSU,
    OP_MULHU,
    OP_DIV,
    OP_DIVU,
    OP_REM,
    OP_REMU,
    OP_LR,
    OP_SC,
    OP_AMOSWAP,
    OP_AMOADD,
    OP_AMOXOR,
    OP_AMOAND,
    OP_AMOOR,
    OP_AMOMIN,
    OP_AMOMAX,
    OP_AMOMINU,
    OP_AMOMAXU,
    OP_SH1ADD,
    OP_SH2ADD,
    OP_SH3ADD,
    OP_ANDN,
    OP_ORN,
    OP_XNOR,
    OP_CLZ,
    OP_CTZ,
    OP_CPOP,
    OP_MAX,
    OP_MAXU,
    OP_MIN,
    OP_MINU,
    OP_SEXTB,
    OP_SEXTH,
    OP_ROL,
    OP_ROR,
    OP_RORI,
    OP_ORCB,
    OP_REV8,
    OP_BCLR,
    OP_BCLRI,
    OP_BEXT,
    OP_BEXTI,
    OP_BINV,
    OP_BINVI,
    OP_BSET,
    OP_BSETI,
    OP_PACK,
    OP_PACKH,
    OP_BREV8,
    OP_ZIP,
    OP_UNZIP,
    OP_BEXTM,
    OP_BEXTMI,
    OP_ECALL,
    OP_EBREAK,
    OP_MRET,
    OP_WFI,
    OP_BLOCK,
    OP_UNBLOCK,
    OP_CSRRW,
    OP_CSRRS,
    OP_CSRRC,
    OP_CSRRWI,
    OP_CSRRSI,
    OP_CSRRCI,
    OP_CMPUSH,
    OP_CMPOP,
    OP_CMPOPRET,
    OP_CMPOPRETZ,
    OP_CMMVSA01,
    OP_CMMVA01S,
};

// rd is 32 for x0, so results need no test; rs1 and rs2 are 0 where the instruction reads none.
typedef struct {
    uint32_t raw;
    int32_t imm;
    uint8_t op, rd, rs1, rs2;
} rvd_t;

static rvd_t dc16[0x10000];
static rvd_t dc32[0x10000];

#define MSTATUS_MIE 0x8u
#define MSTATUS_MPIE 0x80u
#define MSTATUS_MPP 0x1800u
#define MSTATUS_MPRV (1u << 17)
#define MSTATUS_TW (1u << 21)

#define NUM_EIRQ 52
#define EIRQ_MASK ((1ull << NUM_EIRQ) - 1)

static inline int32_t sx(uint32_t v, int bits)
{
    return (int32_t)(v << (32 - bits)) >> (32 - bits);
}

// ---------------------------------------------------------------------------------------------
// Decoding

static void dec32(uint32_t w, rvd_t *d)
{
    uint32_t opc = w & 0x7f, rd = (w >> 7) & 31, f3 = (w >> 12) & 7, rs1 = (w >> 15) & 31, rs2 = (w >> 20) & 31;
    uint32_t f7 = w >> 25;
    int32_t immi = (int32_t)w >> 20;
    uint8_t rdx = rd ? (uint8_t)rd : 32;
    d->raw = w;
    d->op = OP_ILL;
    d->rd = 32;
    d->rs1 = d->rs2 = 0;
    d->imm = 0;
    switch (opc) {
    case 0x37:
    case 0x17:
        d->op = opc == 0x37 ? OP_LUI : OP_AUIPC;
        d->rd = rdx;
        d->imm = (int32_t)(w & 0xfffff000u);
        break;
    case 0x6f:
        d->op = OP_JAL;
        d->rd = rdx;
        d->imm = sx(((w >> 31) << 20) | (w & 0xff000) | (((w >> 20) & 1) << 11) | (((w >> 21) & 0x3ff) << 1), 21);
        break;
    case 0x67:
        if (f3 == 0) {
            d->op = OP_JALR;
            d->rd = rdx;
            d->rs1 = (uint8_t)rs1;
            d->imm = immi;
        }
        break;
    case 0x63: {
        static const uint8_t ops[8] = {OP_BEQ, OP_BNE, OP_ILL, OP_ILL, OP_BLT, OP_BGE, OP_BLTU, OP_BGEU};
        d->op = ops[f3];
        d->rs1 = (uint8_t)rs1;
        d->rs2 = (uint8_t)rs2;
        d->imm = sx(((w >> 31) << 12) | (((w >> 7) & 1) << 11) | (((w >> 25) & 0x3f) << 5) | (((w >> 8) & 0xf) << 1), 13);
        break;
    }
    case 0x03: {
        static const uint8_t ops[8] = {OP_LB, OP_LH, OP_LW, OP_ILL, OP_LBU, OP_LHU, OP_ILL, OP_ILL};
        d->op = ops[f3];
        d->rd = rdx;
        d->rs1 = (uint8_t)rs1;
        d->imm = immi;
        break;
    }
    case 0x23: {
        static const uint8_t ops[8] = {OP_SB, OP_SH, OP_SW, OP_ILL, OP_ILL, OP_ILL, OP_ILL, OP_ILL};
        d->op = ops[f3];
        d->rs1 = (uint8_t)rs1;
        d->rs2 = (uint8_t)rs2;
        d->imm = sx((f7 << 5) | rd, 12);
        break;
    }
    case 0x13: {
        uint8_t op = OP_ILL;
        d->imm = immi;
        switch (f3) {
        case 0: op = OP_ADDI; break;
        case 2: op = OP_SLTI; break;
        case 3: op = OP_SLTIU; break;
        case 4: op = OP_XORI; break;
        case 6: op = OP_ORI; break;
        case 7: op = OP_ANDI; break;
        case 1:
            d->imm = (int32_t)rs2;
            switch (f7) {
            case 0x00: op = OP_SLLI; break;
            case 0x14: op = OP_BSETI; break;
            case 0x24: op = OP_BCLRI; break;
            case 0x34: op = OP_BINVI; break;
            case 0x04: op = rs2 == 15 ? OP_ZIP : OP_ILL; break;
            case 0x30:
                op = rs2 == 0 ? OP_CLZ : rs2 == 1 ? OP_CTZ : rs2 == 2 ? OP_CPOP : rs2 == 4 ? OP_SEXTB : rs2 == 5 ? OP_SEXTH : OP_ILL;
                break;
            default: break;
            }
            break;
        case 5:
            d->imm = (int32_t)rs2;
            switch (f7) {
            case 0x00: op = OP_SRLI; break;
            case 0x20: op = OP_SRAI; break;
            case 0x30: op = OP_RORI; break;
            case 0x24: op = OP_BEXTI; break;
            case 0x14: op = rs2 == 7 ? OP_ORCB : OP_ILL; break;
            case 0x34: op = rs2 == 24 ? OP_REV8 : rs2 == 7 ? OP_BREV8 : OP_ILL; break;
            case 0x04: op = rs2 == 15 ? OP_UNZIP : OP_ILL; break;
            default: break;
            }
            break;
        default: break;
        }
        d->op = op;
        d->rd = rdx;
        d->rs1 = (uint8_t)rs1;
        break;
    }
    case 0x33: {
        uint8_t op = OP_ILL;
        if (w == 0x00002033u) {
            d->op = OP_BLOCK;
            break;
        }
        if (w == 0x00102033u) {
            d->op = OP_UNBLOCK;
            break;
        }
        switch (f7) {
        case 0x00: {
            static const uint8_t ops[8] = {OP_ADD, OP_SLL, OP_SLT, OP_SLTU, OP_XOR, OP_SRL, OP_OR, OP_AND};
            op = ops[f3];
            break;
        }
        case 0x20: op = f3 == 0 ? OP_SUB : f3 == 5 ? OP_SRA : f3 == 7 ? OP_ANDN : f3 == 6 ? OP_ORN : f3 == 4 ? OP_XNOR : OP_ILL; break;
        case 0x01: {
            static const uint8_t ops[8] = {OP_MUL, OP_MULH, OP_MULHSU, OP_MULHU, OP_DIV, OP_DIVU, OP_REM, OP_REMU};
            op = ops[f3];
            break;
        }
        case 0x10: op = f3 == 2 ? OP_SH1ADD : f3 == 4 ? OP_SH2ADD : f3 == 6 ? OP_SH3ADD : OP_ILL; break;
        case 0x05: op = f3 == 4 ? OP_MIN : f3 == 5 ? OP_MINU : f3 == 6 ? OP_MAX : f3 == 7 ? OP_MAXU : OP_ILL; break;
        case 0x30: op = f3 == 1 ? OP_ROL : f3 == 5 ? OP_ROR : OP_ILL; break;
        case 0x04: op = f3 == 4 ? OP_PACK : f3 == 7 ? OP_PACKH : OP_ILL; break;
        case 0x24: op = f3 == 1 ? OP_BCLR : f3 == 5 ? OP_BEXT : OP_ILL; break;
        case 0x34: op = f3 == 1 ? OP_BINV : OP_ILL; break;
        case 0x14: op = f3 == 1 ? OP_BSET : OP_ILL; break;
        default: break;
        }
        d->op = op;
        d->rd = rdx;
        d->rs1 = (uint8_t)rs1;
        d->rs2 = (uint8_t)rs2;
        break;
    }
    case 0x0f:
        // fence and fence.i: there is nothing to order or flush here
        if (f3 < 2)
            d->op = OP_NOP;
        break;
    case 0x73:
        if (f3 == 0) {
            if (w == 0x00000073u)
                d->op = OP_ECALL;
            else if (w == 0x00100073u)
                d->op = OP_EBREAK;
            else if (w == 0x30200073u)
                d->op = OP_MRET;
            else if (w == 0x10500073u)
                d->op = OP_WFI;
        } else if (f3 != 4) {
            static const uint8_t ops[8] = {0, OP_CSRRW, OP_CSRRS, OP_CSRRC, 0, OP_CSRRWI, OP_CSRRSI, OP_CSRRCI};
            d->op = ops[f3];
            d->rd = rdx;
            d->imm = (int32_t)(w >> 20);
            if (f3 < 4)
                d->rs1 = (uint8_t)rs1;
            else
                d->imm |= (int32_t)(rs1 << 12);
        }
        break;
    case 0x2f:
        if (f3 == 2) {
            uint8_t op = OP_ILL;
            switch (w >> 27) {
            case 0x02: op = rs2 == 0 ? OP_LR : OP_ILL; break;
            case 0x03: op = OP_SC; break;
            case 0x01: op = OP_AMOSWAP; break;
            case 0x00: op = OP_AMOADD; break;
            case 0x04: op = OP_AMOXOR; break;
            case 0x0c: op = OP_AMOAND; break;
            case 0x08: op = OP_AMOOR; break;
            case 0x10: op = OP_AMOMIN; break;
            case 0x14: op = OP_AMOMAX; break;
            case 0x18: op = OP_AMOMINU; break;
            case 0x1c: op = OP_AMOMAXU; break;
            default: break;
            }
            d->op = op;
            d->rd = rdx;
            d->rs1 = (uint8_t)rs1;
            d->rs2 = (uint8_t)rs2;
        }
        break;
    case 0x0b:
        // Xh3bextm: the field's width is in bits 28:26
        if ((w >> 29) == 0 && !((w >> 25) & 1) && (f3 == 0 || f3 == 4)) {
            uint32_t size = ((w >> 26) & 7) + 1;
            d->rd = rdx;
            d->rs1 = (uint8_t)rs1;
            if (f3 == 0) {
                d->op = OP_BEXTM;
                d->rs2 = (uint8_t)rs2;
                d->imm = (int32_t)size;
            } else {
                d->op = OP_BEXTMI;
                d->imm = (int32_t)(rs2 | (size << 8));
            }
        }
        break;
    default:
        break;
    }
}

// The saved registers Zcmp's three-bit fields name: s0, s1, then s2 to s7.
static inline uint8_t sreg(uint32_t n)
{
    return (uint8_t)(n < 2 ? 8 + n : 16 + n);
}

static void dec16(uint32_t w, rvd_t *d)
{
    uint32_t f3 = (w >> 13) & 7, q = w & 3;
    uint8_t r1p = (uint8_t)(8 + ((w >> 7) & 7)), r2p = (uint8_t)(8 + ((w >> 2) & 7));
    uint8_t r = (uint8_t)((w >> 7) & 31), r2 = (uint8_t)((w >> 2) & 31);
    uint8_t rx = r ? r : 32;
    bool b12 = (w >> 12) & 1;
    int32_t imm6 = sx(((w >> 7) & 0x20) | ((w >> 2) & 0x1f), 6);
    int32_t cj = sx(((w >> 1) & 0x800) | ((w >> 7) & 0x10) | ((w >> 1) & 0x300) | ((w << 2) & 0x400) | ((w >> 1) & 0x40) |
                        ((w << 1) & 0x80) | ((w >> 2) & 0xe) | ((w << 3) & 0x20),
                    12);
    int32_t cb = sx(((w >> 4) & 0x100) | ((w >> 7) & 0x18) | ((w << 1) & 0xc0) | ((w >> 2) & 6) | ((w << 3) & 0x20), 9);
    d->raw = w;
    d->op = OP_ILL;
    d->rd = 32;
    d->rs1 = d->rs2 = 0;
    d->imm = 0;
#define SET(o, d_, s1_, s2_, i_) (d->op = (o), d->rd = (d_), d->rs1 = (s1_), d->rs2 = (s2_), d->imm = (int32_t)(i_))
    switch ((q << 3) | f3) {
    case 0: {
        uint32_t imm = ((w >> 7) & 0x30) | ((w >> 1) & 0x3c0) | ((w >> 4) & 4) | ((w >> 2) & 8);
        if (imm)
            SET(OP_ADDI, r2p, 2, 0, imm);
        break;
    }
    case 2:
    case 6: {
        uint32_t off = ((w >> 7) & 0x38) | ((w >> 4) & 4) | ((w << 1) & 0x40);
        if (f3 == 2)
            SET(OP_LW, r2p, r1p, 0, off);
        else
            SET(OP_SW, 32, r1p, r2p, off);
        break;
    }
    case 4: {
        // Zcb's loads and stores
        uint32_t u2 = ((w >> 6) & 1) | ((w >> 4) & 2), u1 = (w >> 4) & 2;
        switch ((w >> 10) & 7) {
        case 0: SET(OP_LBU, r2p, r1p, 0, u2); break;
        case 1: SET((w & 0x40) ? OP_LH : OP_LHU, r2p, r1p, 0, u1); break;
        case 2: SET(OP_SB, 32, r1p, r2p, u2); break;
        case 3:
            if (!(w & 0x40))
                SET(OP_SH, 32, r1p, r2p, u1);
            break;
        default: break;
        }
        break;
    }
    case 8: SET(OP_ADDI, rx, r, 0, imm6); break;
    case 9: SET(OP_JAL, 1, 0, 0, cj); break;
    case 10: SET(OP_ADDI, rx, 0, 0, imm6); break;
    case 11:
        if (r == 2) {
            int32_t imm = sx(((w >> 3) & 0x200) | ((w >> 2) & 0x10) | ((w << 1) & 0x40) | ((w << 4) & 0x180) | ((w << 3) & 0x20), 10);
            if (imm)
                SET(OP_ADDI, 2, 2, 0, imm);
        } else if (imm6) {
            SET(OP_LUI, rx, 0, 0, imm6 * 4096);
        }
        break;
    case 12:
        switch ((w >> 10) & 3) {
        case 0:
        case 1:
            if (!b12)
                SET(((w >> 10) & 3) ? OP_SRAI : OP_SRLI, r1p, r1p, 0, (w >> 2) & 0x1f);
            break;
        case 2: SET(OP_ANDI, r1p, r1p, 0, imm6); break;
        default:
            if (!b12) {
                static const uint8_t ops[4] = {OP_SUB, OP_XOR, OP_OR, OP_AND};
                SET(ops[(w >> 5) & 3], r1p, r1p, r2p, 0);
            } else if (((w >> 5) & 3) == 2) {
                SET(OP_MUL, r1p, r1p, r2p, 0);
            } else if (((w >> 5) & 3) == 3) {
                switch ((w >> 2) & 7) {
                case 0: SET(OP_ANDI, r1p, r1p, 0, 0xff); break;
                case 1: SET(OP_SEXTB, r1p, r1p, 0, 0); break;
                case 2: SET(OP_PACK, r1p, r1p, 0, 0); break;   // zext.h
                case 3: SET(OP_SEXTH, r1p, r1p, 0, 0); break;
                case 5: SET(OP_XORI, r1p, r1p, 0, -1); break;
                default: break;
                }
            }
            break;
        }
        break;
    case 13: SET(OP_JAL, 32, 0, 0, cj); break;
    case 14: SET(OP_BEQ, 32, r1p, 0, cb); break;
    case 15: SET(OP_BNE, 32, r1p, 0, cb); break;
    case 16:
        if (!b12)
            SET(OP_SLLI, rx, r, 0, (w >> 2) & 0x1f);
        break;
    case 18:
        if (r)
            SET(OP_LW, r, 2, 0, ((w >> 7) & 0x20) | ((w >> 2) & 0x1c) | ((w << 4) & 0xc0));
        break;
    case 20:
        if (!b12) {
            if (!r2) {
                if (r)
                    SET(OP_JALR, 32, r, 0, 0);
            } else {
                SET(OP_ADD, rx, 0, r2, 0);
            }
        } else if (!r2) {
            if (!r)
                SET(OP_EBREAK, 32, 0, 0, 0);
            else
                SET(OP_JALR, 1, r, 0, 0);
        } else {
            SET(OP_ADD, rx, r, r2, 0);
        }
        break;
    case 22: SET(OP_SW, 32, 2, r2, ((w >> 7) & 0x3c) | ((w >> 1) & 0xc0)); break;
    case 21: {
        // Zcmp
        uint32_t top = (w >> 8) & 0x1f, rlist = (w >> 4) & 0xf;
        if ((top & 0x19) == 0x18) {
            static const uint8_t ops[4] = {OP_CMPUSH, OP_CMPOP, OP_CMPOPRETZ, OP_CMPOPRET};
            if (rlist >= 4)
                SET(ops[(top >> 1) & 3], 32, 0, 0, rlist | (((w >> 2) & 3) << 4));
        } else if (((w >> 10) & 7) == 3) {
            uint32_t a = (w >> 7) & 7, b = (w >> 2) & 7;
            if (((w >> 5) & 3) == 1 && a != b)
                SET(OP_CMMVSA01, 32, 0, 0, sreg(a) | (sreg(b) << 8));
            else if (((w >> 5) & 3) == 3)
                SET(OP_CMMVA01S, 32, sreg(a), sreg(b), 0);
        }
        break;
    }
    default:
        break;
    }
#undef SET
}

// ---------------------------------------------------------------------------------------------
// Modes, the PMP and traps

static void rvj_guard_changed(void);

// Works out where the PMP can refuse an access in the current mode: nowhere, everywhere (U-mode,
// which has no permission by default) or within the span of the regions M-mode must heed. Only
// accesses inside that window ask the PMP; all others take the quick paths.
static void rv_pmp_update(cpu_t *c)
{
    rv_t *h = &c->h3;
    bool all = !h->m_mode || ((h->mstatus & MSTATUS_MPRV) && !(h->mstatus & MSTATUS_MPP));
    uint64_t lo = ~0ull, hi = 0;
    for (int i = 0; i < 11; i++) {
        uint32_t cfg = (h->pmpcfg[i >> 2] >> (8 * (i & 3))) & 0xff;
        unsigned mode = (cfg >> 3) & 3;
        if (mode < 2 || !((cfg & 0x80) || ((h->pmpcfgm0 >> i) & 1)))
            continue;
        uint32_t pa = h->pmpaddr[i], mask = mode == 2 ? 0 : (pa ^ (pa + 1)) & 0x3fffffffu;
        uint64_t base = (uint64_t)(pa & ~mask) << 2, size = ((uint64_t)mask + 1) << 2;
        if (base < lo)
            lo = base;
        if (base + size > hi)
            hi = base + size;
    }
    uint32_t g_lo = 0, g_len = 0;
    if (all) {
        g_len = 0xffffffffu;
    } else if (hi) {
        g_lo = (uint32_t)lo;
        g_len = hi - lo > 0xffffffffull ? 0xffffffffu : (uint32_t)(hi - lo);
    }
    h->pmp_chk = all || hi != 0;
    h->pmp_all = all;
    if (g_lo != h->g_lo || g_len != h->g_len) {
        h->g_lo = g_lo;
        h->g_len = g_len;
        rvj_guard_changed();
    }
}

static void rv_set_mode(cpu_t *c, bool m_mode)
{
    c->h3.m_mode = m_mode;
    c->control = m_mode ? 0 : 1;
    rv_pmp_update(c);
}

// Whether the PMP lets this mode read (1), write (2) or execute (4) at a: the lowest-numbered
// region that matches decides, and M-mode only heeds regions that are locked or named in pmpcfgm0.
static bool rv_pmp_ok(cpu_t *c, uint32_t a, unsigned perm, bool m_mode)
{
    rv_t *h = &c->h3;
    for (int i = 0; i < 11; i++) {
        uint32_t cfg = (h->pmpcfg[i >> 2] >> (8 * (i & 3))) & 0xff;
        unsigned mode = (cfg >> 3) & 3;
        if (mode < 2)
            continue;
        uint32_t pa = h->pmpaddr[i], mask = mode == 2 ? 0 : (pa ^ (pa + 1));
        if (((a >> 2) ^ pa) & ~mask & 0x3fffffffu)
            continue;
        if (m_mode && !(cfg & 0x80) && !((h->pmpcfgm0 >> i) & 1))
            return true;
        return (cfg & perm) != 0;
    }
    return m_mode;
}

// Whether a lies where the PMP may refuse an access (see rv_pmp_update).
static inline bool rv_guarded(const rv_t *h, uint32_t a)
{
    return a - h->g_lo < h->g_len;
}

static inline bool rv_data_mode(const cpu_t *c)
{
    const rv_t *h = &c->h3;
    return (h->mstatus & MSTATUS_MPRV) ? (h->mstatus & MSTATUS_MPP) != 0 : h->m_mode;
}

// The lines Xh3irq sees: the chip's, and those software forces.
static inline uint64_t eirq_lines(const cpu_t *c)
{
    return ((c->m->irq_ext | c->m->irq_core_ext[c->id]) | c->h3.meifa) & EIRQ_MASK;
}

// The highest-priority line that is pending, enabled and at least of priority min; the lowest
// number wins a tie.
static bool eirq_best(const cpu_t *c, unsigned min, unsigned *irq, unsigned *pri)
{
    const rv_t *h = &c->h3;
    uint64_t act = eirq_lines(c) & h->meiea;
    int best = -1;
    unsigned bp = 0;
    while (act) {
        int i = __builtin_ctzll(act);
        act &= act - 1;
        unsigned p = h->meipra[i];
        if (p >= min && (best < 0 || p > bp)) {
            best = i;
            bp = p;
        }
    }
    if (best < 0)
        return false;
    *irq = (unsigned)best;
    *pri = bp;
    return true;
}

static uint32_t rv_mip(const cpu_t *c)
{
    const rv_t *h = &c->h3;
    uint32_t mip = 0;
    unsigned irq, pri;
    if (eirq_lines(c) & h->meiea && eirq_best(c, (h->meictx >> 16) & 0x1f, &irq, &pri))
        mip |= 0x800;
    if ((c->m->irq_core[c->id] >> IRQ_SIO_MTIMECMP) & 1)
        mip |= 0x80;
    if ((c->m->rv_softirq >> c->id) & 1)
        mip |= 0x8;
    return mip;
}

bool rv_irq_wakes(cpu_t *c)
{
    return (rv_mip(c) & c->h3.mie) != 0;
}

// The preemption level meicontext takes when the interrupt meinext shows is entered.
static uint32_t eirq_next_level(const cpu_t *c, bool *noirq, unsigned *irq)
{
    unsigned pri = 0;
    *irq = 0;
    *noirq = !eirq_best(c, (c->h3.meictx >> 24) & 0xf, irq, &pri);
    return *noirq ? 0x10 : pri + 1;
}

static void rv_trap(cpu_t *c, uint32_t cause, bool irq)
{
    rv_t *h = &c->h3;
    if (c->m->trace && !irq && cause != 8 && cause != 11)
        emu_log("core %d RISC-V exception %u at %08x", c->id, cause, c->r[15]);
    h->mepc = c->r[15] & ~1u;
    h->mcause = cause | (irq ? 0x80000000u : 0);
    uint32_t st = h->mstatus & ~(MSTATUS_MPIE | MSTATUS_MPP | MSTATUS_MIE);
    if (h->mstatus & MSTATUS_MIE)
        st |= MSTATUS_MPIE;
    if (h->m_mode)
        st |= MSTATUS_MPP;
    h->mstatus = st;
    if (irq && cause == 11) {
        bool noirq;
        unsigned n;
        uint32_t level = eirq_next_level(c, &noirq, &n), x = h->meictx;
        h->meictx = (x & 0x0000fff0u) | ((x & 0x0f000000u) << 4) | ((x & 0x000f0000u) << 8) | (level << 16) | 1;
    } else {
        h->meictx &= ~1u;
    }
    rv_set_mode(c, true);
    c->r[15] = (h->mtvec & ~3u) | ((irq && (h->mtvec & 1)) ? cause << 2 : 0);
    c->cyc += 2;
    h->jumped = true;
    h->ld_rd = 0;
}

static void rv_mret(cpu_t *c)
{
    rv_t *h = &c->h3;
    bool to_m = (h->mstatus & MSTATUS_MPP) != 0;
    uint32_t st = h->mstatus & ~(MSTATUS_MIE | MSTATUS_MPP);
    if (h->mstatus & MSTATUS_MPIE)
        st |= MSTATUS_MIE;
    st |= MSTATUS_MPIE;
    if (!to_m)
        st &= ~MSTATUS_MPRV;
    h->mstatus = st;
    if (h->meictx & 1) {
        uint32_t x = h->meictx;
        h->meictx = (x & 0x0000fff0u) | ((x & 0xf0000000u) >> 4) | ((x & 0x0f000000u) >> 8);
    }
    rv_set_mode(c, to_m);
    c->r[15] = h->mepc;
    c->cyc++;
    h->jumped = true;
    c->check_exc = true;
}

// Wakes a sleeping core for which an interrupt is pending and enabled, and enters the interrupt a
// running one may take.
static void rv_check_irq(cpu_t *c)
{
    rv_t *h = &c->h3;
    c->check_exc = false;
    uint32_t pend = rv_mip(c) & h->mie;
    if (c->state == CPU_WFE && c->event) {
        c->event = false;
        c->state = CPU_RUN;
    }
    if ((c->state == CPU_WFI || c->state == CPU_WFE) && pend)
        c->state = CPU_RUN;
    if (c->state != CPU_RUN)
        return;
    if (pend && ((h->mstatus & MSTATUS_MIE) || !h->m_mode))
        rv_trap(c, (pend & 0x800) ? 11 : (pend & 0x8) ? 3 : 7, true);
}

// ---------------------------------------------------------------------------------------------
// CSRs

static inline uint64_t rv_cycles_now(const cpu_t *c)
{
    return c->t / c->m->sys_ps;
}

static uint64_t rv_mcycle(const cpu_t *c)
{
    const rv_t *h = &c->h3;
    return (h->mcountinhibit & 1) ? h->cyc_val : h->cyc_val + (rv_cycles_now(c) - h->cyc_t0);
}

static uint64_t rv_minstret(const cpu_t *c)
{
    const rv_t *h = &c->h3;
    return (h->mcountinhibit & 4) ? h->ret_val : h->ret_val + (c->insns - h->ret_t0);
}

static uint32_t pmpcfg_write(rv_t *h, unsigned n, uint32_t v)
{
    uint32_t old = h->pmpcfg[n], out = 0;
    for (unsigned b = 0; b < 4; b++) {
        unsigned i = n * 4 + b;
        uint32_t o = (old >> (8 * b)) & 0xff, w = (v >> (8 * b)) & 0x9f;
        if (i >= 8) {
            w = o;   // the three hardwired regions, and none above them
        } else if (o & 0x80) {
            w = o;
        } else if (((w >> 3) & 3) == 1) {
            w &= ~0x18u;   // top-of-range is not supported
        }
        out |= w << (8 * b);
    }
    return out;
}

// One CSR instruction: wtype is 1 to write, 2 to set and 3 to clear the bits of wraw, wen whether
// it writes at all and ren whether it reads. Returns false for an illegal instruction.
static bool rv_csr(cpu_t *c, uint32_t addr, int wtype, bool wen, bool ren, uint32_t wraw, uint32_t *out)
{
    rv_t *h = &c->h3;
    chip_t *m = c->m;
    bool counter_u = false;
    if (addr == 0xc00 || addr == 0xc80)
        counter_u = h->mcounteren & 1;
    else if (addr == 0xc02 || addr == 0xc82)
        counter_u = (h->mcounteren >> 2) & 1;
    if (!h->m_mode && !counter_u)
        return false;
    if (wen && (addr >> 10) == 3)
        return false;

    uint32_t rd = 0;
    // What the instruction writes, from what it read.
#define NEW(old) (wtype == 1 ? wraw : wtype == 2 ? ((old) | wraw) : ((old) & ~wraw))
    switch (addr) {
    case 0x300:
        rd = h->mstatus;
        if (wen) {
            uint32_t v = NEW(rd) & (MSTATUS_TW | MSTATUS_MPRV | MSTATUS_MPP | MSTATUS_MPIE | MSTATUS_MIE);
            // Only M and U exist: anything else is rounded to one of them.
            v = (v & MSTATUS_MPP) == MSTATUS_MPP || (v & 0x1000) ? v | MSTATUS_MPP : v & ~MSTATUS_MPP;
            h->mstatus = v;
            rv_pmp_update(c);
            c->check_exc = true;
        }
        break;
    case 0x301: rd = 0x40901105u; break;
    case 0x304:
        rd = h->mie;
        if (wen) {
            h->mie = NEW(rd) & 0x888;
            c->check_exc = true;
        }
        break;
    case 0x305:
        rd = h->mtvec;
        if (wen)
            h->mtvec = NEW(rd) & 0xfffffffdu;
        break;
    case 0x306:
        rd = h->mcounteren;
        if (wen)
            h->mcounteren = NEW(rd) & 7;
        break;
    case 0x30a:
    case 0x31a:
    case 0x310: break;
    case 0x320:
        rd = h->mcountinhibit;
        if (wen) {
            uint32_t v = NEW(rd) & 5;
            uint64_t cy = rv_mcycle(c), ir = rv_minstret(c);
            h->mcountinhibit = v;
            h->cyc_val = cy;
            h->cyc_t0 = rv_cycles_now(c);
            h->ret_val = ir;
            h->ret_t0 = c->insns;
        }
        break;
    case 0x340:
        rd = h->mscratch;
        if (wen)
            h->mscratch = NEW(rd);
        break;
    case 0x341:
        rd = h->mepc;
        if (wen)
            h->mepc = NEW(rd) & ~1u;
        break;
    case 0x342:
        rd = h->mcause;
        if (wen)
            h->mcause = NEW(rd) & 0x8000000fu;
        break;
    case 0x343: break;
    case 0x344: rd = rv_mip(c); break;
    case 0x3a0:
    case 0x3a1:
    case 0x3a2:
    case 0x3a3:
        rd = h->pmpcfg[addr - 0x3a0];
        if (wen) {
            h->pmpcfg[addr - 0x3a0] = pmpcfg_write(h, addr - 0x3a0, NEW(rd));
            rv_pmp_update(c);
            c->cyc += 2;
        }
        break;
    case 0x7a0:
    case 0x7a3: break;
    case 0x7a1: rd = 0x20000000u; break;
    case 0x7a2:
        rd = h->tdata2;
        if (wen)
            h->tdata2 = NEW(rd) & ~1u;
        break;
    case 0x7a4: rd = 4; break;
    case 0x7a5:
        rd = h->tcontrol;
        if (wen)
            h->tcontrol = NEW(rd) & 0x88;
        break;
    case 0xb00:
    case 0xb80:
    case 0xc00:
    case 0xc80: {
        uint64_t v = rv_mcycle(c);
        bool hi = (addr & 0x80) != 0;
        rd = (uint32_t)(hi ? v >> 32 : v);
        if (wen) {
            uint32_t nv = NEW(rd);
            h->cyc_val = hi ? (v & 0xffffffffull) | ((uint64_t)nv << 32) : (v & ~0xffffffffull) | nv;
            h->cyc_t0 = rv_cycles_now(c);
        }
        break;
    }
    case 0xb02:
    case 0xb82:
    case 0xc02:
    case 0xc82: {
        uint64_t v = rv_minstret(c);
        bool hi = (addr & 0x80) != 0;
        rd = (uint32_t)(hi ? v >> 32 : v);
        if (wen) {
            uint32_t nv = NEW(rd);
            h->ret_val = hi ? (v & 0xffffffffull) | ((uint64_t)nv << 32) : (v & ~0xffffffffull) | nv;
            // The instruction doing the write is not counted on top of what it wrote.
            h->ret_t0 = c->insns + 1;
        }
        break;
    }
    case 0xbd0:
        rd = h->pmpcfgm0;
        if (wen) {
            h->pmpcfgm0 = NEW(rd) & 0xff;
            rv_pmp_update(c);
        }
        break;
    case 0xbe0:
    case 0xbe1:
    case 0xbe2: {
        // A 16-bit window into the array, chosen by the low bits of what the instruction wrote.
        unsigned win = wraw & 0x1f, sh = win * 16;
        uint64_t arr = addr == 0xbe0 ? h->meiea : addr == 0xbe1 ? eirq_lines(c) : h->meifa;
        rd = sh < 64 ? (uint32_t)((arr >> sh) & 0xffff) << 16 : 0;
        if (wen && addr != 0xbe1 && sh < 64) {
            uint64_t nv = (uint64_t)(NEW(rd) >> 16) << sh, mask = 0xffffull << sh;
            uint64_t *p = addr == 0xbe0 ? &h->meiea : &h->meifa;
            *p = ((*p & ~mask) | nv) & EIRQ_MASK;
            c->check_exc = true;
        }
        break;
    }
    case 0xbe3: {
        unsigned win = wraw & 0x7f, base = win * 4;
        for (unsigned i = 0; i < 4; i++)
            if (base + i < NUM_EIRQ)
                rd |= (uint32_t)h->meipra[base + i] << (16 + 4 * i);
        if (wen) {
            uint32_t nv = NEW(rd);
            for (unsigned i = 0; i < 4; i++)
                if (base + i < NUM_EIRQ)
                    h->meipra[base + i] = (nv >> (16 + 4 * i)) & 0xf;
            c->check_exc = true;
        }
        break;
    }
    case 0xbe4: {
        bool noirq;
        unsigned irq;
        uint32_t level = eirq_next_level(c, &noirq, &irq);
        rd = (noirq ? 0x80000000u : 0) | (irq << 2);
        if (ren && !noirq)
            h->meifa &= ~(1ull << irq);
        if (wen && (NEW(rd) & 1)) {
            h->meictx = (h->meictx & 0xff000001u) | (level << 16) | (noirq ? 0x8000u : 0) | (irq << 4);
            c->check_exc = true;
        }
        break;
    }
    case 0xbe5: {
        bool clearts = wen && wtype != 3 && (wraw & 2);
        rd = h->meictx;
        if (clearts)
            rd |= ((h->mie >> 7) & 1) << 3 | ((h->mie >> 3) & 1) << 2;
        if (wen) {
            uint32_t nv = NEW(rd);
            h->meictx = nv & 0xff1f9ff1u;
            if (clearts)
                h->mie &= ~0x88u;
            else
                h->mie |= ((nv >> 3) & 1) << 7 | ((nv >> 2) & 1) << 3;
            c->check_exc = true;
        }
        break;
    }
    case 0xbf0:
        rd = h->msleep;
        if (wen)
            h->msleep = NEW(rd) & 7;
        break;
    case 0xf11: break;
    case 0xf12: rd = 0x1b; break;
    case 0xf13: rd = 0x86fc4e3fu; break;
    case 0xf14: rd = (uint32_t)c->id; break;
    case 0xf15: break;
    default:
        if (addr >= 0x3b0 && addr <= 0x3bf) {
            unsigned i = addr - 0x3b0;
            rd = h->pmpaddr[i];
            if (wen && i < 8 && !((h->pmpcfg[i >> 2] >> (8 * (i & 3))) & 0x80)) {
                h->pmpaddr[i] = NEW(rd) & 0x3fffffffu;
                rv_pmp_update(c);
            }
            if (wen)
                c->cyc += 2;
            break;
        }
        // The performance counters and their events exist, hardwired to zero.
        if ((addr >= 0xb03 && addr <= 0xb1f) || (addr >= 0xb83 && addr <= 0xb9f) || (addr >= 0x323 && addr <= 0x33f))
            break;
        if (m->trace)
            emu_log("core %d: no CSR %03x (pc %08x)", c->id, addr, c->r[15]);
        return false;
    }
#undef NEW
    *out = rd;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Memory

// 0: a trap was entered; 1: the bus did it; 2: plain memory, with nothing else touched.
static __attribute__((noinline)) int rv_ld_slow(cpu_t *c, uint32_t a, int size, uint32_t *v)
{
    if (a & (uint32_t)(size - 1)) {
        rv_trap(c, 4, false);
        return 0;
    }
    if (c->h3.pmp_chk && !rv_pmp_ok(c, a, 1, rv_data_mode(c))) {
        rv_trap(c, 5, false);
        return 0;
    }
    uint8_t *p = c->rpage[a >> 16];
    if (p) {
        if ((a >> 28) == 1)
            c->cyc += xip_cost(c->m, a);
        p += a & 0xffff;
        *v = size == 4 ? rd32le(p) : size == 2 ? rd16le(p) : *p;
        return 2;
    }
    *v = bus_read(c, a, size);
    if (unlikely(c->fault)) {
        c->fault = false;
        rv_trap(c, 5, false);
        return 0;
    }
    return 1;
}

static __attribute__((noinline)) int rv_st_slow(cpu_t *c, uint32_t a, uint32_t v, int size)
{
    if (a & (uint32_t)(size - 1)) {
        rv_trap(c, 6, false);
        return 0;
    }
    if (c->h3.pmp_chk && !rv_pmp_ok(c, a, 2, rv_data_mode(c))) {
        rv_trap(c, 7, false);
        return 0;
    }
    uint8_t *p = c->wpage[a >> 16];
    if (p) {
        p += a & 0xffff;
        if (size == 4)
            wr32le(p, v);
        else if (size == 2)
            wr16le(p, (uint16_t)v);
        else
            *p = (uint8_t)v;
        return 2;
    }
    bus_write(c, a, size == 4 ? v : size == 2 ? v & 0xffff : v & 0xff, size);
    if (unlikely(c->fault)) {
        c->fault = false;
        rv_trap(c, 7, false);
        return 0;
    }
    return 1;
}

static inline bool rv_ld32(cpu_t *c, uint32_t a, uint32_t *v)
{
    uint8_t *p = c->rpage[a >> 16];
    if (likely(p && !(a & 3) && !rv_guarded(&c->h3, a) && (a >> 28) != 1)) {
        *v = rd32le(p + (a & 0xffff));
        return true;
    }
    return rv_ld_slow(c, a, 4, v) != 0;
}

static inline bool rv_st32(cpu_t *c, uint32_t a, uint32_t v)
{
    uint8_t *p = c->wpage[a >> 16];
    if (likely(p && !(a & 3) && !rv_guarded(&c->h3, a))) {
        wr32le(p + (a & 0xffff), v);
        return true;
    }
    return rv_st_slow(c, a, v, 4) != 0;
}

// A fetch the quick path cannot do: at the end of a page, from memory without one, or with the
// PMP to ask. Returns false once the fault is entered.
static __attribute__((noinline)) bool rv_fetch_slow(cpu_t *c, uint32_t pc, uint32_t *w)
{
    chip_t *m = c->m;
    rv_t *h = &c->h3;
    uint32_t v = 0;
    for (int half = 0; half < 2; half++) {
        uint32_t a = pc + 2 * (uint32_t)half;
        if (h->pmp_chk && !rv_pmp_ok(c, a, 4, h->m_mode)) {
            rv_trap(c, 1, false);
            return false;
        }
        uint8_t *p = m->rpage[a >> 16];
        if (p && (a >> 28) == 1 && (a & ~7u) != c->fetch_line) {
            c->cyc += xip_cost(m, a);
            c->fetch_line = a & ~7u;
        }
        uint32_t hw = p ? rd16le(p + (a & 0xffff)) : bus_fetch16_slow(c, a);
        if (unlikely(c->fault)) {
            c->fault = false;
            rv_trap(c, 1, false);
            return false;
        }
        v |= hw << (16 * half);
        if (!half && (hw & 3) != 3)
            break;
    }
    *w = v;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Running

// The registers Zcmp's rlist names, in the order they lie on the stack from the lowest address.
static const uint8_t zcmp_regs[13] = {1, 8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};

static inline unsigned zcmp_count(uint32_t rlist)
{
    return rlist == 15 ? 13 : rlist - 3;
}

static inline uint32_t zcmp_adj(uint32_t imm)
{
    uint32_t rlist = imm & 15;
    uint32_t base = rlist < 8 ? 16 : rlist < 12 ? 32 : rlist < 15 ? 48 : 64;
    return base + ((imm >> 4) & 3) * 16;
}

static inline uint32_t brev8(uint32_t v)
{
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    return ((v >> 4) & 0x0f0f0f0fu) | ((v & 0x0f0f0f0fu) << 4);
}

static uint32_t zip32(uint32_t v)
{
    uint32_t r = 0;
    for (int i = 0; i < 16; i++)
        r |= ((v >> i) & 1) << (2 * i) | ((v >> (i + 16)) & 1) << (2 * i + 1);
    return r;
}

static uint32_t unzip32(uint32_t v)
{
    uint32_t r = 0;
    for (int i = 0; i < 16; i++)
        r |= ((v >> (2 * i)) & 1) << i | ((v >> (2 * i + 1)) & 1) << (i + 16);
    return r;
}

// The decoded form of the instruction whose first bits are w.
static inline const rvd_t *rv_decode(uint32_t w)
{
    if ((w & 3) != 3) {
        rvd_t *e = &dc16[w & 0xffff];
        if (unlikely(!e->op))
            dec16(w & 0xffff, e);
        return e;
    }
    rvd_t *e = &dc32[(w * 0x9e3779b1u) >> 16];
    if (unlikely(e->raw != w))
        dec32(w, e);
    return e;
}

enum { RX_LOOK = 1, RX_TRAP = 2 };

// Executes the instruction d found at pc, for the interpreter's loop and for native code's calls
// (rvjit.inc). *npc comes in as the address after the instruction and goes out as where to go on;
// *cyc gets the cycles it took beyond its first. RX_LOOK in the result says that something the run
// loop looks at may have changed (the bus was used, a CSR written, the core put to sleep), RX_TRAP
// that a trap or mret has set the program counter itself.
static inline __attribute__((always_inline)) unsigned rv_exec(cpu_t *c, const rvd_t *d, uint32_t pc, uint32_t *npc_io,
                                                              uint32_t *cyc_out)
{
    chip_t *m = c->m;
    rv_t *h = &c->h3;
    uint32_t *x = h->x;
    uint32_t npc = *npc_io, cyc = 0;
    bool look = false;
// Around anything that counts its cycles in c->cyc, as the bus and the traps do.
#define SLOW_BEGIN (c->cyc = 0, look = true)
#define SLOW_END (cyc += c->cyc)
#define TRAP(cause) \
    do { \
SLOW_BEGIN; \
rv_trap(c, (cause), false); \
SLOW_END; \
goto trapped; \
    } while (0)
#define BRANCH(cond) \
    if (cond) { \
if (h->bp_pc != pc) { \
    cyc++; \
    h->jumped = true; \
    if (d->imm < 0) \
        h->bp_pc = pc; \
} \
npc = pc + (uint32_t)d->imm; \
    } else if (h->bp_pc == pc) { \
cyc++; \
h->bp_pc = 0; \
    } \
    break
// A load: straight from a page of RAM or ROM, or by way of the bus, the flash cache and the PMP.
#define LOAD(size, align, conv) \
    { \
uint32_t a = A + (uint32_t)d->imm, v; \
uint8_t *p = c->rpage[a >> 16]; \
if (likely(p && !(a & (align)) && !rv_guarded(h, a) && (a >> 28) != 1)) { \
    p += a & 0xffff; \
    v = size == 4 ? rd32le(p) : size == 2 ? rd16le(p) : *p; \
} else { \
    c->cyc = 0; \
    int ok = rv_ld_slow(c, a, size, &v); \
    SLOW_END; \
    if (!ok) { \
        look = true; \
        goto trapped; \
    } \
    look |= ok == 1; \
} \
R = (uint32_t)conv v; \
h->ld_rd = d->rd; \
break; \
    }
#define STORE(size, align) \
    { \
uint32_t a = A + (uint32_t)d->imm, v = B; \
uint8_t *p = c->wpage[a >> 16]; \
if (likely(p && !(a & (align)) && !rv_guarded(h, a))) { \
    p += a & 0xffff; \
    if (size == 4) \
        wr32le(p, v); \
    else if (size == 2) \
        wr16le(p, (uint16_t)v); \
    else \
        *p = (uint8_t)v; \
} else { \
    c->cyc = 0; \
    int ok = rv_st_slow(c, a, v, size); \
    SLOW_END; \
    if (!ok) { \
        look = true; \
        goto trapped; \
    } \
    look |= ok == 1; \
} \
break; \
    }
#define A x[d->rs1]
#define B x[d->rs2]
#define R x[d->rd]
    switch (d->op) {
    case OP_NOP: break;
    case OP_LUI: R = (uint32_t)d->imm; break;
    case OP_AUIPC: R = pc + (uint32_t)d->imm; break;
    case OP_JAL:
        R = npc;
        npc = pc + (uint32_t)d->imm;
        cyc++;
        h->jumped = true;
        break;
    case OP_JALR: {
        uint32_t target = (A + (uint32_t)d->imm) & ~1u;
        R = npc;
        npc = target;
        cyc++;
        h->jumped = true;
        break;
    }
    case OP_BEQ: BRANCH(A == B);
    case OP_BNE: BRANCH(A != B);
    case OP_BLT: BRANCH((int32_t)A < (int32_t)B);
    case OP_BGE: BRANCH((int32_t)A >= (int32_t)B);
    case OP_BLTU: BRANCH(A < B);
    case OP_BGEU: BRANCH(A >= B);
    case OP_LB: LOAD(1, 0, (int8_t))
    case OP_LBU: LOAD(1, 0, )
    case OP_LH: LOAD(2, 1, (int16_t))
    case OP_LHU: LOAD(2, 1, )
    case OP_LW: LOAD(4, 3, )
    case OP_SB: STORE(1, 0)
    case OP_SH: STORE(2, 1)
    case OP_SW: STORE(4, 3)
    case OP_ADDI: R = A + (uint32_t)d->imm; break;
    case OP_SLTI: R = (int32_t)A < d->imm; break;
    case OP_SLTIU: R = A < (uint32_t)d->imm; break;
    case OP_XORI: R = A ^ (uint32_t)d->imm; break;
    case OP_ORI: R = A | (uint32_t)d->imm; break;
    case OP_ANDI: R = A & (uint32_t)d->imm; break;
    case OP_SLLI: R = A << d->imm; break;
    case OP_SRLI: R = A >> d->imm; break;
    case OP_SRAI: R = (uint32_t)((int32_t)A >> d->imm); break;
    case OP_ADD: R = A + B; break;
    case OP_SUB: R = A - B; break;
    case OP_SLL: R = A << (B & 31); break;
    case OP_SLT: R = (int32_t)A < (int32_t)B; break;
    case OP_SLTU: R = A < B; break;
    case OP_XOR: R = A ^ B; break;
    case OP_SRL: R = A >> (B & 31); break;
    case OP_SRA: R = (uint32_t)((int32_t)A >> (B & 31)); break;
    case OP_OR: R = A | B; break;
    case OP_AND: R = A & B; break;
    case OP_MUL: R = A * B; break;
    case OP_MULH: R = (uint32_t)(((int64_t)(int32_t)A * (int64_t)(int32_t)B) >> 32); break;
    case OP_MULHSU: R = (uint32_t)(((int64_t)(int32_t)A * (int64_t)(uint64_t)B) >> 32); break;
    case OP_MULHU: R = (uint32_t)(((uint64_t)A * (uint64_t)B) >> 32); break;
    case OP_DIV: {
        int32_t a = (int32_t)A, b = (int32_t)B;
        R = b == 0 ? 0xffffffffu : (a == INT32_MIN && b == -1) ? (uint32_t)a : (uint32_t)(a / b);
        cyc += 17 + ((a ^ b) < 0);
        break;
    }
    case OP_DIVU: {
        uint32_t a = A, b = B;
        R = b == 0 ? 0xffffffffu : a / b;
        cyc += 17;
        break;
    }
    case OP_REM: {
        int32_t a = (int32_t)A, b = (int32_t)B;
        R = b == 0 ? (uint32_t)a : (a == INT32_MIN && b == -1) ? 0 : (uint32_t)(a % b);
        cyc += 17 + (a < 0);
        break;
    }
    case OP_REMU: {
        uint32_t a = A, b = B;
        R = b == 0 ? a : a % b;
        cyc += 17;
        break;
    }
    case OP_LR: {
        uint32_t v, a = A;
        SLOW_BEGIN;
        bool ok = rv_ld32(c, a, &v);
        SLOW_END;
        if (!ok)
            goto trapped;
        R = v;
        c->excl_valid = true;
        c->excl_addr = a;
        h->ld_rd = d->rd;
        break;
    }
    case OP_SC: {
        uint32_t a = A;
        if (c->excl_valid && c->excl_addr == a) {
            SLOW_BEGIN;
            bool ok = rv_st32(c, a, B);
            SLOW_END;
            if (!ok)
                goto trapped;
            R = 0;
            // The bus's monitor: the other core's reservation does not outlive this store.
            m->core[1 - c->id].excl_valid = false;
        } else {
            if (a & 3)
                TRAP(6);
            R = 1;
        }
        c->excl_valid = false;
        h->ld_rd = d->rd;
        break;
    }
    case OP_AMOSWAP:
    case OP_AMOADD:
    case OP_AMOXOR:
    case OP_AMOAND:
    case OP_AMOOR:
    case OP_AMOMIN:
    case OP_AMOMAX:
    case OP_AMOMINU:
    case OP_AMOMAXU: {
        uint32_t a = A, b = B, v, nv;
        if (a & 3)
            TRAP(6);
        SLOW_BEGIN;
        bool ok = rv_ld32(c, a, &v);
        if (ok) {
            switch (d->op) {
            case OP_AMOSWAP: nv = b; break;
            case OP_AMOADD: nv = v + b; break;
            case OP_AMOXOR: nv = v ^ b; break;
            case OP_AMOAND: nv = v & b; break;
            case OP_AMOOR: nv = v | b; break;
            case OP_AMOMIN: nv = (int32_t)v < (int32_t)b ? v : b; break;
            case OP_AMOMAX: nv = (int32_t)v > (int32_t)b ? v : b; break;
            case OP_AMOMINU: nv = v < b ? v : b; break;
            default: nv = v > b ? v : b; break;
            }
            ok = rv_st32(c, a, nv);
        }
        SLOW_END;
        if (!ok)
            goto trapped;
        R = v;
        m->core[1 - c->id].excl_valid = false;
        cyc += 3;
        break;
    }
    case OP_SH1ADD: R = B + (A << 1); break;
    case OP_SH2ADD: R = B + (A << 2); break;
    case OP_SH3ADD: R = B + (A << 3); break;
    case OP_ANDN: R = A & ~B; break;
    case OP_ORN: R = A | ~B; break;
    case OP_XNOR: R = ~(A ^ B); break;
    case OP_CLZ: R = A ? (uint32_t)__builtin_clz(A) : 32; break;
    case OP_CTZ: R = A ? (uint32_t)__builtin_ctz(A) : 32; break;
    case OP_CPOP: R = (uint32_t)__builtin_popcount(A); break;
    case OP_MAX: R = (int32_t)A > (int32_t)B ? A : B; break;
    case OP_MAXU: R = A > B ? A : B; break;
    case OP_MIN: R = (int32_t)A < (int32_t)B ? A : B; break;
    case OP_MINU: R = A < B ? A : B; break;
    case OP_SEXTB: R = (uint32_t)(int8_t)A; break;
    case OP_SEXTH: R = (uint32_t)(int16_t)A; break;
    case OP_ROL: {
        uint32_t a = A, sh = B & 31;
        R = (a << sh) | (a >> ((32 - sh) & 31));
        break;
    }
    case OP_ROR: {
        uint32_t a = A, sh = B & 31;
        R = (a >> sh) | (a << ((32 - sh) & 31));
        break;
    }
    case OP_RORI: {
        uint32_t a = A, sh = (uint32_t)d->imm;
        R = (a >> sh) | (a << ((32 - sh) & 31));
        break;
    }
    case OP_ORCB: {
        uint32_t a = A, r = 0;
        for (int i = 0; i < 4; i++)
            if (a & (0xffu << (8 * i)))
                r |= 0xffu << (8 * i);
        R = r;
        break;
    }
    case OP_REV8: R = __builtin_bswap32(A); break;
    case OP_BCLR: R = A & ~(1u << (B & 31)); break;
    case OP_BCLRI: R = A & ~(1u << d->imm); break;
    case OP_BEXT: R = (A >> (B & 31)) & 1; break;
    case OP_BEXTI: R = (A >> d->imm) & 1; break;
    case OP_BINV: R = A ^ (1u << (B & 31)); break;
    case OP_BINVI: R = A ^ (1u << d->imm); break;
    case OP_BSET: R = A | (1u << (B & 31)); break;
    case OP_BSETI: R = A | (1u << d->imm); break;
    case OP_PACK: R = (A & 0xffff) | (B << 16); break;
    case OP_PACKH: R = (A & 0xff) | ((B & 0xff) << 8); break;
    case OP_BREV8: R = brev8(A); break;
    case OP_ZIP: R = zip32(A); break;
    case OP_UNZIP: R = unzip32(A); break;
    case OP_BEXTM: R = (A >> (B & 31)) & ((1u << d->imm) - 1); break;
    case OP_BEXTMI: R = (A >> (d->imm & 31)) & ((1u << (d->imm >> 8)) - 1); break;
    case OP_ECALL: TRAP(h->m_mode ? 11 : 8);
    case OP_EBREAK: TRAP(3);
    case OP_MRET:
        if (!h->m_mode)
            TRAP(2);
        SLOW_BEGIN;
        rv_mret(c);
        SLOW_END;
        goto trapped;
    case OP_WFI:
        if (!h->m_mode && (h->mstatus & MSTATUS_TW))
            TRAP(2);
        if (!rv_irq_wakes(c))
            c->state = CPU_WFI;
        c->check_exc = true;
        look = true;
        break;
    case OP_BLOCK:
        if (!h->m_mode && (h->mstatus & MSTATUS_TW))
            TRAP(2);
        if (c->event)
            c->event = false;
        else if (!rv_irq_wakes(c))
            c->state = CPU_WFE;
        c->check_exc = true;
        look = true;
        break;
    case OP_UNBLOCK:
        if (!h->m_mode && (h->mstatus & MSTATUS_TW))
            TRAP(2);
        chip_sev(m, c->id);
        look = true;
        break;
    case OP_CSRRW:
    case OP_CSRRS:
    case OP_CSRRC: {
        int wtype = d->op - OP_CSRRW + 1;
        uint32_t out;
        SLOW_BEGIN;
        bool ok = rv_csr(c, (uint32_t)d->imm & 0xfff, wtype, wtype == 1 || d->rs1 != 0, wtype != 1 || d->rd != 32, A, &out);
        SLOW_END;
        if (!ok)
            TRAP(2);
        R = out;
        break;
    }
    case OP_CSRRWI:
    case OP_CSRRSI:
    case OP_CSRRCI: {
        int wtype = d->op - OP_CSRRWI + 1;
        uint32_t out, imm = ((uint32_t)d->imm >> 12) & 31;
        SLOW_BEGIN;
        bool ok = rv_csr(c, (uint32_t)d->imm & 0xfff, wtype, wtype == 1 || imm != 0, wtype != 1 || d->rd != 32, imm, &out);
        SLOW_END;
        if (!ok)
            TRAP(2);
        R = out;
        break;
    }
    case OP_CMPUSH: {
        unsigned n = zcmp_count((uint32_t)d->imm & 15);
        uint32_t sp = x[2];
        // Only a stack outside plain RAM can involve the bus.
        look |= !c->wpage[(sp - 4) >> 16] || !c->wpage[(sp - 4 * n) >> 16];
        c->cyc = 0;
        for (unsigned i = 0; i < n; i++)
            if (!rv_st32(c, sp - 4 * (n - i), x[zcmp_regs[i]])) {
                look = true;
                SLOW_END;
                goto trapped;
            }
        SLOW_END;
        x[2] = sp - zcmp_adj((uint32_t)d->imm);
        cyc += n;
        break;
    }
    case OP_CMPOP:
    case OP_CMPOPRET:
    case OP_CMPOPRETZ: {
        unsigned n = zcmp_count((uint32_t)d->imm & 15);
        uint32_t sp = x[2] + zcmp_adj((uint32_t)d->imm), v[13];
        look |= !c->wpage[(sp - 4) >> 16] || !c->wpage[(sp - 4 * n) >> 16];
        c->cyc = 0;
        for (unsigned i = 0; i < n; i++)
            if (!rv_ld32(c, sp - 4 * (n - i), &v[i])) {
                look = true;
                SLOW_END;
                goto trapped;
            }
        SLOW_END;
        for (unsigned i = 0; i < n; i++)
            x[zcmp_regs[i]] = v[i];
        x[2] = sp;
        cyc += n;
        if (d->op != OP_CMPOP) {
            if (d->op == OP_CMPOPRETZ)
                x[10] = 0;
            npc = x[1] & ~1u;
            cyc += d->op == OP_CMPOPRETZ || n == 1 ? 2 : 1;
            h->jumped = true;
        }
        break;
    }
    case OP_CMMVSA01:
        x[d->imm & 0xff] = x[10];
        x[(d->imm >> 8) & 0xff] = x[11];
        cyc++;
        break;
    case OP_CMMVA01S: {
        uint32_t a = A, b = B;
        x[10] = a;
        x[11] = b;
        cyc++;
        break;
    }
    default:
        TRAP(2);
    }
#undef TRAP
#undef BRANCH
#undef LOAD
#undef STORE
#undef A
#undef B
#undef R
#undef SLOW_BEGIN
#undef SLOW_END
    *npc_io = npc;
    *cyc_out = cyc;
    return look ? RX_LOOK : 0;
trapped:
    *cyc_out = cyc;
    return RX_LOOK | RX_TRAP;
}

#include "rvjit.inc"

// Runs the core up to `until`, or to where an event scheduled meanwhile moved the end of the chunk.
// The inner loop keeps the program counter, the time and the limit to itself; whatever might
// change what the outer loop looks at ends the inner loop after its instruction.
void rv_run(cpu_t *c, uint64_t until)
{
    chip_t *m = c->m;
    rv_t *h = &c->h3;
    uint64_t n0 = c->insns;
    for (;;) {
        uint64_t lim = m->until < until ? m->until : until;
        if (c->t >= lim)
            break;
        const uint64_t ps = m->sys_ps;
        if (unlikely(c->check_exc)) {
            c->cyc = 0;
            rv_check_irq(c);
            c->t += c->cyc * ps;
        }
        if (unlikely(c->state != CPU_RUN))
            break;
        // A debugger has breakpoints set or steps: one instruction at a time (rvj_run declines).
        const bool slow = gdb_slow;
        if (unlikely(slow) && gdb_before_step(c))
            break;
        if (!slow && !h->pmp_all && rvj_on() && rvj_run(c, lim))
            continue;
        uint32_t pc = c->r[15];
        uint64_t t = c->t;
        // Cycles native code already charged the instruction it left before.
        uint32_t pre = h->jpre;
        h->jpre = 0;
        for (;;) {
            uint32_t w, cyc = 1 + pre, npc, xc = 0;
            pre = 0;
            unsigned r = RX_LOOK | RX_TRAP;
            const rvd_t *d;
            uint8_t *page = m->rpage[pc >> 16];
            if (likely(page && (pc & 0xffff) <= 0xfffc && !rv_guarded(h, pc) && !rv_guarded(h, pc + 2))) {
                w = rd32le(page + (pc & 0xffff));
                if (unlikely((pc >> 28) == 1)) {
                    // Consecutive fetches from one 8-byte cache line cannot miss.
                    if ((pc & ~7u) != c->fetch_line) {
                        cyc += xip_cost(m, pc);
                        c->fetch_line = pc & ~7u;
                    }
                    if ((w & 3) == 3 && ((pc + 2) & ~7u) != c->fetch_line) {
                        cyc += xip_cost(m, pc + 2);
                        c->fetch_line = (pc + 2) & ~7u;
                    }
                }
            } else {
                c->cyc = 0;
                bool ok = rv_fetch_slow(c, pc, &w);
                cyc += c->cyc;
                if (!ok)
                    goto account;
            }
            d = rv_decode(w);
            npc = pc + ((w & 3) == 3 ? 4 : 2);
            // A result a load has just produced costs its user a cycle, and so does a jump to a
            // 32-bit instruction that straddles two words.
            if (unlikely(h->ld_rd | h->jumped)) {
                if (h->ld_rd && (d->rs1 == h->ld_rd || d->rs2 == h->ld_rd))
                    cyc++;
                if (h->jumped && (pc & 2) && npc == pc + 4)
                    cyc++;
                h->ld_rd = 0;
                h->jumped = false;
            }
            r = rv_exec(c, d, pc, &npc, &xc);
            cyc += xc;
            if (!(r & RX_TRAP))
                c->r[15] = npc;
            if (page == NULL || (pc & 0xffff) > 0xfffc)
                r |= RX_LOOK;
        account:
            c->insns++;
            t += cyc * ps;
            c->t = t;
            if (unlikely(r) || t >= lim || slow)
                break;
            pc = npc;
        }
        if (unlikely(slow) && gdb_after_step(c))
            break;
    }
    pbc_stat[c->id][2] += c->insns - n0;
}

void rv_reset(cpu_t *c)
{
    rv_t *h = &c->h3;
    c->rv = true;
    c->v8 = true;
    c->r[15] = 0x7dfc;
    h->mtvec = 0x7ffc;
    h->mstatus = MSTATUS_MPP;
    h->mcountinhibit = 5;
    h->meictx = 0x8000;
    // Three regions are hardwired: U-mode may use the ROM, the peripherals and the SIO.
    h->pmpcfg[2] = 0x001f1f1f;
    h->pmpaddr[8] = 0x01ffffff;
    h->pmpaddr[9] = 0x13ffffff;
    h->pmpaddr[10] = 0x35ffffff;
    rv_set_mode(c, true);
}

// The debugger's registers, in the order its target description lists them: x0 to x31, pc, then
// mstatus, mie, mip, mtvec, mscratch, mepc, mcause and the privilege level (3: M, 0: U).
uint32_t rv_gdb_get(const cpu_t *c, int n)
{
    const rv_t *h = &c->h3;
    if (n == 0)
        return 0;
    if (n < 32)
        return h->x[n];
    switch (n) {
    case 32: return c->r[15];
    case 33: return h->mstatus;
    case 34: return h->mie;
    case 35: return rv_mip(c);
    case 36: return h->mtvec;
    case 37: return h->mscratch;
    case 38: return h->mepc;
    case 39: return h->mcause;
    case 40: return h->m_mode ? 3 : 0;
    default: return 0;
    }
}

void rv_gdb_set(cpu_t *c, int n, uint32_t v)
{
    rv_t *h = &c->h3;
    if (n > 0 && n < 32)
        h->x[n] = v;
    switch (n) {
    case 32:
        c->r[15] = v & ~1u;
        c->fetch_line = ~0u;
        h->jpre = 0;
        break;
    case 33:
        v &= MSTATUS_TW | MSTATUS_MPRV | MSTATUS_MPP | MSTATUS_MPIE | MSTATUS_MIE;
        h->mstatus = (v & MSTATUS_MPP) ? v | MSTATUS_MPP : v;
        rv_pmp_update(c);
        break;
    case 34: h->mie = v & 0x888; break;
    case 36: h->mtvec = v & 0xfffffffdu; break;
    case 37: h->mscratch = v; break;
    case 38: h->mepc = v & ~1u; break;
    case 39: h->mcause = v & 0x8000000fu; break;
    default: break;
    }
    c->check_exc = true;
}

void rv_dump(const cpu_t *c)
{
    static const char *const names[32] = {"zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0",
                                          "a1",   "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5",
                                          "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
    const rv_t *h = &c->h3;
    emu_log("core %d (RISC-V) %s in %c-mode pc=%08x mstatus=%08x mcause=%08x mepc=%08x mtvec=%08x", c->id,
            cpu_state_name(c), h->m_mode ? 'M' : 'U', c->r[15], h->mstatus, h->mcause, h->mepc, h->mtvec);
    for (int i = 0; i < 32; i += 4)
        emu_log("  %s=%08x %s=%08x %s=%08x %s=%08x", names[i], h->x[i], names[i + 1], h->x[i + 1], names[i + 2],
                h->x[i + 2], names[i + 3], h->x[i + 3]);
}
