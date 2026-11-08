# Der PicoBoy Color Plus aus Sicht des Emulators

Was PBConPC nachbilden muss, damit die echten Firmwares laufen, vor allem
`pbc-plus/dist/pbcemu.uf2` (Starter plus alle Emulatoren). Zusammengestellt
aus dem Quelltext von pbc-plus (`~/Documents/micropython_berühren/pbc-plus`)
und den Erfahrungen damit auf dem Gerät. Die allgemeine Hardware-Referenz
für Portierungen steht dort in `docs/HARDWARE.md`; diese Datei hier beschreibt,
**welche Teile des Chips die Firmware wirklich anfasst und wie**.

Maßgeblich bei Registerdetails ist das RP2350-Datenblatt und die Header des
Pico SDK (`~/pico/pico-sdk/src/rp2350/hardware_regs/include/hardware/regs/`).

---

## 1. Chip und Speicher

| | |
|---|---|
| MCU | RP2350A, **zwei Cortex-M33**, ARMv8-M Mainline mit DSP-Erweiterung und einfach genauer FPU; die Firmware läuft im Secure-Zustand (`rp2350-arm-s`), keine TrustZone-Wechsel |
| Takt | Boot mit 150 MHz; Starter und Emulatoren stellen selbst um, die Emulatoren meist auf **200 MHz** (1,15 V) |
| Bootrom | 32 KB ab `0x00000000` |
| Flash | **16 MB**, per XIP ab `0x10000000` (Cache 16 KB), Sektor 4 KB, Seite 256 B |
| SRAM | 520 KB, `0x20000000`–`0x20081FFF`: 8 gestreifte Bänke bis `0x2007FFFF`, dazu SCRATCH_X `0x20080000` (Stack Kern 1) und SCRATCH_Y `0x20081000` (Stack Kern 0, je 4 KB) |
| Peripherie | APB ab `0x40000000`, AHB (DMA, USB, PIO) ab `0x50000000`, SIO `0xD0000000`, PPB (NVIC, SCB, SysTick, MPU, DWT) `0xE0000000` |

Adressen der Blöcke: `hardware/regs/addressmap.h`. Die wichtigsten:
CLOCKS `0x40010000`, RESETS `0x40020000`, IO_BANK0 `0x40028000`,
PADS_BANK0 `0x40038000`, XOSC `0x40048000`, PLL_SYS/USB `0x40050000`/`0x40058000`,
SPI0 `0x40080000`, I2C0 `0x40090000`, ADC `0x400A0000`, PWM `0x400A8000`,
TIMER0 `0x400B0000`, XIP_CTRL `0x400C8000`, QMI `0x400D0000`,
WATCHDOG `0x400D8000`, POWMAN `0x40100000`, DMA `0x50000000`,
USBCTRL `0x50100000` (DPRAM) / `0x50110000` (Register), PIO0 `0x50200000`.

---

## 2. Pins

| GPIO | Funktion | Richtung | Bemerkung |
|---|---|---|---|
| 0 | Joystick Mitte | in, Pullup | **aktiv LOW** |
| 1 | Joystick rechts | in, Pullup | aktiv LOW |
| 2 | Joystick unten | in, Pullup | aktiv LOW |
| 3 | Joystick links | in, Pullup | aktiv LOW |
| 4 | Joystick oben | in, Pullup | aktiv LOW |
| 27 | Taste A | in, Pullup | aktiv LOW |
| 28 | Taste B | in, Pullup | aktiv LOW |
| 8 | Display DC | out | |
| 9 | Display RST | out | |
| 10 | Display CS | out | normaler GPIO, nicht SPI-CSn |
| 18 / 19 | SPI0 SCK / MOSI | SPI | zum ST7789, Mode 0, 62,5–75 MHz |
| 26 | Hintergrundlicht | PWM | **Helligkeit = Tastverhältnis**; aus = schwarz |
| 15 | Lautsprecher | PWM | Kanal **B** des Slice 7, über RC-Tiefpass |
| 12 / 13 / 14 | LED grün / gelb / rot | out | aktiv HIGH |
| 11 | RGB-LED (WS2812) | PIO | nur einige Platinen, `common/pbc_rgb.c` |
| 20 / 21 | I2C0 SDA / SCL | I2C | Beschleunigungssensor STK8BA58 (Atari-Paddles durch Kippen, `a2600/src/tilt.c`) |
| 29 | Batteriespannung | ADC-Eingang 3 | über Spannungsteiler, `pbcemu/src/battery.c` |
| USB D+/D− | USB-C | USB / direkt | Laufwerk am PC **oder** Link-Kabel (Abschnitt 7) |

