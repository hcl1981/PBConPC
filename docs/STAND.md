# Stand

## 2026-11-07 — Anfang

- Projekt angelegt (`git init`, kein Remote, nichts committet).
- `docs/HARDWARE.md`: was die pbc-plus-Firmware vom Chip benutzt (Pins,
  Flash-Aufteilung, Start und Übergabe per Watchdog, Peripherie mit
  Priorität, CPU-Befehlsgruppen einschließlich CP0/CP4/CP7, USB-Laufwerk und
  Link-Kabel, Display, Ton, Zeit).
- `docs/PLAN.md`: Entscheidungen (eigener Kern in C, ohne Fenster zuerst,
  deterministisch, Ersatz-Bootrom), Aufbau, Etappen 0–7, Arbeitsregeln.
- **Etappe 0 erledigt:** `tools/uf2flash.c` schreibt UF2 in ein
  16-MB-Abbild wie das BOOTSEL-Laufwerk (nur berührte 4-KB-Sektoren,
  E10-Block ausgelassen). Geprüft mit `pbcemu.uf2` (v1.11): alle 8073
  Blöcke an ihrer Stelle, Vektortabelle SP `0x20082000`, Reset
  `0x1000015D`; ein zweites Laden lässt Laufwerk (`0x700000`) und
  Einstellungen (`0xFF7000`) unangetastet.

## 2026-11-07 — Fork von picocalc-sim, Starter und NES laufen

- Der Nutzer hat picocalc-sim freigegeben. Statt eines eigenen Kerns ist
  PBConPC jetzt ein Fork davon (Entscheidung und Änderungsliste in PLAN.md).
  Code liegt im Wurzelordner, der unveränderte Klon in `ref/picocalc-sim`.
- PicoBoy-Platine eingebaut (`sim/devices/picoboy_board.c`, `st7789.c`),
  Platine `pbcplus` mit 16 MB als Vorgabe, Oberfläche angepasst (Tasten,
  Bildgröße 240×280, Hilfe, Titel).
- Upstream-Fehler gefunden und behoben: 16-Bit-SPI-Rahmen kamen mit
  doppeltem oberen Byte beim Gerät an (`emu/devices.c`), sichtbar als
  falsche Farben in Streifen. Wäre einen Fehlerbericht an picocalc-sim wert.
- Getestet ohne Fenster mit `pbcemu.uf2` (v1.11):
  - Starter bootet über das echte Bootrom A4, formatiert das Laufwerk
    PBC-ROMS bei `0x700000`, zeigt das Karussell (18 Seiten) in richtigen
    Farben; Tasten per Skript (rechts, B öffnet, A zurück) wirken.
  - Spiele per `tools/pbc-addgames` (mtools) hinein: der Starter zählt sie.
  - NES: Super Mario Bros. 3 startet. „Loading game“ kopiert ins Flash,
    Watchdog-Übergabe an den NES-Emulator, beide Kerne, DMA zu SPI und PWM,
    Titelbild erscheint.
  - Tempo: ungebremst 101 s emulierte Zeit in 48 s (≈ 2,1× Echtzeit) auf
    i5-1335U, einschließlich Starter.
- Das Fenster (SDL3) baut und startet; von Hand noch nicht durchprobiert.

## 2026-11-07 (später) — alle Systeme, Einzel-Firmwares, Gehäusebild

- **pbcemu.uf2, alle Systeme mit Spiel getestet** (`tests/pbc/launch.sh`,
  Spiele aus `~/Desktop/Spielesamlung`): Atari 2600, ZX Spectrum, C64
  (OpenROMs), Vectrex (Mine Storm), NES, SG-1000, MSX (C-BIOS), CPC,
  Master System, Atari 7800, PC Engine, Mega Drive (Sonic 2), Game Gear,
  Game Boy Color: alle bis ins Spiel. Nicht getestet: ColecoVision (kein
  `coleco.rom` vorhanden), Arduboy (kein Spiel vorhanden).
- **Ton** geprüft (WAV von Alex Kidd): Stille im Starter, danach Musik mit
  vernünftigem Pegel.
