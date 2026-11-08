#include "theme.h"

#include <SDL3/SDL.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

// See theme.h.

theme_mode theme_choice = THEME_SYSTEM;

namespace {

ImVec4 hex(unsigned rgb, float a = 1.0f)
{
    return ImVec4((rgb >> 16 & 0xff) / 255.0f, (rgb >> 8 & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, a);
}

// The colours one look is made of; hover and press are shades laid over the background.
struct palette {
    unsigned window, bar, popup, text, dim, border, frame;
    bool tint_hover;   // hover in the accent colour (Arc, macOS) rather than grey (GNOME, Windows 11)
};

const palette adwaita_light = {0xfafafa, 0xebebeb, 0xffffff, 0x2e3436, 0x929595, 0xd8d8d8, 0xffffff, false};
const palette adwaita_dark = {0x242424, 0x303030, 0x383838, 0xf6f5f4, 0x8f8f8f, 0x1b1b1b, 0x2e2e2e, false};
const palette arc_light = {0xf5f6f7, 0xe7e8eb, 0xffffff, 0x5c616c, 0xa9acb2, 0xdcdfe3, 0xffffff, true};
const palette arc_dark = {0x383c4a, 0x2f343f, 0x383c4a, 0xd3dae3, 0x7c818c, 0x2b2e39, 0x404552, true};
const palette win11_light = {0xf3f3f3, 0xf3f3f3, 0xf9f9f9, 0x1b1b1b, 0x8a8a8a, 0xe0e0e0, 0xffffff, false};
const palette win11_dark = {0x202020, 0x202020, 0x2c2c2c, 0xffffff, 0x8a8a8a, 0x3a3a3a, 0x2d2d2d, false};
const palette breeze_light = {0xeff0f1, 0xeff0f1, 0xffffff, 0x232629, 0x7f8c8d, 0xbdc3c7, 0xfcfcfc, true};
const palette breeze_dark = {0x31363b, 0x31363b, 0x2a2e32, 0xeff0f1, 0x7f8c8d, 0x1b1e20, 0x232629, true};
const palette mac_light = {0xececec, 0xe8e8e8, 0xf6f6f6, 0x1d1d1f, 0x9a9a9a, 0xd0d0d0, 0xffffff, true};
const palette mac_dark = {0x1e1e1e, 0x2a2a2a, 0x2c2c2e, 0xf5f5f7, 0x8a8a8d, 0x3c3c3c, 0x3a3a3c, true};

// What the system said, read once.
struct system_look {
    bool read = false;
    std::string family, font_file;   // the UI font
    float font_px = 0;               // its size at scale 1
    std::string gtk_theme;
    unsigned accent = 0x3584e4;
    const palette *light = &adwaita_light, *dark = &adwaita_dark;
} sys;

bool dark_now;

#if defined(__linux__) || defined(__FreeBSD__)
std::string run(const char *cmd)
{
    std::string out;
    if (FILE *p = popen(cmd, "r")) {
        char buf[512];
        while (fgets(buf, sizeof(buf), p))
            out += buf;
        pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return out;
}

std::string gsetting(const char *key)
{
    std::string cmd = std::string("gsettings get org.gnome.desktop.interface ") + key + " 2>/dev/null";
    std::string v = run(cmd.c_str());
    if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'')
        v = v.substr(1, v.size() - 2);
    return v;
}

bool contains_nocase(const std::string &s, const char *what)
{
    std::string a = s;
    for (char &c : a)
        c = (char)std::tolower((unsigned char)c);
    return a.find(what) != std::string::npos;
}
#endif

bool exists(const std::string &path)
{
    if (path.empty())
        return false;
    FILE *f = fopen(path.c_str(), "rb");
    if (f)
        fclose(f);
    return f != nullptr;
}

void read_system()
{
    if (sys.read)
        return;
    sys.read = true;
#if defined(_WIN32)
    sys.light = &win11_light;
    sys.dark = &win11_dark;
    sys.accent = 0x0067c0;
    DWORD abgr = 0, size = sizeof(abgr);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD,
                     nullptr, &abgr, &size) == ERROR_SUCCESS)
        sys.accent = (abgr & 0xff) << 16 | (abgr & 0xff00) | (abgr >> 16 & 0xff);
    char windir[MAX_PATH];
    UINT n = GetWindowsDirectoryA(windir, sizeof(windir));
    if (n && n < sizeof(windir)) {
        sys.font_file = std::string(windir) + "\\Fonts\\segoeui.ttf";
        sys.family = "Segoe UI";
    }
    sys.font_px = 14;   // WinUI menus: Segoe UI 14 px
#elif defined(__APPLE__)
    sys.light = &mac_light;
    sys.dark = &mac_dark;
    sys.accent = 0x007aff;
    for (const char *f : {"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc"})
        if (exists(f)) {
            sys.font_file = f;
            break;
        }
    sys.font_px = 13;
#else
    // KDE Plasma: font and accent colour from kdeglobals, Breeze's colours
    const char *desk = getenv("XDG_CURRENT_DESKTOP");
    if (desk && strstr(desk, "KDE")) {
        std::string cfg = getenv("XDG_CONFIG_HOME") ? getenv("XDG_CONFIG_HOME") : std::string(getenv("HOME") ? getenv("HOME") : "") + "/.config";
        sys.light = &breeze_light;
        sys.dark = &breeze_dark;
        sys.accent = 0x3daee9;
        std::string family = "Noto Sans";
        float pt = 10;
        if (FILE *f = fopen((cfg + "/kdeglobals").c_str(), "r")) {
            char line[512];
            std::string group;
            while (fgets(line, sizeof(line), f)) {
                std::string l = line;
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
                    l.pop_back();
                if (!l.empty() && l[0] == '[') {
                    group = l;
                    continue;
                }
                size_t eq = l.find('=');
                if (eq == std::string::npos)
                    continue;
                std::string key = l.substr(0, eq), val = l.substr(eq + 1);
                if (group == "[General]" && key == "font") {   // "Noto Sans,10,-1,5,50,..."
                    size_t c1 = val.find(',');
                    family = val.substr(0, c1);
                    if (c1 != std::string::npos)
                        pt = (float)atof(val.c_str() + c1 + 1);
                } else if (group == "[General]" && key == "ColorScheme") {   // "BreezeDark": dark
                    sys.gtk_theme = val;
                } else if (group == "[General]" && key == "AccentColor") {   // "61,174,233"
                    int r, g, b;
                    if (sscanf(val.c_str(), "%d,%d,%d", &r, &g, &b) == 3)
                        sys.accent = (unsigned)(r << 16 | g << 8 | b);
                }
            }
            fclose(f);
        }
        if (pt < 6 || pt > 30)
            pt = 10;
        sys.font_px = pt * 96.0f / 72.0f;
        std::string safe;
        for (char c : family)
            if (std::isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_')
                safe += c;
        sys.family = safe.empty() ? "sans-serif" : safe;
        sys.font_file = run(("fc-match -f '%{file}' '" + sys.family + ":weight=regular' 2>/dev/null").c_str());
        return;
    }
    // GNOME and its relatives: the font, theme and accent colour from gsettings
    sys.gtk_theme = gsetting("gtk-theme");
    std::string font = gsetting("font-name");   // e.g. "Ubuntu 11"
    float pt = 11;
    size_t sp = font.rfind(' ');
    if (sp != std::string::npos && sp + 1 < font.size() && std::isdigit((unsigned char)font[sp + 1])) {
        pt = (float)atof(font.c_str() + sp + 1);
        font.resize(sp);
    }
    if (pt < 6 || pt > 30)
        pt = 11;
    sys.font_px = pt * 96.0f / 72.0f;
    std::string safe;
    for (char c : font)
        if (std::isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_')
            safe += c;
    sys.family = safe.empty() ? "sans-serif" : safe;
    std::string cmd = "fc-match -f '%{file}' '" + sys.family + ":weight=regular' 2>/dev/null";
    sys.font_file = run(cmd.c_str());
    if (contains_nocase(sys.gtk_theme, "arc")) {
        sys.light = &arc_light;
        sys.dark = &arc_dark;
        sys.accent = 0x5294e2;
    } else {
        static const struct { const char *name; unsigned rgb; } accents[] = {
            {"blue", 0x3584e4}, {"teal", 0x2190a4},   {"green", 0x3a944a}, {"yellow", 0xc88800}, {"orange", 0xed5b00},
            {"red", 0xe62d42},  {"pink", 0xd56199},   {"purple", 0x9141ac}, {"slate", 0x6f8396},
        };
        std::string a = gsetting("accent-color");
        bool found = false;
        for (auto &x : accents)
            if (a == x.name) {
                sys.accent = x.rgb;
                found = true;
            }
        if (!found && contains_nocase(sys.gtk_theme, "yaru"))
            sys.accent = 0xe95420;   // Ubuntu's orange
    }
#endif
}

bool system_dark()
{
    switch (SDL_GetSystemTheme()) {
    case SDL_SYSTEM_THEME_DARK:
        return true;
    case SDL_SYSTEM_THEME_LIGHT:
        return false;
    default:
        break;
    }
#if defined(__linux__) || defined(__FreeBSD__)
    // No preference given (GNOME's "default"): the GTK theme's name tells; "Arc-Darker" has only a
    // dark title bar.
    const std::string &t = sys.gtk_theme;
    return contains_nocase(t, "dark") && !contains_nocase(t, "darker");
#else
    return false;
#endif
}

} // namespace

bool theme_dark(void)
{
    return dark_now;
}

ImVec4 theme_backdrop(void)
{
    read_system();
    return hex((dark_now ? sys.dark : sys.light)->window);
}

void theme_colours(void)
{
    read_system();
    dark_now = theme_choice == THEME_DARK || (theme_choice == THEME_SYSTEM && system_dark());
    const palette &p = dark_now ? *sys.dark : *sys.light;
    ImGuiStyle &style = ImGui::GetStyle();
    if (dark_now)
        ImGui::StyleColorsDark(&style);
    else
        ImGui::StyleColorsLight(&style);
    ImVec4 *c = style.Colors;
    // shades over the background: black on light, white on dark
    auto shade = [&](float a) { return dark_now ? ImVec4(1, 1, 1, a) : ImVec4(0, 0, 0, a); };
    ImVec4 accent = hex(sys.accent);
    auto tint = [&](float a) { return hex(sys.accent, a); };

    c[ImGuiCol_Text] = hex(p.text);
    c[ImGuiCol_TextDisabled] = hex(p.dim);
    c[ImGuiCol_WindowBg] = hex(p.window);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = hex(p.popup);
    c[ImGuiCol_MenuBarBg] = hex(p.bar);
    c[ImGuiCol_Border] = hex(p.border);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = hex(p.frame);
    c[ImGuiCol_FrameBgHovered] = dark_now ? hex(p.frame) : hex(0xf6f6f6);
    c[ImGuiCol_FrameBgActive] = hex(p.frame);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgCollapsed] = hex(p.bar);
    c[ImGuiCol_TitleBgActive] = hex(p.bar);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = shade(0.22f);
    c[ImGuiCol_ScrollbarGrabHovered] = shade(0.34f);
    c[ImGuiCol_ScrollbarGrabActive] = shade(0.45f);
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = tint(0.8f);
    c[ImGuiCol_Button] = shade(dark_now ? 0.10f : 0.06f);
    c[ImGuiCol_ButtonHovered] = shade(dark_now ? 0.15f : 0.10f);
    c[ImGuiCol_ButtonActive] = shade(dark_now ? 0.22f : 0.16f);
    if (p.tint_hover) {
        c[ImGuiCol_Header] = tint(0.22f);
        c[ImGuiCol_HeaderHovered] = tint(0.28f);
        c[ImGuiCol_HeaderActive] = tint(0.38f);
    } else {
        c[ImGuiCol_Header] = shade(0.08f);
        c[ImGuiCol_HeaderHovered] = shade(dark_now ? 0.10f : 0.07f);
        c[ImGuiCol_HeaderActive] = shade(dark_now ? 0.16f : 0.12f);
    }
    c[ImGuiCol_Separator] = shade(dark_now ? 0.12f : 0.10f);
    c[ImGuiCol_SeparatorHovered] = tint(0.6f);
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = tint(0.5f);
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_InputTextCursor] = hex(p.text);
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered] = shade(0.07f);
    c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = shade(dark_now ? 0.10f : 0.07f);
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmedSelectedOverline] = shade(0.25f);
    c[ImGuiCol_TableHeaderBg] = shade(0.05f);
    c[ImGuiCol_TableBorderStrong] = c[ImGuiCol_TableBorderLight] = hex(p.border);
    c[ImGuiCol_TableRowBgAlt] = shade(0.03f);
    c[ImGuiCol_TextSelectedBg] = tint(0.35f);
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, dark_now ? 0.45f : 0.25f);
}

