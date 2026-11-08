#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "picocalc-sim/devices.h"
#include "picocalc-sim/frontend.h"
#include "picocalc-sim/sim.h"
#include "link.h"
#include "gdb.h"
#include "profile.h"

static void usage(const char *argv0)
{
    printf("usage: %s [options] [FIRMWARE.uf2]\n"
           "\n"
           "PBConPC emulates a PicoBoy Color Plus (RP2350, 16 MB flash). A UF2 given on the command line\n"
           "is written into flash as if dropped on the BOOTSEL drive; without one, whatever the flash\n"
           "already holds boots, as when the PicoBoy is switched on.\n"
           "\n"
           "  -b, --board NAME       pbcplus (the default), or a bare pico2, pimoroni_pico_plus2 or pico\n"
           "      --rom FILE         the RP2040 boot ROM, as b2.elf from github.com/raspberrypi/\n"
           "                         pico-bootrom-rp2040 or a 16 KB image (default\n"
           "                         ~/.local/share/pbconpc/rp2040-rom.elf or .bin); pico needs it\n"
           "      --rp2350-rom A4|A2|stand-in|FILE\n"
           "                         the RP2350 boot ROM: Raspberry Pi's A4 (the default) or A2, both\n"
           "                         built in; picocalc-sim's own stand-in; or a bootrom-combined.bin\n"
           "                         from github.com/raspberrypi/pico-bootrom-rp2350\n"
           "  -f, --flash FILE       flash image to use and keep (default\n"
           "                         ~/.local/share/pbconpc/BOARD.flash; 'none' for one kept in\n"
           "                         memory until PBConPC exits)\n"
           "      --erase-flash      start with erased flash\n"
           "      --headless         no window; use with --script and screenshots\n"
           "  -s, --script FILE      drive the keyboard from FILE (see frontend/script.c)\n"
           "  -t, --timeout SECONDS  quit after this long\n"
           "      --scale N          window scale factor from 1 to 8, such as 1.5 (default 2)\n"
           "      --fullscreen       start in fullscreen: only the screen (F10, Alt+Enter, Esc leave it)\n"
           "      --fast             run SPI, I2C and UART transfers instantly (the default in PBConPC)\n"
           "      --exact-bus        model their timing instead, as upstream does (about half as fast)\n"
           "      --unthrottled      run as fast as the host allows instead of at device speed\n"
           "      --console          print what the firmware sends on UART0\n"

           "      --screenshot-dir D where F11 saves screenshots (default .)\n"
           "      --volume PCT       loudness of the sound in the window (default 50)\n"
           "      --mute             no sound in the window\n"
           "      --wav FILE         record the sound to FILE (48 kHz stereo WAV) in emulated time\n"

           "      --usb              plug the board's USB port into a host that enumerates the device the\n"
           "                         firmware presents and logs what it finds\n"
           "      --usb-console      --usb, with this terminal on the firmware's USB serial port: what it\n"
           "                         sends is printed and what is typed goes to it; Ctrl+] quits\n"
           "      --usb-terminal     in the window, show a terminal on that port beside the LCD\n"
           "      --usbip [ADDR:]PORT\n"
           "                         --usb, exporting the device over USB/IP on PORT (127.0.0.1 unless\n"
           "                         ADDR is given; * for all interfaces), for `usbip attach -r HOST -b 1-1`;\n"
           "                         tools/pbc-usb-attach does that, so PicoBoy Studio finds a serial port\n"
           "      --link[=NAME]      plug in the link cable: two PBConPC started with the same NAME\n"
           "                         (default cable) are connected, as two PicoBoys by the USB-C cable\n"
           "      --gdb[=[ADDR:]PORT] a GDB server (default port 3333): gdb-multiarch FIRMWARE.elf\n"
           "                         -ex \"target extended-remote :3333\"; cores are threads 1 and 2\n"
           "      --gdb-wait[=...]   the same, holding the PicoBoy at its first instruction until the\n"
           "                         debugger continues\n"
           "      --profile=FILE     profile the whole run: where each core spends its time, written\n"
           "                         to FILE at the end (script: profile start / profile stop FILE)\n"
           "      --profile-elf=ELF  name functions from this ELF in the report; repeatable\n"
           "      --trace            log faults, unknown ROM calls and bus errors\n"
           "\n"
           "Keys: arrows = joystick, A or Enter = centre, S = button A, D = button B. In the window, the\n"
           "menus change these settings and restart the machine. F11 saves a screenshot, F12 is the\n"
           "reset button, Shift+F12 resets into BOOTSEL, and dropping a UF2 file on the window flashes\n"
           "it. What the window changes is\n"
           "saved in settings.ini in the data directory for its next launch, where an option given here\n"
           "overrides it; headless runs ignore that file.\n",
           argv0);
}

