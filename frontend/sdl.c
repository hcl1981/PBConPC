#include "picocalc-sim/devices.h"
#include "picocalc-sim/frontend.h"
#include "picocalc-sim/keys.h"
#include "picocalc-sim/sim.h"

#ifndef PICOCALC_SIM_HAVE_SDL

bool picocalc_sim_frontend_sdl_available(void)
{
    return false;
}

int picocalc_sim_frontend_sdl_run(void)
{
    return picocalc_sim_frontend_headless_run();
}

#else

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#ifdef SDL_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "link.h"
#include "skin.h"
#include "keymap.h"
#include <ctype.h>
#include "ui.h"

bool picocalc_sim_frontend_sdl_available(void)
{
    return true;
}

typedef struct {
    SDL_Keycode sym;
    uint8_t key;
    bool synth_shift;
    // Typed from a character rather than a key; the oldest such is let go first. A character the
    // PicoCalc has no key for holds a place with key 0, so that its key-up lets go of nothing.
    bool from_char;
    unsigned seq;
} held_t;

static held_t held[32];
static int nheld;
static unsigned held_seq;

static void press(SDL_Keycode sym, uint8_t key, bool shift, bool from_char)
{
    if (nheld == (int)(sizeof(held) / sizeof(held[0])))
        return;
    bool synth = key && shift;
    if (synth)
        picocalc_sim_keys_press(PC_KEY_LSHIFT);
    if (key)
        picocalc_sim_keys_press(key);
    held[nheld++] = (held_t){sym, key, synth, from_char, held_seq++};
}

static void release(int i)
{
    if (held[i].key)
        picocalc_sim_keys_release(held[i].key);
    if (held[i].synth_shift)
        picocalc_sim_keys_release(PC_KEY_LSHIFT);
    held[i] = held[--nheld];
}

// PBConPC: only the keys Settings -> Keys gives the PicoBoy's controls (keymap.c).
static void key_down(SDL_Keycode sym)
{
    int control = keymap_control((uint32_t)sym);
    if (control >= 0)
        press(sym, keymap_code(control), false, false);
}

static void key_up(SDL_Keycode sym)
{
    for (int i = 0; i < nheld; i++) {
        if (!held[i].from_char && held[i].sym == sym) {
            release(i);
            return;
        }
    }
}

#ifdef SDL_PLATFORM_WINDOWS
// Remote Desktop in its Unicode mode, and other tools that type for a person, send characters as
// VK_PACKET keystrokes, which carry no scancode for SDL to report a key from. TranslateMessage
// turns each into a WM_CHAR with the character, so the PicoCalc is typed from that instead.
static bool char_due;
static int ups_due;

static void char_up(void)
{
    int oldest = -1;
    for (int i = 0; i < nheld; i++)
        if (held[i].from_char && (oldest < 0 || held[i].seq < held[oldest].seq))
            oldest = i;
    if (oldest >= 0)
        release(oldest);
}

static bool SDLCALL windows_message(void *userdata, MSG *msg)
{
    (void)userdata;
    switch (msg->message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (msg->wParam != VK_PACKET) {
            // The character of an ordinary key is SDL's to make text of.
            char_due = false;
            ups_due = 0;
            return true;
        }
        // Bit 30 is set on the repeats of a key held down, which the PicoCalc's keyboard lacks.
        if (!(msg->lParam & (1L << 30))) {
            TranslateMessage(msg);
            char_due = true;
        }
        return false;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (msg->wParam != VK_PACKET)
            return true;
        if (char_due)
            ups_due++;
        else
            char_up();
        return false;
    case WM_CHAR:
        if (!char_due)
            return true;
        char_due = false;
        // A text field or the terminal takes the character as text, through SDL.
        bool to_ui = ui_wants_keyboard() || !picocalc_sim_machine_running();
        uint8_t key = 0;
        bool shift = false;
        if (!to_ui && msg->wParam < 0x80) {
            // PBConPC: a typed character presses what its key would (keymap.c)
            int control = keymap_control((uint32_t)tolower((int)msg->wParam));
            key = control >= 0 ? keymap_code(control) : 0;
        }
        press(0, key, shift, true);
        // A key let go before its character came, as a fast injection can, is let go now.
        for (; ups_due > 0; ups_due--)
            char_up();
        return to_ui;
    default:
        return true;
    }
}
#endif

static void release_all(void)
{
    while (nheld)
        release(nheld - 1);
}