Die Tasten liest die Firmware über `gpio_get_all()` bzw. den SIO-Eingang,
ein gedrückter Knopf ist `0`. Vorschlag für die PC-Tastatur:
Pfeiltasten = Joystick, **A = Mitte, S = A, D = B**, dazu eine Taste für
RESET (Neustart: zurück zum Starter) und eine für „BOOTSEL“ (UF2 laden).

---

## 3. Flash-Aufteilung der Gesamt-Firmware (pbcemu.uf2)

Aus `common/pbc_launch.h`:

```
0x000000 .. 0x1FFFFF   Starter (bis 0x46000) und die Emulatoren, je an eigener
                       Adresse gelinkt (pbcemu/build/gen/pbc_apps.h)
0x200000 .. 0x6FFFFF   ROM-Bereich: das gerade gespielte Spiel (5 MB),
                       vom Emulator beim Start aus der FatFs hineinkopiert
0x700000 .. 0xFEFFFF   FatFs = das USB-Laufwerk PBC-ROMS (9 MB, 4-KB-Sektoren)
0xFF0000               ROM-Info der Einzel-Firmwares (vom Starter gelöscht)
0xFF1000 .. 0xFF5000   Einstellungen der Emulatoren
0xFF6000               Übergabe-Datensatz Starter -> Emulator (pbc_launch_t)
0xFF7000               Einstellungen des Starters
0xFF8000               ROM-Info der Gesamt-Firmware
0xFF9000               Favoriten
0xFFFF00               muss frei bleiben (RP2350-Erratum E10, picotool)
```

Ein leeres Laufwerk formatiert die Firmware selbst (FAT, Label `PBC-ROMS`)
und landet dann im USB-Modus. Für den Emulator heißt das: ein Flash-Abbild
von 16 MB als Datei, persistent über Neustarts. Spiele kommen am einfachsten
mit einem FAT-Werkzeug direkt in den Bereich ab `0x700000` (z. B. mtools:
`mcopy -i flash.img@@0x700000 spiel.nes ::`), ohne den USB-Modus nachzubilden.

UF2-Dateien: Blöcke der Familie RP2350-ARM-S (`0xE48BFF59`) bzw.
„absolute“ (`0xE48BFF57`) an `Zieladresse - 0x10000000` schreiben. picotool
legt einen einzelnen „absolute“-Block an `0x10FFFF00` dazu (Umgehung von
Erratum E10); der gehört **nicht** ins Flash. `tools/uf2flash.c` macht das.

---

## 4. Start, Übergabe, Neustart

1. **Kaltstart:** Das Bootrom sucht im Flash den IMAGE_DEF-Block (picobin)
   und startet über die Vektortabelle am Flashanfang (Starter).
2. **Spielstart:** Der Starter schreibt den Übergabe-Datensatz nach
   `0xFF6000` (Flash löschen + programmieren über das Bootrom), setzt
   `watchdog scratch[0] = 0x504D554A` ("JUMP"), `scratch[1] =` Flash-Offset
   des Emulators und löst einen **Watchdog-Neustart** aus.
3. Nach dem Neustart sieht der Starter (ganz früh in `main`) die Marke und
   springt: Interrupts aus, SysTick aus, NVIC leeren, `SCB->VTOR` auf die
   Vektortabelle des Emulators, `MSPLIM`, `MSP`, Sprung zum Reset-Handler
   (`pbcemu/src/main.c`, `jump_to`). Der Emulator stellt seine Takte selbst ein.
4. **RESET-Taste / „zurück“** im Spiel: normaler Neustart, die Marke ist weg,
   der Starter erscheint wieder.

Daraus folgt für den Emulator:
- Ein Watchdog-Neustart setzt alles zurück **außer** dem Flash und den acht
  `watchdog scratch`-Registern (0–7).
- `scratch[2]` zeigt auf `pbc_shared_t` im Starter (dessen Schriften lesen die
  Emulatoren direkt aus dem Flash); `scratch[3]` = Testmodus-Marke
  (`0xB007B007`), `scratch[5..7]` = Messwerte der Testbuilds.
- POWMAN meldet den Grund des Neustarts (Einschalten / RUN-Pin / Watchdog);
  der Starter spielt sein Intro nur nach Einschalten oder RESET.
- **BOOTSEL** wird per `reset_usb_boot()` (Bootrom) oder beim Einschalten mit
  gehaltenem A/B für den USB-Laufwerksmodus der Firmware genutzt.

---

## 5. Was die Firmware vom Chip benutzt

Gezählt über Starter und alle Emulatoren (Includes im Quelltext):

