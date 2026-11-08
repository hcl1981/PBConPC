# PBConPC — Plan

Ein PicoBoy Color Plus auf dem PC: beliebige `.uf2` laden, der Emulator
verhält sich wie das echte Gerät (16 MB Flash, Display, Tasten, Ton, später
USB-Laufwerk und Link-Kabel zwischen zwei Instanzen). Hauptzweck:
pbc-plus-Firmwares testen, ohne zu flashen, und Link-Spiel ohne zwei Geräte.

Stand und Hardware: `docs/HARDWARE.md`.

---

## Entscheidungen

- **Fork von picocalc-sim** (`sg/picocalc-sim`, C, MIT, Steve Gibson; Stand
  Commit `ff33c99`, November 2026). Am 7.11.2026 vom Nutzer freigegeben,
  nachdem ein eigener Kern geplant war. picocalc-sim bringt einen fertigen
  RP2350 mit: Cortex-M33 mit JIT nach x86, das echte Bootrom (A4) eingebaut,
  DMA, PIO, PWM, PL022-SPI, I2C, UART, USB (Gerät und Host), Watchdog,
  deterministische Zeit, Fenster mit SDL3 und Dear ImGui, Skripte und
  Screenshots ohne Fenster. Ein unveränderter Klon liegt zum Vergleich in
  `ref/picocalc-sim` (nicht im Git, `.gitignore`).
