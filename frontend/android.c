// PBConPC on Android: the PicoBoy Color Plus fills the phone's screen, its joystick and buttons
// are touched where the picture shows them, and a menu in the top right corner loads a UF2,
// resets, mutes and shows the speed.
// The PC's window (sdl.c, ui.cpp) is not built here.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jni.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "picocalc-sim/devices.h"
#include "picocalc-sim/frontend.h"
#include "picocalc-sim/keys.h"
#include "picocalc-sim/sim.h"
#include "keymap.h"
#include "skin.h"

int picocalc_sim_frontend_headless_run(void)
{
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Keys: several sources (fingers, a gamepad, a keyboard) may hold the same button, which goes
// down with the first and up with the last.

static const uint8_t all_keys[7] = {PC_KEY_UP, PC_KEY_DOWN, PC_KEY_LEFT, PC_KEY_RIGHT, 'a', 's', 'd'};
enum { SRC_TOUCH, SRC_PAD, SRC_STICK, SRC_KEYBOARD, SRC_COUNT };
static bool want[SRC_COUNT][7], down[7];

static int key_index(uint8_t key)
{
    for (int i = 0; i < 7; i++)
        if (all_keys[i] == key)
            return i;
    return -1;
}

static void keys_apply(void)
{
    bool on = picocalc_sim_machine_running();
    for (int i = 0; i < 7; i++) {
        bool w = false;
        for (int s = 0; s < SRC_COUNT; s++)
            w |= want[s][i];
        if (w == down[i])
            continue;
        down[i] = w;
        if (!on)
            continue;
        if (w)
            picocalc_sim_keys_press(all_keys[i]);
        else
            picocalc_sim_keys_release(all_keys[i]);
    }
}

static void key_set(int src, uint8_t key, bool held)
{
    int i = key_index(key);
    if (i >= 0)
        want[src][i] = held;
}

// Before the devices are freed (a restart): nothing is held on them any more.
static void keys_drop(void)
{
    if (picocalc_sim_machine_running())
        for (int i = 0; i < 7; i++)
            if (down[i])
                picocalc_sim_keys_release(all_keys[i]);
    memset(down, 0, sizeof(down));
}

// ---------------------------------------------------------------------------------------------
// Fingers

typedef struct {
    bool used;
    SDL_FingerID id;
    float x0, y0, x, y;
} finger_t;

static finger_t fingers[10];
static SDL_FRect skin_dst;
static uint8_t touch_keys[8];
static int ntouch_keys;

static finger_t *finger_find(SDL_FingerID id, bool make)
{
    finger_t *free_ = NULL;
    for (int i = 0; i < 10; i++) {
        if (fingers[i].used && fingers[i].id == id)
            return &fingers[i];
        if (!fingers[i].used && !free_)
            free_ = &fingers[i];
    }
    if (make && free_) {
        free_->used = true;
        free_->id = id;
    }
    return make ? free_ : NULL;
}

static void touch_update(void)
{
    memset(want[SRC_TOUCH], 0, sizeof(want[SRC_TOUCH]));
    ntouch_keys = 0;
    for (int i = 0; i < 10; i++) {
        if (!fingers[i].used)
            continue;
        uint8_t k[2];
        int n = skin_touch_keys(skin_dst, fingers[i].x0, fingers[i].y0, fingers[i].x, fingers[i].y, k);
        for (int j = 0; j < n; j++) {
            key_set(SRC_TOUCH, k[j], true);
            if (ntouch_keys < 8)
                touch_keys[ntouch_keys++] = k[j];
        }
    }
    keys_apply();
}

// ---------------------------------------------------------------------------------------------
// Gamepads, as in the PC's window: D-pad and left stick are the joystick, the lower and left face
// buttons button A, the right and upper ones button B, Start, Back, Guide and a stick click the
// joystick's centre.

static uint8_t pad_key(SDL_GamepadButton b)
{
    switch (b) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return PC_KEY_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return PC_KEY_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return PC_KEY_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return PC_KEY_RIGHT;
    case SDL_GAMEPAD_BUTTON_SOUTH:
    case SDL_GAMEPAD_BUTTON_WEST: return 's';
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_NORTH: return 'd';
    case SDL_GAMEPAD_BUTTON_START:
    case SDL_GAMEPAD_BUTTON_BACK:
    case SDL_GAMEPAD_BUTTON_GUIDE:
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return 'a';
    default: return 0;
    }
}

static uint8_t stick_key(int v, uint8_t held, uint8_t neg, uint8_t pos)
{
    int on = 16000, off = 9000;
    if (held == neg)
        return v < -off ? neg : v > on ? pos : 0;
    if (held == pos)
        return v > off ? pos : v < -on ? neg : 0;
    return v < -on ? neg : v > on ? pos : 0;
}

