// cornertest -- shows the PicoBoy Color Plus panel's visible area for a camera, controlled over
// the USB serial port (tools/cornertest_run.py drives it and takes the photos):
//   g          grey everywhere (white overexposes the camera), the outermost row/column of each
//              side in its own colour (top red, bottom blue, left green, right magenta), in each
//              corner a diagonal from the corner pixel inwards
//   r N        arcs: grey middle for aligning the photo, black corner squares 72 px, in each a
//              quarter circle: the corner curve of a rounded rectangle of radius N
//   b N        backlight N percent
//   boot       back to BOOTSEL
// Each command answers "ok ..." once the panel shows it. Without commands for 120 s it goes back
// to BOOTSEL by itself. Panel set up as pbc-plus does (MADCTL 0xC8, BGR, INVON).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"

#define W 240
#define H 280
#define YOFF 20
#define PIN_SCK 18
#define PIN_MOSI 19
#define PIN_CS 10
#define PIN_DC 8
#define PIN_RST 9
#define PIN_BL 26

static uint16_t rgb(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((b & 0xf8) << 8) | ((g & 0xfc) << 3) | (r >> 3));
}

static void cmd(uint8_t c, const uint8_t *d, int n)
{
    gpio_put(PIN_CS, 0);
    gpio_put(PIN_DC, 0);
    spi_write_blocking(spi0, &c, 1);
    gpio_put(PIN_DC, 1);
    if (n)
        spi_write_blocking(spi0, d, (size_t)n);
    gpio_put(PIN_CS, 1);
}

static uint16_t fb[W * H];

static void put(int x, int y, uint16_t c)
{
    if (x >= 0 && x < W && y >= 0 && y < H)
        fb[y * W + x] = c;
}

static void put4(int x, int y, uint16_t c)
{
    put(x, y, c);
    put(W - 1 - x, y, c);
    put(x, H - 1 - y, c);
    put(W - 1 - x, H - 1 - y, c);
}

static void show(void)
{
    uint8_t win[4] = {0, 0, 0, W - 1};
    cmd(0x2a, win, 4);
    uint8_t rows[4] = {0, YOFF, (uint8_t)((YOFF + H - 1) >> 8), (uint8_t)(YOFF + H - 1)};
    cmd(0x2b, rows, 4);
    uint8_t c = 0x2c;
    gpio_put(PIN_CS, 0);
    gpio_put(PIN_DC, 0);
    spi_write_blocking(spi0, &c, 1);
    gpio_put(PIN_DC, 1);
    for (int i = 0; i < W * H; i++) {
        uint8_t px[2] = {(uint8_t)(fb[i] >> 8), (uint8_t)fb[i]};
        spi_write_blocking(spi0, px, 2);
    }
    gpio_put(PIN_CS, 1);
}

static const uint16_t BLACK = 0;

static void pattern_grey(void)
{
    uint16_t grey = rgb(110, 110, 110), red = rgb(255, 0, 0), blue = rgb(0, 0, 255);
    for (int i = 0; i < W * H; i++)
        fb[i] = grey;
    for (int x = 0; x < W; x++) {
        put(x, 0, red);
        put(x, H - 1, blue);
    }
    for (int y = 0; y < H; y++) {
        put(0, y, rgb(0, 255, 0));
        put(W - 1, y, rgb(255, 0, 255));
    }
    for (int k = 0; k < 48; k++)
        for (int s = -2; s <= 2; s++)
            put4(k + s, k, s == 0 ? ((k / 2) & 1 ? blue : red) : BLACK);
}

static void pattern_arcs(int r)
{
    uint16_t grey = rgb(110, 110, 110), white = rgb(255, 255, 255);
    for (int i = 0; i < W * H; i++)
        fb[i] = grey;
    for (int y = 0; y < 72; y++)
        for (int x = 0; x < 72; x++)
            put4(x, y, BLACK);
    // the corner curve of radius r: centre (r, r) from the corner, 1.5 px wide
    for (int y = 0; y < 72; y++)
        for (int x = 0; x < 72; x++) {
            float dx = (float)r - (float)x - 0.5f, dy = (float)r - (float)y - 0.5f;
            if (dx < 0 || dy < 0)
                continue;
            float d = __builtin_sqrtf(dx * dx + dy * dy);
            if (d > r - 0.75f && d < r + 0.75f)
                put4(x, y, white);
        }
}

static void backlight(int pct)
{
    pwm_set_gpio_level(PIN_BL, (uint16_t)(65535u * (unsigned)pct / 100u));
}

int main(void)
{
    stdio_init_all();
    spi_init(spi0, 40 * 1000 * 1000);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    for (int p = PIN_DC; p <= PIN_CS; p++) {
        gpio_init((unsigned)p);
        gpio_set_dir((unsigned)p, GPIO_OUT);
        gpio_put((unsigned)p, 1);
    }
    gpio_set_function(PIN_BL, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PIN_BL);
    pwm_set_wrap(slice, 65535);
    pwm_set_enabled(slice, true);
    backlight(35);

    gpio_put(PIN_RST, 0);
    sleep_ms(10);
    gpio_put(PIN_RST, 1);
    sleep_ms(120);
    cmd(0x01, NULL, 0);
    sleep_ms(150);
    cmd(0x11, NULL, 0);
    sleep_ms(120);
    uint8_t v = 0x55;
    cmd(0x3a, &v, 1);
    v = 0xc8;
    cmd(0x36, &v, 1);
    cmd(0x21, NULL, 0);
    cmd(0x13, NULL, 0);
    cmd(0x29, NULL, 0);
    pattern_grey();
    show();

    char line[64];
    int n = 0;
    absolute_time_t idle_until = make_timeout_time_ms(120000);
    for (;;) {
        int ch = getchar_timeout_us(100000);
        if (ch == PICO_ERROR_TIMEOUT) {
            if (absolute_time_diff_us(get_absolute_time(), idle_until) < 0)
                break;
            continue;
        }
        if (ch != '\n' && ch != '\r') {
            if (n < (int)sizeof(line) - 1)
                line[n++] = (char)ch;
            continue;
        }
        line[n] = 0;
        n = 0;
        idle_until = make_timeout_time_ms(120000);
        if (!strcmp(line, "g")) {
            pattern_grey();
            show();
            printf("ok grey\n");
        } else if (line[0] == 'r' && line[1] == ' ') {
            int r = atoi(line + 2);
            pattern_arcs(r);
            show();
            printf("ok r %d\n", r);
        } else if (line[0] == 'b' && line[1] == ' ') {
            backlight(atoi(line + 2));
            printf("ok b\n");
        } else if (!strcmp(line, "boot")) {
            printf("ok boot\n");
            sleep_ms(100);
            break;
        } else if (line[0]) {
            printf("? %s\n", line);
        }
    }
    rom_reset_usb_boot(0, 0);
    return 0;
}
