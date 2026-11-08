#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(SDL_Window *window, SDL_Renderer *renderer);
void ui_shutdown(void);
// Sizes the window for the LCD at a scale plus the menu bar, and the terminal beside it when that
// is shown.
void ui_fit_window(float scale);
// The width the terminal takes at the window's right, in render pixels.
float ui_side_width(void);
// True when the event belongs to the UI, such as a key typed into one of its text fields.
bool ui_event(const SDL_Event *e);
// True while a text field, the terminal or a window such as Settings has the keyboard rather than
// the PicoCalc.
bool ui_wants_keyboard(void);
void ui_drop_file(const char *path);
// Saves a screenshot and shows over the LCD where it went, or why it failed.
void ui_screenshot(void);
// Must come before every restart, so that a failed one can show what went wrong.
void ui_restarting(void);
// Lays out this frame's UI and returns the height of the menu bar in render pixels.
float ui_frame(void);
// PBConPC: fullscreen shows only the LCD, without the device frame, menu bar or terminal.
bool ui_fullscreen(void);
// PBConPC: the tilt sensor, x and y in g (the rest of 1 g is z), from the pad.
void ui_tilt(float x, float y);
// PBConPC: light or dark changed (system or View -> Theme); the colour behind the device frame.
void ui_theme_changed(void);
void ui_backdrop(uint8_t *r, uint8_t *g, uint8_t *b);
void ui_set_fullscreen(bool on);
void ui_render(void);

#ifdef __cplusplus
}
#endif