static void pad_event(const SDL_Event *e)
{
    static uint8_t stick_x, stick_y;
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        if (SDL_OpenGamepad(e->gdevice.which))
            picocalc_sim_log("gamepad connected");
        break;
    case SDL_EVENT_GAMEPAD_REMOVED: {
        SDL_Gamepad *g = SDL_GetGamepadFromID(e->gdevice.which);
        if (g)
            SDL_CloseGamepad(g);
        memset(want[SRC_PAD], 0, sizeof(want[SRC_PAD]));
        memset(want[SRC_STICK], 0, sizeof(want[SRC_STICK]));
        stick_x = stick_y = 0;
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        key_set(SRC_PAD, pad_key((SDL_GamepadButton)e->gbutton.button), e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        uint8_t *held, k;
        if (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            held = &stick_x;
            k = stick_key(e->gaxis.value, *held, PC_KEY_LEFT, PC_KEY_RIGHT);
        } else if (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            held = &stick_y;
            k = stick_key(e->gaxis.value, *held, PC_KEY_UP, PC_KEY_DOWN);
        } else {
            break;
        }
        if (k == *held)
            break;
        key_set(SRC_STICK, *held, false);
        key_set(SRC_STICK, k, true);
        *held = k;
        break;
    }
    default:
        break;
    }
    keys_apply();
}

// ---------------------------------------------------------------------------------------------
// Sound

static void SDLCALL audio_cb(void *ctx, SDL_AudioStream *stream, int additional, int total)
{
    (void)ctx;
    (void)total;
    static int16_t buf[2 * 1024];
    int frames = (additional + 3) / 4;
    int gain = picocalc_sim_opts.volume * 256 / 100;
    while (frames > 0) {
        int n = frames < 1024 ? frames : 1024;
        picocalc_audio_pull(picocalc_sim_picocalc.audio, buf, (size_t)n);
        for (int i = 0; i < 2 * n; i++)
            buf[i] = (int16_t)(buf[i] * gain / 256);
        SDL_PutAudioStreamData(stream, buf, n * 4);
        frames -= n;
    }
}

static SDL_AudioStream *open_audio(void)
{
    if (picocalc_sim_opts.mute || !picocalc_sim_machine_running())
        return NULL;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        picocalc_sim_log("no sound: %s", SDL_GetError());
        return NULL;
    }
    SDL_AudioSpec spec = {.format = SDL_AUDIO_S16, .channels = 2, .freq = 48000};
    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, NULL);
    if (!stream) {
        picocalc_sim_log("no sound: %s", SDL_GetError());
        return NULL;
    }
    if (!picocalc_audio_play(picocalc_sim_picocalc.audio)) {
        SDL_DestroyAudioStream(stream);
        return NULL;
    }
    SDL_ResumeAudioStreamDevice(stream);
    return stream;
}

// ---------------------------------------------------------------------------------------------
// Texts in the phone's own font (Roboto on nearly every Android), drawn from an atlas that
// stb_truetype bakes at the size the screen asks for. Without a font file SDL's 8 x 8 one serves.

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#pragma GCC diagnostic pop

enum { ATLAS = 1024 };
static struct {
    unsigned char *ttf;
    bool tried;
    SDL_Texture *tex;
    stbtt_bakedchar ch[95];
    float px, ascent;   // the size baked, and the baseline below a line's top as a part of the size
} font;

static void font_use(SDL_Renderer *r, float px)
{
    if (!font.tried) {
        font.tried = true;
        static const char *const files[] = {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/RobotoStatic-Regular.ttf",
                                            "/system/fonts/NotoSans-Regular.ttf", "/system/fonts/DroidSans.ttf"};
        for (size_t i = 0; i < sizeof(files) / sizeof(files[0]) && !font.ttf; i++) {
            size_t n;
            unsigned char *d = SDL_LoadFile(files[i], &n);
            stbtt_fontinfo info;
            if (d && stbtt_InitFont(&info, d, stbtt_GetFontOffsetForIndex(d, 0))) {
                int asc, desc, gap;
                stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
                font.ascent = (float)asc / (float)(asc - desc);
                font.ttf = d;
            } else {
                SDL_free(d);
            }
        }
        if (!font.ttf)
            picocalc_sim_log("no system font found; the built-in one serves");
    }
    if (!font.ttf || (font.tex && fabsf(px - font.px) < 0.5f))
        return;
    static unsigned char alpha[ATLAS * ATLAS];
    static uint32_t argb[ATLAS * ATLAS];
    stbtt_BakeFontBitmap(font.ttf, 0, px, alpha, ATLAS, ATLAS, 32, 95, font.ch);
    for (int i = 0; i < ATLAS * ATLAS; i++)
        argb[i] = (uint32_t)alpha[i] << 24 | 0xffffff;
    if (!font.tex) {
        font.tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, ATLAS, ATLAS);
        SDL_SetTextureBlendMode(font.tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(font.tex, SDL_SCALEMODE_LINEAR);
    }
    SDL_UpdateTexture(font.tex, NULL, argb, ATLAS * 4);
    font.px = px;
}

