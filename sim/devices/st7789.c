#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "picocalc-sim/bus.h"
#include "picocalc-sim/devices.h"
#include "picocalc-sim/sim.h"

// The PicoBoy Color Plus's 240x280 IPS panel hangs off an ST7789 whose frame memory is 240
// columns by 320 rows; the panel shows rows 20 to 299 of it. Only the commands that change what
// the panel shows are modelled; the power, gamma and timing registers the init sequences also
// write are accepted and ignored. Derived from picocalc-sim's ST7365P model.

#define MEM_W 240
#define MEM_H 320
#define PANEL_Y0 20

enum {
    CMD_NOP = 0x00,
    CMD_SWRESET = 0x01,
    CMD_SLPIN = 0x10,
    CMD_SLPOUT = 0x11,
    CMD_INVOFF = 0x20,
    CMD_INVON = 0x21,
    CMD_DISPOFF = 0x28,
    CMD_DISPON = 0x29,
    CMD_CASET = 0x2A,
    CMD_RASET = 0x2B,
    CMD_RAMWR = 0x2C,
    CMD_RAMRD = 0x2E,
    CMD_VSCRDEF = 0x33,
    CMD_MADCTL = 0x36,
    CMD_VSCSAD = 0x37,
    CMD_COLMOD = 0x3A,
    CMD_RAMWRC = 0x3C,
    CMD_RAMRDC = 0x3E,
};

#define MADCTL_MY 0x80
#define MADCTL_MX 0x40
#define MADCTL_MV 0x20
#define MADCTL_BGR 0x08

// Every pbc-plus firmware sets MADCTL 0xC8 (MY|MX|BGR) with inversion on, and that is what
// shows the picture upright in true colour on the real panel; other settings are rendered
// relative to it. Rows 20..299 are then the visible ones, either way round.
#define MADCTL_NATIVE (MADCTL_MY | MADCTL_MX | MADCTL_BGR)

struct st7789 {
    pthread_mutex_t lock;
    uint16_t gram[MEM_H][MEM_W];
    unsigned dc_pin;

    uint8_t cmd;
    uint8_t params[16];
    int nparams;

    uint16_t xs, xe, ys, ye;
    uint16_t cx, cy;
    bool have_hi;
    uint8_t rgb[3];
    int nrgb;
    uint8_t hi;

    bool reading;
    bool read_dummy_done;
    bool read_lo_next;
    uint16_t read_pixel;

    uint8_t madctl;
    uint8_t colmod;
    bool inverted;
    bool sleeping;
    bool display_on;
    uint16_t tfa, vsa, bfa, vsp;

    // PBConPC: written by the emulation thread only, read by the window's; no lock (see lcd_xfer)
    volatile uint32_t generation;
};

static void reset_registers(st7789_t *lcd)
{
    lcd->cmd = CMD_NOP;
    lcd->nparams = 0;
    lcd->xs = 0;
    lcd->xe = MEM_W - 1;
    lcd->ys = 0;
    lcd->ye = MEM_H - 1;
    lcd->cx = lcd->cy = 0;
    lcd->have_hi = false;
    lcd->nrgb = 0;
    lcd->reading = false;
    lcd->madctl = 0;
    lcd->colmod = 0x66;
    lcd->inverted = false;
    lcd->sleeping = true;
    lcd->display_on = false;
    lcd->tfa = 0;
    lcd->vsa = MEM_H;
    lcd->bfa = 0;
    lcd->vsp = 0;
    lcd->generation++;
}

// Frame memory powers up holding noise, which is what a driver that skips its clear would show.
static void power_on_noise(st7789_t *lcd)
{
    uint32_t s = 0x12345678u;
    for (int y = 0; y < MEM_H; y++)
        for (int x = 0; x < MEM_W; x++) {
            s = s * 1664525u + 1013904223u;
            lcd->gram[y][x] = (uint16_t)(s >> 16);
        }
}