- **Einzel-Firmwares aus `pbc-plus/dist`** (`tests/pbc/boot.sh`): noiz2sa,
  Spout, Stransball 2, Micropolis, Jump'n'Bump, i2collection, PicoBoyGB,
  Doom mit Freedoom, Tyrian laufen. `uf2flash` nimmt dafür jetzt auch Blöcke
  der Familie `0xE48BFF58` (Daten: Tyrian-Daten, Doom-WAD).
- **MicroPython** (`micropython-PBC_PLUS.uf2`): REPL über den emulierten
  USB-Anschluss (`--usb-console`, Skriptbefehl `usb send`), `import pbc`,
  Zeichnen auf dem Display, LEDs, Tasten abfragen funktionieren. Im Fenster:
  „View → USB serial terminal“.
- **Tyrian:** Taste A im Hauptmenü führt reproduzierbar zu einem Busfehler
  in newlib `free()` (Heap beschädigt; wohl der Beenden-Pfad der Portierung).
  Am Gerät noch nicht gegengeprüft; mit B/Mitte läuft alles.
- **Gehäusebild:** Das Fenster zeigt das LCD im Render des PBC+ (aus dem
  3D-Modell der Webseiten-Instanz), Joystick/Tasten anklickbar, LEDs
  leuchten; abschaltbar unter „View → Device frame“.

## 2026-11-07 (Nacht) — WiFi raus, USB zum PC, Windows

- WiFi aus Oberfläche und Kommandozeile entfernt (der PicoBoy hat keins).
- „Connect USB to this PC“: das USB-Gerät der Firmware wird per USB/IP zu
  einem echten USB-Gerät des PCs, allgemein für jede Firmware (serielle
  Schnittstelle für PicoBoy Studio, Thonny …; Laufwerke werden eingehängt).
  Geprüft: `usbip list -r 127.0.0.1` sieht MicroPython (2e8a:0005, CDC);
  pbcemu „Add Games“ meldet ein Massenspeichergerät (cafe:4009, 4-KB-Blöcke;
  der Test-Host von upstream kann 4-KB-Blöcke nicht einhängen, der
  Linux-Kernel schon). Das eigentliche Anhängen braucht root und ist noch
  nicht getestet.
- Windows-Fassung gebaut (`build-win/`), nur Windows-eigene DLLs; unter Wine
  bootet `pbcemu.uf2` in den Starter. Auf echtem Windows noch nicht getestet,
  ebenso usbip-win2.

## 2026-11-08 — Link-Kabel, GDB, Profiler

- **Link-Kabel** zwischen zwei PBConPC (`--link`, Menü „Link cable“):
  getestet mit pbcemu: beide zeigen „Link connected“; A startet Super Mario
  Bros. 3 als Player 1, B fehlt das Spiel, A schickt es (Balken und
  Restzeit auf beiden Seiten), dann laufen beide im Gleichschritt (gleiche
  Bilder), B ist Player 2. Unter Wine mit der Windows-Fassung ebenfalls
  „Link connected“. Tempo mit zwei Instanzen ohne Fenster: etwa 60 % der
  Echtzeit während der Übertragung (Last zweier Prozesse; noch nicht
  optimiert).
- **GDB-Server** (`--gdb`, `--gdb-wait`): mit einem Python-Client nach dem
  GDB-Protokoll geprüft (spout.elf): Halt am Reset, Haltepunkt auf `main`
  trifft genau, Einzelschritte, Register, Speicher, Thread 2 = Kern 1,
  `monitor reset`. Mit echtem gdb-multiarch noch nicht (nicht installiert).
- **Profiler**: Sonic 2 in pbcemu (10 s): Kern 0 zu 87 % beschäftigt
  (Z80 20 %, 68000 13 %, IM2-Zeilen 11 %), Kern 1 zu 100 %
  (`video_service` 40 % – vermutlich Warteschleife, `render` 16 %).

## 2026-11-08 — RGB-LED, Gamepads

- RGB-LED: mit MicroPython (`pbc.rgb_led(255, 90, 0)`) geprüft, das „+“ leuchtet
  orange; rote und grüne LED leuchten mit.
- Gamepads eingebaut (Linux mit System-SDL3, Windows mit XInput/DirectInput/
  WGI/HIDAPI); mangels Gerät noch nicht getestet.