static int glyph(char c)
{
    return c >= 32 && c < 127 ? c - 32 : '?' - 32;
}

static float text_width(const char *s, float px)
{
    if (!font.tex)
        return (float)strlen(s) * px * 0.8f;
    float w = 0;
    for (; *s; s++)
        w += font.ch[glyph(*s)].xadvance;
    return w * px / font.px;
}

// Draws s in the renderer's draw colour, its line's top left corner at (x, y), px high.
static void text(SDL_Renderer *r, float x, float y, float px, const char *s)
{
    if (!font.tex) {
        float scale = px * 0.8f / SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
        SDL_SetRenderScale(r, scale, scale);
        SDL_RenderDebugText(r, x / scale, (y + px * 0.1f) / scale, s);
        SDL_SetRenderScale(r, 1, 1);
        return;
    }
    Uint8 cr, cg, cb, ca;
    SDL_GetRenderDrawColor(r, &cr, &cg, &cb, &ca);
    SDL_SetTextureColorMod(font.tex, cr, cg, cb);
    SDL_SetTextureAlphaMod(font.tex, ca);
    float k = px / font.px, pen = 0, base = y + font.ascent * px;
    for (; *s; s++) {
        const stbtt_bakedchar *g = &font.ch[glyph(*s)];
        SDL_FRect src = {g->x0, g->y0, (float)(g->x1 - g->x0), (float)(g->y1 - g->y0)};
        SDL_FRect dst = {x + (pen + g->xoff) * k, base + g->yoff * k, src.w * k, src.h * k};
        if (k == 1) {
            dst.x = floorf(dst.x + 0.5f);
            dst.y = floorf(dst.y + 0.5f);
        }
        SDL_RenderTexture(r, font.tex, &src, &dst);
        pen += g->xadvance;
    }
}

// ---------------------------------------------------------------------------------------------
// The firmware's USB serial port (MicroPython's REPL) as a Bluetooth serial port, so that PBC+
// Studio in Chrome on a PC paired with the phone can connect to it as to the real PicoBoy on a
// cable. MainActivity.java offers the port (the Serial Port Profile) and moves the bytes with
// the three functions it calls here; the emulated host has the port open while a PC is connected.

enum { BT_OFF, BT_WAITING, BT_CONNECTED, BT_NO_PERMISSION, BT_UNAVAILABLE };
static bool bt_on;
static SDL_AtomicInt bt_state;
static SDL_Mutex *bt_lock;
static SDL_Condition *bt_cond;
static uint8_t bt_ring[1 << 16];
static unsigned bt_head, bt_len;

// From the emulation thread: what the firmware sent. What does not fit is lost, as on a cable
// nobody reads from.
static void bt_out(const uint8_t *data, size_t len)
{
    SDL_LockMutex(bt_lock);
    for (size_t i = 0; i < len && bt_len < sizeof(bt_ring); i++, bt_len++)
        bt_ring[(bt_head + bt_len) % sizeof(bt_ring)] = data[i];
    SDL_SignalCondition(bt_cond);
    SDL_UnlockMutex(bt_lock);
}

// Gives the writer thread what the firmware sent, waiting a moment for some.
JNIEXPORT jint JNICALL Java_de_pbcplus_emulator_MainActivity_nativeBtOut(JNIEnv *env, jobject self, jbyteArray arr)
{
    (void)self;
    static uint8_t tmp[4096];   // one writer thread at a time
    unsigned cap = (unsigned)(*env)->GetArrayLength(env, arr), n = 0;
    if (cap > sizeof(tmp))
        cap = sizeof(tmp);
    SDL_LockMutex(bt_lock);
    if (!bt_len)
        SDL_WaitConditionTimeout(bt_cond, bt_lock, 100);
    for (; n < cap && bt_len; n++, bt_len--, bt_head = (bt_head + 1) % sizeof(bt_ring))
        tmp[n] = bt_ring[bt_head];
    SDL_UnlockMutex(bt_lock);
    if (n)
        (*env)->SetByteArrayRegion(env, arr, 0, (jsize)n, (const jbyte *)tmp);
    return (jint)n;
}