- **Was PBConPC ändert** (Liste pflegen, damit spätere Übernahmen von
  upstream gelingen):
  - `sim/devices/picoboy_board.c` statt `picocalc_board.c`: ST7789, Tasten,
    Lautsprecher, LEDs (siehe HARDWARE.md Abschnitt 2).
  - `sim/devices/st7789.c`: aus `st7365p.c` abgeleitet (RAM 240×320,
    MADCTL `0xC8` als Grundstellung, Ausgabe Zeilen 20..299, BGR = Blau oben).
  - `sim/include/picocalc-sim/devices.h`: Bildgröße 240×280, `picocalc_t`
    mit `st7789_t *lcd`, ohne Southbridge/PSRAM; `picoboy_*`-Funktionen.
  - `emu/chip.c/.h`: Platine `pbcplus` (RP2350A, 16 MB, keine PSRAM);
    `emu/machine.c`: RP2350-Images laufen ohne `--board` immer darauf.
  - `emu/devices.c`: `picocalc_sim_gpio_duty()` (PWM-Tastverhältnis für das
    Hintergrundlicht); **Fehlerkorrektur** bei 16-Bit-SPI-Rahmen (upstream
    schickte das obere Byte zweimal; der Starter sendet Pixel so).
  - `sim/devices/sdcard.c`: verträgt `NULL` (kein Kartenschacht).
  - `frontend/`: Tasten an `picoboy_key`, Bild aus `st7789_render`,
    Fenstertitel, Hilfe „Keys“, Menü „SD card“ entfernt, Fenstergröße.
  - `sim/src/runtime.c`: Datenordner `~/.local/share/pbconpc`.
  - Programm heißt `pbconpc`; Projektname in `CMakeLists.txt`.
  - Eigene Werkzeuge: `tools/uf2flash.c`, `tools/pbc-addgames`.
  - Gehäusebild: `frontend/skin.c/.h`, `assets/pbc_front.png` (Render aus dem
    3D-Modell, siehe `assets/README.md`), eingebettet über
    `cmake/EmbedBinary.cmake` (schreibt jetzt auch `NAME_size`); Menü
    „View → Device frame“, Mausklicks auf Joystick/Tasten, LED-Leuchten;
    `ui_fit_window` passt das Fenster an die Bildschirmhöhe an.
  - Testhaken `PBCONPC_WINDOW_SHOT=datei.png` (+ `PBCONPC_WINDOW_SHOT_MS`)
    in `frontend/sdl.c`: speichert das ganze Fenster; mit
    `SDL_VIDEO_DRIVER=offscreen SDL_RENDER_DRIVER=software` ohne Bildschirm.
  - Skripte laufen in **emulierter Zeit**: `picocalc_sim_sleep_emulated_ms()`
    in `emu/machine.c`, benutzt von `frontend/script.c` (`wait`, Tasten) und
    `frontend/keys.c`; Tastendruck 60 ms. Upstream wartete nach PC-Uhr, so
    gingen unter Last Tastendrücke verloren.
  - WiFi entfernt: Reiter „WiFi“ ausgeblendet, Boards mit Funk nicht mehr
    in der Auswahl, `--wifi-ap/--forward/--no-wifi` aus Hilfe und Parser,
    Hilfetext auf den PicoBoy umgeschrieben (`frontend/main.c`).
  - „Machine → Connect USB to this PC“ (`usb_link_set` in `frontend/ui.cpp`):
    schaltet den USB/IP-Server von upstream ein (Port 3240) und startet
    `tools/pbc-usb-attach` per `pkexec` (Linux) bzw.
    `tools/pbc-usb-attach.ps1` per UAC mit usbip-win2 (Windows). Das Skript
    hängt das Gerät an und nach Firmware-Neustarts wieder an, solange
    PBConPC läuft und die Datei `usb-link.on` im Datenordner existiert.
    Gespeichert als `usb-link`, das Gehäusebild als `device-frame`.
  - Windows-Build (upstream-Weg, MinGW): `shell32` dazugelinkt; CMake
    kopiert `pbc-usb-attach.ps1` neben die .exe.
  - **Link-Kabel** `emu/link.c/.h`: Shared Memory `pbconpc-link-NAME`
    (POSIX `shm_open` bzw. Windows `CreateFileMapping`), je Seite ein Ring
    mit (Zeit, Leitungszustand); Lesen von `phy_direct` (usb.c) wartet, bis
    die andere Seite die Zeit erreicht hat (konservative Synchronisation,
    kein Verklemmen, da der Zurückliegende nie wartet). `chip.access_t` =
    Zeit des Kerns, der gerade auf den Bus greift (`touch_now`).
    `link_publish` nach jedem Chunk in `run_slice`. Option `--link[=NAME]`,
    Menü „Link cable“ (gespeichert als `link`).
  - **Flash-Sperre** (`claim_flash` in machine.c): eine Instanz pro
    Flash-Abbild; die zweite nimmt `BOARD-2.flash`, beim ersten Mal als Kopie.
  - **GDB-Server** `emu/gdb.c/.h`: `--gdb[=PORT]`, `--gdb-wait`; hält die
    Maschine über `gdb_halt_req` (machine_main, run_slice, run_chunk), bei
    Haltepunkten/Schritten läuft `cpu_run` Befehl für Befehl (`gdb_slow`).
  - **Profiler** `emu/profile.c/.h`: Stichprobe alle 4 Chunks (≈4 µs) pro
    Kern, Funktionsnamen aus ELF-Symboltabellen; Optionen `--profile`,
    `--profile-elf`, Skriptbefehle `profile start/stop/elf`, Menüeintrag.
  - **RGB-LED**: `picoboy_board.c` dekodiert das WS2812-Signal an GP11 aus
    den Flankenzeiten (`picocalc_sim_gpio_watch` + `gpio_edge_time`),
    `picoboy_rgb()`; `skin.c` färbt das „+“ multiplikativ ein (eigene
    vormultiplizierte Textur, SDL_BLENDMODE_MUL) und legt einen Schein darüber.
  - **Gamepads** (`frontend/sdl.c`, `pad_event`): SDL-Gamepad-API; mit
    Link-Kabel nimmt Seite n das n-te Gamepad (`link_side()`).
    `cmake/Dependencies.cmake` baut SDL für Windows jetzt mit Joystick und
    HIDAPI.
  - **Vollbild**: `ui_set_fullscreen` (ui.cpp) ohne Menü, Terminal und
    Gehäusebild, schwarzer Rand, Mauszeiger aus; F10, Alt+Enter, Esc
    (sdl.c), Menü „View → Fullscreen“, Option `--fullscreen`.
  - **Tempo (8.11.2026)**, alles in upstream-Code, jeweils gemessen und mit allen
    Systemen/Firmwares gegengeprüft:
    - `emu/qmi.c` `flash_code_changed` + `chip.code_page[]` (gesetzt in
      `block.inc` `block_build`): Flash schreiben macht übersetzten Code nur
      ungültig, wo Blöcke herkamen. Upstream erhöhte `code_gen` bei jedem
      Byte – „Loading game“ übersetzte nach jedem Byte alles neu.
    - `BLK_SETS` 4096→32768, `JIT_LOOKUP` 32768→262144: im Link-Betrieb flog
      Code aus den Tabellen, wurde neu übersetzt, der 64-MB-Puffer lief über.
    - `block_find_for_jit` (jit.inc) und `jit_entries[2][4096]`
      (`jit_entry_for`/`jit_run_code`, cpu.c): Wiedereintritt in nativen Code
      ohne die große Blocktabelle und ohne doppelten Codevergleich; die
      Übersetzung prüft sich beim Betreten selbst.
    - `pl022.c` `frame_ps` zwischengespeichert, `dma.c` `busy_mask`.
    - `link.c`: schneller Lesepfad ohne Schreiben in den gemeinsamen Speicher,
      Veröffentlichen alle 16 Chunks oder bei `waiting` der Gegenseite und
      vor jedem Schlafen (`link_publish_now` in machine_main), Warteschleifen
      auf der Leitung werden bis zur nächsten möglichen Änderung übersprungen
      (`poll_same`, höchstens 1 µs pro Lesezugriff).
    - Ergebnis (i5-1335U, am Netzteil): Sonic 2 allein 76 % → 98,5 %, zu zweit
      per Link 40–45 % → 69–77 %, NES-Linktest mit Übertragung 100 s → 65 s.
    - Messwerkzeuge: `PBCONPC_STATS=1` (Tempo, PC-Takte je Kern, JIT-
      Übersetzungen/Ausgänge, Link-Warten; Skriptbefehl `stats` gibt aus und
      setzt zurück), `PBCONPC_HOSTPROF=datei` + `tools/hostprof.py`
      (Abtastprofil von PBConPC selbst, Ersatz für das gesperrte perf; SIGUSR1
      schreibt sofort), `PBCONPC_PROFILE_PCS=1` (heißeste Firmware-Adressen im
      Profiler-Bericht). Für Zeilenangaben `build-prof` mit `-g` bauen.
    - Zwei emulierte Kerne auf zwei PC-Kerne zu verteilen lohnt kaum: bei
      Sonic 2 braucht Kern 1 nur 12 % der PC-Zeit.
    - **Sofortige Busübertragungen als Standard** (`runtime.c` `.fast = true`,
      `--exact-bus` schaltet das genaue Zeitmodell wieder ein): das Modell
      jedes SPI-Rahmens zum Display kostete rund die Hälfte der PC-Zeit
      (Sonic 2: 92 % → 170 %). Geprüft: NES gleiche Bilder, Mega Drive gleiches
      Spieltempo (nur das Laden davor geht schneller), weitere Systeme laufen.
    - **Display ohne Mutex** (`st7789.c`): upstream sperrte für jedes SPI-Byte
      einen Mutex gegen das Fenster; das waren 11,7 % der CPU-Zeit (in libc,
      gefunden mit `tools/hostprof_libs.py`). Jetzt schreibt nur der
      Emulations-Thread, das Fenster liest ohne Sperre (höchstens Tearing wie
      am echten Display), `generation` ist `volatile`. −16 % CPU-Zeit.
    - **DMA→SPI-Schnellweg** (`dma.c` `bulk_spi`, `pl022.c` `pl022_bulk_tx`):
      bei sofortigem Bus übergibt ein DMA-Kanal, der aus normalem RAM ins
      SPI-Datenregister schreibt, seinen Lauf seitenweise direkt an das Gerät;
      Empfangs-FIFO und Überlauf wie beim einzelnen Rahmen, erstes Element und
      letzte 8 über den normalen Weg (Zugriffsprüfung, Fertig-IRQ, Verkettung).
      Nicht bei Empfangs-DMA auf demselben SPI, Ringpuffer, Byte-Tausch,
      Sniffer oder Quellen außerhalb von normalem RAM. −15 bis 20 % CPU-Zeit;
      Bilder gleich bis auf winzige Zeitverschiebungen.
    - Zähler unter `PBCONPC_STATS=1`: SPI-Datenschreiber (DMA/Kern 0/Kern 1),
      Schnellweg-Läufe und -Rahmen.
    - **Compiler**: lokaler Linux-Build mit `-march=native` und LTO
      (`PBCONPC_NATIVE`, Standard an, nicht für Windows), +≈5 %;
      `tools/build-pgo.sh` baut zusätzlich mit profilgesteuerter Optimierung
      nach `build-pgo/pbconpc`, +≈4 %.
  - **Adaptive Chunks** (machine.c): 4 µs statt 1 µs, solange die Kerne
    nichts austauschen; nach FIFO, Doorbell, umkämpftem Spinlock oder SEV an
    einen Kern in WFE 20 µs lang 1 µs (`chip_cores_talk`). Das Chunk-Ende
    `m->until` rückt an jedes Ereignis heran, das im laufenden Chunk angesetzt
    wird (ev_schedule); der JIT liest es nach jedem Aufruf in C neu
    (`call_a`: `cmp/cmova r13`). Ereignisse feuern dadurch pünktlich (vorher
    bis 1 µs zu spät). Sonic 2: −24 % CPU-Zeit, gleiche Bilder.
    `PBCONPC_CHUNK_NS=n` erzwingt eine feste Länge (Messungen). Testfirmware
    `tests/pbc/coretalk` (FIFO-, Flag- und Spinlock-Pingpong über UART,
    `--console`): FIFO 1 µs/Runde wie vorher, Flag im RAM 4 statt 1 µs.
    Profiler tastet nach emulierter Zeit ab.
  - **JIT: Flags nur, wo sie gebraucht werden** (jit.inc `flag_liveness`):
    rückwärts durch den Block; ein Befehl, dessen Flags sicher überschrieben
    werden, bevor etwas sie liest, berechnet keine (`flag_effect` kennt
    Leser/Schreiber je Handler; Unbekanntes liest alle). Am Blockende zählen
    die Folgeblöcke (`succ_live_in`, bis 6 Befehle direkt aus dem Speicher;
    Flash-Seiten werden als Code markiert, SRAM-Blöcke prüfen die Folge-Bytes
    beim Eintritt und verwerfen sich bei Änderung, `jit_drop`). Ein
    abschließendes `bcc` nimmt seine Bedingung per SETcc direkt aus den
    x86-Flags des Befehls davor (`K.fuse`, `jx.fcond`; der generische Pfad
    setzt sie aus APSR). `PBCONPC_FLAGOPT=1|3|5|7` / `PBCONPC_NO_FLAGOPT=1`
    zum Vergleichen. Exakt: Zustandsprüfsummen von 17 Firmwares/Systemen
    gleich; Test `tests/pbc/smc` (Code nach einem Block ändert sich).
  - **JIT: kein Neuladen gerade gespeicherter Register** (`mov_rm`/`mov_mr`):
    folgt auf `mov [rbx+rX],eax` direkt `mov eax,[rbx+rX]`, entfällt es;
    Rücksprünge aus Kaltpfaden laden eax nach (`jmp_main`).
    `PBCONPC_NO_RELOADOPT=1` schaltet es ab.
  - **JIT: SRAM ohne Seitentabelle** (`emit_page`): Adressen im SRAM (linear
    auf `chip.sram` abgebildet) werden per Bereichstest erkannt, ohne den
    Tabellenzugriff, auf den der eigentliche Zugriff warten musste; alles
    andere sucht weiter in der Tabelle, aus dem Weg (`emit_page_table`).
    `PBCONPC_NO_PAGEOPT=1` schaltet es ab.
  - **JIT: Register-Zuteilung** (`RG`, `reg_map_init`): Gastregister r0–r3
    und SP liegen im übersetzten Code dauerhaft in RBP, RSI, RDI, R10, R11
    (gleich für alle Übersetzungen, verkettete Blöcke laden nichts um);
    Abgleich mit `c->r[]` beim Ein- und Austritt (`jit_emit_fixed`) und um
    jeden Aufruf in C (`emit_spill`, `call_a`). `PBCONPC_REGMAP=0,1,2,3,13`
    bzw. `none`. Allein ≈2 %.
  - **JIT: bisher generische Befehle direkt** (Messung: `PBCONPC_STATS`
    zählt Aufrufe je Befehlsart, `pbc_jit_gen_report`): LDR/LDRH/LDRB
    (literal, 32-Bit) und `ldr pc,[pc]` (Sprungbrücke, auch aus SRAM und
    mitten im Block); nicht ausgerichtete LDR/STR/LDRH/STRH ins SRAM (wie
    ld32u: Bytezyklen, und Zeitbudget wie der Handler, `emit_page_unaligned`);
    UADD8/USUB8/UQADD8/UQSUB8/SEL (SSE2, GE wie `parallel_addsub`),
    PKHBT/PKHTB, SMULxy/SMLAxy (Q), USAT (LSL), MOVS.W mit Verschiebung,
    Verschiebungen um ein Register bei toten Flags. Sonic 2: generische
    Aufrufe 166 M → 5 M; Schalter `PBCONPC_NO_LITOPT`, `_UAOPT`, `_DSPOPT`.
    Ein Rücksprung nach XIP-Fehlabruf (`PBCONPC_FETCHOPT=1`) bleibt aus: ein
    MMIO-Zugriff im selben Befehl sähe die Fehlzyklen zu früh.
  - Außerdem direkt: breite Befehle unter IT, die ein zweites Halbwort
    abrufen (`PBCONPC_NO_ITFETCHOPT`), und die RCP-Kanarien `mrc2/mcr2 p7,#0`
    (`emit_rcp`, Tabelle `rcp.canary` je Kern, gefüllt beim Setzen des Salzes).
  - **Display im Block** (`xfer_bulk` im SPI-Gerät, st7789 `lcd_bulk`): ein
    DMA-Lauf von Datenbytes geht auf einmal ans Display, mit derselben
    Parameterlogik wie Byte für Byte; das Empfangs-FIFO füllt sich wie bei
    Einzelrahmen. `PBCONPC_NO_LCDBULK=1` schaltet es ab.
  - **FPU direkt** (`emit_fp`): VADD/VSUB/VMUL/VNMUL/VDIV/VSQRT, VMLA/VMLS/
    VNMLA/VNMLS, VFMA/VFMS/VFNMA/VFNMS (nur mit FMA3 auf dem Host), VABS/
    VNEG/VMOV, VCMP(E), VCVT Ganzzahl↔Float, VLDR/VSTR (einfach), VMOV
    Kern↔FPU, VMRS. Exakt wie fpu.inc: nativ nur bei FPSCR ohne
    Rundungsmodus/FZ/DN und ohne NaN hinein oder heraus, sonst Handler.
    Testfirmware `tests/pbc/fptest` (Rechnungen, Grenzfälle, FPSCR):
    gleiche Ausgabe, 7× schneller. `PBCONPC_NO_FPOPT`, `PBCONPC_NO_FMA`.
  - **Sechstes Host-Register**: das Zeitlimit liegt nur noch in `jx.until`
    (R13 frei), Zuordnung jetzt r0–r4 und SP: −4,5 % bei Sonic 2.
  - Ausprobiert, exakt, aber ohne Gewinn und daher standardmäßig aus:
    Taktzeiten als Konstanten (`PBCONPC_PSIMM=1`), SRAM-Eintrittsprüfung mit
    AVX2 (`PBCONPC_AVX2ENTRY=1`), Flags in x86-Form (`PBCONPC_FXFLAGS=1`,
    Doom 3 % langsamer).
  - **Ergebnis gesamt** (fest auf einem E-Kern gemessen, i5-1335U): Sonic 2
    19,4 → 15,6 s (−20 %), Doom 16,3 → 15,7 s; alle 17 Prüfsummen und alle
    109 Bildschirmfotos gleich.
  - **Settings**: nur noch pbcplus mit ROM A4 (keine Board- und ROM-Auswahl
    mehr im Fenster, gespeicherte board/rom-Einträge älterer settings.ini
    werden übergangen; `--board`/`--rom` auf der Kommandozeile bleiben für
    Tests); ohne „Charging“ (AA-Zellen, keine Ladeschaltung; `--charging`
    entfernt); neuer Reiter „Keys“ (`frontend/keymap.c`, settings.ini
    `key-<control>-1..3`, Voreinstellung wie zuvor fest im Code).
  - **Neigungssensor STK8BA58** (picoboy_board.c, I2C0 GP20/21, Adresse
    0x18, Chip-ID 87h, Bereich 0Fh, Achsen gedreht wie auf der Platine): Fenster
    „Tilt sensor“ öffnet sich bei der ersten Abfrage; der Punkt zeigt wie in PBC+ Studio die tiefere Seite (Punkt rechts: x negativ, oben: y positiv). Skriptbefehl `tilt X Y` (Rohwerte in g); das Feld zeigt den Zustand des Sensors (`picoboy_get_tilt`), also auch per Skript gesetzte Neigung. Test `tests/pbc/tilttest`; MicroPythons
    `pbc_hw.accel_*_g()` liest die eingestellten Werte.
  - **Fehlerbehebung (upstream) dw_i2c.c**: Abschalten des I2C-Blocks
    (`IC_ENABLE = 0`, das SDK tut es vor jeder Übertragung) verwarf noch nicht
    weitergegebene Schreibbytes; „Register setzen, dann lesen“ kam so nie an.
  - **Linux-Paket** (`packaging/linux/`): `build-packages.sh VERSION` baut in
    einem Ubuntu-22.04-Container (glibc 2.35, ohne `-march=native`, SDL3
    eingebaut) ein .deb und mit `APPIMAGETOOL` ein AppImage.
    `cmake/Dependencies.cmake` gibt `DOWNLOAD_EXTRACT_TIMESTAMP` erst ab CMake
    3.24 an (3.22 brach daran ab). ui.cpp sucht `pbc-usb-attach` zur Laufzeit
    neben dem Programm, in `../lib/pbconpc` und erst dann in `tools/`; aus einem
    AppImage wird es vor `pkexec` in den Datenordner kopiert.
  - **Skripte auf die emulierte Mikrosekunde genau**: mit `--script` hält die
    Maschine an der Zielzeit jedes `wait` an (machine.c `script_wait`,
    `hold_ps`); Tasten und Bildschirmfotos fallen so auf exakte Zeitpunkte,
    gleiche Skripte geben gleiche Prüfsummen (vorher je nach Host-Tempo
    verschieden).
  - **Messwerkzeuge**: `PBCONPC_HOSTPROF` schreibt zusätzlich den JIT-Puffer
    (`.jit`, `.jit.index`), `tools/jitprof.py` zeigt die Zeit je übersetztem
    Block und je x86-Befehl; `PBCONPC_JITPAD=n` verschiebt die Übersetzungen
    (Prüfung auf Lageabhängigkeit).
  - **SPI-Schnellweg**: `pl022_bulk_tx` bestimmt die Zielgeräte einmal je
    DMA-Lauf (`devices_spi_targets`) statt je Byte.
  - **Fehlerbehebung (upstream)**: `cpu_run` leert die Übersetzungen bei
    `code_flush` auch ohne `check_exc`; `jit_look` konnte `check_exc` schon
    abgearbeitet haben, dann hing der Kern in einer Endlosschleife ohne
    Zeitfortschritt (pbcemu startete Kern 1 neu, das Boot-ROM änderte die SAU).
  - **Abgerundete Display-Ecken**: `skin_draw_lcd_corners` (skin.c) legt eine
    4-fach geglättete Maske über die LCD-Ecken (im Gehäusebild der Render selbst, sonst schwarz), Radius 42 Panel-Pixel
    (gemessen, HARDWARE.md Abschnitt 8), im Gehäusebild, ohne Rahmen und im
    Vollbild. Messfirmware `tests/pbc/cornertest`, Auswertung
    `tools/cornertest_measure.py`.
  - **Glas im Gehäusebild**: `skin_init` färbt das Glas innerhalb der weißen
    Kante einheitlich schwarz (Füllung von der Mitte aus, begrenzt auf das
    abgerundete Rechteck x 78–721, y 77–882, Radius 90 des Renders, 4 px
    Abstand). Der Render hatte oben einen grauen Streifen und die runden
    Ecken des Displaymoduls, was an den oberen LCD-Ecken als Stufen auffiel.
  - **Systemstil** (`frontend/theme.cpp`): Menüs und Fenster mit der Schrift,
    Hell/Dunkel (folgt Umschaltungen im Betrieb) und Akzentfarbe des Systems;
    GNOME/Ubuntu/Arc über gsettings, KDE über kdeglobals (Breeze), Windows 11
    (Segoe UI, Akzent aus der Registry), macOS. „View → Theme“: System, Light,
    Dark (gespeichert als `theme`). Testhaken `PBCONPC_TEST_MENU=View,Theme`
    hält Menüs für Fensterfotos offen.
  - Tests: `tests/pbc/launch.sh` (ein System in pbcemu starten),
    `tests/pbc/boot.sh` (beliebige UF2s aus leerem Flash).
