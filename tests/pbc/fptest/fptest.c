// fptest: see CMakeLists.txt.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

static uint32_t bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static float fl(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static uint32_t get_fpscr(void)
{
    uint32_t v;
    __asm volatile("vmrs %0, fpscr" : "=r"(v));
    return v;
}

static void set_fpscr(uint32_t v)
{
    __asm volatile("vmsr fpscr, %0" ::"r"(v));
}

static uint32_t hash;
static void mix(uint32_t v)
{
    hash = (hash ^ v) * 16777619u;
}

static volatile float vin[16] = {1.5f, -2.25f, 3.0f, 0.1f, 1e30f, -1e-30f, 0.0f, -0.0f,
                                 1e-40f, 3.4e38f, 2.5f, -7.5f, 1234567.0f, 0.333333f, 16.0f, -1.0f};

static void edge_cases(void)
{
    float inf = fl(0x7f800000u), nan = fl(0x7fc00001u), snan = fl(0x7f800001u);
    volatile float a, b;
    float r[32];
    int n = 0;
    a = inf; b = inf;
    r[n++] = a - b;            // NaN from inf - inf
    a = 0.0f; b = inf;
    r[n++] = a * b;            // 0 * inf
    a = 1.0f; b = 0.0f;
    r[n++] = a / b;            // inf, DZC
    a = 0.0f; b = 0.0f;
    r[n++] = a / b;            // NaN
    a = nan; b = 2.0f;
    r[n++] = a + b;
    a = snan; b = 2.0f;
    r[n++] = a * b;
    a = -4.0f;
    r[n++] = __builtin_sqrtf(a);
    a = 1e-40f; b = 3.0f;      // subnormals
    r[n++] = a * b;
    r[n++] = a + a;
    a = 3e38f; b = 3e38f;
    r[n++] = a + b;            // overflow to inf
    a = 1.0f; b = 3.0f;
    r[n++] = __builtin_fmaf(a, b, 1e-8f);
    a = inf; b = 0.0f;
    r[n++] = __builtin_fmaf(a, b, 1.0f);
    for (int i = 0; i < n; i++)
        printf("fptest: edge %2d %08lx\n", i, (unsigned long)bits(r[i]));
    // conversions, including out of range
    volatile float cv[8] = {2.5f, -2.5f, 3e9f, -3e9f, 1e20f, -0.5f, 4294967040.0f, 2147483520.0f};
    for (int i = 0; i < 8; i++) {
        float f = cv[i];
        int32_t si = (int32_t)f;
        uint32_t ui = (uint32_t)f;
        printf("fptest: cvt %08lx s %08lx u %08lx fpscr %08lx\n", (unsigned long)bits(f), (unsigned long)si,
               (unsigned long)ui, (unsigned long)get_fpscr());
    }
    volatile int32_t iv[4] = {-7, 16777217, 0x7fffffff, (int32_t)0x80000000};
    for (int i = 0; i < 4; i++)
        printf("fptest: i2f %08lx %08lx\n", (unsigned long)bits((float)iv[i]), (unsigned long)bits((float)(uint32_t)iv[i]));
    // compares
    volatile float c1 = 1.0f, c2 = nan, c3 = -0.0f, c4 = 0.0f;
    printf("fptest: cmp %d %d %d %d %d fpscr %08lx\n", c1 < c2, c1 == c1, c3 == c4, c1 > c4, c2 != c2,
           (unsigned long)get_fpscr());
    // round toward zero, then flush to zero and default NaN
    uint32_t saved = get_fpscr();
    set_fpscr((saved & ~0x00c00000u) | 0x00c00000u);
    a = 1.0f; b = 3.0f;
    printf("fptest: rz %08lx %08lx\n", (unsigned long)bits(a / b), (unsigned long)(int32_t)(a / b * 10.0f));
    set_fpscr(saved | 0x03000000u);
    a = 1e-40f; b = 1.0f;
    printf("fptest: fz %08lx %08lx\n", (unsigned long)bits(a * b), (unsigned long)bits(nan + b));
    set_fpscr(saved);
    printf("fptest: fpscr %08lx\n", (unsigned long)get_fpscr());
}

static uint32_t mandel(void)
{
    uint32_t total = 0;
    for (int py = 0; py < 48; py++)
        for (int px = 0; px < 64; px++) {
            float x0 = px * (3.5f / 64) - 2.5f, y0 = py * (2.0f / 48) - 1.0f;
            float x = 0, y = 0;
            int it = 0;
            while (x * x + y * y <= 4.0f && it < 64) {
                float xt = x * x - y * y + x0;
                y = 2 * x * y + y0;
                x = xt;
                it++;
            }
            total += it;
            mix(bits(x));
        }
    return total;
}

static void work(void)
{
    float acc = 0, prod = 1;
    for (int i = 0; i < 2000; i++) {
        float v = vin[i & 15];
        acc += v * 0.5f;
        acc = __builtin_fmaf(v, 0.001f, acc);
        prod = prod * 1.0001f + __builtin_sqrtf(i * 0.5f) / (1.0f + i);
        int32_t q = (int32_t)(acc * 3.0f);
        acc -= (float)q * 0.25f;
        mix(bits(acc));
        mix(bits(prod));
        mix((uint32_t)q);
    }
}

int main(void)
{
    stdio_init_all();
    sleep_ms(50);
    edge_cases();
    for (int round = 0; round < 300; round++) {
        uint64_t t0 = time_us_64();
        hash = 2166136261u;
        uint32_t m = mandel();
        work();
        if (round % 100 == 99) printf("fptest: round %d mandel %lu hash %08lx fpscr %08lx in %llu us\n", round, (unsigned long)m,
               (unsigned long)hash, (unsigned long)get_fpscr(), (unsigned long long)(time_us_64() - t0));
    }
    printf("fptest: done\n");
    for (;;)
        sleep_ms(1000);
}