// What the PC sent, for the firmware. The host takes it as fast as the firmware reads; bytes
// nobody takes for three seconds (no firmware with a serial port runs) are dropped.
JNIEXPORT void JNICALL Java_de_pbcplus_emulator_MainActivity_nativeBtIn(JNIEnv *env, jobject self, jbyteArray arr,
                                                                       jint len)
{
    (void)self;
    uint8_t tmp[4096];
    if (len > (jint)sizeof(tmp))
        len = (jint)sizeof(tmp);
    (*env)->GetByteArrayRegion(env, arr, 0, len, (jbyte *)tmp);
    int idle = 0;
    for (size_t off = 0; off < (size_t)len && idle < 1500 && SDL_GetAtomicInt(&bt_state) == BT_CONNECTED;) {
        size_t k = picocalc_sim_usb_serial_send(tmp + off, (size_t)len - off);
        off += k;
        idle = k ? 0 : idle + 1;
        if (!k)
            SDL_Delay(2);
    }
}

JNIEXPORT void JNICALL Java_de_pbcplus_emulator_MainActivity_nativeBtState(JNIEnv *env, jobject self, jint state)
{
    (void)env;
    (void)self;
    if (state == BT_CONNECTED) {
        SDL_LockMutex(bt_lock);
        bt_len = 0;
        SDL_UnlockMutex(bt_lock);
    }
    SDL_SetAtomicInt(&bt_state, state);
    picocalc_sim_usb_serial_attach(PICOCALC_SIM_TERMINAL_REMOTE, state == BT_CONNECTED);
}

