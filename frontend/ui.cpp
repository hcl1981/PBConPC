#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>
extern char **environ;
#endif

#include "imgui.h"
// Only to ask the SDL backend for text input while the terminal has the keyboard.
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "misc/cpp/imgui_stdlib.h"

#include "picocalc-sim/devices.h"
#include "picocalc-sim/frontend.h"
#include "picocalc-sim/sim.h"
#include "terminal.h"
#include "link.h"
#include "profile.h"
#include "skin.h"
#include "theme.h"
#include "keymap.h"
#include "ui.h"

namespace {

SDL_Window *window;
SDL_Renderer *renderer;
float content_scale = 1.0f;
float menu_height;
Uint32 dialog_event;

bool show_settings, show_log, show_keys, show_about, show_terminal, show_tilt;
bool ask_erase;
// The terminal's character cell, and what it has to send that the host has not taken yet.
float term_char_w, term_line_h;
std::string term_input;
unsigned term_seen;

struct access_point {
    std::string ssid, password;
    bool operator==(const access_point &o) const { return ssid == o.ssid && password == o.password; }
};

enum { FLASH_DEFAULT, FLASH_MEMORY, FLASH_FILE };

// The options the machine reads only as it starts.
struct start_options {
    std::string board, rom, rp2350_rom, flash_file, usbip;
    int flash = FLASH_DEFAULT;
    bool fast = false, trace = false, no_wifi = false;
    std::vector<access_point> aps;
    std::vector<std::string> forwards;

    // Rows left empty are not passed on, so they change nothing.
    bool operator==(const start_options &o) const
    {
        return board == o.board && rom == o.rom && rp2350_rom == o.rp2350_rom && flash == o.flash &&
               flash_file == o.flash_file && usbip == o.usbip && fast == o.fast &&
               trace == o.trace && no_wifi == o.no_wifi && named(aps) == named(o.aps) &&
               filled(forwards) == filled(o.forwards);
    }

    static std::vector<access_point> named(std::vector<access_point> v)
    {
        v.erase(std::remove_if(v.begin(), v.end(), [](const access_point &a) { return a.ssid.empty(); }), v.end());
        return v;
    }

