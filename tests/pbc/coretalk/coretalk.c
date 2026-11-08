// coretalk: see CMakeLists.txt. Prints one line per test, then repeats every few seconds.
#include <stdio.h>

#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#define ROUNDS 20000

static volatile uint32_t flag_ping, flag_pong;
static volatile uint32_t counter, per_core[2];
static volatile bool contend_run;
static spin_lock_t *lock;

static void core1_main(void)
{
    for (;;) {
        uint32_t cmd = multicore_fifo_pop_blocking();
        if (cmd == 1) {   // FIFO ping-pong
            for (int i = 0; i < ROUNDS; i++)
                multicore_fifo_push_blocking(multicore_fifo_pop_blocking() + 1);
        } else if (cmd == 2) {   // RAM flag ping-pong
            for (uint32_t i = 1; i <= ROUNDS; i++) {
                while (flag_ping != i)
                    tight_loop_contents();
                flag_pong = i;
            }
        } else if (cmd == 3) {   // contended spinlock
            while (contend_run) {
                uint32_t s = spin_lock_blocking(lock);
                counter++;
                per_core[1]++;
                spin_unlock(lock, s);
            }
            multicore_fifo_push_blocking(0);
        }
    }
}

int main(void)
{
    stdio_init_all();
    lock = spin_lock_init(spin_lock_claim_unused(true));
    multicore_launch_core1(core1_main);
    sleep_ms(300);
    for (int run = 0; run < 3; run++) {
        // 1: FIFO
        multicore_fifo_push_blocking(1);
        uint64_t t0 = time_us_64();
        uint32_t v = 0;
        for (int i = 0; i < ROUNDS; i++) {
            multicore_fifo_push_blocking(v);
            v = multicore_fifo_pop_blocking();
        }
        uint64_t t_fifo = time_us_64() - t0;
        // 2: RAM flag
        flag_ping = flag_pong = 0;
        multicore_fifo_push_blocking(2);
        t0 = time_us_64();
        for (uint32_t i = 1; i <= ROUNDS; i++) {
            flag_ping = i;
            while (flag_pong != i)
                tight_loop_contents();
        }
        uint64_t t_flag = time_us_64() - t0;
        // 3: spinlock, 20 ms
        counter = per_core[0] = per_core[1] = 0;
        contend_run = true;
        multicore_fifo_push_blocking(3);
        t0 = time_us_64();
        while (time_us_64() - t0 < 20000) {
            uint32_t s = spin_lock_blocking(lock);
            counter++;
            per_core[0]++;
            spin_unlock(lock, s);
        }
        contend_run = false;
        multicore_fifo_pop_blocking();
        printf("coretalk: fifo %.3f us/round  flag %.3f us/round  spinlock %lu+%lu in 20 ms  (v=%lu)\n",
               (double)t_fifo / ROUNDS, (double)t_flag / ROUNDS, (unsigned long)per_core[0],
               (unsigned long)per_core[1], (unsigned long)v);
    }
    printf("coretalk: done\n");
    for (;;)
        sleep_ms(1000);
}