// A key held through the power switch is down as the PicoCalc starts, which is how uf2loader is
// asked for its menu, so keys still held on the host are pressed again on the new machine.
static void press_held(const SDL_Keycode *syms, int n)
{
    const bool *down = SDL_GetKeyboardState(NULL);
    for (int i = 0; i < n; i++)
        if (down[SDL_GetScancodeFromKey(syms[i], NULL)])
            key_down(syms[i]);
}

static void SDLCALL audio_cb(void *ctx, SDL_AudioStream *stream, int additional, int total)
{
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
    // SDL3 would pick 1024 frames at 48 kHz; 512 keeps the sound closer behind the picture.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
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

static SDL_Window *create_window(void)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, picocalc_sim_window_title());
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, PICOCALC_SIM_LCD_WIDTH);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, PICOCALC_SIM_LCD_HEIGHT);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    // shown once the UI has worked out its size
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    SDL_Window *window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    return window;
}

// The LCD fills the space it has. A whole multiple of its pixels, give or take a screen pixel, is
// drawn as exactly that, each LCD pixel a square; any other size takes SDL's pixel-art scaling,
// which keeps every LCD pixel the same size and blends only the screen pixel where two meet.
// PBConPC: game controllers. The D-pad and left stick are the joystick, the lower and left face
// buttons (Xbox A and X) button A, the right and upper ones (B and Y) button B, Start, Back,
// Guide and a stick click the joystick's center.
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

// The stick's direction on one axis as a key, with some hysteresis so it does not chatter.
static uint8_t stick_key(int v, uint8_t held, uint8_t neg, uint8_t pos)
{
    int on = 16000, off = 9000;
    if (held == neg)
        return v < -off ? neg : v > on ? pos : 0;
    if (held == pos)
        return v > off ? pos : v < -on ? neg : 0;
    return v < -on ? neg : v > on ? pos : 0;
}

// With two PBConPC on the link cable, side 0 takes the first gamepad and side 1 the second, so two
// players can share one PC; otherwise every gamepad plays.
static bool pad_is_mine(SDL_JoystickID id)
{
    int side = link_side();
    if (side < 0 || !link_partner_present())
        return true;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    int pos = -1;
    for (int i = 0; ids && i < n; i++)
        if (ids[i] == id)
            pos = i;
    SDL_free(ids);
    return n < 2 || pos == side;
}

static void pad_event(const SDL_Event *e)
{
    static uint8_t stick_x, stick_y;
    if ((e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e->type == SDL_EVENT_GAMEPAD_BUTTON_UP) &&
        !pad_is_mine(e->gbutton.which))
        return;
    if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && !pad_is_mine(e->gaxis.which))
        return;
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED: {
        SDL_Gamepad *g = SDL_OpenGamepad(e->gdevice.which);
        if (g)
            picocalc_sim_log("gamepad: %s", SDL_GetGamepadName(g) ? SDL_GetGamepadName(g) : "connected");
        break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
        SDL_Gamepad *g = SDL_GetGamepadFromID(e->gdevice.which);
        if (g)
            SDL_CloseGamepad(g);
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        uint8_t k = pad_key((SDL_GamepadButton)e->gbutton.button);
        if (!k || !picocalc_sim_machine_running())
            break;
        if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
            picocalc_sim_keys_press(k);
        else
            picocalc_sim_keys_release(k);
        break;
    }
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
        if (k == *held || !picocalc_sim_machine_running())
            break;
        if (*held)
            picocalc_sim_keys_release(*held);
        if (k)
            picocalc_sim_keys_press(k);
        *held = k;
        break;
    }
    default:
        break;
    }
}

// Where the device frame was drawn last, for the mouse (PBConPC).
static SDL_FRect skin_dst;

static SDL_FRect lcd_rect(SDL_Renderer *renderer, float top, bool *whole)
{
    int w, h;
    SDL_GetCurrentRenderOutputSize(renderer, &w, &h);
    float aw = fmaxf((float)w - ui_side_width(), 0.0f), ah = (float)h - top;
    if (skin_enabled && !ui_fullscreen()) {
        SDL_FRect lcd;
        skin_dst = skin_fit(0, top, aw, ah, &lcd);
        *whole = false;
        return lcd;
    }
    skin_dst = (SDL_FRect){0};
    float scale = fmaxf(fminf(aw / PICOCALC_SIM_LCD_WIDTH, ah / PICOCALC_SIM_LCD_HEIGHT), 0.0f);
    float n = floorf(scale);
    *whole = n >= 1 && (scale - n) * PICOCALC_SIM_LCD_WIDTH < 1;
    if (*whole)
        scale = n;
    float lw = floorf(PICOCALC_SIM_LCD_WIDTH * scale), lh = floorf(PICOCALC_SIM_LCD_HEIGHT * scale);
    return (SDL_FRect){floorf((aw - lw) / 2), top + floorf((ah - lh) / 2), lw, lh};
}