| Baustein | Wofür | Muss nachgebildet werden |
|---|---|---|
| SIO | GPIO lesen/setzen, CPUID, Kern-zu-Kern-FIFO, Spinlocks, `multicore_launch_core1`, Lockout bei Flash-Schreiben | ja, früh |
| TIMER0 | `time_us_64`, `sleep_us/ms`, Alarme für `sleep_until` (mit WFE) und `add_repeating_timer_ms` (Lobby des Starters) | ja, früh |
| CLOCKS, PLL_SYS/USB, XOSC, VREG (POWMAN) | Takt auf 200 MHz, `clk_peri` aus der USB-PLL (150 MHz), `clk_usb` | Register lesbar mit plausiblen Werten (PLL „locked“); die Frequenzen nur für SPI/PWM-Raten |
| RESETS | Bausteine aus dem Reset holen | Bits sofort „done“ |
| IO_BANK0, PADS_BANK0 | Pinfunktionen, Pullups | speichern; Funktion der Pins 18/19/26/15/20/21 |
| SPI0 + DMA | Display: Kommandos 8 Bit, Pixel 8 oder 16 Bit, DMA mit DREQ von SPI0 TX | ja, Kern der Bildausgabe |
| DMA | Display, bei manchen Emulatoren Ton | ja (Kanäle, Ketten, IRQ, DREQ) |
| PWM | Hintergrundlicht (GP26), Lautsprecher (GP15, Slice 7 B, Werte per IRQ oder DMA zum Takt des Wrap) | ja (Ton: Tastverhältnis als Abtastwert) |
| WATCHDOG | Neustart, scratch | ja |
| QMI, XIP_CTRL | Flash-Timing beim Übertakten, Cache-Wartung | Schreiben ignorieren; Cache-Flush darf nichts kaputt machen |
| Bootrom | Flash löschen/programmieren (`flash_range_erase/program`), `reset_usb_boot`, Funktionstabelle | ja: entweder das echte Bootrom (BSD-3) oder Ersatzfunktionen |
| ADC | Batterie (GP29) | ein fester, „voller“ Wert genügt |
| I2C0 | Beschleunigungssensor | antwortet mit Ruhelage; später per Maus/Tasten kippbar |
| PIO | nur WS2812-RGB-LED | darf fehlen (die Firmware kommt ohne aus) |
| USBCTRL | USB-Laufwerk (TinyUSB, Massenspeicher) **und** Link-Kabel (Direktmodus der Leitungen) | später, Abschnitt 7 |
| NVIC, SCB, SysTick, MPU | Interrupts, VTOR, Stack-Schutz per MPU (`PICO_USE_STACK_GUARDS=1`) | NVIC/SCB/SysTick ja; MPU darf anfangs fehlen |
| DWT | Zykluszähler nur in Messbuilds | einfacher Zähler |
| SYSINFO / unique ID | Kennung des Geräts (Lobby unterscheidet damit zwei Geräte) | je Instanz verschieden |

---

## 6. Der CPU-Kern

Die Firmware ist mit `-mcpu=cortex-m33` übersetzt (Thumb-2) und enthält:

- den vollen Thumb-2-Ganzzahlbefehlssatz von ARMv8-M Mainline, dazu die
  **DSP-Befehle** (`sel`, `usub8`, `uqsub8`, `pkhbt`, `smlad`, …), die die
  Zeichenroutinen von Mega Drive und anderen bewusst benutzen;
- **FPU** (einfache Genauigkeit), wenige Befehle;
- **Coprozessoren:** CP0 = GPIO-Coprozessor (`mcrr p0`, schnelles
  `gpio_put`), CP4 = Double-Coprozessor DCP (`mcrr/mrrc/mrc2 p4`, die
  `double`-Rechnung des SDK), CP7 = Redundanz-Coprozessor RCP (Startcode);
- exklusive Zugriffe (`ldrex/strex`), `wfe/sev/wfi`, `cpsid/cpsie`,
  `msr/mrs` (MSP, PSP, MSPLIM, PSPLIM, PRIMASK, BASEPRI, CONTROL), `tt`.

Besonderheiten, die ein Emulator kennen muss:
- Viel heißer Code läuft **aus dem SRAM** (`.time_critical`), nicht aus dem
  Flash.
- Der **Arduboy-Emulator erzeugt zur Laufzeit ARM-Code** im SRAM und patcht
  ihn (`arduboy/core/ard_jit.c`): selbstmodifizierender Code, nach dem
  Schreiben `dsb; isb`. Ein späterer Übersetzer (JIT) im PC-Emulator muss
  Schreibzugriffe auf übersetzten Code bemerken.
