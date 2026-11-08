#include "skin.h"

#ifdef PICOCALC_SIM_HAVE_SDL

#include <math.h>
#include <stdlib.h>

#include "picocalc-sim/devices.h"
#include "picocalc-sim/keys.h"
#include "picocalc-sim/sim.h"

extern const uint8_t pbc_front_png[];
extern const unsigned pbc_front_png_size;

bool skin_enabled = true;

// Geometry in the image's pixels (800 x 1373, 21.33 px per mm), from the render's camera:
// the LCD's active area of 27.97 x 32.63 mm, and the parts' positions from pbc.kicad_pcb
// (see assets/README.md).
#define SKIN_W 800.0f
#define SKIN_H 1373.0f
static const SDL_FRect screen = {101.7f, 158.3f, 596.6f, 696.0f};
static const float joy_x = 197.5f, joy_y = 1174.2f;
static const float btn_a_x = 482.1f, btn_b_x = 638.7f, btn_y = 1174.2f, btn_r = 50.0f;
static const float led_x = 45.1f, led_y[3] = {732.3f, 508.0f, 284.4f};   // green, yellow, red
static const float rgb_x = 424.2f, rgb_y = 962.3f;                           // the "+" of PBC+

static SDL_Texture *frame, *glow, *tint, *corners, *frame_corners;

// The visible area's corners are rounded: radius 42 panel pixels, measured on a PicoBoy Color
// Plus (tools/cornertest_run.py, 2026-11-08): quarter circles of radius r shown in the corners
// are hidden in their middle up to r = 41 (40 % visible), 42 (73 %), fully visible from 43, the
// same in all four corners. (Fitting the edge of a grey area in the photos gave 52: the camera's
// blur rounds corners further.) The emulator shows the panel straight, whatever one board's glueing.
#define CORNER_RADIUS 42.0f
#define CORNER_SS 4   // the mask is made at 4 times the LCD's resolution, for smooth edges

