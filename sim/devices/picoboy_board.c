#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "picocalc-sim/bus.h"
#include "picocalc-sim/devices.h"
#include "picocalc-sim/keys.h"
#include "picocalc-sim/sim.h"

// The PicoBoy Color Plus board (PBConPC), in place of picocalc-sim's PicoCalc mainboard. Pins as
// in docs/HARDWARE.md section 2: ST7789 on SPI0 (SCK 18, MOSI 19, CS 10, DC 8, RST 9), backlight
// PWM on GP26, speaker PWM on GP15, buttons to ground on GP0-4, 27 and 28, LEDs on GP12-14.

picocalc_t picocalc_sim_picocalc;

enum {
    PIN_CENTER = 0,
    PIN_RIGHT = 1,
    PIN_DOWN = 2,
    PIN_LEFT = 3,
    PIN_UP = 4,
    PIN_A = 27,
    PIN_B = 28,
    PIN_BACKLIGHT = 26,
    PIN_SPEAKER = 15,
    PIN_LED_GREEN = 12,
};

static const unsigned button_pins[] = {PIN_CENTER, PIN_RIGHT, PIN_DOWN, PIN_LEFT, PIN_UP, PIN_A, PIN_B};

// The WS2812 in the "+" of the PBC+ lettering on GP11 (pbc-plus common/pbc_rgb.c): 24 bits GRB,
// MSB first, each a high pulse of about 0.25 us (0) or 0.875 us (1) in 1.25 us; more than 50 us
// low latches. Only the first LED's 24 bits count; there is one.
#define PIN_RGB 11
#define PS_PER_US_ 1000000ull
static struct {
    uint64_t rise, fall;
    uint32_t bits;
    int n;
} ws;
static _Atomic uint32_t rgb_now;

static void ws2812_edge(void *ctx, unsigned pin, bool level)
{
    (void)ctx;
    (void)pin;
    uint64_t t = picocalc_sim_gpio_edge_time();
    if (level) {
        if (t - ws.fall > 50 * PS_PER_US_)
            ws.n = ws.bits = 0;
        ws.rise = t;
        return;
    }
    uint64_t high = t - ws.rise;
    ws.fall = t;
    if (ws.n >= 24 || high > 5 * PS_PER_US_)
        return;
    ws.bits = ws.bits << 1 | (high > 560000ull);
    if (++ws.n == 24) {
        uint32_t g = ws.bits >> 16 & 0xff, r = ws.bits >> 8 & 0xff, b = ws.bits & 0xff;
        atomic_store(&rgb_now, r << 16 | g << 8 | b);
    }
}

uint32_t picoboy_rgb(void)
{
    return atomic_load(&rgb_now);
}

// The STK8BA58 accelerometer on I2C0 (SDA 20, SCL 21, address 0x18), as pbc-plus's drivers use it
// (a2600/src/tilt.c, micropython .../pbc_hw/accel.c): chip ID 87h in register 00h, X, Y, Z in
// 02h..07h as 10-bit values left-aligned in each LSB/MSB pair, range in 0Fh (3: 2 g, 5: 4 g,
// 8: 8 g), anything else a plain register. The chip sits turned on the board: its X is the
// device's -Y, its Y the device's X, its Z the device's -Z, so that the drivers' corrections give
// back the device's tilt as the window sets it (picoboy_set_tilt, in g; flat: 0, 0, 1).
#define PIN_I2C_SDA 20
#define PIN_I2C_SCL 21
static struct {
    uint8_t regs[64];
    uint8_t ptr;
} acc;
static _Atomic uint32_t tilt_g[3] = {0, 0, 0x3f800000u};   // float bits; flat to begin with
static atomic_bool tilt_used;

