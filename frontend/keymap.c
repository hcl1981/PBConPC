#include "keymap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "picocalc-sim/sim.h"

#include "picocalc-sim/frontend.h"
#include "picocalc-sim/keys.h"

#ifdef PICOCALC_SIM_HAVE_SDL
#include <SDL3/SDL.h>
#endif

// See keymap.h.

uint32_t keymap_keys[KM_COUNT][KM_SLOTS];

static const char *const ids[KM_COUNT] = {"up", "down", "left", "right", "centre", "a", "b"};
static const char *const names[KM_COUNT] = {"Joystick up", "Joystick down", "Joystick left", "Joystick right",
                                            "Joystick center", "Button A", "Button B"};

#ifdef PICOCALC_SIM_HAVE_SDL
static const uint32_t defaults[KM_COUNT][KM_SLOTS] = {
    {SDLK_UP}, {SDLK_DOWN}, {SDLK_LEFT}, {SDLK_RIGHT}, {SDLK_A, SDLK_RETURN, SDLK_KP_ENTER}, {SDLK_S}, {SDLK_D},
};
#else
static const uint32_t defaults[KM_COUNT][KM_SLOTS] = {{0}};
#endif

void keymap_reset(void)
{
    memcpy(keymap_keys, defaults, sizeof(keymap_keys));
}

void keymap_load(void)
{
    keymap_reset();
#ifdef PICOCALC_SIM_HAVE_SDL
    for (int c = 0; c < KM_COUNT; c++) {
        bool any = false;
        uint32_t keys[KM_SLOTS] = {0};
        for (int s = 0; s < KM_SLOTS; s++) {
            char key[32];
            snprintf(key, sizeof(key), "key-%s-%d", ids[c], s + 1);
            const char *v = picocalc_sim_settings_get(key);
            if (!v)
                continue;
            any = true;
            keys[s] = *v && strcmp(v, "none") ? (uint32_t)SDL_GetKeyFromName(v) : 0;
        }
        if (any)
            memcpy(keymap_keys[c], keys, sizeof(keys));
    }
#endif
    if (getenv("PBCONPC_KEYMAP_LOG"))   // for tests: the map as loaded
        for (int c = 0; c < KM_COUNT; c++) {
            char text[128];
            keymap_describe(c, text, sizeof(text));
            picocalc_sim_log("keys: %s = %s", names[c], text);
        }
}

void keymap_save(int c)
{
#ifdef PICOCALC_SIM_HAVE_SDL
    bool same = !memcmp(keymap_keys[c], defaults[c], sizeof(defaults[c]));
    for (int s = 0; s < KM_SLOTS; s++) {
        char key[32];
        snprintf(key, sizeof(key), "key-%s-%d", ids[c], s + 1);
        uint32_t k = keymap_keys[c][s];
        // the default is saved as nothing at all, so that a later default applies
        picocalc_sim_settings_set(key, same ? NULL : k ? SDL_GetKeyName((SDL_Keycode)k) : "none");
    }
    picocalc_sim_settings_save();
#else
    (void)c;
#endif
}

int keymap_control(uint32_t sym)
{
    if (!sym)
        return -1;
    for (int c = 0; c < KM_COUNT; c++)
        for (int s = 0; s < KM_SLOTS; s++)
            if (keymap_keys[c][s] == sym)
                return c;
    return -1;
}

uint8_t keymap_code(int c)
{
    static const uint8_t codes[KM_COUNT] = {PC_KEY_UP, PC_KEY_DOWN, PC_KEY_LEFT, PC_KEY_RIGHT, PC_KEY_ENTER, 's', 'd'};
    return c >= 0 && c < KM_COUNT ? codes[c] : 0;
}

const char *keymap_name(int c)
{
    return c >= 0 && c < KM_COUNT ? names[c] : "";
}

void keymap_describe(int c, char *out, unsigned size)
{
    out[0] = 0;
#ifdef PICOCALC_SIM_HAVE_SDL
    for (int s = 0; s < KM_SLOTS; s++)
        if (keymap_keys[c][s]) {
            size_t n = strlen(out);
            snprintf(out + n, size - n, "%s%s", n ? ", " : "", SDL_GetKeyName((SDL_Keycode)keymap_keys[c][s]));
        }
#endif
    if (!out[0])
        snprintf(out, size, "(none)");
}