- Für Fenstertests: `XDG_DATA_HOME=<eigener Ordner>` setzen, sonst liest der
  Test die gespeicherten Einstellungen des Nutzers (z. B. `usb-link = on`,
  das `pkexec` startet).

## 2026-11-08 (Nacht) — Tempo

- Gemessen mit eigenem Abtastprofil (perf ist gesperrt) und Zählern; Details
  und Änderungen in PLAN.md („Tempo“). Sonic 2 allein 76 % → 98,5 %, zu zweit
  per Link-Kabel 40–45 % → 69–77 % (ungebremst, am Netzteil; auf Akku drosselt
  der Laptop stark, Messungen dann nicht vergleichbar).
- Gefundene Ursachen: Neuübersetzen nach jedem programmierten Flash-Byte;
  zu kleine JIT-Tabellen im Link-Betrieb; doppelte Codeprüfung beim
  Wiedereintritt; Link-Leitung: teure Speicher-Schreibzugriffe pro Lesen und
  Warten über die Schlafphasen der Gegenseite.
- Nach allen Änderungen gegengeprüft: alle 14 Systeme in pbcemu, die
  Einzel-Firmwares, Tyrian, MicroPython-REPL, NES-Link mit Übertragung
  (identische Bilder wie vorher), Fenster, Windows unter Wine.

## 2026-11-08 (Vormittag) — weiteres Tempo

- Display-Mutex entfernt (−16 % CPU), DMA→SPI-Schnellweg (−15 bis 20 %),
  zusammen −21 % CPU-Zeit für dieselbe Arbeit; Bilder gleich bis auf winzige
  Zeitverschiebungen; Link-Spiel, 8 Systeme, Doom, Spout gegengeprüft.
- Messung bei laufendem Chrome (Nutzer arbeitet am PC): CPU-Zeit
  (`/usr/bin/time %U`) und Abtastpunkte vergleichen, nicht Wanduhrzeit.
- Nächste Kandidaten laut Profil: Peripheriezugriffe über `chip.c`
  (mem_read/mem_write/periph_write, ≈12 %), `devices_spi_frame` je Rahmen
  (Gerätesuche pro Rahmen), Chunk-Wiedereintritt (`cpu_run`, `jit_entry_for`,
  `run_chunk` ≈6 %), im JIT selbst die Flag-Berechnung (13 x86-Befehle pro
  `cmp`) und die Seitentabellen-Abfrage pro Speicherzugriff.

## 2026-11-08 (Mittag) — Display-Ecken

- Radius der abgerundeten Display-Ecken mit der IPEVO-Kamera am PBC+
  gemessen: 42 Panel-Pixel in allen vier Ecken (Viertelkreise mit Radius r,
  über USB-seriell von `tools/cornertest_run.py` umgeschaltet; HARDWARE.md
  Abschnitt 8). Gilt im Gehäusebild, ohne Rahmen und im Vollbild.
- Gehäusebild: Glas innerhalb der weißen Kante einheitlich schwarz; die
  Stufen an den oberen LCD-Ecken sind weg. Windows-Paket neu gebaut.
- Menüs und Fenster im Systemstil (Schrift, Hell/Dunkel, Akzentfarbe);
  geprüft: Arc hell und dunkel, KDE Breeze (simuliert). Windows/macOS nur
  gebaut bzw. ungetestet.

## 2026-11-08 (Nachmittag) — Chunks, Fehler beim Neustart von Kern 1

- Adaptive Chunk-Länge (4 µs / 1 µs bei Kern-zu-Kern-Verkehr), Chunk-Ende
  rückt an neu angesetzte Ereignisse: Sonic 2 −24 % CPU-Zeit. Bildvergleich
  9 Systeme, Doom, Spout, noiz2sa, Jump'n'Bump, MicroPython: gleich bis auf
  kleine Animationsphasen. Link-Paar läuft in beiden Varianten gleich.
- Endlosschleife behoben, wenn eine Firmware Kern 1 neu startet (Link-Partner
  meldet sich ab → pbcemu startet Kern 1 neu): `code_flush` ohne `check_exc`.
- Messhilfen: `PBCONPC_STATS` zählt LCD-Pixel und RAMWR.

## 2026-11-08 (Nachmittag) — JIT: Flags, Neuladen; exakte Skripte