- **Deterministisch** wie upstream: emulierte Zeit in Pikosekunden;
  `--unthrottled` und `PICOCALC_SIM_STOP_AT_MS` für reproduzierbare Tests.
- Die interne Benennung `picocalc_sim_*` bleibt, damit upstream-Änderungen
  sich weiter einspielen lassen. Nur Sichtbares heißt PBConPC.

---

## Aufbau

```
emu/              der Chip (upstream): cpu.c + thumb2.inc + jit.inc, dma, pio,
                  pwm, pl022 (SPI), dw_i2c, usb, qmi (Flash), rom.c, machine.c
sim/devices/      was auf der Platine sitzt: picoboy_board.c, st7789.c,
                  audio.c (PWM-Ton-Filter); PicoCalc-Teile ungenutzt
frontend/         main.c (Kommandozeile), sdl.c + ui.cpp (Fenster),
                  keys.c (Tasten, Screenshots), script.c (Testskripte)
roms/rp2350/      Raspberry Pis Bootroms A2/A4 (werden eingebaut)
tools/            uf2flash.c, pbc-addgames, mkcard (upstream, ungenutzt)
ci/, tests/       Tests von upstream (PicoCalc-bezogen, noch nicht angepasst)
docs/             HARDWARE.md, PLAN.md, STAND.md, upstream/
```

