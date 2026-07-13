# WatchOS

Eine eigenständige Firmware für die **LilyGo T-Watch S3** (ESP32-S3). WatchOS
vereint mehrere Zifferblätter, eine Sammlung alltagstauglicher Mini-Apps, ein
konfigurierbares Always-On-Display, ein vollständiges Web-Dashboard mit
OTA-Update und – klar als solche gekennzeichnet – eine Reihe von
WLAN-/BLE-Sicherheitswerkzeugen für autorisierte Tests am eigenen Netz.

> ⚠️ **Wichtiger Hinweis vorab:** Der „Bad Apps“-Bereich enthält Werkzeuge
> (Deauther, Beacon-/BLE-Spam, Captive Portal, BadUSB), deren Einsatz gegen
> fremde Geräte oder Netze in vielen Ländern strafbar ist. Bitte lies den
> Abschnitt [Rechtliches & Ethik](#rechtliches--ethik), bevor du sie nutzt.

---

## Inhalt

- [Hardware](#hardware)
- [Funktionsübersicht](#funktionsübersicht)
- [Bedienung (Gesten)](#bedienung-gesten)
- [Web-Dashboard & OTA](#web-dashboard--ota)
- [Bauen & Flashen](#bauen--flashen)
- [Erste Inbetriebnahme](#erste-inbetriebnahme)
- [Projektstruktur](#projektstruktur)
- [Konfiguration & Speicherung](#konfiguration--speicherung)
- [Rechtliches & Ethik](#rechtliches--ethik)
- [Version](#version)

---

## Hardware

Zielplattform ist die **LilyGo T-Watch S3** mit folgenden Komponenten, die von
WatchOS angesteuert werden:

| Komponente        | Baustein          | Bus / Pins                     |
|-------------------|-------------------|--------------------------------|
| SoC               | ESP32-S3 (16 MB)  | –                              |
| Display           | ST7789 (240×240)  | via `TFT_eSPI`, Backlight GPIO 45 |
| Power Management  | AXP2101           | I²C `Wire` (SDA 10 / SCL 11)   |
| Touch             | FT6X36            | I²C `Wire1` (SDA 39 / SCL 40), INT 16 |
| Haptik            | DRV2605           | I²C `Wire` (SDA 10 / SCL 11)   |
| Beschleunigung    | BMA423            | I²C `Wire` (Addr. 0x19)        |
| Mikrofon          | PDM               | I²S (WS 44 / DIN 47)           |
| Lautsprecher      | I²S-DAC           | I²S (BCK 48 / WS 15 / DOUT 46) |
| USB               | HID / CDC         | nativer USB des ESP32-S3       |

---

## Funktionsübersicht

### Zifferblätter (Seiten 0–3)

Per horizontalem Wischen umschaltbar; einzelne Faces lassen sich im
Web-Dashboard aktivieren/deaktivieren.

- **Lebensziel** – Countdown (Jahre/Monate/Tage/…) bis zu einem gewählten
  Zielalter, mit Fortschrittsbalken.
- **Opportunität** – heute bereits „verdientes“ Netto in Echtzeit sowie der
  *echte* Stundenlohn (inklusive Urlaub, Krankheit, Teilzeit).
- **Kinder** – Alter von bis zu drei Kindern in Jahren/Monaten/Wochen/Tagen.
- **Elternvergleich** – zeigt das Datum, an dem ein Elternteil so alt war, wie
  du es heute bist.

### Quick Settings (Seite 5)

Kacheln für **WLAN** (Hotspot/Client), **Bluetooth**, **Vibration**, **AOD**,
**Akku-Details**, **Taschenlampe**, **Kill All** (stoppt alle Hintergrund-Apps)
und **USB-Modus** (HID ↔ serielles COM).

### Good Apps (Menü Seite 22)

- **Stoppuhr** mit Rundenzeiten
- **Timer** mit einstellbaren Presets und Alarm (Haptik/Ton)
- **Dezibel-Meter** (PDM-Mikrofon, RMS → dB SPL)
- **Rechner**
- **Mouse-Jiggler** (hält den PC über USB-HID wach)
- **Würfel** (d4–d100 + Münze)
- **Wasserwaage** (BMA423-Neigungsmesser)

### Bad Apps (Menü Seite 23) — *nur für autorisierte Tests*

- **Rubber Ducky / BadUSB** – Ducky-Script-Interpreter über USB-HID
- **WiFi Beacon Spammer**
- **BLE Spammer** (Apple-Popup-Payloads)
- **Nmap** – einfacher Portscan im verbundenen Netz
- **WiFi Deauther**
- **Probe-Request-Monitor** (Promiscuous-Mode-Sniffer)
- **BLE-Scanner**
- **Evil Portal** – Captive Portal mit anpassbarer Login-Seite

### Weitere Features

- **Always-On-Display (AOD):** frei konfigurierbare Elemente (Digital-/
  7-Segment-Uhr, Datum, Akku, Wortuhr, Binäruhr, Analoguhr, Sekunden-/
  Minutenbogen, Wochentag) mit Position, Größe und Farbe, inkl.
  Burn-in-Schutz (Pixel-Shift).
- **Retro-Boot-Animationen:** Award-BIOS/MS-DOS, Commodore 64, Windows-BSOD
  oder ein stiller Schnellstart – mit passenden, synthetisch erzeugten
  Sound-Effekten.
- **5-stufiges Power-Management:** Aktiv → Dimmen → AOD → Light-Sleep →
  Stealth, inklusive dynamischer CPU-Taktung (80/160/240 MHz).
- **Mehrsprachig:** Deutsch, Englisch, Französisch (im Dashboard umschaltbar).
- **Matrix-Bildschirmschoner.**

---

## Bedienung (Gesten)

| Aktion                              | Geste                                            |
|-------------------------------------|--------------------------------------------------|
| Zifferblatt wechseln                | horizontal wischen (Seiten 0–3)                  |
| Quick Settings öffnen               | vom Zifferblatt nach **unten** wischen           |
| App-Menü öffnen                     | vom Zifferblatt nach **oben** wischen            |
| Good- / Bad-Menü                    | im App-Menü links / rechts tippen                |
| Aktive App beenden                  | in der App nach **oben** wischen (Kill)          |
| Zurück zum Menü                     | Good-App nach links, Bad-App nach rechts wischen |
| Aus AOD/Sleep aufwecken             | doppelt tippen                                   |
| Ins AOD springen / Taschenlampe aus | Krone (PEK-Taste) kurz drücken                   |

---

## Web-Dashboard & OTA

WatchOS bringt einen eingebauten Webserver mit. Über **Quick Settings → WLAN**
wird entweder ein Access Point geöffnet (Standard-SSID `WatchOS`) oder die Uhr
verbindet sich mit dem hinterlegten Heimnetz (Client-Modus). Die aktuelle IP
wird oben in den Quick Settings angezeigt; per mDNS ist die Uhr zusätzlich
unter **`http://watchos.local/`** erreichbar.

Im Dashboard konfigurierbar:

- Uhrzeit / NTP / Zeitzone
- Farbschema und einzelne UI-Farben
- aktive Zifferblätter und komplette AOD-Gestaltung
- persönliche Daten (Gehalt, Kinder, Elternteil)
- Parameter der Bad Apps (Ziel-SSIDs, Ducky-Skripte, Evil-Portal-HTML …)
- Sprache, Boot-Modus, Timing des Power-Managements
- **Passwortschutz** des Dashboards (HTTP-Basic-Auth)
- **OTA-Update:** neue `WatchOS.ino.bin` direkt über den Browser einspielen

> 🔒 Ist der Passwortschutz aktiv, ist **auch** der OTA-Upload
> (`/update`) durch die Authentifizierung abgesichert – ohne gültiges Passwort
> wird nichts in den Flash geschrieben.

---

## Bauen & Flashen

### Voraussetzungen

- **Arduino IDE** oder **arduino-cli** mit installiertem
  **ESP32-Core (esp32:esp32)**.
- Board: **LilyGo T-Watch S3** (`twatchs3`).
- Bibliotheken:
  - [`TFT_eSPI`](https://github.com/Bodmer/TFT_eSPI) – passend für die
    T-Watch S3 konfiguriert
  - [`XPowersLib`](https://github.com/lewisxhe/XPowersLib) (AXP2101)
  - [`SensorLib`](https://github.com/lewisxhe/SensorLib) (FT6X36-Touch,
    DRV2605-Haptik)
  - ESP32-Core-Bestandteile (WiFi, BLE, USB HID, FFat, WebServer, Update, …)
    sind bereits im Core enthalten.

### Board-Einstellungen (FQBN)

```
esp32:esp32:twatchs3:USBMode=hwcdc,CDCOnBoot=default,CPUFreq=240,PartitionScheme=app3M_fat9M_16MB,LoopCore=1,EventsCore=1
```

Wichtig ist insbesondere das **Partitionsschema `app3M_fat9M_16MB`** (App +
FAT-Dateisystem auf 16 MB Flash). Die FAT-Partition (`FFat`) wird für große
Konfigurationsdaten wie das Evil-Portal-HTML und die BadUSB-Skripte genutzt.

### Kompilieren mit arduino-cli

```bash
arduino-cli compile \
  --fqbn "esp32:esp32:twatchs3:PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CPUFreq=240,LoopCore=1" \
  --output-dir build/esp32.esp32.twatchs3 \
  .
```

### Windows-Komfort-Skript

`build_firmware.ps1` kapselt den arduino-cli-Aufruf, zeigt eine
Fortschrittsanzeige mit ETA (aus vergangenen Build-Zeiten) und legt das
Ergebnis unter `build/esp32.esp32.twatchs3/WatchOS.ino.bin` ab. Vor der Nutzung
den Pfad zur `arduino-cli.exe` in der Variablen `$cli` an die eigene
Installation anpassen.

```powershell
./build_firmware.ps1
```

### Flashen

- **Erstflash:** per USB mit `arduino-cli upload` bzw. der Arduino IDE.
- **Danach:** komfortabel per **OTA** über das Web-Dashboard – einfach die neu
  erzeugte `WatchOS.ino.bin` hochladen.

---

## Erste Inbetriebnahme

1. Firmware per USB flashen (siehe oben).
2. Uhr startet mit der gewählten Boot-Animation.
3. In **Quick Settings** WLAN aktivieren.
4. Mit dem Access Point `WatchOS` verbinden (Standard-Passwort siehe unten) und
   das Dashboard im Browser öffnen (`http://watchos.local/` oder die angezeigte
   IP).
5. Persönliche Daten, Farben, Zifferblätter und ggf. das Heimnetz eintragen.

> 🔐 **Standard-Zugangsdaten unbedingt ändern.** Ab Werk sind hinterlegt:
> Access-Point `WatchOS` / `12345678`. Setze im Dashboard eigene AP-Daten und
> aktiviere den Passwortschutz, bevor du unterwegs bist.

---

## Projektstruktur

| Datei                | Inhalt                                                            |
|----------------------|------------------------------------------------------------------|
| `WatchOS.ino`        | `setup()` / `loop()`, globale Instanzen, Task-Stack-Größe         |
| `Globals.h`          | zentrale `extern`-Deklarationen und Konstanten (inkl. `WATCHOS_VERSION`) |
| `HardwareConfig.*`   | PMU/Touch/Haptik-Init, NVS-Laden, Boot-Sequenz, Akku-Telemetrie, Sound-Synthese |
| `Display.*`          | Rendering aller Seiten, Header, 7-Segment-Renderer, AOD, Seiten-Slide-Animation |
| `Logic.*`            | Touch-Auswertung, Power-Management, Hintergrund-Tasks, Mikrofon, WLAN/BLE-Tools |
| `GoodApps.*`         | Menü + Good-Apps (Rechner, Jiggler, Würfel, Wasserwaage)          |
| `BadApps.*`          | Menü + Evil Portal                                                |
| `WebManager.*`       | kompletter Webserver, Dashboard-HTML, API, Import/Export          |
| `UpdateOTA.*`        | OTA-Firmware-Upload (`/update`, authentifiziert)                  |
| `build_firmware.ps1` | Build-Wrapper für Windows                                         |

---

## Konfiguration & Speicherung

- Einstellungen liegen im **NVS** (`Preferences`, Namespace `clock`). Beim Start
  prüft WatchOS den Namespace auf Funktion und freien Platz.
- Große Strings (Evil-Portal-HTML, BadUSB-Skripte, USB-Modus als Fallback)
  werden zusätzlich im **FFat**-Dateisystem gehalten.
- Die Akku-Verlaufskurve wird zur Schonung des Flash nur alle ~50 Minuten
  persistiert.

---

## Rechtliches & Ethik

Die unter **Bad Apps** zusammengefassten Funktionen (WiFi-Deauth,
Beacon-/BLE-Spam, Captive Portal / Evil Portal, BadUSB, Portscan) sind
ausschließlich für **Bildungszwecke** sowie für **Sicherheitstests an Systemen
und Netzen gedacht, für die du eine ausdrückliche Erlaubnis hast**.

Der Einsatz gegen fremde Geräte, Netze oder Personen ohne Einwilligung ist in
vielen Rechtsordnungen (u. a. Deutschland: §§ 202a ff., 303a/b StGB) strafbar.
Die Autoren und Mitwirkenden übernehmen **keine Haftung** für Missbrauch. Die
Verantwortung für einen rechtmäßigen Einsatz liegt allein bei dir.

---

## Version

Aktuelle Firmware-Version: **1.3.0** (`WATCHOS_VERSION` in `Globals.h`).

Die Versionsnummer erscheint im Boot-Banner (seriell) und im Web-Dashboard.