// Maps the address counter through the row/column exchange and mirror bits, relative to the
// orientation the panel is mounted in.
static bool to_memory(const st7789_t *lcd, unsigned col, unsigned row, unsigned *mx, unsigned *my)
{
    uint8_t m = lcd->madctl ^ MADCTL_NATIVE;
    if (m & MADCTL_MV) {
        unsigned t = col;
        col = row;
        row = t;
    }
    if (col >= MEM_W || row >= MEM_H)
        return false;
    *mx = (m & MADCTL_MX) ? MEM_W - 1 - col : col;
    *my = (m & MADCTL_MY) ? MEM_H - 1 - row : row;
    return true;
}

static void advance(st7789_t *lcd)
{
    if (lcd->cx < lcd->xe) {
        lcd->cx++;
        return;
    }
    lcd->cx = lcd->xs;
    lcd->cy = lcd->cy < lcd->ye ? lcd->cy + 1 : lcd->ys;
}

uint64_t pbc_lcd_pixels, pbc_lcd_ramwr;   // PBConPC measurement (PBCONPC_STATS)

static void write_pixel(st7789_t *lcd, uint16_t color)
{
    pbc_lcd_pixels++;
    unsigned mx, my;
    if (to_memory(lcd, lcd->cx, lcd->cy, &mx, &my))
        lcd->gram[my][mx] = color;
    advance(lcd);
}

static uint16_t read_pixel(st7789_t *lcd)
{
    unsigned mx, my;
    uint16_t c = to_memory(lcd, lcd->cx, lcd->cy, &mx, &my) ? lcd->gram[my][mx] : 0;
    advance(lcd);
    return c;
}

