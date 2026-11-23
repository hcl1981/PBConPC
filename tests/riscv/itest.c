#include <stdio.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/timer.h"
#include "hardware/sync.h"

static uint32_t seed = 12345;
static uint32_t rnd(void) {
    seed = seed * 1664525u + 1013904223u;
    uint32_t r = seed ^ (seed >> 13);
    switch ((seed >> 28) & 7) {   // edge cases often
    case 0: return 0;
    case 1: return 0xffffffffu;
    case 2: return 0x80000000u;
    case 3: return r & 0x1f;
    default: return r;
    }
}
static int fails, total;
static void check(const char *name, uint32_t a, uint32_t b, uint32_t got, uint32_t want) {
    total++;
    if (got != want && fails++ < 40) printf("FAIL %s a=%08x b=%08x got=%08x want=%08x\n", name, (unsigned)a, (unsigned)b, (unsigned)got, (unsigned)want);
}
#define R2(name, insn) static uint32_t __attribute__((noinline)) i_##name(uint32_t a, uint32_t b) { uint32_t r; __asm__ volatile(insn " %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); return r; }
#define R1(name, insn) static uint32_t __attribute__((noinline)) i_##name(uint32_t a, uint32_t b) { uint32_t r; (void)b; __asm__ volatile(insn " %0, %1" : "=r"(r) : "r"(a)); return r; }
#define RI(name, insn, imm) static uint32_t __attribute__((noinline)) i_##name(uint32_t a, uint32_t b) { uint32_t r; (void)b; __asm__ volatile(insn " %0, %1, " #imm : "=r"(r) : "r"(a)); return r; }
R2(andn,"andn") R2(orn,"orn") R2(xnor,"xnor") R2(min,"min") R2(max,"max") R2(minu,"minu") R2(maxu,"maxu")
R2(rol,"rol") R2(ror,"ror") R2(bclr,"bclr") R2(bext,"bext") R2(binv,"binv") R2(bset,"bset")
R2(sh1add,"sh1add") R2(sh2add,"sh2add") R2(sh3add,"sh3add") R2(pack,"pack") R2(packh,"packh")
R2(mul,"mul") R2(mulh,"mulh") R2(mulhsu,"mulhsu") R2(mulhu,"mulhu") R2(div,"div") R2(divu,"divu") R2(rem,"rem") R2(remu,"remu")
R2(sll,"sll") R2(srl,"srl") R2(sra,"sra") R2(slt,"slt") R2(sltu,"sltu")
R1(clz,"clz") R1(ctz,"ctz") R1(cpop,"cpop") R1(sextb,"sext.b") R1(sexth,"sext.h") R1(zexth,"zext.h") R1(orcb,"orc.b") R1(rev8,"rev8")
R1(brev8,"brev8") R1(zip,"zip") R1(unzip,"unzip")
RI(rori7,"rori",7) RI(rori31,"rori",31) RI(bclri5,"bclri",5) RI(bexti31,"bexti",31) RI(binvi0,"binvi",0) RI(bseti17,"bseti",17)
RI(srai3,"srai",3) RI(slti,"slti",-5) RI(sltiu,"sltiu",-5)
static uint32_t __attribute__((noinline)) i_bextm3(uint32_t a, uint32_t b) { uint32_t r; __asm__ volatile(".insn r 0x0b, 0, 4, %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); return r; }
static uint32_t __attribute__((noinline)) i_bextm8(uint32_t a, uint32_t b) { uint32_t r; __asm__ volatile(".insn r 0x0b, 0, 14, %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); return r; }
static uint32_t __attribute__((noinline)) i_bextmi(uint32_t a, uint32_t b) { uint32_t r; (void)b; __asm__ volatile(".insn i 0x0b, 4, %0, %1, (9 | (4 << 6))" : "=r"(r) : "r"(a)); return r; }
#define AMO(name, insn) static uint32_t __attribute__((noinline)) i_##name(uint32_t *p, uint32_t b) { uint32_t r; __asm__ volatile(insn " %0, %2, (%1)" : "=&r"(r) : "r"(p), "r"(b) : "memory"); return r; }
AMO(amoswap,"amoswap.w") AMO(amoadd,"amoadd.w") AMO(amoxor,"amoxor.w") AMO(amoand,"amoand.w") AMO(amoor,"amoor.w")
AMO(amomin,"amomin.w") AMO(amomax,"amomax.w") AMO(amominu,"amominu.w") AMO(amomaxu,"amomaxu.w")

static uint32_t m_clz(uint32_t a) { int n = 0; while (n < 32 && !(a & (0x80000000u >> n))) n++; return n; }
static uint32_t m_ctz(uint32_t a) { int n = 0; while (n < 32 && !(a & (1u << n))) n++; return n; }
static uint32_t m_cpop(uint32_t a) { int n = 0; for (int i = 0; i < 32; i++) n += (a >> i) & 1; return n; }
static uint32_t m_rol(uint32_t a, uint32_t s) { s &= 31; return s ? (a << s) | (a >> (32 - s)) : a; }
static uint32_t m_ror(uint32_t a, uint32_t s) { s &= 31; return s ? (a >> s) | (a << (32 - s)) : a; }
static uint32_t m_orcb(uint32_t a) { uint32_t r = 0; for (int i = 0; i < 4; i++) if ((a >> (8 * i)) & 0xff) r |= 0xffu << (8 * i); return r; }
static uint32_t m_rev8(uint32_t a) { return (a >> 24) | ((a >> 8) & 0xff00) | ((a << 8) & 0xff0000) | (a << 24); }
static uint32_t m_brev8(uint32_t a) { uint32_t r = 0; for (int i = 0; i < 32; i++) if (a & (1u << i)) r |= 1u << ((i & ~7) | (7 - (i & 7))); return r; }
static uint32_t m_zip(uint32_t a) { uint32_t r = 0; for (int i = 0; i < 16; i++) { r |= ((a >> i) & 1) << (2 * i); r |= ((a >> (i + 16)) & 1) << (2 * i + 1); } return r; }
static uint32_t m_unzip(uint32_t a) { uint32_t r = 0; for (int i = 0; i < 16; i++) { r |= ((a >> (2 * i)) & 1) << i; r |= ((a >> (2 * i + 1)) & 1) << (i + 16); } return r; }
static uint32_t m_div(uint32_t a, uint32_t b) { if (!b) return ~0u; if (a == 0x80000000u && b == ~0u) return a; int64_t q = (int64_t)(int32_t)a / (int32_t)b; return (uint32_t)q; }
static uint32_t m_rem(uint32_t a, uint32_t b) { if (!b) return a; if (a == 0x80000000u && b == ~0u) return 0; int64_t q = (int64_t)(int32_t)a % (int32_t)b; return (uint32_t)q; }

static volatile int order[8], norder;
static void high_isr(void) { order[norder++ & 7] = 2; }
static void nest_low(void) { order[norder++ & 7] = 10; irq_set_pending(47); order[norder++ & 7] = 11; }
static void nest_high(void) { order[norder++ & 7] = 20; }

int main(void) {
    stdio_init_all();
    printf("itest start\n");
    for (int n = 0; n < 20000; n++) {
        uint32_t a = rnd(), b = rnd();
#define C(name, want) check(#name, a, b, i_##name(a, b), (want))
        C(andn, a & ~b); C(orn, a | ~b); C(xnor, ~(a ^ b));
        C(min, (int32_t)a < (int32_t)b ? a : b); C(max, (int32_t)a > (int32_t)b ? a : b);
        C(minu, a < b ? a : b); C(maxu, a > b ? a : b);
        C(rol, m_rol(a, b)); C(ror, m_ror(a, b));
        C(bclr, a & ~(1u << (b & 31))); C(bext, (a >> (b & 31)) & 1); C(binv, a ^ (1u << (b & 31))); C(bset, a | (1u << (b & 31)));
        C(sh1add, b + (a << 1)); C(sh2add, b + (a << 2)); C(sh3add, b + (a << 3));
        C(pack, (a & 0xffff) | (b << 16)); C(packh, (a & 0xff) | ((b & 0xff) << 8));
        C(mul, a * b); C(mulh, (uint32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32));
        C(mulhsu, (uint32_t)(((int64_t)(int32_t)a * (int64_t)(uint64_t)b) >> 32)); C(mulhu, (uint32_t)(((uint64_t)a * b) >> 32));
        C(div, m_div(a, b)); C(divu, b ? a / b : ~0u); C(rem, m_rem(a, b)); C(remu, b ? a % b : a);
        C(sll, a << (b & 31)); C(srl, a >> (b & 31)); C(sra, (uint32_t)((int32_t)a >> (b & 31)));
        C(slt, (int32_t)a < (int32_t)b); C(sltu, a < b);
        C(clz, m_clz(a)); C(ctz, m_ctz(a)); C(cpop, m_cpop(a));
        C(sextb, (uint32_t)(int8_t)a); C(sexth, (uint32_t)(int16_t)a); C(zexth, a & 0xffff);
        C(orcb, m_orcb(a)); C(rev8, m_rev8(a)); C(brev8, m_brev8(a)); C(zip, m_zip(a)); C(unzip, m_unzip(a));
        C(rori7, m_ror(a, 7)); C(rori31, m_ror(a, 31)); C(bclri5, a & ~32u); C(bexti31, a >> 31); C(binvi0, a ^ 1); C(bseti17, a | (1u << 17));
        C(srai3, (uint32_t)((int32_t)a >> 3)); C(slti, (int32_t)a < -5); C(sltiu, a < (uint32_t)-5);
        C(bextm3, (a >> (b & 31)) & 7); C(bextm8, (a >> (b & 31)) & 0xff); C(bextmi, (a >> 9) & 0x1f);
        uint32_t mem;
#define M(name, newv) mem = a; check(#name, a, b, i_##name(&mem, b), a); check(#name "(mem)", a, b, mem, (newv))
        M(amoswap, b); M(amoadd, a + b); M(amoxor, a ^ b); M(amoand, a & b); M(amoor, a | b);
        M(amomin, (int32_t)a < (int32_t)b ? a : b); M(amomax, (int32_t)a > (int32_t)b ? a : b);
        M(amominu, a < b ? a : b); M(amomaxu, a > b ? a : b);
    }
    printf("ops: %d checks, %d fails\n", total, fails);

    // lr/sc
    static uint32_t cell = 5;
    uint32_t r0, r1, r2;
    __asm__ volatile("lr.w %0, (%3)\n addi %0, %0, 1\n sc.w %1, %0, (%3)\n sc.w %2, %0, (%3)" : "=&r"(r0), "=&r"(r1), "=&r"(r2) : "r"(&cell) : "memory");
    printf("lrsc: val %u first %u second %u cell %u (want 6 0 1 6)\n", (unsigned)r0, (unsigned)r1, (unsigned)r2, (unsigned)cell);

    // interrupt priorities: irq 46 and 47 are spare (software only)
    irq_set_exclusive_handler(46, nest_low);
    irq_set_exclusive_handler(47, nest_high);
    irq_set_priority(46, 0xc0);
    irq_set_priority(47, 0x40);
    irq_set_enabled(46, true);
    irq_set_enabled(47, true);
    irq_set_pending(46);
    __asm__ volatile("nop\n nop\n nop");
    printf("preempt order:");
    for (int i = 0; i < norder; i++) printf(" %d", order[i]);
    printf(" (want 10 20 11)\n");
    norder = 0;
    // both pending while interrupts are off: the more urgent one goes first, no nesting
    uint32_t s = save_and_disable_interrupts();
    irq_set_exclusive_handler(45, high_isr);
    irq_set_priority(45, 0x40);
    irq_set_enabled(45, true);
    irq_set_pending(46);
    irq_set_pending(45);
    restore_interrupts(s);
    __asm__ volatile("nop\n nop\n nop");
    printf("pending order:");
    for (int i = 0; i < norder; i++) printf(" %d", order[i]);
    printf(" (want 2 10 20 11)\n");
    uint64_t t = time_us_64();
    sleep_ms(50);
    printf("slept %u us\n", (unsigned)(time_us_64() - t));
    printf("itest done\n");
    for (;;) sleep_ms(1000);
}
