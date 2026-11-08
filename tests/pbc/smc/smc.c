// smc: see CMakeLists.txt.
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

// A at 0: cmp r0, r1; b B. B at 0x20.
static uint16_t __attribute__((aligned(64))) code[32];

static void set_b(int variant)
{
    uint16_t *b = &code[0x10];
    if (variant == 1) {
        b[0] = 0x429b;   // cmp r3, r3     (writes all flags: A's are dead)
        b[1] = 0x4770;   // bx lr
    } else {
        b[0] = 0xd001;   // beq.n 0x26     (reads A's Z)
        b[1] = 0x2001;   // movs r0, #1
        b[2] = 0x4770;   // bx lr
        b[3] = 0x2002;   // movs r0, #2
        b[4] = 0x4770;   // bx lr
    }
    __asm volatile("dsb; isb" ::: "memory");
}

int main(void)
{
    stdio_init_all();
    sleep_ms(100);
    code[0] = 0x4288;    // cmp r0, r1
    code[1] = 0xe00d;    // b.n 0x20
    typedef int (*fn_t)(int, int);
    fn_t f = (fn_t)((uintptr_t)code | 1);
    for (int round = 0; round < 3; round++) {
        set_b(1);
        volatile int sink = 0;
        for (int i = 0; i < 20000; i++)
            sink += f(i, i & 3);   // hot: translated with A's flags dead
        set_b(2);
        int bad = 0;
        for (int i = 0; i < 20000; i++) {
            int a = i & 7, b = (i >> 3) & 1 ? a : a + 1;
            int r = f(a, b);
            if (r != (a == b ? 2 : 1))
                bad++;
        }
        printf("smc: round %d: %d wrong of 20000 (sink %d)\n", round, bad, sink);
    }
    printf("smc: done\n");
    for (;;)
        sleep_ms(1000);
}