    static std::vector<std::string> filled(std::vector<std::string> v)
    {
        v.erase(std::remove(v.begin(), v.end(), std::string()), v.end());
        return v;
    }
};

start_options pending, applied;

// What picocalc_sim_opts points at; kept apart from pending, so editing a setting never moves a
// string that the machine may still read.
struct {
    std::string board, rom, rp2350_rom, flash, card, shots, wav, usbip;
    std::string aps[16], forwards[16];
} store;

std::deque<std::pair<unsigned, std::string>> log_lines;
unsigned log_seen, start_mark;
bool log_scroll;

// When changed settings are next written to the settings file, or 0.
Uint64 save_at;

enum pick_kind { PICK_UF2, PICK_CARD, PICK_WAV, PICK_ROM, PICK_RP2350_ROM, PICK_FLASH, PICK_SHOTS };
int prompt_kind = -1;
bool ask_path;
std::string prompt_path;

start_options from_opts()
{
    const picocalc_sim_options_t &o = picocalc_sim_opts;
    start_options s;
    if (o.board)
        s.board = o.board;
    if (o.rom_path)
        s.rom = o.rom_path;
    if (o.rp2350_rom_path)
        s.rp2350_rom = o.rp2350_rom_path;
    if (o.flash_path && !strcmp(o.flash_path, "none"))
        s.flash = FLASH_MEMORY;
    else if (o.flash_path) {
        s.flash = FLASH_FILE;
        s.flash_file = o.flash_path;
    }
    if (o.usbip)
        s.usbip = o.usbip;
    s.fast = o.fast;
    s.trace = o.trace;
    s.no_wifi = o.no_wifi;
    for (int i = 0; i < o.wifi_ap_count; i++) {
        const char *ap = o.wifi_aps[i], *colon = strchr(ap, ':');
        if (colon)
            s.aps.push_back({std::string(ap, (size_t)(colon - ap)), colon + 1});
        else
            s.aps.push_back({ap, ""});
    }
    for (int i = 0; i < o.forward_count; i++)
        s.forwards.push_back(o.forwards[i]);
    return s;
}

std::string ap_spec(const access_point &ap)
{
    return ap.password.empty() ? ap.ssid : ap.ssid + ":" + ap.password;
}

void save_soon()
{
    // A slider being dragged changes its setting every frame, so the file waits for it to settle.
    save_at = SDL_GetTicks() + 1000;
}

void remember(const char *key, const char *value)
{
    const char *old = picocalc_sim_settings_get(key);
    if (value ? old && !strcmp(old, value) : !old)
        return;
    picocalc_sim_settings_set(key, value);
    save_soon();
}

void remember_on(const char *key, bool on)
{
    remember(key, on ? "on" : "off");
}

void remember_int(const char *key, int n)
{
    char text[16];
    snprintf(text, sizeof(text), "%d", n);
    remember(key, text);
}

void remember_list(const char *key, const std::vector<std::string> &values)
{
    std::vector<const char *> v;
    for (const std::string &s : values)
        v.push_back(s.c_str());
    picocalc_sim_settings_set_list(key, v.data(), (int)v.size());
    save_soon();
}

// Only what was changed here, so an option given on the command line stays with its own run.
void remember_start_options(const start_options &was, const start_options &now)
{
    if (now.rom != was.rom)
        remember("rom", now.rom.empty() ? nullptr : now.rom.c_str());
    if (now.rp2350_rom != was.rp2350_rom)
        remember("rp2350-rom", now.rp2350_rom.empty() ? nullptr : now.rp2350_rom.c_str());
    if (now.flash != was.flash || now.flash_file != was.flash_file)
        remember("flash", now.flash == FLASH_MEMORY                          ? "none"
                          : now.flash == FLASH_FILE && !now.flash_file.empty() ? now.flash_file.c_str()
                                                                               : nullptr);
    if (now.usbip != was.usbip)
        remember("usbip", now.usbip.empty() ? nullptr : now.usbip.c_str());
    if (now.fast != was.fast)
        remember_on("fast", now.fast);
    if (now.trace != was.trace)
        remember_on("trace", now.trace);
    if (now.no_wifi != was.no_wifi)
        remember_on("no-wifi", now.no_wifi);
    if (!(start_options::named(now.aps) == start_options::named(was.aps))) {
        std::vector<std::string> specs;
        for (const access_point &ap : start_options::named(now.aps))
            specs.push_back(ap_spec(ap));
        remember_list("wifi-ap", specs);
    }
    if (start_options::filled(now.forwards) != start_options::filled(was.forwards))
        remember_list("forward", start_options::filled(now.forwards));
}

void to_opts(const start_options &s)
{
    picocalc_sim_options_t &o = picocalc_sim_opts;
    store.board = s.board;
    o.board = store.board.empty() ? nullptr : store.board.c_str();
    store.rom = s.rom;
    o.rom_path = store.rom.empty() ? nullptr : store.rom.c_str();
    store.rp2350_rom = s.rp2350_rom;
    o.rp2350_rom_path = store.rp2350_rom.empty() ? nullptr : store.rp2350_rom.c_str();
    store.flash = s.flash == FLASH_MEMORY ? "none" : s.flash_file;
    o.flash_path = s.flash == FLASH_DEFAULT || store.flash.empty() ? nullptr : store.flash.c_str();
    store.usbip = s.usbip;
    o.usbip = store.usbip.empty() ? nullptr : store.usbip.c_str();
    // The server is on the far end of the cable, so it needs the cable in.
    if (o.usbip)
        o.usb = true;
    o.fast = s.fast;
    o.trace = s.trace;
    o.no_wifi = s.no_wifi;
    o.wifi_ap_count = 0;
    for (const access_point &ap : s.aps) {
        if (ap.ssid.empty() || o.wifi_ap_count == 16)
            continue;
        std::string &spec = store.aps[o.wifi_ap_count];
        spec = ap_spec(ap);
        o.wifi_aps[o.wifi_ap_count++] = spec.c_str();
    }
    o.forward_count = 0;
    for (const std::string &f : s.forwards) {
        if (f.empty() || o.forward_count == 16)
            continue;
        store.forwards[o.forward_count] = f;
        o.forwards[o.forward_count] = store.forwards[o.forward_count].c_str();
        o.forward_count++;
    }
}

void restart(bool erase)
{
    if (picocalc_sim_machine_starts() > 0) {
        // The launch's one-off steps have happened, so from now on a restart is the power switch:
        // the flash keeps what it holds and a recording is not begun again over the old one.
        picocalc_sim_opts.uf2_path = nullptr;
        picocalc_sim_opts.wav_path = nullptr;
    }
    picocalc_sim_opts.erase_flash = erase;
    remember_start_options(applied, pending);
    applied = pending;
    to_opts(applied);
    picocalc_sim_request_restart();
}

// PBConPC: the emulated USB device as a real one of this PC, for PicoBoy Studio (Web Serial),
// Thonny or mpremote. The USB/IP server is switched on (a restart), then tools/pbc-usb-attach
// runs as root through pkexec, asking for the password once; it keeps the device attached,
// also after firmware restarts, until the flag file goes or PBConPC quits.
#define USB_LINK_PORT "3240"
bool usb_link;
char usb_link_flag[4096];

#ifndef _WIN32
// Where pbc-usb-attach lies: beside the program (the AppImage), in ../lib/pbconpc (the .deb's
// /usr/lib/pbconpc), else in the source tree's tools/. Root cannot read inside a running AppImage
// (a FUSE mount of the user's), so from there it is copied into the data folder first.
static void usb_attach_helper(char *out, size_t size)
{
    const char *base = SDL_GetBasePath();
    const char *tries[] = {"pbc-usb-attach", "../lib/pbconpc/pbc-usb-attach"};
    snprintf(out, size, "%s/pbc-usb-attach", PBCONPC_TOOLS_DIR);
    for (const char *t : tries) {
        char path[4096];
        snprintf(path, sizeof(path), "%s%s", base ? base : "", t);
        if (base && access(path, X_OK) == 0) {
            snprintf(out, size, "%s", path);
            break;
        }
    }
    if (!getenv("APPIMAGE") || !getenv("APPDIR") || strncmp(out, getenv("APPDIR"), strlen(getenv("APPDIR"))))
        return;
    char copy[4096];
    picocalc_sim_data_path(copy, sizeof(copy), "pbc-usb-attach", true);
    FILE *in = fopen(out, "rb"), *o = in ? fopen(copy, "wb") : nullptr;
    if (!o) {
        if (in)
            fclose(in);
        return;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, o);
    fclose(in);
    if (fclose(o) == 0 && chmod(copy, 0755) == 0)
        snprintf(out, size, "%s", copy);
}
#endif

void usb_link_set(bool on)
{
    if (!usb_link_flag[0])
        picocalc_sim_data_path(usb_link_flag, sizeof(usb_link_flag), "usb-link.on", true);
    if (!on) {
        remove(usb_link_flag);
        usb_link = false;
        picocalc_sim_log("usb: detaching from this PC");
        return;
    }
    if (pending.usbip.empty() || applied.usbip.empty()) {
        pending.usbip = "127.0.0.1:" USB_LINK_PORT;
        restart(false);
    }
    FILE *f = fopen(usb_link_flag, "w");
    if (f)
        fclose(f);
    const char *port = strrchr(applied.usbip.c_str(), ':');
    port = port ? port + 1 : applied.usbip.c_str();
#ifdef _WIN32
    // usbip-win2 does the attaching; the helper script lies beside pbconpc.exe.
    char script[MAX_PATH];
    const char *base = SDL_GetBasePath();
    snprintf(script, sizeof(script), "%spbc-usb-attach.ps1", base ? base : "");
    char params[2 * MAX_PATH + 128];
    snprintf(params, sizeof(params),
             "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%s\" %s %lu \"%s\"", script, port,
             (unsigned long)GetCurrentProcessId(), usb_link_flag);
    if ((INT_PTR)ShellExecuteA(nullptr, "runas", "powershell.exe", params, nullptr, SW_HIDE) <= 32) {
        picocalc_sim_log("usb: could not start %s as administrator", script);
        remove(usb_link_flag);
        return;
    }
    usb_link = true;
    picocalc_sim_log("usb: attaching to this PC with usbip-win2 (github.com/vadimgrn/usbip-win2, install it once); "
                     "the serial port appears as a COM port");
#else
    char pid[16], helper[4096];
    snprintf(pid, sizeof(pid), "%d", (int)getpid());
    usb_attach_helper(helper, sizeof(helper));
    char *argv[] = {(char *)"pkexec", helper, (char *)port, pid, usb_link_flag, nullptr};
    pid_t child;
    if (posix_spawnp(&child, "pkexec", nullptr, nullptr, argv, environ) != 0) {
        picocalc_sim_log("usb: cannot start pkexec; run: sudo %s", helper);
        remove(usb_link_flag);
        return;
    }
    usb_link = true;
    picocalc_sim_log("usb: attaching to this PC (asks for the password); the serial port appears as /dev/ttyACM...");
#endif
}

// A tooltip that wraps to fit the window it shows in, rather than running off its edge.
void tip(const char *text)
{
    if (!ImGui::BeginItemTooltip())
        return;
    float room = ImGui::GetMainViewport()->Size.x - 4 * ImGui::GetStyle().WindowPadding.x;
    ImGui::PushTextWrapPos(std::max(std::min(ImGui::GetFontSize() * 30, room), ImGui::GetFontSize() * 8));
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (back > slash)
        slash = back;
    return slash ? slash + 1 : path;
}

void set_battery()
{
    if (picocalc_sim_machine_running())
        picoboy_set_battery((uint8_t)picocalc_sim_opts.battery, false);   // AA cells, no charging
}

bool card_in()
{
    return picocalc_sim_machine_running() ? sdcard_present(picocalc_sim_picocalc.sd) : picocalc_sim_opts.card_path != nullptr;
}

void insert_card(const char *path)
{
    // A card swapped under the firmware's cached FAT would be written with the old card's tables.
    if (card_in()) {
        picocalc_sim_log("eject the SD card before inserting another");
        return;
    }
    if (picocalc_sim_machine_running() && !sdcard_insert(picocalc_sim_picocalc.sd, path))
        return;
    store.card = path;
    picocalc_sim_opts.card_path = store.card.c_str();
    remember("card", path);
}

[[maybe_unused]] void eject_card()
{
    if (picocalc_sim_machine_running())
        sdcard_eject(picocalc_sim_picocalc.sd);
    picocalc_sim_opts.card_path = nullptr;
    remember("card", "none");
    picocalc_sim_log("SD card ejected");
}

void flash_uf2(const char *path)
{
    if (!picocalc_sim_machine_running()) {
        picocalc_sim_log("the machine is not running, so %s cannot be flashed", path);
        return;
    }
    char err[256];
    if (picocalc_sim_flash_uf2(path, err, sizeof(err)))
        picocalc_sim_log("flashing %s", path);
    else
        picocalc_sim_log("%s", err);
}

void record(const char *path)
{
    if (picocalc_sim_machine_running() && picocalc_audio_record(picocalc_sim_picocalc.audio, path)) {
        store.wav = path;
        picocalc_sim_log("recording the sound to %s", path);
    }
}

void stop_recording()
{
    picocalc_audio_close(picocalc_sim_picocalc.audio);
    picocalc_sim_log("saved %s", store.wav.c_str());
}

void set_shots(const char *dir)
{
    store.shots = dir;
    picocalc_sim_opts.screenshot_dir = store.shots.c_str();
}

void choose_shots(const char *dir)
{
    set_shots(dir);
    remember("screenshot-dir", dir);
}

// What the last screenshot came to, shown over the LCD until `until`.
struct {
    bool ok;
    std::string why, path;
    Uint64 until;
} shot_note;

void chose(int kind, const char *path)
{
    switch (kind) {
    case PICK_UF2: flash_uf2(path); break;
    case PICK_CARD: insert_card(path); break;
    case PICK_WAV: record(path); break;
    case PICK_ROM: pending.rom = path; break;
    case PICK_RP2350_ROM: pending.rp2350_rom = path; break;
    case PICK_FLASH:
        pending.flash = FLASH_FILE;
        pending.flash_file = path;
        break;
    case PICK_SHOTS: choose_shots(path); break;
    }
}

// SDL may call this on another thread, so the answer goes back through the event queue.
void SDLCALL dialog_done(void *kind, const char *const *files, int filter)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = dialog_event;
    e.user.code = (Sint32)(intptr_t)kind;
    if (!files)
        e.user.data2 = (void *)1;
    else if (files[0])
        e.user.data1 = SDL_strdup(files[0]);
    SDL_PushEvent(&e);
}

const char *pick_title(int kind)
{
    switch (kind) {
    case PICK_UF2: return "UF2 file to flash";
    case PICK_CARD: return "SD card image to insert";
    case PICK_WAV: return "WAV file to record to";
    case PICK_ROM: return "RP2040 boot ROM (b2.elf or a 16 KB image)";
    case PICK_RP2350_ROM: return "RP2350 boot ROM (bootrom-combined.bin)";
    case PICK_FLASH: return "Flash image";
    case PICK_SHOTS: return "Folder for screenshots";
    }
    return "";
}

// The file choices made in Settings, whose dialogs and prompts belong to its window.
bool settings_pick(int kind)
{
    return kind == PICK_ROM || kind == PICK_RP2350_ROM || kind == PICK_FLASH || kind == PICK_SHOTS;
}

SDL_Window *dialog_parent(int kind);