bool skin_init(SDL_Renderer *renderer)
{
    SDL_IOStream *io = SDL_IOFromConstMem(pbc_front_png, pbc_front_png_size);
    SDL_Surface *s = io ? SDL_LoadPNG_IO(io, true) : NULL;
    if (!s) {
        picocalc_sim_log("device frame: %s", SDL_GetError());
        skin_enabled = false;
        return false;
    }
    {
        // The panel's glass, inside the white edge, is evenly black on the real PicoBoy; the
        // render has a grey strip at its top and the display module's own rounded corners below
        // it, which showed as steps by the LCD's top corners. Fill it from the middle: every dark
        // pixel reached without crossing the white edge becomes the glass's black.
        SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888);
        if (c) {
            SDL_DestroySurface(s);
            s = c;
            int w = s->w, h = s->h;
            uint8_t *seen = calloc((size_t)w * h, 1);
            int *stack = malloc(sizeof(int) * (size_t)w * h);
            if (seen && stack) {
                int sp = 0, start = (int)(screen.y + screen.h / 2) * w + (int)(screen.x + screen.w / 2);
                stack[sp++] = start;
                seen[start] = 1;
                while (sp) {
                    int i = stack[--sp], x = i % w, y = i / w;
                    uint32_t *px = (uint32_t *)((uint8_t *)s->pixels + y * s->pitch) + x;
                    unsigned r = *px >> 16 & 0xff, g = *px >> 8 & 0xff, b = *px & 0xff;
                    // the white edge's outside is a rounded rectangle, x 78..721, y 77..882, radius
                    // 90 (measured in the render): stay 4 pixels inside it, also where the edge's
                    // anti-aliasing leaves dark gaps in its arcs
                    if (x < 82 || x > 717 || y < 81 || y > 878)
                        continue;
                    float cx = x < 168 ? 168 - x : x > 631 ? x - 631 : 0;
                    float cy = y < 167 ? 167 - y : y > 792 ? y - 792 : 0;
                    if (cx * cx + cy * cy > 86.0f * 86.0f)
                        continue;
                    bool inside_lcd = x >= screen.x && x < screen.x + screen.w && y >= screen.y && y < screen.y + screen.h;
                    // dark pixels, or anything under the LCD (the render's screen shows text there)
                    if (!inside_lcd && (r * 3 + g * 6 + b) / 10 > 75)
                        continue;
                    *px = (*px & 0xff000000u) | 0x0a0a0cu;
                    const int nb[4] = {i - 1, i + 1, i - w, i + w};
                    const bool ok[4] = {x > 0, x < w - 1, y > 0, y < h - 1};
                    for (int k = 0; k < 4; k++)
                        if (ok[k] && !seen[nb[k]]) {
                            seen[nb[k]] = 1;
                            stack[sp++] = nb[k];
                        }
                }
            }
            free(seen);
            free(stack);
        }
    }
    frame = SDL_CreateTextureFromSurface(renderer, s);
    if (frame)
        SDL_SetTextureScaleMode(frame, SDL_SCALEMODE_LINEAR);
    // The render's own pixels where the panel's rounded corners leave the LCD rectangle (the
    // dark glass and, at the bottom, its white edge), to lay over the LCD: transparent
    // elsewhere, the share outside the rounding as alpha.
    SDL_Surface *fc = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(s);
    if (fc) {
        float r = CORNER_RADIUS * screen.w / PICOCALC_SIM_LCD_WIDTH;
        float x0 = screen.x, y0 = screen.y, x1 = screen.x + screen.w, y1 = screen.y + screen.h;
        for (int y = 0; y < fc->h; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)fc->pixels + y * fc->pitch);
            for (int x = 0; x < fc->w; x++) {
                float out = 0;
                // a few pixels beyond the LCD too: scaled down, the window mixes neighbours,
                // and a transparent ring there would let the LCD's edge show through
                if (x + 5 > x0 && x < x1 + 4 && y + 5 > y0 && y < y1 + 4) {
                    int n = 0;
                    for (int sy = 0; sy < 4; sy++)
                        for (int sx = 0; sx < 4; sx++) {
                            float px = x + 0.125f + sx * 0.25f, py = y + 0.125f + sy * 0.25f;
                            if (px < x0 || px > x1 || py < y0 || py > y1) {
                                // outside the LCD: the render shows there anyway; in a corner
                                // the overlay must cover the LCD's cut-off edge pixel fully
                                bool corner = (px < x0 + r || px > x1 - r) && (py < y0 + r || py > y1 - r);
                                n += corner;
                                continue;
                            }
                            float cx = px < x0 + r ? x0 + r - px : px > x1 - r ? px - (x1 - r) : 0;
                            float cy = py < y0 + r ? y0 + r - py : py > y1 - r ? py - (y1 - r) : 0;
                            n += cx > 0 && cy > 0 && cx * cx + cy * cy > r * r;
                        }
                    out = n / 16.0f;
                }
                uint32_t a = (uint32_t)((row[x] >> 24) * out + 0.5f);
                row[x] = (row[x] & 0xffffffu) | a << 24;
            }
        }
        frame_corners = SDL_CreateTextureFromSurface(renderer, fc);
        SDL_DestroySurface(fc);
        if (frame_corners) {
            SDL_SetTextureBlendMode(frame_corners, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(frame_corners, SDL_SCALEMODE_LINEAR);
        }
    }

    // A soft round light for the LEDs, white so that its colour can be set when it is drawn.
    enum { G = 64 };
    SDL_Surface *g = SDL_CreateSurface(G, G, SDL_PIXELFORMAT_ARGB8888);
    if (g) {
        uint32_t *px = g->pixels;
        for (int y = 0; y < G; y++)
            for (int x = 0; x < G; x++) {
                float dx = (x + 0.5f - G / 2.0f) / (G / 2.0f), dy = (y + 0.5f - G / 2.0f) / (G / 2.0f);
                float d = sqrtf(dx * dx + dy * dy), a = d >= 1 ? 0 : powf(1 - d, 2.2f);
                if (d < 0.18f)
                    a = 1;
                px[y * (g->pitch / 4) + x] = (uint32_t)(a * 255) << 24 | 0xffffffu;
            }
        glow = SDL_CreateTextureFromSurface(renderer, g);
        if (glow)
            SDL_SetTextureBlendMode(glow, SDL_BLENDMODE_ADD);
        // the same disc, flatter and premultiplied, to tint what lies under it: SDL's MUL mixes
        // dst * src + dst * (1 - alpha), which with src = colour * alpha fades the tint out
        for (int y = 0; y < G; y++)
            for (int x = 0; x < G; x++) {
                float dx = (x + 0.5f - G / 2.0f) / (G / 2.0f), dy = (y + 0.5f - G / 2.0f) / (G / 2.0f);
                float d = sqrtf(dx * dx + dy * dy), a = d >= 1 ? 0 : d < 0.6f ? 1 : (1 - d) / 0.4f;
                uint32_t v = (uint32_t)(a * 255);
                px[y * (g->pitch / 4) + x] = v << 24 | v << 16 | v << 8 | v;
            }
        tint = SDL_CreateTextureFromSurface(renderer, g);
        if (tint)
            SDL_SetTextureBlendMode(tint, SDL_BLENDMODE_MUL);
        SDL_DestroySurface(g);
    }
    // black outside the rounded corners, coverage-weighted alpha, transparent elsewhere
    int mw = PICOCALC_SIM_LCD_WIDTH * CORNER_SS, mh = PICOCALC_SIM_LCD_HEIGHT * CORNER_SS;
    SDL_Surface *ms = SDL_CreateSurface(mw, mh, SDL_PIXELFORMAT_ARGB8888);
    if (ms) {
        float r = CORNER_RADIUS * CORNER_SS;
        for (int y = 0; y < mh; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)ms->pixels + y * ms->pitch);
            for (int x = 0; x < mw; x++) {
                // distance into the corner square, 0 outside it
                float cx = x < r ? r - x : x >= mw - r ? x - (mw - r) : 0;
                float cy = y < r ? r - y : y >= mh - r ? y - (mh - r) : 0;
                float out = 0;
                if (cx > 0 && cy > 0) {
                    // 4x4 samples of the pixel: the share outside the circle
                    int n = 0;
                    for (int sy = 0; sy < 4; sy++)
                        for (int sx = 0; sx < 4; sx++) {
                            float px = cx - 0.375f + sx * 0.25f, py = cy - 0.375f + sy * 0.25f;
                            n += px * px + py * py > r * r;
                        }
                    out = n / 16.0f;
                }
                row[x] = (uint32_t)(out * 255.0f + 0.5f) << 24;
            }
        }
        corners = SDL_CreateTextureFromSurface(renderer, ms);
        SDL_DestroySurface(ms);
        if (corners) {
            SDL_SetTextureBlendMode(corners, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(corners, SDL_SCALEMODE_LINEAR);
        }
    }
    return frame != NULL;
}

