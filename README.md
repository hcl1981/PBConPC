# PBConPC — ein PicoBoy Color Plus auf dem PC

Emulator für den **PicoBoy Color Plus** (RP2350, 16 MB Flash, ST7789
240×280): beliebige `.uf2` laden, und er verhält sich wie das echte Gerät.
Zweck: die Firmwares aus pbc-plus (`pbcemu.uf2` mit Starter und allen
Emulatoren) testen, ohne zu flashen; später zwei Instanzen mit Link-Kabel.

PBConPC ist ein **Fork von [picocalc-sim](https://github.com/sg/picocalc-sim)**
(Steve Gibson, MIT-Lizenz, Stand Commit `ff33c99`). Von dort kommen der
RP2350-Kern (Cortex-M33 mit JIT, echtes Bootrom A4, DMA, PIO, PWM, SPI, USB …)
und die Oberfläche (SDL3 + Dear ImGui). Hier dazu: die PicoBoy-Platine
statt der PicoCalc-Platine. Die Original-README liegt in
[`docs/upstream/picocalc-sim-README.md`](docs/upstream/picocalc-sim-README.md).

**Stand:** `pbcemu.uf2` mit allen getesteten Systemen, die Einzel-Firmwares
aus `pbc-plus/dist` (Doom, Tyrian, Micropolis …) und MicroPython laufen.
Das Fenster zeigt das Display im Render des PBC+; Joystick und Tasten lassen
sich anklicken, die LEDs leuchten. Fortschritt:
[`docs/STAND.md`](docs/STAND.md), Plan und Arbeitsregeln:
[`docs/PLAN.md`](docs/PLAN.md), was nachzubilden ist:
[`docs/HARDWARE.md`](docs/HARDWARE.md).

## Herunterladen

Fertige Pakete liegen unter
[Releases](https://github.com/hcl1981/PBConPC/releases) (x86-64):

- **Windows 10/11:** `PBConPC-windows.zip` entpacken, `pbconpc.exe` starten.
- **Ubuntu, Debian, Mint …:** `pbconpc_<version>_amd64.deb` installieren
  (`sudo apt install ./pbconpc_<version>_amd64.deb`); danach steht PBConPC
  im Startmenü, dazu `pbc-addgames` und `pbc-uf2flash` (= `uf2flash`) auf
  der Kommandozeile.

Das Linux-Paket braucht glibc 2.35 oder neuer (Ubuntu 22.04, Debian 12,
Mint 21 und später); SDL3 ist eingebaut. Gebaut wird es mit
`packaging/linux/build-packages.sh VERSION` (in einem Ubuntu-22.04-Container,
docker).

## Bauen

Braucht `cmake`, einen C/C++-Compiler und `libsdl3-dev` (ohne SDL3 entsteht
nur die Fassung ohne Fenster). lwIP nimmt der Build aus dem Pico-SDK
(`PICO_SDK_PATH`), Dear ImGui lädt CMake einmal herunter (Prüfsumme fest).

```bash
cmake -S . -B build
cmake --build build -j
```

## Starten

```bash
# mit Fenster; die UF2 wird ins Flash geschrieben wie beim BOOTSEL-Laufwerk
build/pbconpc ~/Documents/micropython_berühren/pbc-plus/dist/pbcemu.uf2

# danach reicht
build/pbconpc
```

Das Flash (16 MB) liegt in `~/.local/share/pbconpc/pbcplus.flash` und bleibt
erhalten, wie auf dem Gerät. Eine neue UF2 kann man auch einfach auf das
Fenster ziehen.

| PC-Taste | PicoBoy |
|---|---|
| Pfeiltasten | Joystick |
| **A** oder Enter | Joystick Mitte |
| **S** | Taste A |
| **D** | Taste B |
| F12 | RESET |
| Shift+F12 | Neustart in BOOTSEL |
| F10 oder Alt+Enter | Vollbild an/aus (nur das Display, ohne Gehäusebild; Esc verlässt es) |
| F11 | Screenshot |
| Maus | Joystick und Tasten im Gehäusebild anklicken |
| Gamepad (USB/Bluetooth) | Steuerkreuz/linker Stick = Joystick, A/X = Taste A, B/Y = Taste B, Start/Select = Mitte |

**Neigungssensor (STK8BA58):** Sobald eine Firmware den Sensor abfragt,
öffnet sich das Fenster „Tilt sensor“ (auch unter „View → Tilt sensor“): den
Punkt ziehen kippt den PicoBoy; wie in PBC+ Studio zeigt der Punkt die
tiefere Seite (Punkt rechts: x negativ, Punkt oben: y positiv, z ergibt sich
aus 1 g), Doppelklick legt ihn flach. In Skripten `tilt X Y` (Werte in g).

Die Tasten für Joystick und Knöpfe lassen sich unter „Machine → Settings →
Keys“ ändern (bis zu drei je Bedienelement; „Reset to defaults“ stellt die
obige Belegung wieder her). Das Settings-Fenster kennt nur noch den PicoBoy
Color Plus mit Raspberry Pis Boot-ROM A4.

Mit zwei per Link-Kabel verbundenen Fenstern nimmt jedes sein eigenes Gamepad
(das erste Fenster das erste), auch im Hintergrund: zwei Spieler an einem PC.

Die LEDs leuchten im Gehäusebild: rot, gelb, grün (GP14/13/12) und die
RGB-LED im „+“ des PBC+-Schriftzugs (WS2812 an GP11, aus dem Signal am Pin
dekodiert).

MicroPython: Den REPL gibt es im Fenster unter „View → USB serial terminal“,
ohne Fenster mit `--usb-console`.

## Link-Kabel: zwei PicoBoys

**Machine → Link cable** in zwei PBConPC-Fenstern verbindet sie wie zwei
Geräte mit dem USB-C-Kabel: Link-Spiel (NES zu zweit usw.), „Send to
partner“, Game-Boy-Linkkabel, Jump'n'Bump, i2collection. PBConPC einfach
zweimal starten; die zweite Instanz bekommt automatisch eine eigene Kopie
des Flashs (`pbcplus-2.flash`). Ohne Fenster: `--link` bzw. `--link=NAME`
(gleicher Name = gleiches Kabel).

Technik (`emu/link.c`): Die Firmware treibt D+/D− selbst über die
PHY-Direktregister. Jede Flanke kommt mit ihrem genauen emulierten Zeitpunkt
über Shared Memory zur anderen Seite; wer die Leitung liest, wartet, bis die
andere Seite so weit gerechnet hat. Die Zeitbasis ist also exakt, egal wie
schnell die beiden Prozesse laufen.

## Debugger (GDB)

```bash
build/pbconpc --gdb-wait firmware.uf2       # hält am ersten Befehl
gdb-multiarch firmware.elf -ex "target extended-remote :3333"
```

`--gdb` lässt die Firmware laufen, bis der Debugger sich verbindet. Haltepunkte
(`break`, `hbreak`), Einzelschritt, `continue`, Strg+C, Register (auch
MSP/PSP, PRIMASK …, FPU) und Speicher; `load` schreibt ins Flash, `monitor
reset` drückt RESET. Die beiden Kerne sind Thread 1 und 2 (`info threads`).
Mit gesetzten Haltepunkten läuft die Emulation Befehl für Befehl (langsamer).
Geht auch mit VS Code (cortex-debug, Servertyp „external“, Port 3333).
Das System-GDB von Ubuntu kann kein ARM: `sudo apt install gdb-multiarch`.

## Profiler

Wo jeder Kern seine Zeit verbringt, nach Funktionen, in emulierter Zeit (also
wie auf dem Gerät):

```bash
build/pbconpc --profile=bericht.txt --profile-elf=firmware.elf firmware.uf2
```

Für ein Zeitfenster im Testskript: `profile start` … `profile stop bericht.txt`;
im Fenster: **Machine → Start profiler** / **Stop profiler** (Bericht im Log
und in `profile.txt` im Datenordner). Mehrere `--profile-elf` gehen, z. B. für
pbcemu `pbcemu/build/pbcemu.elf` plus `megadrive/build-multi/pbc_megadrive.elf`.
Beispiel Sonic 2 in pbcemu, Kern 0: `redcode_z80_run` 20 %, `m68k_run` 13 %,
`draw_line_a/b_im2_asm` 11 %, 13 % Leerlauf.

## USB zum PC: serielle Schnittstelle und Laufwerk wie echt

**Machine → Connect USB to this PC** macht den USB-Anschluss des emulierten
PicoBoy zu einem echten USB-Gerät dieses PCs, für jede Firmware:

- eine serielle Schnittstelle (MicroPython, Arduino-Sketche …) erscheint als
  `/dev/ttyACM…` bzw. COM-Port: für PicoBoy Studio (Web Serial im Browser),
  Thonny, mpremote, die Arduino-IDE, ein Terminal;
- ein Laufwerk (pbcemu „Add Games“, PicoBoyGB, das BOOTSEL-Laufwerk nach
  Shift+F12) wird vom Desktop eingehängt wie ein USB-Stick.

Technik: PBConPC exportiert das Gerät per USB/IP (`--usbip 3240`); das
Hilfsskript hängt es mit dem Treiber des Betriebssystems an, auch nach jedem
Neustart der Firmware wieder, bis man den Menüpunkt abwählt oder PBConPC
schließt. Die Wahl wird gespeichert.

- **Linux:** fragt einmal pro Sitzung nach dem Passwort (`pkexec`, lädt
  `vhci-hcd` und ruft `usbip attach`). Braucht das Paket mit `usbip`
  (Ubuntu: `linux-tools-generic`). Von Hand:
  `build/pbconpc --usbip 3240` und `sudo tools/pbc-usb-attach`.
- **Windows:** einmal [usbip-win2](https://github.com/vadimgrn/usbip-win2/releases)
  installieren; der Menüpunkt fragt dann per UAC nach Administratorrechten
  und startet `pbc-usb-attach.ps1` (liegt neben `pbconpc.exe`).

## Tempo

PBConPC übersetzt den ARM-Code der Firmware nach x86 (JIT) und lässt
Busübertragungen (SPI zum Display, I2C, UART) sofort fertig sein; das genaue
Zeitmodell gibt es mit `--exact-bus` (etwa halb so schnell). Der lokale Build
ist für die eigene CPU optimiert; noch etwas schneller wird
`tools/build-pgo.sh` (Ergebnis `build-pgo/pbconpc`). Am Laptop: mit Netzteil
und Energieprofil „Leistung“ läuft es deutlich schneller als auf Akku.

## Spiele ins Laufwerk

Der Starter legt beim ersten Start das Laufwerk PBC-ROMS an (FatFs ab
`0x700000`). Spiele kommen bei **geschlossenem** Emulator hinein:

```bash
tools/pbc-addgames ~/Desktop/Spielesamlung/*.nes
tools/pbc-addgames -l          # Inhalt zeigen
```

## Windows

Gebaut wird unter Linux mit MinGW-w64 (`apt install mingw-w64`):

```bash
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/mingw-w64-x86_64.cmake
cmake --build build-win -j
```

Ergebnis: `build-win/pbconpc.exe` (Fenster), `pbconpc-console.exe` (dasselbe
für die Kommandozeile), `uf2flash.exe` und `pbc-usb-attach.ps1`. Die
Programme brauchen nur DLLs, die Windows 10/11 mitbringt; einfach den
Ordnerinhalt kopieren. Daten liegen in `%LOCALAPPDATA%\pbconpc`. Spiele
kommen unter Windows am einfachsten wie beim echten Gerät hinein: „Add
Games“ mit „Connect USB to this PC“.

## Ohne Fenster (Tests)

```bash
build/pbconpc --headless --flash test.flash --script test.pcs --timeout 60 pbcemu.uf2
```

Skriptbefehle: `wait <ms>`, `key <taste>` (`up`, `down`, `left`, `right`,
`a`, `s`, `d`, `enter`), `screenshot <datei.png>`, `restart`, `quit`; siehe
`frontend/script.c`. `--trace` zeigt am Ende Register und Zustand,
`--unthrottled` läuft so schnell wie möglich.

## Werkzeuge

```bash
# UF2 in ein 16-MB-Flash-Abbild schreiben, ohne den Emulator
build/uf2flash test.flash pbcemu.uf2
```

## Lizenz

MIT, wie picocalc-sim (siehe `LICENSE`). Das RP2350-Bootrom in `roms/`
ist von Raspberry Pi (BSD-3). Firmwares, die man darin laufen lässt,
behalten ihre eigenen Lizenzen.