void theme_setup(float scale)
{
    read_system();
    ImGuiStyle &style = ImGui::GetStyle();
    style = ImGuiStyle();
    theme_colours();
    style.WindowPadding = ImVec2(12, 10);
    style.FramePadding = ImVec2(8, 5);
    style.ItemSpacing = ImVec2(8, 6);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.CellPadding = ImVec2(6, 4);
    style.IndentSpacing = 20;
    style.ScrollbarSize = 12;
    style.GrabMinSize = 12;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 1;
    style.TabBorderSize = 0;
    style.WindowRounding = 0;   // windows fill their OS window
    style.ChildRounding = 6;
    style.FrameRounding = 6;
    style.PopupRounding = 8;
    style.ScrollbarRounding = 6;
    style.GrabRounding = 6;
    style.TabRounding = 6;
    style.SeparatorTextBorderSize = 1;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;

    ImGuiIO &io = ImGui::GetIO();
    ImFont *font = nullptr;
    if (exists(sys.font_file)) {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        font = io.Fonts->AddFontFromFileTTF(sys.font_file.c_str(), sys.font_px, &cfg);
    }
    if (font) {
        style.FontSizeBase = sys.font_px;
    } else {
        io.Fonts->AddFontDefault();
    }
}

bool theme_begin_menu(const char *label, bool enabled)
{
    const float s = ImGui::GetStyle().FontScaleDpi;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * s, 6 * s));
    // test hook: PBCONPC_TEST_MENU=View,Theme keeps those menus open (for window shots)
    static const char *test = SDL_getenv("PBCONPC_TEST_MENU");
    if (test) {
        std::string list = std::string(",") + test + ",";
        if (list.find(std::string(",") + label + ",") != std::string::npos && !ImGui::IsPopupOpen(label))
            ImGui::OpenPopup(label);
    }
    bool open = ImGui::BeginMenu(label, enabled);
    ImGui::PopStyleVar();
    if (open)
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * s, 10 * s));
    return open;
}

void theme_end_menu(void)
{
    ImGui::PopStyleVar();
    ImGui::EndMenu();
}
