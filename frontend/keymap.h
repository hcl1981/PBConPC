#pragma once

// PBConPC: which host keys press the PicoBoy's joystick and buttons, set in Settings -> Keys and
// kept in settings.ini as key-<control>-1..3 (SDL key names). The default is the old fixed map:
// arrow keys for the joystick, A, Enter or keypad Enter for its center, S for button A, D for B.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { KM_UP, KM_DOWN, KM_LEFT, KM_RIGHT, KM_CENTRE, KM_A, KM_B, KM_COUNT };
#define KM_SLOTS 3

// The host keys (SDL_Keycode, 0 for none) of each control.
extern uint32_t keymap_keys[KM_COUNT][KM_SLOTS];

// Loads the map from the settings (the default where none is saved).
void keymap_load(void);
// Saves one control's keys.
void keymap_save(int control);
void keymap_reset(void);
// The control a host key presses, or -1.
int keymap_control(uint32_t sym);
// The key code picoboy_key takes for a control.
uint8_t keymap_code(int control);
const char *keymap_name(int control);
// "Up, W" and so on, for the Keys window.
void keymap_describe(int control, char *out, unsigned size);

#ifdef __cplusplus
}
#endif