void pick(int kind)
{
    static const SDL_DialogFileFilter uf2[] = {{"UF2 firmware", "uf2"}, {"All files", "*"}};
    static const SDL_DialogFileFilter card[] = {{"SD card images", "img;bin"}, {"All files", "*"}};
    static const SDL_DialogFileFilter wav[] = {{"WAV audio", "wav"}};
    static const SDL_DialogFileFilter rom[] = {{"RP2040 boot ROM", "elf;bin"}, {"All files", "*"}};
    static const SDL_DialogFileFilter rom2350[] = {{"RP2350 boot ROM", "bin"}, {"All files", "*"}};
    static const SDL_DialogFileFilter flash[] = {{"Flash images", "flash;bin"}, {"All files", "*"}};
    void *ud = (void *)(intptr_t)kind;
    SDL_Window *parent = dialog_parent(kind);
    switch (kind) {
    case PICK_UF2: SDL_ShowOpenFileDialog(dialog_done, ud, parent, uf2, 2, nullptr, false); break;
    case PICK_CARD: SDL_ShowOpenFileDialog(dialog_done, ud, parent, card, 2, picocalc_sim_opts.card_path, false); break;
    case PICK_WAV: SDL_ShowSaveFileDialog(dialog_done, ud, parent, wav, 1, nullptr); break;
    case PICK_ROM:
        SDL_ShowOpenFileDialog(dialog_done, ud, parent, rom, 2, pending.rom.empty() ? nullptr : pending.rom.c_str(),
                               false);
        break;
    case PICK_RP2350_ROM:
        SDL_ShowOpenFileDialog(dialog_done, ud, parent, rom2350, 2,
                               pending.rp2350_rom.empty() ? nullptr : pending.rp2350_rom.c_str(), false);
        break;
    case PICK_FLASH:
        SDL_ShowSaveFileDialog(dialog_done, ud, parent, flash, 2,
                               pending.flash_file.empty() ? nullptr : pending.flash_file.c_str());
        break;
    case PICK_SHOTS: SDL_ShowOpenFolderDialog(dialog_done, ud, parent, picocalc_sim_opts.screenshot_dir, false); break;
    }
}

void poll_log()
{
    char line[512];
    for (unsigned n = picocalc_sim_log_count(); log_seen != n; log_seen++) {
        if (!picocalc_sim_log_line(log_seen, line, sizeof(line)))
            continue;
        log_lines.emplace_back(log_seen, line);
        if (log_lines.size() > 1000)
            log_lines.pop_front();
        log_scroll = true;
    }
}

const char *machine_status()
{
    static char text[64];
    if (!picocalc_sim_machine_running())
        return "not running";
    switch (picocalc_sim_state()) {
    case PICOCALC_SIM_HALTED: return "halted";
    case PICOCALC_SIM_BOOTSEL: return "BOOTSEL";
    case PICOCALC_SIM_POWERED_OFF: return "off";
    case PICOCALC_SIM_RUNNING: break;
    }
    if (picocalc_sim_speed() < 0.95) {
        snprintf(text, sizeof(text), "%.0f%% speed", picocalc_sim_speed() * 100);
        return text;
    }
    return "";
}

// The terminal sits beside the LCD, the window widening to make room, rather than over it.
float terminal_width()
{
    if (!show_terminal)
        return 0;
    const ImGuiStyle &st = ImGui::GetStyle();
    return term_char_w * TERM_COLS + st.ScrollbarSize + 2 * (st.WindowPadding.x + st.ChildBorderSize) + 2;
}

void set_terminal(bool on)
{
    show_terminal = on;
    picocalc_sim_opts.usb_terminal = on;
    picocalc_sim_usb_serial_attach(PICOCALC_SIM_TERMINAL_WINDOW, on);
    remember_on("usb-terminal", on);
    ui_fit_window(picocalc_sim_opts.scale);
}

void term_send(const char *s, size_t n)
{
    term_input.append(s, n);
}

// A paste ends lines with Enter, as typing them would.
void term_paste()
{
    const char *clip = ImGui::GetClipboardText();
    for (const char *p = clip ? clip : ""; *p; p++) {
        if (*p == '\r' && p[1] == '\n')
            continue;
        char c = *p == '\n' ? '\r' : *p;
        term_send(&c, 1);
    }
}

void term_copy()
{
    std::string all;
    term_lock();
    unsigned n = term_count();
    for (unsigned i = 0; i < n; i++) {
        const term_cell_t *l = term_line(i);
        unsigned end = TERM_COLS;
        while (end && l[end - 1].ch == ' ')
            end--;
        for (unsigned x = 0; x < end; x++)
            all += l[x].ch;
        all += '\n';
    }
    term_unlock();
    while (all.size() > 1 && all[all.size() - 1] == '\n' && all[all.size() - 2] == '\n')
        all.pop_back();
    ImGui::SetClipboardText(all.c_str());
}

void term_keys()
{
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextFrameWantCaptureKeyboard(true);
    ImGui::GetCurrentContext()->PlatformImeData.WantTextInput = true;
    bool ctrl = io.KeyCtrl && !io.KeyAlt;
    if (ctrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_V)) {
        term_paste();
        return;
    }
    if (ctrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_C)) {
        term_copy();
        return;
    }
    static const struct {
        ImGuiKey key;
        int code;
    } special[] = {
        {ImGuiKey_Enter, '\r'},       {ImGuiKey_KeypadEnter, '\r'},     {ImGuiKey_Backspace, 0x7f},
        {ImGuiKey_Tab, '\t'},         {ImGuiKey_Escape, 0x1b},          {ImGuiKey_UpArrow, TERM_KEY_UP},
        {ImGuiKey_DownArrow, TERM_KEY_DOWN}, {ImGuiKey_RightArrow, TERM_KEY_RIGHT},
        {ImGuiKey_LeftArrow, TERM_KEY_LEFT}, {ImGuiKey_Home, TERM_KEY_HOME},
        {ImGuiKey_End, TERM_KEY_END},  {ImGuiKey_Insert, TERM_KEY_INSERT}, {ImGuiKey_Delete, TERM_KEY_DELETE},
        {ImGuiKey_PageUp, TERM_KEY_PAGE_UP}, {ImGuiKey_PageDown, TERM_KEY_PAGE_DOWN},
    };
    uint8_t buf[8];
    for (const auto &k : special)
        if (ImGui::IsKeyPressed(k.key))
            term_send((const char *)buf, term_key_bytes(k.code, false, buf, sizeof(buf)));
    if (ctrl) {
        // Ctrl with a letter, or with [ \ ] for Esc, FS and GS, is a control code; the OS sends no
        // text for it.
        for (int k = ImGuiKey_A; k <= ImGuiKey_Z; k++)
            if (ImGui::IsKeyPressed((ImGuiKey)k))
                term_send((const char *)buf, term_key_bytes('a' + (k - ImGuiKey_A), true, buf, sizeof(buf)));
        static const struct {
            ImGuiKey key;
            int ch;
        } punct[] = {{ImGuiKey_LeftBracket, '['}, {ImGuiKey_Backslash, '\\'}, {ImGuiKey_RightBracket, ']'}};
        for (const auto &p : punct)
            if (ImGui::IsKeyPressed(p.key))
                term_send((const char *)buf, term_key_bytes(p.ch, true, buf, sizeof(buf)));
        return;
    }
    for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
        unsigned c = io.InputQueueCharacters[i];
        char u[4];
        int n = 0;
        if (c < 0x20 || c == 0x7f)
            continue;
        if (c < 0x80) {
            u[n++] = (char)c;
        } else if (c < 0x800) {
            u[n++] = (char)(0xc0 | c >> 6);
            u[n++] = (char)(0x80 | (c & 0x3f));
        } else {
            u[n++] = (char)(0xe0 | c >> 12);
            u[n++] = (char)(0x80 | ((c >> 6) & 0x3f));
            u[n++] = (char)(0x80 | (c & 0x3f));
        }
        term_send(u, (size_t)n);
    }
}

// The 16 ANSI colours, as xterm shows them.
ImU32 term_colour(uint8_t c, bool fg)
{
    static const ImU32 palette[16] = {
        IM_COL32(0, 0, 0, 255),       IM_COL32(205, 0, 0, 255),     IM_COL32(0, 205, 0, 255),
        IM_COL32(205, 205, 0, 255),   IM_COL32(0, 0, 238, 255),     IM_COL32(205, 0, 205, 255),
        IM_COL32(0, 205, 205, 255),   IM_COL32(229, 229, 229, 255), IM_COL32(127, 127, 127, 255),
        IM_COL32(255, 0, 0, 255),     IM_COL32(0, 255, 0, 255),     IM_COL32(255, 255, 0, 255),
        IM_COL32(92, 92, 255, 255),   IM_COL32(255, 0, 255, 255),   IM_COL32(0, 255, 255, 255),
        IM_COL32(255, 255, 255, 255),
    };
    if (c == TERM_DEFAULT)
        return fg ? IM_COL32(204, 204, 204, 255) : IM_COL32(12, 12, 12, 255);
    return palette[c & 15];
}

void term_draw_line(ImDrawList *dl, ImVec2 at, const term_cell_t *l, int cursor_col)
{
    for (unsigned x = 0; x < TERM_COLS;) {
        const term_cell_t &c = l[x];
        unsigned end = x + 1;
        while (end < TERM_COLS && l[end].fg == c.fg && l[end].bg == c.bg && l[end].attr == c.attr)
            end++;
        uint8_t fgc = c.fg, bgc = c.bg;
        // Bold brightens the eight basic colours, as most terminals show it.
        if ((c.attr & TERM_BOLD) && fgc < 8)
            fgc += 8;
        ImU32 fg = term_colour(fgc, true), bg = term_colour(bgc, false);
        if (c.attr & TERM_INVERSE)
            std::swap(fg, bg);
        ImVec2 a(at.x + x * term_char_w, at.y), b(at.x + end * term_char_w, at.y + term_line_h);
        if (bg != term_colour(TERM_DEFAULT, false))
            dl->AddRectFilled(a, b, bg);
        char text[TERM_COLS + 1];
        for (unsigned i = x; i < end; i++)
            text[i - x] = l[i].ch;
        dl->AddText(a, fg, text, text + (end - x));
        if (c.attr & TERM_UNDERLINE)
            dl->AddLine(ImVec2(a.x, b.y - 1), ImVec2(b.x, b.y - 1), fg);
        x = end;
    }
    if (cursor_col >= 0) {
        ImVec2 a(at.x + cursor_col * term_char_w, at.y);
        dl->AddRectFilled(a, ImVec2(a.x + term_char_w, a.y + term_line_h), IM_COL32(204, 204, 204, 160));
    }
}

