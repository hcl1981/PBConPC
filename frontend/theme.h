#pragma once

// PBConPC: the menus and windows in the style of the system they run on: its UI font and size,
// light or dark as the system is set (following changes while running), its accent colour, and
// the spacing and rounded popups of GNOME, Windows 11 or macOS rather than ImGui's own look.

#include "imgui.h"

enum theme_mode { THEME_SYSTEM, THEME_LIGHT, THEME_DARK };

extern theme_mode theme_choice;

// Sizes, colours and font for the current ImGui context (once per context, before its first frame).
void theme_setup(float scale);
// Colours only, for the current context: after the system's or the user's choice changed.
void theme_colours(void);
// Whether the colours in use are dark, and the colour behind the device frame.
bool theme_dark(void);
ImVec4 theme_backdrop(void);
// Menus: popups with the system's padding and roomier items.
bool theme_begin_menu(const char *label, bool enabled = true);
void theme_end_menu(void);