int picocalc_sim_frontend_sdl_run(void)
{
    // game controllers are welcome but not needed
    // also while the window is in the background: two linked windows on one PC, one gamepad each
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        picocalc_sim_log("SDL could not start (%s); running headless", SDL_GetError());
        return picocalc_sim_frontend_headless_run();
    }
#ifdef SDL_PLATFORM_WINDOWS
    SDL_SetWindowsMessageHook(windows_message, NULL);
#endif
    SDL_Window *window = create_window();
    if (!window) {
        picocalc_sim_log("SDL window failed (%s); running headless", SDL_GetError());
        SDL_Quit();
        return picocalc_sim_frontend_headless_run();
    }
    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
    if (renderer)
        picocalc_sim_log("window: SDL's %s renderer", SDL_GetRendererName(renderer));
    SDL_SetRenderVSync(renderer, 1);
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                             PICOCALC_SIM_LCD_WIDTH, PICOCALC_SIM_LCD_HEIGHT);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    SDL_ScaleMode lcd_mode = SDL_SCALEMODE_NEAREST;
    static uint32_t pixels[PICOCALC_SIM_LCD_WIDTH * PICOCALC_SIM_LCD_HEIGHT];
    skin_init(renderer);
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        picocalc_sim_log("no gamepads: %s", SDL_GetError());
    ui_init(window, renderer);
    ui_fit_window(picocalc_sim_opts.scale);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);
    if (picocalc_sim_opts.fullscreen)
        ui_set_fullscreen(true);
    SDL_AudioStream *audio = open_audio();
    bool muted = picocalc_sim_opts.mute;

    uint64_t start = picocalc_sim_mono_ns();
    uint32_t last_lcd = ~0u, last_sb = ~0u;
    uint8_t mouse_key = 0;
    char last_title[256] = "";
    int code = 0;
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            bool ui_took = ui_event(&e);
            switch (e.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                // SDL quits by itself only once every window is closed, Settings and the like too.
                if (SDL_GetWindowFromEvent(&e) == window)
                    running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.repeat || ui_took || !picocalc_sim_machine_running())
                    break;
                if (e.key.key == SDLK_F10 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)))
                    ui_set_fullscreen(!ui_fullscreen());
                else if (e.key.key == SDLK_ESCAPE && ui_fullscreen())
                    ui_set_fullscreen(false);
                else if (e.key.key == SDLK_F11)
                    ui_screenshot();
                else if (e.key.key == SDLK_F12) {
                    // The BOOTSEL button on the Pico, held while the reset button is pressed.
                    picocalc_sim_bootsel_button((e.key.mod & SDL_KMOD_SHIFT) != 0);
                    picocalc_sim_reboot("reset button");
                } else
                    key_down(e.key.key);
                break;
            case SDL_EVENT_KEY_UP:
                // even when the UI has the keyboard, a key the PicoCalc saw go down must come up
                key_up(e.key.key);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EVENT_GAMEPAD_REMOVED:
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                pad_event(&e);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                // Clicking the joystick and buttons of the device frame presses them.
                SDL_Event c = e;
                SDL_ConvertEventToRenderCoordinates(renderer, &c);
                uint8_t key = 0;
                if (e.type == SDL_EVENT_MOUSE_BUTTON_UP)
                    key = 0;
                else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !ui_took && c.button.button == SDL_BUTTON_LEFT)
                    key = skin_key_at(skin_dst, c.button.x, c.button.y);
                else if (e.type == SDL_EVENT_MOUSE_MOTION && mouse_key && (c.motion.state & SDL_BUTTON_LMASK))
                    key = skin_key_at(skin_dst, c.motion.x, c.motion.y);
                else
                    break;
                if (key != mouse_key && picocalc_sim_machine_running()) {
                    if (mouse_key)
                        picocalc_sim_keys_release(mouse_key);
                    if (key)
                        picocalc_sim_keys_press(key);
                    mouse_key = key;
                }
                break;
            }
            case SDL_EVENT_DROP_FILE:
                ui_drop_file(e.drop.data);
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                release_all();
                break;
            default:
                break;
            }
        }

        if (picocalc_sim_restart_requested()) {
            SDL_Keycode carried[sizeof(held) / sizeof(held[0])];
            int ncarried = nheld;
            for (int i = 0; i < nheld; i++)
                carried[i] = held[i].sym;
            // The devices are about to be freed, so nothing may hold a key or pull sound from them.
            release_all();
            if (audio)
                SDL_DestroyAudioStream(audio);
            audio = NULL;
            ui_restarting();
            // A machine that fails to start stays stopped, and the UI says why.
            if (picocalc_sim_machine_restart()) {
                press_held(carried, ncarried);
                audio = open_audio();
            }
            last_lcd = last_sb = ~0u;
        }
        if (picocalc_sim_opts.mute != muted) {
            muted = picocalc_sim_opts.mute;
            if (audio)
                SDL_DestroyAudioStream(audio);
            audio = open_audio();
        }
        if (picocalc_sim_quit_requested(&code))
            break;
        if (picocalc_sim_opts.timeout_s > 0 && (picocalc_sim_mono_ns() - start) / 1e9 >= picocalc_sim_opts.timeout_s)
            break;

        bool on = picocalc_sim_machine_running();
        uint32_t lcd_gen = on ? st7789_generation(picocalc_sim_picocalc.lcd) : 0;
        uint32_t sb_gen = on ? picoboy_backlight() : 0;
        if (on && (lcd_gen != last_lcd || sb_gen != last_sb)) {
            last_lcd = lcd_gen;
            last_sb = sb_gen;
            st7789_render(picocalc_sim_picocalc.lcd, pixels, (uint8_t)sb_gen);
            SDL_UpdateTexture(texture, NULL, pixels, PICOCALC_SIM_LCD_WIDTH * 4);
        }
        const char *title = picocalc_sim_window_title();
        if (strcmp(title, last_title) != 0) {
            snprintf(last_title, sizeof(last_title), "%s", title);
            SDL_SetWindowTitle(window, title);
        }
        float top = ui_frame();
        if (ui_fullscreen())
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        else {
            uint8_t br, bg, bb;   // the theme's window colour
            ui_backdrop(&br, &bg, &bb);
            SDL_SetRenderDrawColor(renderer, br, bg, bb, 255);
        }
        SDL_RenderClear(renderer);
        bool whole;
        SDL_FRect dst = lcd_rect(renderer, top, &whole);
        bool frame = skin_enabled && !ui_fullscreen();
        if (frame)
            skin_draw_frame(renderer, skin_dst);
        if (on) {
            SDL_ScaleMode mode = whole ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_PIXELART;
            if (mode != lcd_mode && SDL_SetTextureScaleMode(texture, mode))
                lcd_mode = mode;
            SDL_RenderTexture(renderer, texture, NULL, &dst);
            skin_draw_lcd_corners(renderer, dst, frame ? &skin_dst : NULL);
            if (frame)
                skin_draw_leds(renderer, skin_dst, picoboy_leds(), picoboy_rgb());
        }
        ui_render();
        // PBConPC test hook: PBCONPC_WINDOW_SHOT=file.png saves the whole window after
        // PBCONPC_WINDOW_SHOT_MS (default 6000) ms, so the frame can be checked without a screen
        // (SDL_VIDEO_DRIVER=offscreen).
        static const char *shot_path;
        static bool shot_done;
        if (!shot_done && (shot_path || (shot_path = SDL_getenv("PBCONPC_WINDOW_SHOT")))) {
            const char *ms = SDL_getenv("PBCONPC_WINDOW_SHOT_MS");
            if ((picocalc_sim_mono_ns() - start) / 1000000 >= (uint64_t)(ms ? atoi(ms) : 6000)) {
                shot_done = true;
                SDL_Surface *shot = SDL_RenderReadPixels(renderer, NULL);
                SDL_Surface *argb = shot ? SDL_ConvertSurface(shot, SDL_PIXELFORMAT_ARGB8888) : NULL;
                if (argb && argb->pitch == argb->w * 4)
                    picocalc_sim_png_write(shot_path, argb->pixels, argb->w, argb->h);
                picocalc_sim_log("window shot: %s", argb ? shot_path : SDL_GetError());
                SDL_DestroySurface(argb);
                SDL_DestroySurface(shot);
            }
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(8);
    }

    if (audio)
        SDL_DestroyAudioStream(audio);
    ui_shutdown();
    skin_free();
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return code;
}

#endif