void skin_draw_lcd_corners(SDL_Renderer *renderer, SDL_FRect lcd, const SDL_FRect *frame_dst)
{
    // in the device frame the render itself shows around the rounding; elsewhere black
    if (frame_dst && frame_corners)
        SDL_RenderTexture(renderer, frame_corners, NULL, frame_dst);
    else if (corners)
        SDL_RenderTexture(renderer, corners, NULL, &lcd);
}

void skin_free(void)
{
    if (frame)
        SDL_DestroyTexture(frame);
    if (glow)
        SDL_DestroyTexture(glow);
    if (tint)
        SDL_DestroyTexture(tint);
    if (corners)
        SDL_DestroyTexture(corners);
    if (frame_corners)
        SDL_DestroyTexture(frame_corners);
    frame = glow = tint = corners = frame_corners = NULL;
}

void skin_window_size(float scale, float *w, float *h)
{
    float k = scale * PICOCALC_SIM_LCD_WIDTH / screen.w;
    *w = SKIN_W * k;
    *h = SKIN_H * k;
}

SDL_FRect skin_fit(float x, float y, float w, float h, SDL_FRect *lcd)
{
    float k = fminf(w / SKIN_W, h / SKIN_H);
    SDL_FRect d = {floorf(x + (w - SKIN_W * k) / 2), floorf(y + (h - SKIN_H * k) / 2), SKIN_W * k, SKIN_H * k};
    *lcd = (SDL_FRect){d.x + screen.x * k, d.y + screen.y * k, screen.w * k, screen.h * k};
    return d;
}

