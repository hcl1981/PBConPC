// tilttest: see CMakeLists.txt.
#include <stdio.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"

#define ADDR 0x18

static int reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    int r = i2c_write_blocking(i2c0, ADDR, &reg, 1, true);
    return r < 0 ? r : i2c_read_blocking(i2c0, ADDR, buf, len, false);
}

static int16_t axis(uint8_t lsb)
{
    uint8_t b[2] = {0};
    reg_read(lsb, b, 2);
    return (int16_t)((int16_t)(b[1] << 8 | b[0]) >> 6);
}

int main(void)
{
    stdio_init_all();
    i2c_init(i2c0, 400000);
    gpio_set_function(20, GPIO_FUNC_I2C);
    gpio_set_function(21, GPIO_FUNC_I2C);
    gpio_pull_up(20);
    gpio_pull_up(21);
    uint8_t id = 0;
    int r = reg_read(0x00, &id, 1);
    uint8_t cfg[] = {0x0f, 0x03};
    i2c_write_blocking(i2c0, ADDR, cfg, 2, false);
    printf("tilttest: id %02x (%d)\n", id, r);
    for (int i = 0; i < 40; i++) {
        float x = axis(0x04) * 2.0f / 512, y = -axis(0x02) * 2.0f / 512, z = -axis(0x06) * 2.0f / 512;
        printf("tilttest: %d x %+.2f y %+.2f z %+.2f\n", i, x, y, z);
        sleep_ms(100);
    }
    for (;;)
        sleep_ms(1000);
}
