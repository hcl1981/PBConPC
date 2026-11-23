#pragma once

// PBConPC: the window shows the LCD inside a render of the PicoBoy Color Plus (assets/pbc_front.png,
// made from the board's 3D model with Blender for pbcplus.de). Clicking the joystick and buttons
// presses them; the three LEDs light up.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef PICOCALC_SIM_HAVE_SDL
#include <SDL3/SDL.h>

// Whether the frame is drawn; the View menu switches it.
extern bool skin_enabled;

bool skin_init(SDL_Renderer *renderer);
void skin_free(void);
// The frame's size in its own pixels for an LCD drawn `scale` times its size.
void skin_window_size(float scale, float *w, float *h);
// Fits the frame into the area and gives where the LCD goes inside it.
SDL_FRect skin_fit(float x, float y, float w, float h, SDL_FRect *lcd);
// Draws the frame, and over the LCD the glow of the LEDs that are on (bit 0 green, 1 yellow, 2 red).
void skin_draw_frame(SDL_Renderer *renderer, SDL_FRect dst);
void skin_draw_leds(SDL_Renderer *renderer, SDL_FRect dst, unsigned leds, uint32_t rgb);
// The panel's rounded corners: black over the LCD's corners where the real glass hides them
// (radius 42 panel pixels, measured with a camera, tests/pbc/cornertest), in any view.
// frame_dst: where the device frame is drawn, or NULL without it (then the corners go black).
void skin_draw_lcd_corners(SDL_Renderer *renderer, SDL_FRect lcd, const SDL_FRect *frame_dst);
// The key (keys.h) under a point in render coordinates, or 0.
uint8_t skin_key_at(SDL_FRect dst, float x, float y);
// A finger on the frame (Android): the keys it holds, up to two for a diagonal, given where it
// went down (x0, y0) and where it is now. A finger that went down on the joystick steers by its
// direction from the joystick's middle however far it strays; one that went down on the knob and
// stays there pushes the joystick in. Right of the joystick, the nearer of A and B is pressed.
int skin_touch_keys(SDL_FRect dst, float x0, float y0, float x, float y, uint8_t keys[2]);
// A light on the controls these keys belong to, so a finger sees what it holds.
void skin_draw_touch(SDL_Renderer *renderer, SDL_FRect dst, const uint8_t *keys, int n);
#endif

#ifdef __cplusplus
}
#endif