static void bt_set(bool on)
{
    bt_on = on;
    // the serial port needs the cable in the emulated host
    picocalc_sim_opts.usb = on;
    picocalc_sim_usb_plug(on);
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    jobject activity = SDL_GetAndroidActivity();
    if (!env || !activity)
        return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID m = (*env)->GetMethodID(env, cls, on ? "btStart" : "btStop", "()V");
    if (m)
        (*env)->CallVoidMethod(env, activity, m);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

// ---------------------------------------------------------------------------------------------
// The menu: three lines in the top right corner open it. It loads a UF2, resets, mutes and
// switches the speed display on, which is off unless asked for.

enum { M_LOAD, M_RESET, M_SOUND, M_SPEED, M_BT, M_COUNT, HIT_ICON = 100, HIT_SPEED, HIT_OUTSIDE };
static float ui_px = 40;   // the size of the menu's letters
static bool menu_open, show_speed;
static SDL_FRect icon_rect, speed_rect, menu_rect, item_rect[M_COUNT];

static const char *item_label(int i)
{
    switch (i) {
    case M_LOAD: return "Load UF2 ...";
    case M_RESET: return "Reset";
    case M_SOUND: return "Sound";
    case M_SPEED: return "Speed display";
    default: return "Bluetooth";
    }
}

// On or off for the items that switch something, -1 for the others.
static int item_state(int i)
{
    return i == M_SOUND ? !picocalc_sim_opts.mute : i == M_SPEED ? show_speed : i == M_BT ? bt_on : -1;
}

// Lays the strip along the top of `area` and the menu out and gives the strip's height.
static float ui_layout(SDL_FRect area)
{
    float h = ui_px * 2.2f, row = ui_px * 2.7f;
    icon_rect = (SDL_FRect){area.x + area.w - h * 1.15f, area.y, h * 1.15f, h};
    float w = ui_px * 13;
    if (w > area.w - ui_px)
        w = area.w - ui_px;
    menu_rect = (SDL_FRect){area.x + area.w - w - ui_px * 0.5f, area.y + h, w, row * M_COUNT};
    for (int i = 0; i < M_COUNT; i++)
        item_rect[i] = (SDL_FRect){menu_rect.x, menu_rect.y + row * (float)i, w, row};
    return h;
}

static bool inside(SDL_FRect q, float x, float y)
{
    return x >= q.x && x < q.x + q.w && y >= q.y && y < q.y + q.h;
}

// What a finger at (x, y) is on: a menu item, HIT_ICON, HIT_SPEED, HIT_OUTSIDE (beside the open
// menu, which closes it) or -1 for the PicoBoy itself.
static int ui_hit(float x, float y)
{
    if (menu_open) {
        for (int i = 0; i < M_COUNT; i++)
            if (inside(item_rect[i], x, y))
                return i;
        return HIT_OUTSIDE;
    }
    if (inside(icon_rect, x, y))
        return HIT_ICON;
    if (inside(speed_rect, x, y))
        return HIT_SPEED;
    return -1;
}

static void ui_draw(SDL_Renderer *r, SDL_FRect area, float h, int pressed, int out_w, int out_h)
{
    // How fast the PicoBoy runs, in per cent of the real one: amber when it falls behind. A tap
    // on it switches the translation to native code off (then an "i" for interpreter follows the
    // number) and on again, to see what it gains.
    speed_rect = (SDL_FRect){0, 0, 0, 0};
    if (show_speed && picocalc_sim_machine_running() && picocalc_sim_state() == PICOCALC_SIM_RUNNING) {
        int pct = (int)(picocalc_sim_speed() * 100 + 0.5);
        char s[16];
        snprintf(s, sizeof(s), "%d %%%s", pct, picocalc_sim_jit_on() ? "" : " i");
        float small = ui_px * 0.8f;
        if (pct < 97)
            SDL_SetRenderDrawColor(r, 255, 190, 60, 255);
        else
            SDL_SetRenderDrawColor(r, 140, 150, 170, 255);
        text(r, area.x + ui_px * 0.6f, area.y + (h - small) / 2, small, s);
        speed_rect = (SDL_FRect){area.x, area.y, ui_px * 6, h};
    }
    // the three lines
    float lw = ui_px * 0.95f, lt = floorf(ui_px / 10 + 0.5f), gap = ui_px * 0.3f;
    if (lt < 2)
        lt = 2;
    if (pressed == HIT_ICON || menu_open)
        SDL_SetRenderDrawColor(r, 235, 238, 245, 255);
    else
        SDL_SetRenderDrawColor(r, 150, 156, 170, 255);
    for (int i = -1; i <= 1; i++) {
        SDL_FRect line = {icon_rect.x + (icon_rect.w - lw) / 2, icon_rect.y + icon_rect.h / 2 + (float)i * gap - lt / 2,
                          lw, lt};
        SDL_RenderFillRect(r, &line);
    }
    if (!menu_open)
        return;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 150);
    SDL_FRect all = {0, icon_rect.y + h, (float)out_w, (float)out_h};
    SDL_RenderFillRect(r, &all);
    SDL_FRect edge = {menu_rect.x - 2, menu_rect.y - 2, menu_rect.w + 4, menu_rect.h + 4};
    SDL_SetRenderDrawColor(r, 74, 80, 98, 255);
    SDL_RenderFillRect(r, &edge);
    SDL_SetRenderDrawColor(r, 32, 35, 44, 255);
    SDL_RenderFillRect(r, &menu_rect);
    for (int i = 0; i < M_COUNT; i++) {
        SDL_FRect q = item_rect[i];
        if (i == pressed) {
            SDL_SetRenderDrawColor(r, 58, 66, 90, 255);
            SDL_RenderFillRect(r, &q);
        }
        if (i > 0) {
            SDL_SetRenderDrawColor(r, 52, 57, 70, 255);
            SDL_FRect sep = {q.x + ui_px * 0.9f, q.y, q.w - ui_px * 1.8f, 2};
            SDL_RenderFillRect(r, &sep);
        }
        SDL_SetRenderDrawColor(r, 235, 238, 245, 255);
        text(r, q.x + ui_px * 0.9f, q.y + (q.h - ui_px) / 2, ui_px, item_label(i));
        int st = item_state(i);
        if (st >= 0) {
            const char *v = st ? "On" : "Off";
            if (i == M_BT && st)
                v = SDL_GetAtomicInt(&bt_state) == BT_CONNECTED ? "Connected" : "Waiting";
            if (st)
                SDL_SetRenderDrawColor(r, 120, 205, 150, 255);
            else
                SDL_SetRenderDrawColor(r, 140, 150, 170, 255);
            text(r, q.x + q.w - ui_px * 0.9f - text_width(v, ui_px), q.y + (q.h - ui_px) / 2, ui_px, v);
        }
    }
}

// Text in the middle of a rectangle, broken into lines at "\n" and shrunk to fit.
static void text_box(SDL_Renderer *r, SDL_FRect box, const char *s)
{
    char line[128];
    int lines = 0;
    float longest = 1;
    for (const char *p = s; *p;) {
        size_t n = strcspn(p, "\n");
        snprintf(line, sizeof(line), "%.*s", (int)n, p);
        float w = text_width(line, ui_px);
        if (w > longest)
            longest = w;
        lines++;
        p += n;
        if (*p == '\n')
            p++;
    }
    float px = ui_px;
    if (longest > box.w * 0.9f)
        px *= box.w * 0.9f / longest;
    float lh = px * 1.35f, y = box.y + (box.h - (float)lines * lh) / 2 + (lh - px) / 2;
    for (const char *p = s; *p;) {
        size_t n = strcspn(p, "\n");
        snprintf(line, sizeof(line), "%.*s", (int)n, p);
        text(r, box.x + (box.w - text_width(line, px)) / 2, y, px, line);
        y += lh;
        p += n;
        if (*p == '\n')
            p++;
    }
}