const char *term_status(bool *unplugged)
{
    static char text[200];
    char product[128];
    *unplugged = false;
    if (!picocalc_sim_machine_running())
        return "The machine is stopped.";
    switch (picocalc_sim_usb_serial_state(product, sizeof(product))) {
    case PICOCALC_SIM_USB_SERIAL_UNPLUGGED:
        *unplugged = true;
        return "The USB cable is unplugged.";
    case PICOCALC_SIM_USB_SERIAL_NO_DEVICE: return "Waiting for the firmware's USB device...";
    case PICOCALC_SIM_USB_SERIAL_FAILED: return "The host could not enumerate the USB device.";
    case PICOCALC_SIM_USB_SERIAL_NO_PORT: snprintf(text, sizeof(text), "%s has no USB serial port.", product); break;
    case PICOCALC_SIM_USB_SERIAL_CLOSED: snprintf(text, sizeof(text), "Opening %s's serial port...", product); break;
    case PICOCALC_SIM_USB_SERIAL_EXPORTED:
        snprintf(text, sizeof(text), "%s is exported over USB/IP, so its serial port is the computer's.", product);
        break;
    default: snprintf(text, sizeof(text), "%s: serial port open", product); break;
    }
    return text;
}

void terminal_panel()
{
    ImGuiViewport *vp = ImGui::GetMainViewport();
    float w = terminal_width();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - w, vp->Pos.y + menu_height));
    ImGui::SetNextWindowSize(ImVec2(w, vp->Size.y - menu_height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar;
    if (!ImGui::Begin("USB serial", nullptr, flags)) {
        ImGui::End();
        return;
    }
    bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    bool unplugged;
    const char *status = term_status(&unplugged);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(status);
    if (unplugged && picocalc_sim_machine_running()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Plug in")) {
            picocalc_sim_opts.usb = true;
            picocalc_sim_usb_plug(true);
            remember_on("usb", true);
        }
    }
    const ImGuiStyle &st = ImGui::GetStyle();
    const char *const buttons[] = {"Paste", "Copy", "Clear", "Close"};
    float bw = 0;
    for (const char *b : buttons)
        bw += ImGui::CalcTextSize(b).x + st.FramePadding.x * 2 + st.ItemSpacing.x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - st.WindowPadding.x - bw + st.ItemSpacing.x));
    if (ImGui::SmallButton("Paste"))
        term_paste();
    tip("Send the clipboard, each line ended with Enter (Ctrl+Shift+V)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy"))
        term_copy();
    tip("Copy everything the terminal holds (Ctrl+Shift+C)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        term_clear();
    ImGui::SameLine();
    if (ImGui::SmallButton("Close"))
        set_terminal(false);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, term_colour(TERM_DEFAULT, false));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::BeginChild("screen", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoNav);
    bool follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - term_line_h;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    // The cursor blinks while the terminal has the keyboard, and shows steadily otherwise.
    bool blink_on = !focused || std::fmod(ImGui::GetTime(), 1.0) < 0.6;
    term_lock();
    unsigned n = term_count(), gen = term_generation(), cl, cc;
    bool cursor_shown;
    term_cursor(&cl, &cc, &cursor_shown);
    ImGuiListClipper clip;
    clip.Begin((int)n, term_line_h);
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            ImVec2 at = ImGui::GetCursorScreenPos();
            bool here = cursor_shown && blink_on && (unsigned)i == cl;
            term_draw_line(dl, at, term_line((unsigned)i), here ? (int)cc : -1);
            ImGui::Dummy(ImVec2(term_char_w * TERM_COLS, term_line_h));
        }
    term_unlock();
    if (gen != term_seen && follow)
        ImGui::SetScrollHereY(1.0f);
    term_seen = gen;
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (focused)
        term_keys();
    ImGui::End();

    // What the host could not take yet goes at the next frame, in order.
    if (!term_input.empty()) {
        size_t sent = picocalc_sim_usb_serial_send((const uint8_t *)term_input.data(), term_input.size());
        if (sent)
            term_input.erase(0, sent);
        else if (!picocalc_sim_usb_serial_is_open())
            term_input.clear();
    }
}

void menu_bar()
{
    // a bar as tall as the system's
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * content_scale, 7 * content_scale));
    bool bar = ImGui::BeginMainMenuBar();
    ImGui::PopStyleVar();
    if (!bar)
        return;
    menu_height = ImGui::GetWindowSize().y;
    bool running = picocalc_sim_machine_running();
    if (theme_begin_menu("Machine")) {
        if (ImGui::MenuItem("Restart"))
            restart(false);
        tip("Switch off and on again, with any changed settings");
        if (ImGui::MenuItem("Reset", "F12", false, running)) {
            picocalc_sim_bootsel_button(false);
            picocalc_sim_reboot("reset button");
        }
        if (ImGui::MenuItem("Reset into BOOTSEL", "Shift+F12", false, running)) {
            picocalc_sim_bootsel_button(true);
            picocalc_sim_reboot("reset button");
        }
        if (ImGui::MenuItem("Power button", nullptr, false, running))
            picocalc_sim_power_button();
        ImGui::Separator();
        if (ImGui::MenuItem("Flash UF2...", nullptr, false, running))
            pick(PICK_UF2);
        tip("Writes it into flash as the BOOTSEL drive would, then resets; with a boot ROM's drive "
            "mounted on the USB cable, copies it onto that drive for the ROM to write");
        if (ImGui::MenuItem("Erase flash and restart..."))
            ask_erase = true;
        ImGui::Separator();
        bool cable = link_is_open();
        if (ImGui::MenuItem("Link cable", nullptr, cable)) {
            picocalc_sim_machine_lock();
            if (cable)
                link_close();
            else
                link_open(picocalc_sim_opts.link, picocalc_sim_uptime_us() * 1000000ull);
            picocalc_sim_machine_unlock();
            remember_on("link", !cable);
        }
        tip("Two PBConPC windows with this on are connected like two PicoBoys by the USB-C cable: "
            "link play, Send to partner, Game Boy link. Start PBConPC twice; the second one gets its "
            "own copy of the flash.");
        if (ImGui::MenuItem(profile_running() ? "Stop profiler and show report" : "Start profiler")) {
            if (profile_running()) {
                char path[4096];
                picocalc_sim_data_path(path, sizeof(path), "profile.txt", true);
                picocalc_sim_machine_lock();
                profile_stop(path);
                picocalc_sim_machine_unlock();
                show_log = true;
            } else {
                picocalc_sim_machine_lock();
                profile_start();
                picocalc_sim_machine_unlock();
            }
        }
        tip("Where each core spends its time, by function: the top lines go to the Log, the whole "
            "report to profile.txt in the data folder. Function names need the firmware's ELF: "
            "--profile-elf=FILE on the command line.");
        if (ImGui::MenuItem("Connect USB to this PC", nullptr, usb_link)) {
            usb_link_set(!usb_link);
            remember_on("usb-link", usb_link);
        }
        tip("For PicoBoy Studio, Thonny or mpremote: the PicoBoy's USB port becomes a real USB device "
            "of this PC (a serial port /dev/ttyACM..., or the PBC-ROMS drive in pbcemu's Add Games). "
            "Asks for the administrator password once; switching it on restarts the PicoBoy.");
        ImGui::Separator();
        ImGui::MenuItem("Settings...", nullptr, &show_settings);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit"))
            picocalc_sim_request_quit(0);
        theme_end_menu();
    }
    if (theme_begin_menu("Sound")) {
        if (ImGui::MenuItem("Mute", nullptr, &picocalc_sim_opts.mute))
            remember_on("mute", picocalc_sim_opts.mute);
        ImGui::SetNextItemWidth(120 * content_scale);
        if (ImGui::SliderInt("Volume", &picocalc_sim_opts.volume, 0, 100, "%d%%"))
            remember_int("volume", picocalc_sim_opts.volume);
        ImGui::Separator();
        if (running && picocalc_audio_recording(picocalc_sim_picocalc.audio)) {
            if (ImGui::MenuItem("Stop recording"))
                stop_recording();
        } else if (ImGui::MenuItem("Record to WAV...", nullptr, false, running)) {
            pick(PICK_WAV);
        }
        theme_end_menu();
    }
    if (theme_begin_menu("View")) {
        for (int half = 2; half <= 8; half++) {
            float n = half / 2.0f;
            char label[16];
            snprintf(label, sizeof(label), "%gx", n);
            if (ImGui::MenuItem(label, nullptr, std::fabs(picocalc_sim_opts.scale - n) < 0.001f)) {
                picocalc_sim_opts.scale = n;
                ui_fit_window(n);
                char text[16];
                snprintf(text, sizeof(text), "%g", n);
                remember("scale", text);
            }
        }
        if (ImGui::MenuItem("Fullscreen", "F10, Alt+Enter", false))
            ui_set_fullscreen(true);
        tip("Only the PicoBoy's screen, as large as it fits; Esc, F10 or Alt+Enter goes back");
        if (ImGui::MenuItem("Device frame", nullptr, &skin_enabled)) {
            ui_fit_window(picocalc_sim_opts.scale);
            remember_on("device-frame", skin_enabled);
        }
        tip("The LCD inside a picture of the PicoBoy Color Plus; click the joystick and buttons");
        if (theme_begin_menu("Theme")) {
            static const char *const names[] = {"System", "Light", "Dark"};
            for (int i = 0; i < 3; i++)
                if (ImGui::MenuItem(names[i], nullptr, theme_choice == i)) {
                    theme_choice = (theme_mode)i;
                    ui_theme_changed();
                    remember("theme", i == THEME_LIGHT ? "light" : i == THEME_DARK ? "dark" : "system");
                }
            theme_end_menu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Screenshot", "F11", false, running))
            ui_screenshot();
        ImGui::MenuItem("Log", nullptr, &show_log);
        ImGui::MenuItem("Tilt sensor", nullptr, &show_tilt);
        tip("Opens by itself when the firmware first asks the tilt sensor");
        if (ImGui::MenuItem("USB serial terminal", nullptr, show_terminal))
            set_terminal(!show_terminal);
        theme_end_menu();
    }
    if (theme_begin_menu("Help")) {
        ImGui::MenuItem("Keys", nullptr, &show_keys);
        ImGui::MenuItem("About PBConPC", nullptr, &show_about);
        theme_end_menu();
    }
    const char *status = machine_status();
    float w = ImGui::CalcTextSize(status).x + ImGui::GetStyle().ItemSpacing.x + 8 * content_scale;
    if (*status && ImGui::GetContentRegionAvail().x > w) {
        ImGui::SameLine(ImGui::GetWindowWidth() - w);
        ImGui::TextDisabled("%s", status);
    }
    ImGui::EndMainMenuBar();
}