static float tilt_axis(int i)
{
    uint32_t u = atomic_load(&tilt_g[i]);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

void picoboy_set_tilt(float x, float y, float z)
{
    float v[3] = {x, y, z};
    for (int i = 0; i < 3; i++) {
        uint32_t u;
        memcpy(&u, &v[i], 4);
        atomic_store(&tilt_g[i], u);
    }
}

void picoboy_get_tilt(float *x, float *y, float *z)
{
    *x = tilt_axis(0);
    *y = tilt_axis(1);
    *z = tilt_axis(2);
}

bool picoboy_tilt_used(void)
{
    return atomic_load(&tilt_used);
}

static int16_t acc_counts(float g)
{
    unsigned range = acc.regs[0x0f] == 0x05 ? 4 : acc.regs[0x0f] == 0x08 ? 8 : acc.regs[0x0f] == 0x0c ? 16 : 2;
    float c = g * 512.0f / (float)range;
    return (int16_t)(c > 511.0f ? 511 : c < -512.0f ? -512 : (c < 0 ? c - 0.5f : c + 0.5f));
}

static void acc_latch(void)
{
    // chip axes from the device's
    int16_t v[3] = {acc_counts(-tilt_axis(1)), acc_counts(tilt_axis(0)), acc_counts(-tilt_axis(2))};
    for (int i = 0; i < 3; i++) {
        uint16_t w = (uint16_t)(v[i] << 6);
        acc.regs[2 + 2 * i] = (uint8_t)(w & 0xc0) | 1;   // bit 0: new data
        acc.regs[3 + 2 * i] = (uint8_t)(w >> 8);
    }
}

static int acc_write(void *ctx, const uint8_t *src, size_t len, bool nostop)
{
    (void)ctx;
    (void)nostop;
    if (!len)
        return 0;
    acc.ptr = src[0] & 63;
    for (size_t i = 1; i < len; i++) {
        uint8_t r = acc.ptr;
        if (r != 0x00 && (r < 0x02 || r > 0x07))   // ID and data are read-only
            acc.regs[r] = src[i];
        acc.ptr = (acc.ptr + 1) & 63;
    }
    atomic_store(&tilt_used, true);
    return (int)len;
}

static int acc_read(void *ctx, uint8_t *dst, size_t len, bool nostop)
{
    (void)ctx;
    (void)nostop;
    acc_latch();
    for (size_t i = 0; i < len; i++) {
        dst[i] = acc.regs[acc.ptr];
        acc.ptr = (acc.ptr + 1) & 63;
    }
    atomic_store(&tilt_used, true);
    return (int)len;
}

static void acc_attach(void)
{
    memset(&acc, 0, sizeof(acc));
    acc.regs[0x00] = 0x87;
    acc.regs[0x0f] = 0x03;
    acc.regs[0x10] = 0x0f;
    atomic_store(&tilt_used, false);
    picocalc_sim_i2c_device_t *dev = calloc(1, sizeof(*dev));
    dev->name = "STK8BA58";
    dev->sda_pin = PIN_I2C_SDA;
    dev->scl_pin = PIN_I2C_SCL;
    dev->addr = 0x18;
    dev->write = acc_write;
    dev->read = acc_read;
    picocalc_sim_i2c_attach(dev);
}

bool picocalc_board_init(void)
{
    picocalc_t *b = &picocalc_sim_picocalc;
    // MISO is not wired; 16 is a pin nothing on the board uses.
    b->lcd = st7789_create((picocalc_sim_spi_pins_t){.sck = 18, .mosi = 19, .miso = 16, .cs = 10}, 8, 9);
    // One speaker: the same pin on both channels gives mono.
    b->audio = picocalc_audio_create(PIN_SPEAKER, PIN_SPEAKER);
    // Released buttons read high (pulled up), as on the board.
    for (unsigned i = 0; i < sizeof(button_pins) / sizeof(button_pins[0]); i++)
        picocalc_sim_gpio_drive_input_locked(button_pins[i], 1);
    // a power cycle leaves the LED dark; a chip reset (the machine restarting) is not one
    atomic_store(&rgb_now, 0);
    ws.n = ws.bits = 0;
    picocalc_sim_gpio_watch(PIN_RGB, ws2812_edge, NULL);
    acc_attach();
    return !picocalc_sim_opts.wav_path || picocalc_audio_record(b->audio, picocalc_sim_opts.wav_path);
}

void picocalc_board_free(void)
{
    picocalc_t *b = &picocalc_sim_picocalc;
    if (b->lcd)
        st7789_destroy(b->lcd);
    if (b->audio)
        picocalc_audio_destroy(b->audio);
    *b = (picocalc_t){0};
}

static int key_pin(uint8_t key)
{
    switch (key) {
    case PC_KEY_UP: return PIN_UP;
    case PC_KEY_DOWN: return PIN_DOWN;
    case PC_KEY_LEFT: return PIN_LEFT;
    case PC_KEY_RIGHT: return PIN_RIGHT;
    case 'a':
    case PC_KEY_ENTER: return PIN_CENTER;
    case 's': return PIN_A;
    case 'd': return PIN_B;
    default: return -1;
    }
}

void picoboy_key(uint8_t key, bool pressed)
{
    int pin = key_pin(key);
    if (pin >= 0 && picocalc_sim_picocalc.lcd)
        picocalc_sim_gpio_drive_input_locked((unsigned)pin, pressed ? 0 : 1);
}

uint8_t picoboy_backlight(void)
{
    return (uint8_t)(picocalc_sim_gpio_duty(PIN_BACKLIGHT) >> 8);
}

unsigned picoboy_leds(void)
{
    unsigned v = 0;
    for (unsigned i = 0; i < 3; i++)
        v |= (unsigned)picocalc_sim_gpio_level(PIN_LED_GREEN + i) << i;
    return v;
}

void picoboy_set_battery(uint8_t percent, bool charging)
{
    // The battery divider on GP29 (ADC 3) is not modelled yet.
    (void)percent;
    (void)charging;
}