static void save_settings(void)
{
    picocalc_sim_settings_set("mute", picocalc_sim_opts.mute ? "on" : "off");
    picocalc_sim_settings_set("speed-display", show_speed ? "on" : "off");
    picocalc_sim_settings_set("bluetooth-serial", bt_on ? "on" : "off");
    picocalc_sim_settings_save();
}

// ---------------------------------------------------------------------------------------------
// Loading a UF2: Android's file chooser gives a content:// address, which SDL can read but the
// emulator's fopen cannot, so the file is copied into the app's own storage first.

static SDL_Mutex *pick_lock;
static char picked[2048];
static char data_dir[1024];
static char note[160];
static Uint64 note_until;

static void set_note(const char *s)
{
    snprintf(note, sizeof(note), "%s", s);
    note_until = SDL_GetTicks() + 6000;
}

static void SDLCALL picked_cb(void *userdata, const char *const *files, int filter)
{
    (void)userdata;
    (void)filter;
    if (!files || !files[0])
        return;   // an error, or nothing chosen
    SDL_LockMutex(pick_lock);
    snprintf(picked, sizeof(picked), "%s", files[0]);
    SDL_UnlockMutex(pick_lock);
}

static bool copy_file(const char *from, const char *to)
{
    SDL_IOStream *in = SDL_IOFromFile(from, "rb");
    if (!in)
        return false;
    SDL_IOStream *out = SDL_IOFromFile(to, "wb");
    if (!out) {
        SDL_CloseIO(in);
        return false;
    }
    static char buf[1 << 16];
    bool ok = true;
    for (;;) {
        size_t n = SDL_ReadIO(in, buf, sizeof(buf));
        if (n == 0) {
            ok = SDL_GetIOStatus(in) == SDL_IO_STATUS_EOF;
            break;
        }
        if (SDL_WriteIO(out, buf, n) != n) {
            ok = false;
            break;
        }
    }
    SDL_CloseIO(in);
    return SDL_CloseIO(out) && ok;
}

static void load_uf2(const char *from)
{
    static char path[1200];
    // two names in turn: the machine reads the file a moment after it is asked to
    static int turn;
    snprintf(path, sizeof(path), "%s/loaded%d.uf2", data_dir, turn ^= 1);
    if (!copy_file(from, path)) {
        set_note("That file could not be read.");
        picocalc_sim_log("cannot copy %s: %s", from, SDL_GetError());
        return;
    }
    char err[256];
    if (picocalc_sim_machine_running()) {
        if (picocalc_sim_flash_uf2(path, err, sizeof(err))) {
            set_note("Flashing...");
        } else {
            set_note("That is not a UF2 for the PicoBoy.");
            picocalc_sim_log("%s", err);
        }
    } else {
        // the machine did not start: start it with this UF2
        picocalc_sim_opts.uf2_path = path;
        picocalc_sim_request_restart();
    }
}

// ---------------------------------------------------------------------------------------------