- Kern 1 wartet nach dem Reset im Bootrom auf drei Werte über den FIFO
  (Vektortabelle, Stack, Einsprung; `multicore_launch_core1`). Ohne echtes
  Bootrom muss der Emulator diese Wartezeit selbst spielen.

---

## 7. USB: zwei völlig verschiedene Betriebsarten

1. **USB-Laufwerk** (Starter „Add Games“, Einschalten mit gehaltenem A/B):
   TinyUSB als Massenspeicher, die FatFs ab `0x700000` erscheint am PC als
   `PBC-ROMS`. Im Emulator entbehrlich, solange Spiele per FAT-Werkzeug ins
   Abbild kommen.
2. **Link-Kabel zwischen zwei Geräten** (`common/usblink.c`): kein USB-
   Protokoll. Die Firmware übernimmt die Leitungen per
   `usb_hw->phy_direct`/`phy_direct_override` und taktet selbst:
   D− = CLK (nur der Master), D+ = DATA (halbduplex), Halbbit 1000 ns. Der
   Slave wartet in Schleifen auf Flanken (`time_us_32()` als Wächter).
   Darauf laufen die Lobby des Starters (`common/pbc_lobby.c`), die
   Dateiübertragung (`pbc_xfer.c`), der Gleichschritt im Spiel
   (`pbc_lockstep.c`) und das Game-Boy-Linkkabel (`pbc_cable.c`).
   **Wichtig:** Der USB-Baustein braucht dafür `clk_usb`; ohne Takt hängt
   er (genau dieser Fehler war bis v1.11 in sieben Emulatoren).

Für zwei Emulator-Instanzen heißt das: eine gemeinsame „Leitung“ (D+/D−
mit Treiber-Zustand je Seite) und eine abgestimmte emulierte Zeit, denn
das Protokoll misst Wartezeiten in Mikrosekunden.

---

## 8. Display ST7789

240×280 sichtbar, Controller-RAM 240×320, **sichtbar sind die Zeilen
20..299**. Kommandos über SPI mit DC (GP8) = 0, Daten mit DC = 1, CS (GP10)
low. Die Firmware benutzt: SWRESET, SLPOUT, COLMOD `0x55` (RGB565), MADCTL
`0xC8` (gedreht, **BGR**), INVON, NORON, DISPON, CASET, RASET, RAMWR (Pixel
als 16-Bit-Wörter, MSB zuerst). Bilder werden streifenweise per DMA
geschickt. Ein Bildschirm-Modell braucht: Fenster (CASET/RASET), RAM-Zeiger,
Farbumrechnung mit BGR und Invertierung, und das Hintergrundlicht (PWM GP26)
als Helligkeit.

---

**Sichtbare Ecken (gemessen 8.11.2026):** Die sichtbare Fläche hat stark
abgerundete Ecken, Radius **42 Panel-Pixel** (≈4,9 mm), in allen vier Ecken
gleich. Messung: `tests/pbc/cornertest` (über die USB-Konsole gesteuert) zeigt
in jeder Ecke einen Viertelkreis mit Radius r (die Eckkurve eines
abgerundeten Rechtecks); `tools/cornertest_run.py` schaltet r durch,
fotografiert jeweils mit der IPEVO-Kamera und prüft, ob die Bogenmitte
sichtbar ist: r = 41 zu 40 %, 42 zu 73 %, ab 43 ganz. Die erste Methode
(Rand einer grauen Fläche im Foto anpassen, `cornertest_measure.py` ohne
`--arcs`) ergab 52 – die Unschärfe der Kamera rundet Ecken zusätzlich, das
war zu groß. Am Testgerät ist das Display leicht schräg aufgeklebt; der
Emulator zeigt es gerade (`frontend/skin.c`, `CORNER_RADIUS`). Fototipp: mit
Zimmerlicht belichtet die Kamera ausgewogen; im Dunkeln überstrahlt das
Display.

## 9. Ton

PWM an GP15 (Slice 7, Kanal B) mit fester Trägerfrequenz; die Firmware
schreibt pro Abtastwert das Tastverhältnis, meist 45 454 Hz (Mega Drive
22 kHz), per Timer-IRQ oder DMA zum PWM-Wrap. Für den PC: das
Tastverhältnis je Abtastzeitpunkt als Wert nehmen, auf 44,1/48 kHz umsetzen.

---

## 10. Zeit

Alles hängt an `time_us_64()` (TIMER0) und am Systemtakt. Für einen
deterministischen Emulator: emulierte Zeit = emulierte Takte / 200 MHz, der
Timer zählt daraus Mikrosekunden. Echtzeit ist ein Tempo-Regler darüber.
Testbuilds lesen außerdem den DWT-Zykluszähler.