---

## Etappen

| # | Ziel | Fertig, wenn |
|---|---|---|
| 0 | `tools/uf2flash`: UF2 in ein Flash-Abbild | **erledigt** |
| 1 | Grundlage | **erledigt** durch den Fork von picocalc-sim |
| 2 | Starter bis zum ersten Bild | **erledigt**: Karussell als PNG, Farben richtig |
| 3 | Bedienen und Spiele starten | **erledigt** für NES: Tasten, Übergabe per Watchdog, Spiel ins Flash geladen, läuft |
| 4 | Alle Emulatoren prüfen, Ton, Fenster im Alltag | jeder Emulator aus pbcemu.uf2 zeigt ein Spiel; Ton aus GP15 klingt richtig; Batterie (ADC GP29), Beschleunigungssensor (I2C) |
| 5 | Tempo | alle Emulatoren in Echtzeit (NES: ungebremst etwa 2,1× Echtzeit auf i5-1335U) |
| 6 | Link-Kabel | **erledigt**: Lobby findet sich, Spiel wird übertragen, NES zu zweit im Gleichschritt (auch unter Windows/Wine) |
| 7 | USB zum PC | serielle Schnittstelle und Laufwerk als echtes USB-Gerät (USB/IP): eingebaut, Export geprüft (`usbip list`), Anhängen mit root noch vom Nutzer zu testen; Windows mit usbip-win2 ungetestet |