static void log_to_logcat(void)
{
    static unsigned seen;
    unsigned n = picocalc_sim_log_count();
    if (n - seen > 200)
        seen = n - 200;
    char line[512];
    for (; seen < n; seen++)
        if (picocalc_sim_log_line(seen, line, sizeof(line)))
            SDL_Log("%s", line);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "Portrait");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("SDL could not start: %s", SDL_GetError());
        return 1;
    }
    // The flash image and the settings live in the app's own storage.
    const char *store = SDL_GetAndroidInternalStoragePath();
    snprintf(data_dir, sizeof(data_dir), "%s", store ? store : ".");
    setenv("XDG_DATA_HOME", data_dir, 1);
    setenv("HOME", data_dir, 1);
    picocalc_sim_opts.screenshot_dir = data_dir;
    pick_lock = SDL_CreateMutex();
    bt_lock = SDL_CreateMutex();
    bt_cond = SDL_CreateCondition();
    picocalc_sim_usb_serial_remote = bt_out;

    SDL_Window *window = SDL_CreateWindow("PBConPC", PICOCALC_SIM_LCD_WIDTH, PICOCALC_SIM_LCD_HEIGHT,
                                          SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, NULL) : NULL;
    if (!renderer) {
        SDL_Log("no window: %s", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                             PICOCALC_SIM_LCD_WIDTH, PICOCALC_SIM_LCD_HEIGHT);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_PIXELART);
    static uint32_t pixels[PICOCALC_SIM_LCD_WIDTH * PICOCALC_SIM_LCD_HEIGHT];
    skin_init(renderer);
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        picocalc_sim_log("no gamepads: %s", SDL_GetError());
    // what the menu switched last time
    picocalc_sim_settings_load(NULL, 0);
    const char *sd = picocalc_sim_settings_get("speed-display");
    show_speed = sd && !strcmp(sd, "on");
    keymap_load();

    picocalc_sim_machine_start();
    SDL_AudioStream *audio = open_audio();
    bool muted = picocalc_sim_opts.mute;
    const char *bs = picocalc_sim_settings_get("bluetooth-serial");
    if (bs && !strcmp(bs, "on"))
        bt_set(true);

    uint32_t last_lcd = ~0u, last_sb = ~0u;
    int pressed_btn = -1;
    SDL_FingerID btn_finger = 0;
    bool background = false, running = true;
    int code = 0;
    while (running) {
        int ow, oh;
        SDL_GetCurrentRenderOutputSize(renderer, &ow, &oh);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_TERMINATING:
                running = false;
                break;
            case SDL_EVENT_WILL_ENTER_BACKGROUND:
            case SDL_EVENT_DID_ENTER_BACKGROUND:
                // nothing runs, sounds or is held while the app is out of sight
                background = true;
                picocalc_sim_machine_pause(true);
                memset(fingers, 0, sizeof(fingers));
                memset(want, 0, sizeof(want));
                keys_apply();
                pressed_btn = -1;
                break;
            case SDL_EVENT_WILL_ENTER_FOREGROUND:
            case SDL_EVENT_DID_ENTER_FOREGROUND:
                background = false;
                picocalc_sim_machine_pause(false);
                break;
            case SDL_EVENT_FINGER_DOWN: {
                float x = e.tfinger.x * (float)ow, y = e.tfinger.y * (float)oh;
                int b = ui_hit(x, y);
                if (b >= 0) {
                    if (pressed_btn < 0) {
                        pressed_btn = b;
                        btn_finger = e.tfinger.fingerID;
                    }
                    break;
                }
                finger_t *f = finger_find(e.tfinger.fingerID, true);
                if (f) {
                    f->x0 = f->x = x;
                    f->y0 = f->y = y;
                    touch_update();
                }
                break;
            }
            case SDL_EVENT_FINGER_MOTION: {
                finger_t *f = finger_find(e.tfinger.fingerID, false);
                if (f) {
                    f->x = e.tfinger.x * (float)ow;
                    f->y = e.tfinger.y * (float)oh;
                    touch_update();
                }
                break;
            }
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_CANCELED: {
                if (pressed_btn >= 0 && e.tfinger.fingerID == btn_finger) {
                    int b = pressed_btn;
                    pressed_btn = -1;
                    // a button acts when the finger leaves it while still on it
                    if (e.type != SDL_EVENT_FINGER_UP || ui_hit(e.tfinger.x * (float)ow, e.tfinger.y * (float)oh) != b)
                        break;
                    if (b == HIT_ICON) {
                        menu_open = true;
                    } else if (b == HIT_OUTSIDE) {
                        menu_open = false;
                    } else if (b == HIT_SPEED) {
                        picocalc_sim_jit_set(!picocalc_sim_jit_on());
                    } else if (b == M_LOAD) {
                        menu_open = false;
                        SDL_ShowOpenFileDialog(picked_cb, NULL, window, NULL, 0, NULL, false);
                    } else if (b == M_RESET) {
                        menu_open = false;
                        if (picocalc_sim_machine_running())
                            picocalc_sim_reboot("reset button");
                        else
                            picocalc_sim_request_restart();
                    } else if (b == M_SOUND) {
                        picocalc_sim_opts.mute = !picocalc_sim_opts.mute;
                        save_settings();
                    } else if (b == M_SPEED) {
                        show_speed = !show_speed;
                        save_settings();
                    } else if (b == M_BT) {
                        bt_set(!bt_on);
                        save_settings();
                        if (bt_on)
                            set_note("Pair the PC with this phone, then connect in PBC+ Studio.");
                    }
                    break;
                }
                finger_t *f = finger_find(e.tfinger.fingerID, false);
                if (f) {
                    f->used = false;
                    touch_update();
                }
                break;
            }
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                // a keyboard plugged into the phone: the PC's keys (arrows, A, S, D, Enter)
                if (e.key.repeat)
                    break;
                int control = keymap_control((uint32_t)e.key.key);
                if (control >= 0) {
                    key_set(SRC_KEYBOARD, keymap_code(control), e.type == SDL_EVENT_KEY_DOWN);
                    keys_apply();
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EVENT_GAMEPAD_REMOVED:
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                pad_event(&e);
                break;
            case SDL_EVENT_DROP_FILE:
                // a UF2 opened with this app from a file manager
                SDL_LockMutex(pick_lock);
                snprintf(picked, sizeof(picked), "%s", e.drop.data);
                SDL_UnlockMutex(pick_lock);
                break;
            default:
                break;
            }
        }
        log_to_logcat();

        char from[sizeof(picked)];
        SDL_LockMutex(pick_lock);
        memcpy(from, picked, sizeof(from));
        picked[0] = 0;
        SDL_UnlockMutex(pick_lock);
        if (from[0])
            load_uf2(from);

        if (picocalc_sim_restart_requested()) {
            // The devices are about to be freed, so nothing may hold a key or pull sound from them.
            keys_drop();
            if (audio)
                SDL_DestroyAudioStream(audio);
            audio = NULL;
            if (picocalc_sim_machine_restart())
                audio = open_audio();
            picocalc_sim_opts.uf2_path = NULL;
            keys_apply();
            last_lcd = last_sb = ~0u;
        }
        if (picocalc_sim_opts.mute != muted) {
            muted = picocalc_sim_opts.mute;
            if (audio)
                SDL_DestroyAudioStream(audio);
            audio = open_audio();
        }
        // Bluetooth could not be used: the menu item goes back to off
        int bst = SDL_GetAtomicInt(&bt_state);
        if (bt_on && (bst == BT_NO_PERMISSION || bst == BT_UNAVAILABLE)) {
            set_note(bst == BT_NO_PERMISSION ? "Bluetooth is not allowed for this app." : "Switch Bluetooth on first.");
            bt_set(false);
            SDL_SetAtomicInt(&bt_state, BT_OFF);
            save_settings();
        }
        if (picocalc_sim_quit_requested(&code))
            break;
        if (background) {
            SDL_Delay(50);
            continue;
        }

        bool on = picocalc_sim_machine_running();
        uint32_t lcd_gen = on ? st7789_generation(picocalc_sim_picocalc.lcd) : 0;
        uint32_t sb_gen = on ? picoboy_backlight() : 0;
        if (on && (lcd_gen != last_lcd || sb_gen != last_sb)) {
            last_lcd = lcd_gen;
            last_sb = sb_gen;
            st7789_render(picocalc_sim_picocalc.lcd, pixels, (uint8_t)sb_gen);
            SDL_UpdateTexture(texture, NULL, pixels, PICOCALC_SIM_LCD_WIDTH * 4);
        }

        // Everything stays clear of the camera cut-out and the system's bars.
        SDL_Rect safe;
        if (!SDL_GetWindowSafeArea(window, &safe) || safe.w <= 0 || safe.h <= 0)
            safe = (SDL_Rect){0, 0, ow, oh};
        float density = SDL_GetWindowDisplayScale(window);
        if (density < 1)
            density = 1;
        SDL_FRect area = {(float)safe.x, (float)safe.y, (float)safe.w, (float)safe.h};
        ui_px = floorf(density * 17);
        if (ui_px > area.w / 16)
            ui_px = floorf(area.w / 16);
        font_use(renderer, ui_px);
        float bar_h = ui_layout(area);

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_FRect lcd;
        skin_dst = skin_fit(area.x, area.y + bar_h, area.w, area.h - bar_h, &lcd);
        skin_draw_frame(renderer, skin_dst);
        picocalc_sim_state_t state = picocalc_sim_state();
        if (on) {
            SDL_RenderTexture(renderer, texture, NULL, &lcd);
            skin_draw_lcd_corners(renderer, lcd, &skin_dst);
            skin_draw_leds(renderer, skin_dst, picoboy_leds(), picoboy_rgb());
        }
        skin_draw_touch(renderer, skin_dst, touch_keys, ntouch_keys);
        SDL_SetRenderDrawColor(renderer, 235, 238, 245, 255);
        if (!on)
            text_box(renderer, lcd, "The PicoBoy did not start.\n\nOpen the menu (top right)\nand choose Load UF2.");
        else if (state == PICOCALC_SIM_BOOTSEL)
            text_box(renderer, lcd, "No program yet.\n\nOpen the menu (top right)\nand choose Load UF2.");
        else if (state == PICOCALC_SIM_POWERED_OFF)
            text_box(renderer, lcd, "Switched off.\n\nChoose Reset in the menu.");
        else if (state == PICOCALC_SIM_HALTED)
            text_box(renderer, lcd, "The program stopped.\n\nChoose Reset in the menu.");
        if (note[0] && SDL_GetTicks() < note_until) {
            SDL_FRect box = {area.x, lcd.y + lcd.h - 2.6f * ui_px, area.w, 2.2f * ui_px};
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_RenderFillRect(renderer, &box);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            text_box(renderer, box, note);
        }
        ui_draw(renderer, area, bar_h, pressed_btn, ow, oh);
        SDL_RenderPresent(renderer);
    }

    if (audio)
        SDL_DestroyAudioStream(audio);
    picocalc_sim_machine_stop();
    skin_free();
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return code;
}