static void command(st7789_t *lcd, uint8_t cmd)
{
    lcd->cmd = cmd;
    lcd->nparams = 0;
    lcd->have_hi = false;
    lcd->nrgb = 0;
    lcd->reading = false;
    switch (cmd) {
    case CMD_SWRESET:
        reset_registers(lcd);
        break;
    case CMD_SLPIN:
        lcd->sleeping = true;
        break;
    case CMD_SLPOUT:
        lcd->sleeping = false;
        break;
    case CMD_INVOFF:
        lcd->inverted = false;
        break;
    case CMD_INVON:
        lcd->inverted = true;
        break;
    case CMD_DISPOFF:
        lcd->display_on = false;
        break;
    case CMD_DISPON:
        lcd->display_on = true;
        break;
    case CMD_RAMWR:
        pbc_lcd_ramwr++;
        // fall through
    case CMD_RAMRD:
        lcd->cx = lcd->xs;
        lcd->cy = lcd->ys;
        // fall through
    case CMD_RAMRDC:
        lcd->reading = cmd != CMD_RAMWR;
        lcd->read_dummy_done = false;
        lcd->read_lo_next = false;
        break;
    default:
        break;
    }
    lcd->generation++;
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static void parameter(st7789_t *lcd, uint8_t b)
{
    if (lcd->cmd == CMD_RAMWR || lcd->cmd == CMD_RAMWRC) {
        if ((lcd->colmod & 0x07) == 0x06) {
            // 18-bit pixels arrive as three bytes, R, G and B in their top six bits; the frame
            // memory is kept at 16 bits, which is all the panel's colours this model shows.
            lcd->rgb[lcd->nrgb++] = b;
            if (lcd->nrgb == 3) {
                write_pixel(lcd, (uint16_t)(((lcd->rgb[0] >> 3) << 11) | ((lcd->rgb[1] >> 2) << 5) | (lcd->rgb[2] >> 3)));
                lcd->nrgb = 0;
                lcd->generation++;
            }
            return;
        }
        if (!lcd->have_hi) {
            lcd->hi = b;
            lcd->have_hi = true;
        } else {
            write_pixel(lcd, (uint16_t)(lcd->hi << 8 | b));
            lcd->have_hi = false;
    lcd->nrgb = 0;
            lcd->generation++;
        }
        return;
    }
    if (lcd->nparams < (int)sizeof(lcd->params))
        lcd->params[lcd->nparams++] = b;
    const uint8_t *p = lcd->params;
    switch (lcd->cmd) {
    case CMD_CASET:
        if (lcd->nparams == 4) {
            lcd->xs = be16(p);
            lcd->xe = be16(p + 2);
        }
        break;
    case CMD_RASET:
        if (lcd->nparams == 4) {
            lcd->ys = be16(p);
            lcd->ye = be16(p + 2);
        }
        break;
    case CMD_MADCTL:
        lcd->madctl = p[0];
        lcd->generation++;
        break;
    case CMD_COLMOD:
        lcd->colmod = p[0];
        if ((p[0] & 0x07) != 0x05 && (p[0] & 0x07) != 0x06)
            picocalc_sim_log("LCD: COLMOD 0x%02x set; only 16- and 18-bit pixels are modelled", p[0]);
        break;
    case CMD_VSCRDEF:
        if (lcd->nparams == 6) {
            lcd->tfa = be16(p);
            lcd->vsa = be16(p + 2);
            lcd->bfa = be16(p + 4);
            lcd->generation++;
        }
        break;
    case CMD_VSCSAD:
        if (lcd->nparams == 2) {
            lcd->vsp = be16(p);
            lcd->generation++;
        }
        break;
    default:
        break;
    }
}

// A read returns one dummy byte, then each pixel as big-endian RGB565.
static uint8_t read_byte(st7789_t *lcd)
{
    if (!lcd->read_dummy_done) {
        lcd->read_dummy_done = true;
        return 0x00;
    }
    if (!lcd->read_lo_next) {
        lcd->read_pixel = read_pixel(lcd);
        lcd->read_lo_next = true;
        return (uint8_t)(lcd->read_pixel >> 8);
    }
    lcd->read_lo_next = false;
    return (uint8_t)lcd->read_pixel;
}

static uint8_t lcd_xfer(void *ctx, uint8_t mosi)
{
    st7789_t *lcd = ctx;
    bool data = picocalc_sim_gpio_level(lcd->dc_pin);
    // PBConPC: no lock. Only the emulation thread changes the controller; the window reads the
    // frame memory as the real panel scans it, and at worst shows a frame half old (tearing,
    // which the real panel shows too). A mutex here, twice per pixel, took a tenth of the host.
    uint8_t miso = 0xff;
    if (!data)
        command(lcd, mosi);
    else if (lcd->reading)
        miso = read_byte(lcd);
    else
        parameter(lcd, mosi);
    return miso;
}

// PBConPC: a DMA run of data bytes at once (pl022_bulk_tx), exactly as lcd_xfer one byte at a
// time: data mode, not reading, so each byte is a parameter and 0xff comes back.
static bool lcd_bulk(void *ctx, const uint8_t *src, size_t n, unsigned size, unsigned bits)
{
    st7789_t *lcd = ctx;
    static int on = -1;   // PBCONPC_NO_LCDBULK=1: byte by byte (measurements)
    if (on < 0)
        on = !getenv("PBCONPC_NO_LCDBULK");
    if (!on || !picocalc_sim_gpio_level(lcd->dc_pin) || lcd->reading)
        return false;
    if (size == 1 && bits == 8) {
        for (size_t i = 0; i < n; i++)
            parameter(lcd, src[i]);
    } else {
        for (size_t i = 0; i < n; i++) {
            parameter(lcd, src[2 * i + 1]);
            parameter(lcd, src[2 * i]);
        }
    }
    return true;
}

// Raising CSX resets the serial interface mid-byte and ends a memory read; a write's command
// state survives, which is why drivers may pulse CSX between parameters.
static void lcd_select(void *ctx, bool selected)
{
    st7789_t *lcd = ctx;
    if (selected)
        return;
    lcd->have_hi = false;
    lcd->nrgb = 0;
    lcd->reading = false;
}

static void lcd_reset_pin(void *ctx, unsigned pin, bool level)
{
    (void)pin;
    st7789_t *lcd = ctx;
    if (!level)
        reset_registers(lcd);
}

st7789_t *st7789_create(picocalc_sim_spi_pins_t pins, unsigned dc_pin, unsigned rst_pin)
{
    st7789_t *lcd = calloc(1, sizeof(*lcd));
    pthread_mutex_init(&lcd->lock, NULL);
    lcd->dc_pin = dc_pin;
    reset_registers(lcd);
    power_on_noise(lcd);

    picocalc_sim_spi_device_t *dev = calloc(1, sizeof(*dev));
    dev->name = "ST7789 LCD";
    dev->sck_pin = pins.sck;
    dev->mosi_pin = pins.mosi;
    dev->miso_pin = pins.miso;
    dev->cs_pin = pins.cs;
    dev->select = lcd_select;
    dev->xfer = lcd_xfer;
    dev->ctx = lcd;
    dev->xfer_bulk = lcd_bulk;
    picocalc_sim_spi_attach(dev);
    picocalc_sim_gpio_watch(rst_pin, lcd_reset_pin, lcd);
    return lcd;
}

void st7789_destroy(st7789_t *lcd)
{
    pthread_mutex_destroy(&lcd->lock);
    free(lcd);
}

uint32_t st7789_generation(st7789_t *lcd)
{
    return lcd->generation;
}

// VSCRDEF splits the 320 lines the controller scans into top fixed, scrolling and bottom fixed
// bands, and VSCSAD names the row shown at the top of the scrolling band. The datasheet asks for
// TFA + VSA + BFA = 320; for other settings the scrolling band still ends at TFA + VSA here, but
// wraps over all 320 - TFA - BFA unfixed rows, which is unverified on hardware.
static unsigned panel_line_to_row(const st7789_t *lcd, unsigned line)
{
    unsigned tfa = lcd->tfa, bfa = lcd->bfa;
    if (tfa + bfa >= MEM_H)
        return line;
    if (line < tfa || line >= (unsigned)tfa + lcd->vsa)
        return line;
    unsigned span = MEM_H - tfa - bfa;
    unsigned vsp = lcd->vsp >= tfa ? lcd->vsp - tfa : 0;
    return tfa + (vsp + (line - tfa)) % span;
}

static uint32_t to_argb(const st7789_t *lcd, uint16_t px, uint32_t scale)
{
    // INVON is how this panel shows true colour, so leaving it off is what inverts the picture.
    if (!lcd->inverted)
        px = (uint16_t)~px;
    // With BGR set the controller takes blue from the top five bits (pbc-plus color565.h).
    uint32_t r = (px >> 11) & 0x1f, g = (px >> 5) & 0x3f, b = px & 0x1f;
    if (lcd->madctl & MADCTL_BGR) {
        uint32_t t = r;
        r = b;
        b = t;
    }
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    r = r * scale / 255;
    g = g * scale / 255;
    b = b * scale / 255;
    return 0xff000000u | r << 16 | g << 8 | b;
}

void st7789_render(st7789_t *lcd, uint32_t *argb, uint8_t backlight)
{
    if (lcd->sleeping || !lcd->display_on || backlight == 0) {
        for (int i = 0; i < PICOCALC_SIM_LCD_WIDTH * PICOCALC_SIM_LCD_HEIGHT; i++)
            argb[i] = 0xff000000u;
        return;
    }
    // Brightness follows the backlight PWM loosely: the panel looks far less dimmed to the eye
    // than the duty suggests, and a picture on a PC screen should stay readable.
    uint32_t scale = 128 + (uint32_t)backlight * 127 / 255;
    for (unsigned line = 0; line < PICOCALC_SIM_LCD_HEIGHT; line++) {
        const uint16_t *row = lcd->gram[panel_line_to_row(lcd, line + PANEL_Y0)];
        uint32_t *out = argb + line * PICOCALC_SIM_LCD_WIDTH;
        for (unsigned x = 0; x < PICOCALC_SIM_LCD_WIDTH; x++)
            out[x] = to_argb(lcd, row[x], scale);
    }
    
}