---

## Arbeitsweise (für alle Instanzen)

- **pbc-plus nur lesen.** Hier wird nichts an pbc-plus geändert; dort gelten
  eigene Regeln (Commit nur auf Wunsch des Nutzers usw.).
- **Commits nur auf Wunsch des Nutzers**, als
  `Jan Schulz <info@picoboy.de>` mit
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Ein Remote gibt
  es noch nicht.
- **Ein Baustein, ein Besitzer.** Wer an einer Datei in `emu/`,
  `sim/devices/` oder `frontend/` arbeitet, trägt das unten unter „Wer macht was“ ein und nimmt
  es nach getaner Arbeit wieder heraus.
- **Jede Änderung mit Test.** Am einfachsten ein Skript mit `--headless`
  und Screenshots (siehe README); Änderungen am CPU-Kern zusätzlich mit
  dem `state digest` von `--trace` vor und nach der Änderung vergleichen.
- **Wissen in die Doku.** Was man über die Hardware oder die Firmware lernt,
  gehört nach `docs/HARDWARE.md`; Fortschritt nach `docs/STAND.md`.
- picocalc-sim auszuführen und zu ändern ist freigegeben (Nutzer,
  7.11.2026); anderen heruntergeladenen Code nur nach Rückfrage.
- Freigaben in `~/.claude/settings.json`: `cmake`, `build/pbconpc`,
  `build/tools/mkcard/picocalc-sim-mkcard`.
- Deutsch in den Dokumenten, Englisch in Code und Kommentaren (wie pbc-plus).

### Wer macht was

| Baustein | Instanz | seit |
|---|---|---|
| — | — | — |