void note(const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

bool path_field(const char *label, const char *hint, std::string *path, int kind)
{
    ImGui::PushID(label);
    float button = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - button - ImGui::GetStyle().ItemInnerSpacing.x);
    bool changed = ImGui::InputTextWithHint("##path", hint, path);
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    if (ImGui::Button("Browse..."))
        pick(kind);
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return changed;
}

void machine_tab()
{
    ImGui::SeparatorText("At the next restart");
    // PBConPC: one board, the PicoBoy Color Plus, with Raspberry Pi's A4 boot ROM built in
    pending.board = "pbcplus";
    pending.rom.clear();
    pending.rp2350_rom.clear();
    ImGui::TextUnformatted("PicoBoy Color Plus: RP2350 (boot ROM A4), 16 MB flash");
    char def[4096];
    static const char *const places[] = {"In the data folder", "In memory until picocalc-sim quits", "In a file"};
    ImGui::Combo("Flash", &pending.flash, places, 3);
    if (pending.flash == FLASH_FILE)
        path_field("Flash image", "a file that holds the whole flash", &pending.flash_file, PICK_FLASH);
    else if (pending.flash == FLASH_DEFAULT) {
        char file[64];
        snprintf(file, sizeof(file), "%s.flash", pending.board.empty() ? "BOARD" : pending.board.c_str());
        picocalc_sim_data_path(def, sizeof(def), file, false);
        note(def);
    }
    ImGui::InputTextWithHint("USB/IP", "off; a port such as 3240 to export the USB device on", &pending.usbip);
    tip("A USB/IP server, so another computer or this one can take the board's USB device with "
        "usbip attach -r HOST -b 1-1: [ADDR:]PORT, 127.0.0.1 unless ADDR is given, * for all "
        "interfaces. It plugs the USB cable in.");
    ImGui::Checkbox("Instant SPI, I2C and UART transfers", &pending.fast);
    ImGui::Checkbox("Log faults, unknown ROM calls and bus errors", &pending.trace);

    ImGui::SeparatorText("Now");
    if (ImGui::Checkbox("Run as fast as the host allows", &picocalc_sim_opts.unthrottled))
        remember_on("unthrottled", picocalc_sim_opts.unthrottled);
    if (ImGui::Checkbox("Print what the firmware sends on UART0", &picocalc_sim_opts.console))
        remember_on("console", picocalc_sim_opts.console);
    if (ImGui::Checkbox("USB cable plugged into emulated host", &picocalc_sim_opts.usb)) {
        picocalc_sim_usb_plug(picocalc_sim_opts.usb);
        remember_on("usb", picocalc_sim_opts.usb);
    }
    tip("picocalc-sim's own USB host resets and enumerates the USB device the firmware presents, and "
        "the Log shows what it found. This computer's USB stack does not see the device; for that, "
        "export it with the USB/IP setting and attach it with usbip attach -r HOST -b 1-1.");
}

// PBConPC: the PicoBoy has no radio; the tab is no longer shown.
[[maybe_unused]] void wifi_tab()
{
    ImGui::SeparatorText("At the next restart");
    ImGui::Checkbox("Leave the radio out, as if it never answered", &pending.no_wifi);
    ImGui::BeginDisabled(pending.no_wifi);
    ImGui::TextUnformatted("Access points");
    int remove = -1;
    if (!pending.aps.empty() &&
        ImGui::BeginTable("aps", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("Network name");
        ImGui::TableSetupColumn("Password");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < pending.aps.size(); i++) {
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##ssid", &pending.aps[i].ssid);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##password", "none: an open network", &pending.aps[i].password);
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Remove"))
                remove = (int)i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove >= 0)
        pending.aps.erase(pending.aps.begin() + remove);
    if (pending.aps.size() < 16 && ImGui::Button("Add access point"))
        pending.aps.push_back({});
    if (pending.aps.empty())
        note("With none listed, any network name and password the firmware tries will join.");

    ImGui::Spacing();
    ImGui::TextUnformatted("Port forwards");
    remove = -1;
    for (size_t i = 0; i < pending.forwards.size(); i++) {
        ImGui::PushID((int)i + 100);
        float button = ImGui::CalcTextSize("Remove").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputTextWithHint("##forward", "[ADDR:]HOSTPORT:PORT", &pending.forwards[i]);
        ImGui::SameLine();
        if (ImGui::Button("Remove"))
            remove = (int)i;
        ImGui::PopID();
    }
    if (remove >= 0)
        pending.forwards.erase(pending.forwards.begin() + remove);
    if (pending.forwards.size() < 16 && ImGui::Button("Add port forward"))
        pending.forwards.push_back({});
    note("Connections to HOSTPORT on the host (127.0.0.1 unless ADDR is given, * for every interface) reach "
         "the firmware's TCP PORT, as in 2121:21.");
    ImGui::EndDisabled();
}

void devices_tab()
{
    ImGui::SeparatorText("Now");
    if (ImGui::SliderInt("Battery", &picocalc_sim_opts.battery, 0, 100, "%d%%")) {
        set_battery();
        remember_int("battery", picocalc_sim_opts.battery);
    }
    if (ImGui::SliderInt("Volume", &picocalc_sim_opts.volume, 0, 100, "%d%%"))
        remember_int("volume", picocalc_sim_opts.volume);
    if (ImGui::Checkbox("Mute", &picocalc_sim_opts.mute))
        remember_on("mute", picocalc_sim_opts.mute);
}

// PBConPC: Settings -> Keys. A click on a field waits for the next key in this window (ui_event,
// key_capture), which goes to that field; Esc stops waiting.
int key_capture_control = -1, key_capture_slot = -1;

void keys_tab()
{
    note("Host keys for the PicoBoy's joystick and buttons, up to three each. Click a field, then "
         "press the key. The gamepad and clicks on the device frame stay as they are.");
    if (ImGui::BeginTable("keymap", 1 + KM_SLOTS, ImGuiTableFlags_SizingStretchSame)) {
        for (int c = 0; c < KM_COUNT; c++) {
            ImGui::PushID(c);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(keymap_name(c));
            for (int sl = 0; sl < KM_SLOTS; sl++) {
                ImGui::TableNextColumn();
                ImGui::PushID(sl);
                bool waiting = key_capture_control == c && key_capture_slot == sl;
                uint32_t k = keymap_keys[c][sl];
                const char *label = waiting ? "press a key..." : k ? SDL_GetKeyName((SDL_Keycode)k) : "-";
                float clear = k && !waiting ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x : 0;
                if (ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x - clear, 0))) {
                    key_capture_control = c;
                    key_capture_slot = sl;
                }
                if (clear > 0) {
                    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
                    if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) {
                        keymap_keys[c][sl] = 0;
                        keymap_save(c);
                    }
                    tip("Remove this key");
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset to defaults")) {
        keymap_reset();
        for (int c = 0; c < KM_COUNT; c++)
            keymap_save(c);
        key_capture_control = -1;
    }
    tip("Arrow keys for the joystick, A, Enter or keypad Enter for its center, S for button A, D for B");
}

// A key pressed while a Keys field waits: it goes there (and leaves any other control it was on).
bool key_capture(const SDL_Event *e)
{
    if (key_capture_control < 0 || e->type != SDL_EVENT_KEY_DOWN)
        return false;
    int c = key_capture_control, sl = key_capture_slot;
    key_capture_control = -1;
    if (e->key.key == SDLK_ESCAPE)
        return true;
    uint32_t k = (uint32_t)e->key.key;
    for (int o = 0; o < KM_COUNT; o++)
        for (int os = 0; os < KM_SLOTS; os++)
            if (keymap_keys[o][os] == k && !(o == c && os == sl)) {
                keymap_keys[o][os] = 0;
                keymap_save(o);
            }
    keymap_keys[c][sl] = k;
    keymap_save(c);
    return true;
}

// PBConPC: the tilt sensor's pad, as in PBC+ Studio's emulator: the dot shows the lower side, and
// as the sensor measures the table's push against gravity, a dot to the right gives x negative,
// a dot at the top y positive (the raised side reads positive); z makes the whole 1 g;
// double-click lays the device flat again. The dot follows the sensor's state, so a script's
// tilt shows as well.
void tilt_contents()
{
    float size = 150 * content_scale, r = size / 2;
    // centred in the window
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > size)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - size) / 2);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("pad", ImVec2(size, size));
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        ui_tilt(0, 0);
    else if (ImGui::IsItemActive()) {
        ImVec2 m = ImGui::GetIO().MousePos;
        ui_tilt(-(m.x - p0.x - r) / r, -(m.y - p0.y - r) / r);
    }
    float tilt_nx, tilt_ny, z;
    picoboy_get_tilt(&tilt_nx, &tilt_ny, &z);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 c(std::floor(p0.x + r) + 0.5f, std::floor(p0.y + r) + 0.5f);
    ImU32 line = ImGui::GetColorU32(ImGuiCol_Border), fill = ImGui::GetColorU32(ImGuiCol_FrameBg);
    dl->AddCircleFilled(c, r - 1, fill, 64);
    dl->AddCircle(c, r - 1, line, 64, 1.5f * content_scale);
    // the cross as one-pixel bars, inside the ring (lines came out offset by their anti-aliasing)
    float h = std::max(1.0f, std::floor(content_scale)) / 2, e = r - 2 * content_scale;
    dl->AddRectFilled(ImVec2(c.x - e, c.y - h), ImVec2(c.x + e, c.y + h), line);
    dl->AddRectFilled(ImVec2(c.x - h, c.y - e), ImVec2(c.x + h, c.y + e), line);
    dl->AddCircleFilled(ImVec2(c.x - tilt_nx * (r - 8 * content_scale), c.y - tilt_ny * (r - 8 * content_scale)),
                        7 * content_scale, ImGui::GetColorU32(ImGuiCol_CheckMark), 24);
    char values[64];
    snprintf(values, sizeof(values), "x %+.2f   y %+.2f   z %+.2f g", tilt_nx, tilt_ny, z);
    float tw = ImGui::CalcTextSize(values).x;
    if (avail > tw)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - tw) / 2);
    ImGui::TextUnformatted(values);
    const char *hint = "Drag to tilt, double-click: flat.";
    float hw = ImGui::CalcTextSize(hint).x;
    if (avail > hw)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - hw) / 2);
    ImGui::TextDisabled("%s", hint);
}