int picocalc_sim_frontend_headless_run(void)
{
    if (!picocalc_sim_machine_running())
        return 1;
    uint64_t start = picocalc_sim_mono_ns();
    int code = 0;
    for (;;) {
        if (picocalc_sim_restart_requested() && !picocalc_sim_machine_restart())
            return 1;
        if (picocalc_sim_quit_requested(&code))
            return code;
        if (picocalc_sim_opts.timeout_s > 0 && (picocalc_sim_mono_ns() - start) / 1e9 >= picocalc_sim_opts.timeout_s)
            return 0;
        picocalc_sim_sleep_ns(10000000ull);
    }
}

static const char *default_card(void)
{
    static char path[4096];
    picocalc_sim_data_path(path, sizeof(path), "sd.img", false);
    struct stat st;
    return stat(path, &st) == 0 ? path : NULL;
}

int main(int argc, char **argv)
{
    const char *card = NULL;
    const char *gdb_spec = NULL;
    const char *profile_path = NULL;
    bool no_card = false;

    enum {
        OPT_HEADLESS = 256,
        OPT_SCALE,
        OPT_FAST,
        OPT_UNTHROTTLED,
        OPT_CONSOLE,
        OPT_BATTERY,
        OPT_SHOTS,
        OPT_NOCARD,
        OPT_TRACE,
        OPT_ERASE,
        OPT_WIFI_AP,
        OPT_FORWARD,
        OPT_NO_WIFI,
        OPT_USB,
        OPT_USB_CONSOLE,
        OPT_USB_TERMINAL,
        OPT_USBIP,
        OPT_VOLUME,
        OPT_MUTE,
        OPT_WAV,
        OPT_ROM,
        OPT_RP2350_ROM,
        OPT_LINK,
        OPT_GDB,
        OPT_GDB_WAIT,
        OPT_PROFILE,
        OPT_PROFILE_ELF,
        OPT_FULLSCREEN,
        OPT_EXACT_BUS,
    };
    static const struct option longopts[] = {
        {"board", required_argument, NULL, 'b'},
        {"flash", required_argument, NULL, 'f'},
        {"rom", required_argument, NULL, OPT_ROM},
        {"rp2350-rom", required_argument, NULL, OPT_RP2350_ROM},
        {"erase-flash", no_argument, NULL, OPT_ERASE},
        {"card", required_argument, NULL, 'c'},
        {"no-card", no_argument, NULL, OPT_NOCARD},
        {"headless", no_argument, NULL, OPT_HEADLESS},
        {"script", required_argument, NULL, 's'},
        {"timeout", required_argument, NULL, 't'},
        {"scale", required_argument, NULL, OPT_SCALE},
        {"fast", no_argument, NULL, OPT_FAST},
        {"unthrottled", no_argument, NULL, OPT_UNTHROTTLED},
        {"console", no_argument, NULL, OPT_CONSOLE},
        {"battery", required_argument, NULL, OPT_BATTERY},
        {"screenshot-dir", required_argument, NULL, OPT_SHOTS},
        {"trace", no_argument, NULL, OPT_TRACE},
        {"usb", no_argument, NULL, OPT_USB},
        {"usb-console", no_argument, NULL, OPT_USB_CONSOLE},
        {"usb-terminal", no_argument, NULL, OPT_USB_TERMINAL},
        {"usbip", required_argument, NULL, OPT_USBIP},
        {"link", optional_argument, NULL, OPT_LINK},
        {"gdb", optional_argument, NULL, OPT_GDB},
        {"gdb-wait", optional_argument, NULL, OPT_GDB_WAIT},
        {"profile", required_argument, NULL, OPT_PROFILE},
        {"profile-elf", required_argument, NULL, OPT_PROFILE_ELF},
        {"fullscreen", no_argument, NULL, OPT_FULLSCREEN},
        {"exact-bus", no_argument, NULL, OPT_EXACT_BUS},
        {"volume", required_argument, NULL, OPT_VOLUME},
        {"mute", no_argument, NULL, OPT_MUTE},
        {"wav", required_argument, NULL, OPT_WAV},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    // The long names of the options given, which the settings file then leaves alone.
    const char *given[sizeof(longopts) / sizeof(longopts[0])];
    int ngiven = 0;
    int opt;
    while ((opt = getopt_long(argc, argv, "b:f:c:s:t:h", longopts, NULL)) != -1) {
        for (const struct option *o = longopts; o->name; o++)
            if (o->val == opt && ngiven < (int)(sizeof(given) / sizeof(given[0])))
                given[ngiven++] = opt == OPT_EXACT_BUS ? "fast" : o->name;
        switch (opt) {
        case 'b': picocalc_sim_opts.board = optarg; break;
        case 'f': picocalc_sim_opts.flash_path = optarg; break;
        case OPT_ROM: picocalc_sim_opts.rom_path = optarg; break;
        case OPT_RP2350_ROM: picocalc_sim_opts.rp2350_rom_path = optarg; break;
        case OPT_ERASE: picocalc_sim_opts.erase_flash = true; break;
        case 'c': card = optarg; break;
        case OPT_NOCARD: no_card = true; break;
        case OPT_HEADLESS: picocalc_sim_opts.headless = true; break;
        case 's': picocalc_sim_opts.script_path = optarg; break;
        case 't': picocalc_sim_opts.timeout_s = atof(optarg); break;
        case OPT_SCALE: {
            float s = strtof(optarg, NULL);
            picocalc_sim_opts.scale = s < 1 ? 1 : s > 8 ? 8 : s;
            break;
        }
        case OPT_FAST: picocalc_sim_opts.fast = true; break;
        case OPT_UNTHROTTLED: picocalc_sim_opts.unthrottled = true; break;
        case OPT_CONSOLE: picocalc_sim_opts.console = true; break;
        case OPT_BATTERY: picocalc_sim_opts.battery = atoi(optarg); break;
        case OPT_SHOTS: picocalc_sim_opts.screenshot_dir = optarg; break;
        case OPT_TRACE: picocalc_sim_opts.trace = true; break;
        case OPT_WIFI_AP:
            if (picocalc_sim_opts.wifi_ap_count < 16)
                picocalc_sim_opts.wifi_aps[picocalc_sim_opts.wifi_ap_count++] = optarg;
            break;
        case OPT_FORWARD:
            if (picocalc_sim_opts.forward_count < 16)
                picocalc_sim_opts.forwards[picocalc_sim_opts.forward_count++] = optarg;
            break;
        case OPT_NO_WIFI: picocalc_sim_opts.no_wifi = true; break;
        case OPT_USB: picocalc_sim_opts.usb = true; break;
        case OPT_USB_CONSOLE: picocalc_sim_opts.usb = picocalc_sim_opts.usb_console = true; break;
        case OPT_USB_TERMINAL: picocalc_sim_opts.usb_terminal = true; break;
        case OPT_USBIP:
            picocalc_sim_opts.usbip = optarg;
            picocalc_sim_opts.usb = true;
            break;
        case OPT_VOLUME: {
            int v = atoi(optarg);
            picocalc_sim_opts.volume = v < 0 ? 0 : v > 100 ? 100 : v;
            break;
        }
        case OPT_MUTE: picocalc_sim_opts.mute = true; break;
        case OPT_WAV: picocalc_sim_opts.wav_path = optarg; break;
        case OPT_LINK: picocalc_sim_opts.link = optarg ? optarg : "cable"; break;
        case OPT_GDB: gdb_spec = optarg ? optarg : "3333"; break;
        case OPT_PROFILE: profile_path = optarg; break;
        case OPT_FULLSCREEN: picocalc_sim_opts.fullscreen = true; break;
        case OPT_EXACT_BUS: picocalc_sim_opts.fast = false; break;
        case OPT_PROFILE_ELF: profile_add_elf(optarg); break;
        case OPT_GDB_WAIT:
            gdb_spec = optarg ? optarg : "3333";
            atomic_store(&gdb_halt_req, true);   // held from the first instruction until the debugger continues
            break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }
    if (optind < argc)
        picocalc_sim_opts.uf2_path = argv[optind++];
    if (optind < argc) {
        usage(argv[0]);
        return 2;
    }
    bool window = !picocalc_sim_opts.headless && picocalc_sim_frontend_sdl_available();
    if (window) {
        picocalc_sim_settings_load(given, ngiven);
        const char *saved = picocalc_sim_settings_get("card");
        if (saved && *saved && !card && !no_card) {
            if (!strcmp(saved, "none"))
                no_card = true;
            else
                card = saved;
        }
    }
    if (!card && !no_card)
        card = default_card();
    picocalc_sim_opts.card_path = no_card ? NULL : card;

    // PBConPC: the link cable, before the machine runs so its clock starts level with the other end
    if (picocalc_sim_opts.link)
        link_open(picocalc_sim_opts.link, 0);
    if (gdb_spec)
        gdb_start(gdb_spec);
    if (profile_path)
        profile_start();

    // The window can show why the machine did not start, and has the settings to fix it.
    if (picocalc_sim_opts.script_path) {
        // PBConPC: exact emulated times for the script (machine.c script_wait)
        extern void picocalc_sim_script_hold(void);
        picocalc_sim_script_hold();
    }
    if (!picocalc_sim_machine_start() && !window)
        return 1;

    if (picocalc_sim_opts.script_path)
        picocalc_sim_script_start(picocalc_sim_opts.script_path);
    if (picocalc_sim_opts.usb_console)
        picocalc_sim_usb_console_start();

    int code;
    if (window)
        code = picocalc_sim_frontend_sdl_run();
    else
        code = picocalc_sim_frontend_headless_run();

    gdb_stop();
    if (profile_path)
        profile_stop(profile_path);
    picocalc_sim_machine_stop();
    link_close();
    fflush(NULL);
    exit(code);
}