void skin_draw_frame(SDL_Renderer *renderer, SDL_FRect dst)
{
    if (frame)
        SDL_RenderTexture(renderer, frame, NULL, &dst);
}

void skin_draw_leds(SDL_Renderer *renderer, SDL_FRect dst, unsigned leds, uint32_t rgb)
{
    static const uint8_t col[3][3] = {{60, 255, 90}, {255, 200, 40}, {255, 50, 40}};
    if (!glow)
        return;
    float k = dst.w / SKIN_W;
    for (int i = 0; i < 3; i++) {
        if (!(leds >> i & 1))
            continue;
        float r = 46 * k;
        SDL_FRect g = {dst.x + led_x * k - r, dst.y + led_y[i] * k - r, 2 * r, 2 * r};
        SDL_SetTextureColorMod(glow, col[i][0], col[i][1], col[i][2]);
        SDL_RenderTexture(renderer, glow, NULL, &g);
    }
    if (rgb) {
        // brighter colours glow wider, as the LED does behind the white lettering
        unsigned r = rgb >> 16 & 0xff, g = rgb >> 8 & 0xff, b = rgb & 0xff;
        unsigned peak = r > g ? (r > b ? r : b) : (g > b ? g : b);
        float rad = (40 + 50 * peak / 255.0f) * k;
        // the LED's colour at full strength, its brightness in the glow's size and alpha
        float s = peak ? 255.0f / peak : 0;
        Uint8 cr = (Uint8)(r * s), cg = (Uint8)(g * s), cb = (Uint8)(b * s);
        // the white "+" takes the LED's colour (multiplied, so the black board around stays black)
        float tr = 46 * k;
        SDL_FRect t = {dst.x + rgb_x * k - tr, dst.y + rgb_y * k - tr, 2 * tr, 2 * tr};
        if (tint) {
            SDL_SetTextureColorMod(tint, cr, cg, cb);
            SDL_RenderTexture(renderer, tint, NULL, &t);
        }
        // and light around it
        SDL_SetTextureColorMod(glow, cr, cg, cb);
        SDL_SetTextureAlphaMod(glow, (Uint8)(60 + 150 * peak / 255));
        SDL_FRect q = {dst.x + rgb_x * k - rad, dst.y + rgb_y * k - rad, 2 * rad, 2 * rad};
        SDL_RenderTexture(renderer, glow, NULL, &q);
        SDL_SetTextureAlphaMod(glow, 255);
    }
}

uint8_t skin_key_at(SDL_FRect dst, float x, float y)
{
    if (dst.w <= 0)
        return 0;
    float k = dst.w / SKIN_W;
    float ix = (x - dst.x) / k, iy = (y - dst.y) / k;
    if (hypotf(ix - btn_a_x, iy - btn_y) < btn_r)
        return 's';
    if (hypotf(ix - btn_b_x, iy - btn_y) < btn_r)
        return 'd';
    // The joystick: its knob is the center push, around it the four directions.
    float dx = ix - joy_x, dy = iy - joy_y, d = hypotf(dx, dy);
    if (d < 32)
        return 'a';
    if (d < 120) {
        if (fabsf(dx) > fabsf(dy))
            return dx > 0 ? PC_KEY_RIGHT : PC_KEY_LEFT;
        return dy > 0 ? PC_KEY_DOWN : PC_KEY_UP;
    }
    return 0;
}

#endif