void screenshots_tab()
{
    ImGui::SeparatorText("Now");
    std::string shots = store.shots;
    if (path_field("Screenshots", "folder for F11's screenshots", &shots, PICK_SHOTS))
        choose_shots(shots.c_str());
}

void path_popup();

void settings_contents()
{
    // Fields fill the window up to their labels, the longest of which is this.
    ImGui::PushItemWidth(-(ImGui::CalcTextSize("Screenshots").x + ImGui::GetStyle().ItemInnerSpacing.x));
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Machine")) {
            machine_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Battery and sound")) {
            devices_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Keys")) {
            keys_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Screenshots")) {
            screenshots_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopItemWidth();
    ImGui::Separator();
    if (pending == applied) {
        ImGui::TextDisabled("No settings are waiting on a restart.");
    } else {
        if (ImGui::Button("Restart now"))
            restart(false);
        ImGui::SameLine();
        if (ImGui::Button("Undo"))
            pending = applied;
        ImGui::SameLine();
        ImGui::TextUnformatted("Some settings are waiting on a restart.");
    }
    if (settings_pick(prompt_kind))
        path_popup();
}

void log_contents()
{
    if (ImGui::Button("Copy")) {
        std::string all;
        for (auto &l : log_lines)
            all += l.second + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    for (auto &l : log_lines)
        ImGui::TextUnformatted(l.second.c_str());
    if (log_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ImGui::GetTextLineHeight() * 2)
        ImGui::SetScrollHereY(1.0f);
    log_scroll = false;
    ImGui::EndChild();
}

void keys_contents()
{
    // PBConPC: the keyboard as Settings -> Keys has it
    static char mapped[KM_COUNT][128];
    for (int c = 0; c < KM_COUNT; c++)
        keymap_describe(c, mapped[c], sizeof(mapped[c]));
    const char *const rows[][2] = {
        {mapped[KM_UP], "joystick up"},
        {mapped[KM_DOWN], "joystick down"},
        {mapped[KM_LEFT], "joystick left"},
        {mapped[KM_RIGHT], "joystick right"},
        {mapped[KM_CENTRE], "joystick center"},
        {mapped[KM_A], "button A"},
        {mapped[KM_B], "button B"},
        {"Gamepad", "D-pad/left stick = joystick, A/X = button A, B/Y = button B, Start = center"},
        {"Mouse", "click the joystick and buttons of the device frame"},
        {"F10, Alt+Enter", "fullscreen on and off (Esc leaves it)"},
        {"F11", "a screenshot, into the screenshot folder"},
        {"F12", "the reset button"},
        {"Shift+F12", "reset into BOOTSEL"},
    };
    note("Keys go to the PicoBoy unless one of PBConPC's text fields has them.");
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Host key");
        ImGui::TableSetupColumn("PicoBoy");
        ImGui::TableHeadersRow();
        for (auto &r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", r[0]);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", r[1]);
        }
        ImGui::EndTable();
    }
    note("Settings -> Keys changes the keyboard's keys. Dropping a .uf2 file on the window flashes it.");
}

void about_contents()
{
    int v = SDL_GetVersion();
    ImGui::Text("PBConPC - %s", picocalc_sim_version);
    ImGui::TextWrapped("A PicoBoy Color Plus emulator (RP2350, 16 MB flash)");
    ImGui::TextWrapped("Based on picocalc-sim, copyright 2026, Steve Gibson <steve@stevegibson.com>");
    ImGui::TextUnformatted("(MIT license)");
    ImGui::Spacing();
    ImGui::Text("SDL %d.%d.%d (zlib license)", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v),
                SDL_VERSIONNUM_MICRO(v));
    ImGui::Text("Dear ImGui %s (MIT license)", IMGUI_VERSION);
    ImGui::TextUnformatted("lwIP (BSD license)");
    ImGui::TextUnformatted("FatFs R0.15 in picocalc-sim-mkcard (BSD-style license)");
    ImGui::TextUnformatted("Raspberry Pi's RP2350 boot ROMs A2 and A4 (BSD license)");
}