- Messung im übersetzten Code (`tools/jitprof.py`): zwei Drittel der Zeit,
  breit verteilt (Register im Speicher 20 %, Sprünge 17 %, Flags ≈14 %,
  Zeitbuchhaltung 9 %, Seitentabelle 8 %).
- Flags nur noch, wo sie gebraucht werden (auch über Blockgrenzen), und
  verschmolzene `cmp`+`bcc`; Neuladen gerade gespeicherter Register entfällt.
  Exakt geprüft über Zustandsprüfsummen (17 Fälle) und `tests/pbc/smc`.
- Skripte laufen jetzt auf die emulierte Mikrosekunde genau (Maschine hält an
  jedem `wait`); vorher hingen Tastendrücke vom Host-Tempo ab.

## 2026-11-08 (Abend) — Register-Zuteilung, generische Befehle

- Register-Zuteilung (r0–r3, SP in Host-Registern): exakt, ≈2 %.
- Der eigentliche Bremsklotz waren nicht übersetzte Befehle (166 M Aufrufe
  in C bei Sonic 2): Literal-Ladebefehle, nicht ausgerichtete Zugriffe,
  DSP-Befehle. Jetzt direkt übersetzt: Sonic 2 −14 % CPU-Zeit.
- Messen auf diesem Rechner: P-/E-Kerne streuen 10 vs. 20 s; mit `taskset -c
  10` (E-Kern) sind Läufe auf ±1 % stabil.
- Dazu breite IT-Befehle, RCP-Kanarien und das Display im Block: Sonic 2
  insgesamt −20 % CPU-Zeit gegenüber dem Stand vor der JIT-Arbeit.
- FPU direkt übersetzt (FPU-lastiger Code 7× schneller), sechstes
  Host-Register (−4,5 %). Zeitkonstanten, AVX2-Eintrittsprüfung und Flags
  in x86-Form gemessen: kein Gewinn, standardmäßig aus.
- Testfirmware `tests/pbc/fptest`; Regressionen löschen ihre Flash-Kopien
  (16 MB je Lauf füllten /tmp).

## Offen / als Nächstes

1. Fenster im Alltag prüfen (Nutzer): Tasten, Mausklicks im Gehäusebild,
   Ton, Skalierung, Ziehen einer UF2 aufs Fenster; „Connect USB to this
   PC“ mit PicoBoy Studio und mit „Add Games“; Windows-Fassung auf einem
   echten Windows mit usbip-win2. Ton: `audio.c` hat noch die Filterwerte der PicoCalc
   (220R/100R/100nF, Hochpass 47 µF); der PicoBoy hat einen eigenen
   RC-Tiefpass an GP15. Mono durch zweimal denselben Pin.
2. Weiteres Tempo: im Link-Betrieb kostet die Gegenseite noch etwa 0,4 s
   PC-Zeit je emulierter Sekunde (Lesepfad der PHY-Register, Warten). Allein
   bleiben der übersetzte Code selbst (≈50 %) und DMA/SPI-Modell.
   PicoBoy Studio / Thonny an den emulierten MicroPython anschließen:
   upstream kann das Gerät per USB/IP exportieren (`--usbip`), dann
   `usbip attach` (braucht root) und es erscheint ein echtes /dev/ttyACM.
3. Batterie: ADC-Kanal 3 (GP29) liefert noch nichts Sinnvolles;
   `picoboy_set_battery` ist ein Platzhalter. (Neigungssensor: erledigt.)
4. (erledigt: LEDs leuchten im Gehäusebild)
5. Link-Kabel zwischen zwei Instanzen (Etappe 6): usblink schaltet die
   USB-Leitungen in den Direktmodus (HARDWARE.md Abschnitt 7); upstream
   `emu/usb.c` müsste die Leitungen D+/D− zwischen zwei Prozessen teilen.
6. Tests in `ci/` und `tests/` sind noch PicoCalc-bezogen.
7. Gehäusebild: RESET/BOOTSEL sitzen auf der Rückseite; evtl. als
   Schaltflächen neben dem Gerät. Die RGB-LED (GP11, WS2812 über PIO) wird
   noch nicht dargestellt.
8. Noch kein Commit (nur auf Wunsch des Nutzers).