void erase_popup()
{
    if (ask_erase) {
        ImGui::OpenPopup("Erase flash?");
        ask_erase = false;
    }
    if (!ImGui::BeginPopupModal("Erase flash?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    const char *board = !pending.board.empty() ? pending.board.c_str() : "the board's";
    ImGui::Text("Everything in %s flash is lost,\nand the machine restarts with it erased.", board);
    if (ImGui::Button("Erase and restart")) {
        restart(true);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Where the desktop has no file dialog to offer, the path is typed instead.
void path_popup()
{
    if (ask_path) {
        ImGui::OpenPopup("Choose a file");
        ask_path = false;
    }
    if (!ImGui::BeginPopupModal("Choose a file", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextUnformatted(pick_title(prompt_kind));
    ImGui::TextDisabled("No file dialog could be opened, so type the path.");
    ImGui::SetNextItemWidth(420 * content_scale);
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    bool enter = ImGui::InputText("##path", &prompt_path, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::Button("OK") || enter) {
        if (!prompt_path.empty())
            chose(prompt_kind, prompt_path.c_str());
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void screenshot_note()
{
    Uint64 now = SDL_GetTicks();
    if (now >= shot_note.until)
        return;
    std::string title = shot_note.ok ? "Screenshot saved" : "Screenshot failed: " + shot_note.why;
    const char *name = base_name(shot_note.path.c_str());
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGuiStyle &style = ImGui::GetStyle();
    float margin = 8 * content_scale, lcd_width = vp->Size.x - terminal_width();
    float text = std::max({ImGui::CalcTextSize(title.c_str()).x, ImGui::CalcTextSize(name).x,
                           ImGui::CalcTextSize(shot_note.path.c_str()).x});
    float width = std::min(text + 2 * style.WindowPadding.x + 1, lcd_width - 2 * margin);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + lcd_width - margin, vp->Pos.y + menu_height + margin),
                            ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, vp->Size.y - menu_height - 2 * margin));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::min(1.0f, (shot_note.until - now) / 500.0f));
    // Never takes a click or the keyboard, so that the PicoCalc keeps both while it shows.
    ImGui::Begin("##screenshot", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::PushStyleColor(ImGuiCol_Text, shot_note.ok ? (theme_dark() ? ImVec4(0.45f, 0.85f, 0.45f, 1) : ImVec4(0.15f, 0.55f, 0.2f, 1))
                                                    : (theme_dark() ? ImVec4(1, 0.45f, 0.4f, 1) : ImVec4(0.8f, 0.15f, 0.1f, 1)));
    ImGui::TextWrapped("%s", title.c_str());
    ImGui::PopStyleColor();
    ImGui::TextWrapped("%s", name);
    note(shot_note.path.c_str());
    ImGui::End();
    ImGui::PopStyleVar();
}

void stopped_panel()
{
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + menu_height));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y - menu_height));
    ImGui::Begin("##stopped", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::TextUnformatted("The machine could not start:");
    ImGui::Spacing();
    for (auto &l : log_lines)
        if (l.first >= start_mark)
            ImGui::TextWrapped("%s", l.second.c_str());
    ImGui::Spacing();
    note("Change what it needs in the settings, then try again.");
    if (ImGui::Button("Settings..."))
        show_settings = true;
    ImGui::SameLine();
    if (ImGui::Button("Try again"))
        restart(false);
    ImGui::End();
}


// The windows of picocalc-sim's own beside the PicoCalc's: OS windows, each with a Dear ImGui context
// of its own, so that each is as big as it needs to be whatever the LCD's scale, and their tooltips
// are not cut off at the edge of the PicoCalc's window.
struct tool_window {
    const char *title;
    bool *shown;
    void (*contents)();
    // Points wide, and whether the window fits its contents each frame rather than taking the
    // size it is given.
    float width;
    bool fits;
    SDL_Window *win;
    SDL_Renderer *ren;
    ImGuiContext *ctx;
    // Frames laid out while hidden, so that it is first shown at the size its contents need.
    int frames;
};

tool_window tools[] = {
    {"Settings", &show_settings, settings_contents, 520, true, nullptr, nullptr, nullptr, 0},
    {"Log", &show_log, log_contents, 560, false, nullptr, nullptr, nullptr, 0},
    {"Keys", &show_keys, keys_contents, 420, true, nullptr, nullptr, nullptr, 0},
    {"Tilt sensor", &show_tilt, tilt_contents, 240, true, nullptr, nullptr, nullptr, 0},
    {"About PBConPC", &show_about, about_contents, 420, true, nullptr, nullptr, nullptr, 0},
};

ImGuiContext *main_ctx;

// Each context looks the same, and its backends drive the one window it draws in.
void setup_context(SDL_Window *w, SDL_Renderer *r)
{
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // PBConPC: the system's look (theme.cpp), opaque so the LCD never shows through
    theme_setup(content_scale);
    ImGui_ImplSDL3_InitForSDLRenderer(w, r);
    ImGui_ImplSDLRenderer3_Init(r);
}

void open_tool(tool_window &t)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, t.title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, (Sint64)(t.width * content_scale));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, (Sint64)(240 * content_scale));
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, !t.fits);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    t.win = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (t.win)
        t.ren = SDL_CreateRenderer(t.win, nullptr);
    if (t.win && !t.ren)
        t.ren = SDL_CreateRenderer(t.win, SDL_SOFTWARE_RENDERER);
    if (!t.ren) {
        picocalc_sim_log("cannot open the %s window: %s", t.title, SDL_GetError());
        if (t.win)
            SDL_DestroyWindow(t.win);
        t.win = nullptr;
        *t.shown = false;
        return;
    }
    // A child of the PicoCalc's window stays above it, and goes with it when that is minimised.
    SDL_SetWindowParent(t.win, window);
    t.ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(t.ctx);
    setup_context(t.win, t.ren);
    ImGui::SetCurrentContext(main_ctx);
    t.frames = 0;
}

void close_tool(tool_window &t)
{
    ImGui::SetCurrentContext(t.ctx);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(t.ctx);
    ImGui::SetCurrentContext(main_ctx);
    SDL_DestroyRenderer(t.ren);
    SDL_DestroyWindow(t.win);
    t.win = nullptr;
    t.ren = nullptr;
    t.ctx = nullptr;
}

// Beside the PicoCalc's window, or beside another of these windows: of the places next to them that
// are on the screen, the one that covers least of the PicoCalc's window and of the others open, and
// among those the first of to the right of the PicoCalc's window, then to its left, highest first.
void place_tool(tool_window &t)
{
    std::vector<SDL_Rect> taken;
    SDL_Rect main;
    SDL_GetWindowPosition(window, &main.x, &main.y);
    SDL_GetWindowSize(window, &main.w, &main.h);
    for (auto &o : tools) {
        SDL_Rect r;
        if (&o == &t || !o.win || o.frames < 3)
            continue;
        SDL_GetWindowPosition(o.win, &r.x, &r.y);
        SDL_GetWindowSize(o.win, &r.w, &r.h);
        taken.push_back(r);
    }
    int tw, th, gap = (int)(8 * content_scale);
    SDL_GetWindowSize(t.win, &tw, &th);
    SDL_Rect screen = main;
    if (!SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &screen))
        screen = {main.x - tw - gap, main.y, main.w + 2 * (tw + gap), std::max(main.h, th)};
    std::vector<int> xs = {main.x + main.w + gap, main.x - gap - tw}, ys = {main.y};
    for (auto &r : taken) {
        xs.push_back(r.x + r.w + gap);
        xs.push_back(r.x - gap - tw);
        ys.push_back(r.y);
        ys.push_back(r.y + r.h + gap);
    }
    std::sort(ys.begin(), ys.end());
    auto overlap = [](const SDL_Rect &a, const SDL_Rect &b) {
        SDL_Rect r;
        return SDL_GetRectIntersection(&a, &b, &r) ? (long)r.w * r.h : 0L;
    };
    long best = -1;
    SDL_Point at = {main.x + gap, main.y + gap};
    for (int x : xs) {
        for (int y : ys) {
            SDL_Rect r = {std::clamp(x, screen.x, std::max(screen.x, screen.x + screen.w - tw)),
                          std::clamp(y, screen.y, std::max(screen.y, screen.y + screen.h - th)), tw, th};
            long cost = 2 * overlap(r, main);
            for (auto &o : taken)
                cost += overlap(r, o);
            if (best < 0 || cost < best) {
                best = cost;
                at = {r.x, r.y};
            }
        }
    }
    SDL_SetWindowPosition(t.win, at.x, at.y);
}

void tool_frame(tool_window &t)
{
    ImGui::SetCurrentContext(t.ctx);
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin(t.title, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    t.contents();
    // What the contents took last frame, whatever the window's size: ImGui's own fitting of a window
    // stops at the edge of the OS window, which here is the very thing being fitted.
    ImGuiWindow *iw = ImGui::GetCurrentWindow();
    ImVec2 size(t.width * content_scale, iw->ContentSizeIdeal.y + 2 * iw->WindowPadding.y);
    ImGui::End();
    ImGui::Render();
    const ImVec4 &bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    SDL_SetRenderDrawColorFloat(t.ren, bg.x, bg.y, bg.z, 1.0f);
    SDL_RenderClear(t.ren);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), t.ren);
    {
        // PBConPC test hook: PBCONPC_TOOL_SHOT=file.png saves the window titled
        // PBCONPC_TOOL_SHOT_NAME once it has been up for 60 frames
        static const char *path = SDL_getenv("PBCONPC_TOOL_SHOT"), *name = SDL_getenv("PBCONPC_TOOL_SHOT_NAME");
        static bool done;
        if (path && name && !done && !strcmp(name, t.title) && t.frames >= 60) {
            done = true;
            SDL_Surface *shot = SDL_RenderReadPixels(t.ren, nullptr);
            SDL_Surface *argb = shot ? SDL_ConvertSurface(shot, SDL_PIXELFORMAT_ARGB8888) : nullptr;
            if (argb && argb->pitch == argb->w * 4)
                picocalc_sim_png_write(path, (const uint32_t *)argb->pixels, argb->w, argb->h);
            picocalc_sim_log("tool window shot: %s", argb ? path : SDL_GetError());
            SDL_DestroySurface(argb);
            SDL_DestroySurface(shot);
        }
    }
    SDL_RenderPresent(t.ren);
    ImGui::SetCurrentContext(main_ctx);
    if (t.fits) {
        int w, h, nw = (int)std::ceil(size.x), nh = (int)std::ceil(size.y);
        SDL_GetWindowSize(t.win, &w, &h);
        if (nw != w || nh != h)
            SDL_SetWindowSize(t.win, nw, nh);
    }
    // The contents are measured a frame after they are laid out, and the size set from that takes
    // another to arrive.
    if (++t.frames == 3) {
        place_tool(t);
        SDL_ShowWindow(t.win);
    }
}

void tool_frames()
{
    for (auto &t : tools) {
        if (*t.shown && !t.win)
            open_tool(t);
        else if (!*t.shown && t.win)
            close_tool(t);
        if (t.win)
            tool_frame(t);
    }
}

tool_window *tool_for(SDL_Window *w)
{
    for (auto &t : tools)
        if (w && t.win == w)
            return &t;
    return nullptr;
}

SDL_Window *dialog_parent(int kind)
{
    if (settings_pick(kind) && tools[0].win)
        return tools[0].win;
    return window;
}

} // namespace

void ui_init(SDL_Window *w, SDL_Renderer *r)
{
    window = w;
    renderer = r;
    content_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (content_scale < 1.0f)
        content_scale = 1.0f;
    dialog_event = SDL_RegisterEvents(1);

    IMGUI_CHECKVERSION();
    main_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(main_ctx);
    if (const char *t = picocalc_sim_settings_get("theme"))
        theme_choice = !strcmp(t, "light") ? THEME_LIGHT : !strcmp(t, "dark") ? THEME_DARK : THEME_SYSTEM;
    setup_context(w, r);

    pending = applied = from_opts();
    to_opts(applied);
    set_shots(picocalc_sim_opts.screenshot_dir);
    // PBConPC: remembered window choices
    auto saved_on = [](const char *key) {
        const char *v = picocalc_sim_settings_get(key);
        return v && (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "yes") || !strcmp(v, "true"));
    };
    keymap_load();
    if (picocalc_sim_settings_get("device-frame"))
        skin_enabled = skin_enabled && saved_on("device-frame");
    if (saved_on("usb-link"))
        usb_link_set(true);
    if (saved_on("link") && !link_is_open()) {
        picocalc_sim_machine_lock();
        link_open(picocalc_sim_opts.link, picocalc_sim_uptime_us() * 1000000ull);
        picocalc_sim_machine_unlock();
    }
    if (picocalc_sim_opts.card_path) {
        store.card = picocalc_sim_opts.card_path;
        picocalc_sim_opts.card_path = store.card.c_str();
    }
    if (picocalc_sim_opts.wav_path)
        store.wav = picocalc_sim_opts.wav_path;

    // The menu bar's height is only known from a frame laid out with the fonts.
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    menu_height = ImGui::GetFrameHeight();
    term_char_w = ImGui::CalcTextSize("M").x;
    term_line_h = ImGui::GetTextLineHeight();
    ImGui::EndFrame();
    if (picocalc_sim_opts.usb_terminal) {
        show_terminal = true;
        picocalc_sim_usb_serial_attach(PICOCALC_SIM_TERMINAL_WINDOW, true);
    }
}

void ui_shutdown(void)
{
    if (usb_link)
        usb_link_set(false);
    if (save_at)
        picocalc_sim_settings_save();
    for (auto &t : tools)
        if (t.win)
            close_tool(t);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void ui_fit_window(float scale)
{
    float w = PICOCALC_SIM_LCD_WIDTH * scale * content_scale, h = PICOCALC_SIM_LCD_HEIGHT * scale * content_scale;
    if (skin_enabled) {
        // PBConPC: the device frame around the LCD, kept within the screen's height
        skin_window_size(scale * content_scale, &w, &h);
        SDL_Rect usable;
        // (not for the window-shot test hook, which wants the frame at full size)
        if (!SDL_getenv("PBCONPC_WINDOW_SHOT") &&
            SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &usable) && usable.h > 0) {
            float most = usable.h * 0.92f - menu_height;
            if (h > most) {
                w *= most / h;
                h = most;
            }
        }
    }
    SDL_SetWindowSize(window, (int)std::lround(w + terminal_width()), (int)std::lround(h + menu_height));
}

bool fullscreen_on;

float ui_side_width(void)
{
    if (fullscreen_on)
        return 0;
    return terminal_width() * SDL_GetWindowPixelDensity(window);
}

// The system's or the user's choice of light or dark changed: every window's colours follow.
void ui_theme_changed(void)
{
    ImGuiContext *was = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(main_ctx);
    theme_colours();
    for (tool_window &t : tools)
        if (t.ctx) {
            ImGui::SetCurrentContext(t.ctx);
            theme_colours();
        }
    ImGui::SetCurrentContext(was);
}

void ui_backdrop(uint8_t *r, uint8_t *g, uint8_t *b)
{
    ImVec4 c = theme_backdrop();
    *r = (uint8_t)(c.x * 255 + 0.5f);
    *g = (uint8_t)(c.y * 255 + 0.5f);
    *b = (uint8_t)(c.z * 255 + 0.5f);
}

// The tilt as the pad sets it, clamped to the unit circle.
void ui_tilt(float nx, float ny)
{
    float len = std::sqrt(nx * nx + ny * ny);
    if (len > 1) {
        nx /= len;
        ny /= len;
    }
    picoboy_set_tilt(nx, ny, std::sqrt(std::max(0.0f, 1 - nx * nx - ny * ny)));
}

bool ui_fullscreen(void)
{
    return fullscreen_on;
}

void ui_set_fullscreen(bool on)
{
    if (on == fullscreen_on)
        return;
    fullscreen_on = on;
    SDL_SetWindowFullscreen(window, on);
    if (on)
        SDL_HideCursor();
    else
        SDL_ShowCursor();
    picocalc_sim_log(on ? "fullscreen: Esc, F10 or Alt+Enter goes back" : "fullscreen off");
}

bool ui_event(const SDL_Event *e)
{
    if (e->type == dialog_event) {
        char *path = (char *)e->user.data1;
        if (path) {
            chose(e->user.code, path);
            SDL_free(path);
        } else if (e->user.data2) {
            prompt_kind = e->user.code;
            prompt_path.clear();
            ask_path = true;
        }
        return true;
    }
    if (e->type == SDL_EVENT_SYSTEM_THEME_CHANGED) {
        ui_theme_changed();
        return false;
    }
    // Nothing done in one of picocalc-sim's own windows is the PicoCalc's.
    // a Keys field waiting takes the next key, whichever window has it
    if (key_capture(e))
        return true;
    if (tool_window *t = tool_for(SDL_GetWindowFromEvent(e))) {
        if (e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            *t->shown = false;
        ImGui::SetCurrentContext(t->ctx);
        ImGui_ImplSDL3_ProcessEvent(e);
        ImGui::SetCurrentContext(main_ctx);
        return true;
    }
    bool wanted = ImGui::GetIO().WantCaptureKeyboard;
    // ImGui takes in one key change a frame, so keys typed at the PicoCalc would queue up ahead of
    // the next click on a menu; letting every key-up through clears any key it saw go down.
    if (e->type == SDL_EVENT_KEY_DOWN && !wanted)
        return false;
    ImGui_ImplSDL3_ProcessEvent(e);
    bool key = e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP || e->type == SDL_EVENT_TEXT_INPUT;
    return key && wanted;
}

bool ui_wants_keyboard(void)
{
    SDL_Window *focus = SDL_GetKeyboardFocus();
    return (focus && focus != window) || main_ctx->IO.WantCaptureKeyboard;
}

void ui_drop_file(const char *path)
{
    size_t n = strlen(path);
    if (n > 4 && SDL_strcasecmp(path + n - 4, ".img") == 0)
        insert_card(path);
    else
        flash_uf2(path);
}

void ui_screenshot(void)
{
    char path[1100], why[256];
    shot_note.ok = picocalc_sim_screenshot_auto(path, sizeof(path), why, sizeof(why));
    shot_note.path = path;
    shot_note.why = shot_note.ok ? "" : why;
    shot_note.until = SDL_GetTicks() + (shot_note.ok ? 4000 : 8000);
}

void ui_restarting(void)
{
    start_mark = picocalc_sim_log_count();
}

float ui_frame(void)
{
    tool_frames();
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    poll_log();
    if (picocalc_sim_machine_running() && applied.board.empty()) {
        // Pinned, so that a restart without the UF2 that chose the board stays on it.
        applied.board = pending.board = picocalc_sim_board_name();
    }
    // PBConPC: the tilt pad comes up when the firmware first talks to the sensor (again after a
    // restart that it did not)
    static bool tilt_shown;
    if (!picoboy_tilt_used())
        tilt_shown = false;
    else if (!tilt_shown) {
        tilt_shown = true;
        show_tilt = true;
    }
    static unsigned seen_starts;
    if (picocalc_sim_machine_running() && picocalc_sim_machine_starts() != seen_starts) {
        // The next launch comes back to it, as the PicoCalc keeps the Pico it was last given.
        seen_starts = picocalc_sim_machine_starts();
        remember("board", picocalc_sim_board_name());
    }
    if (save_at && SDL_GetTicks() >= save_at) {
        save_at = 0;
        picocalc_sim_settings_save();
    }
    if (!fullscreen_on) {
        menu_bar();
        if (show_terminal)
            terminal_panel();
    }
    if (!picocalc_sim_machine_running())
        stopped_panel();
    screenshot_note();
    erase_popup();
    if (!(settings_pick(prompt_kind) && tools[0].win))
        path_popup();
    ImGui::Render();
    return fullscreen_on ? 0 : menu_height * SDL_GetWindowPixelDensity(window);
}

void ui_render(void)
{
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
}
