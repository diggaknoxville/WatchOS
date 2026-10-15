# WatchOS

Eine eigenständige, hochmoderne Firmware für die **LilyGo T-Watch S3** (ESP32-S3). WatchOS vereint anpassbare Zifferblätter, ein modulares App-Paketsystem, 2FA-Authentifizierung, LoRa-Kommunikation, Always-On-Display, ein responsives Web-Dashboard mit Glassmorphism-UI, OTA-Update sowie eine Suite an Sicherheits- und Analysewerkzeugen.

> ⚠️ **Wichtiger Hinweis:** Der Security- und Testbereich („Bad Apps“) enthält Werkzeuge (Deauther, Beacon-/BLE-Spam, Captive Portal, BadUSB, Portscanner), deren Einsatz gegen fremde Netze oder Geräte ohne ausdrückliche Erlaubnis in den meisten Ländern strafbar ist. Bitte beachte den Abschnitt [Rechtliches & Ethik](#rechtliches--ethik).

---

## Inhalt

- [Hardware & Spezifikationen](#hardware--spezifikationen)
- [Funktionsübersicht](#funktionsübersicht)
  - [Zifferblätter](#zifferblätter)
  - [Modulares App-Management](#modulares-app-management)
  - [Produktivitäts-Apps (Good Apps)](#produktivitäts-apps-good-apps)
  - [Security- & Netzwerk-Tools (Bad Apps)](#security--netzwerk-tools-bad-apps)
  - [LoRa-Messenger & Funk](#lora-messenger--funk)
  - [Modernes Web-Dashboard](#modernes-web-dashboard)
  - [Always-On-Display (AOD) & Power-Management](#always-on-display-aod--power-management)
- [Bedienung & Gesten](#bedienung--gesten)
- [Bauen & Flashen](#bauen--flashen)
  - [PlatformIO](#1-platformio)
  - [Linux (arduino-cli & OTA-Skript)](#2-linux-arduino-cli--ota-skript)
  - [Windows (PowerShell)](#3-windows-powershell)
  - [Arduino IDE](#4-arduino-ide)
- [Erste Inbetriebnahme](#erste-inbetriebnahme)
- [Projektstruktur](#projektstruktur)
- [Konfiguration & Speicherung](#konfiguration--speicherung)
- [Rechtliches & Ethik](#rechtliches--ethik)
- [Version & Changelog](#version--changelog)

---

## Hardware & Spezifikationen

Zielplattform ist die **LilyGo T-Watch S3** (inkl. SX1262-Funkrevision):

| Komponente         | Baustein / Spezifikation    | Bus / Anbindung                  |
|--------------------|-----------------------------|----------------------------------|
| **SoC**            | ESP32-S3 (Dual-Core Xtensa) | 16 MB Flash, 8 MB OPI PSRAM      |
| **Display**        | ST7789 IPS LCD (240×240)    | SPI via `TFT_eSPI`, Backlight GPIO 45 |
| **Touchscreen**    | FocalTech FT6X36            | I²C `Wire1` (SDA 39 / SCL 40, INT 16) |
| **Power Management** | X-Powers AXP2101          | I²C `Wire` (SDA 10 / SCL 11)     |
| **Haptik**         | TI DRV2605L Haptic Driver   | I²C `Wire` (Internal Trigger)    |
| **Beschleunigung** | Bosch BMA423 (3-Achsen)     | I²C `Wire` (Addr 0x19)           |
| **LoRa-Transceiver** | Semtech SX1262            | SPI (868 / 915 MHz)              |
| **Mikrofon**       | PDM-Mikrofon                | I²S RX (WS 44 / DIN 47)          |
| **Audio-Ausgabe**  | I²S DAC / Verstärker        | I²S TX (BCK 48 / WS 15 / DOUT 46)|
| **USB**            | Nativer USB OTG             | CDC Serial & HID (Tastatur/Maus) |

---

## Funktionsübersicht

### Zifferblätter

Umschaltbar per horizontalem Wischen; im Dashboard aktivier- und sortierbar:

1. **Lebensziel:** Countdown (Jahre, Monate, Tage, Stunden, Minuten, Sekunden) bis zu einem wählbaren Zielalter mit dynamischem Fortschrittsbalken.
2. **Opportunität:** Zeigt heute bereits verdientes Nettoeinkommen in Echtzeit basierend auf Arbeitszeit, Gehalt, Urlaubstagen und Krankheitstagen sowie den tatsächlichen Stundenlohn.
3. **Kinder:** Exaktes Alter von bis zu 3 Kindern mit Namen und Aufteilung in Jahre/Monate/Wochen/Tage.
4. **Elternvergleich (Mama & Papa):** Vergleicht dein aktuelles Alter mit dem deiner Eltern (separat konfigurierbar für Mama und Papa).
5. **Chronograph:** Modernes analog-inspiriertes Watchface mit Subdials für Batterie, Aktivitätsringe und Sekundenzeiger.
6. **Retro Casio:** Kultige digitale Retro-Optik mit Wochentag, Datum, Sekunden und Akkuanzeige.

### Modulares App-Management

Das neu integrierte `AppManager`-System modularisiert Apps in saubere Pakete (`AppPackage`):
- **Dynamische Installation & Deinstallation:** App-Pakete können als JSON-Deskriptor über das Dashboard hochgeladen und entfernt werden.
- **Kategorien:** Produktivität (`CAT_PRODUCTIVITY`), Security (`CAT_SECURITY`) und System (`CAT_SYSTEM`).
- **Launcher:** Paginierter App-Launcher mit Schnellzugriff und Icon-Rendering.
- **API-Export:** Vollständiger Export des App-Bestands via `/api/apps`.

### Produktivitäts-Apps (Good Apps)

- **2FA-Authenticator (TOTP):** Generiert Zeitschlüssel (RFC 6238) für bis zu 4 Accounts mit Restzeit-Balken.
- **Notification Center:** Eingang für Benachrichtigungen mit Unread-Badge in der Statusleiste.
- **Schrittzähler (Pedometer):** Hardware-nahe Schritterkennung via BMA423 mit 24h-Stundenstatistik im Web-Dashboard und konfigurierbarem Tagesziel.
- **Stoppuhr:** Präzise Zeitmessung mit Rundenzeiten (Laps).
- **Timer:** Schnellwahl-Presets (z. B. 3, 5, 10 min) und Alarmierung via Ton/Vibration.
- **Dezibel-Meter:** Akustische Schallpegelmessung über das PDM-Mikrofon (RMS → dB SPL).
- **Taschenrechner:** Kompakter 4-Grundrechenarten-Kalkulator mit Touch-Tastatur.
- **Mouse-Jiggler:** Hält Rechner über USB-HID wach (wählbare Bewegungsmuster).
- **Würfel:** Zufallsgenerator (d4, d6, d8, d10, d12, d20, d100 sowie Münzwurf).
- **Wasserwaage:** Neigungsmesser mit Blasen-Animation via BMA423-Sensor.

### Security- & Netzwerk-Tools (Bad Apps)

> *Ausschließlich für autorisierte Sicherheitsüberprüfungen.*

- **LoRa-Messenger:** Textnachrichten und Beaconing über das SX1262-Modul mit RSSI/SNR-Telemetrie.
- **BadUSB / Rubber Ducky:** Ducky-Script-Interpreter über nativen USB-HID mit Script-Auswahl.
- **WiFi Deauther:** 802.11 Deauth-Frame-Injektion gegen ausgewählte Ziel-BSSIDs.
- **WiFi Beacon Spammer:** Erzeugt hunderte frei definierbare oder zufällige Fake-Access-Points.
- **BLE Spammer:** Sendet Bluetooth LE Advertising-Pakete (z. B. iOS-Popup-Spam).
- **BLE Scanner:** Schneller Scan nach aktiven Bluetooth Low Energy Geräten mit Signalstärke.
- **Nmap Portscanner:** Erkennt offene TCP-Ports auf Ziel-IPs im verbundenen Subnetz.
- **Probe-Request Monitor:** Snifft 802.11 Probe Requests in der Umgebung (Promiscuous Mode).
- **Evil Portal:** Konfigurierbares Captive Portal mit Phishing-Templates und Zugangsdatenerfassung im FFat-Dateisystem.

### LoRa-Messenger & Funk

- **Frequenzband:** Standardmäßig 868,0 MHz (im Dashboard und in der App frei konfigurierbar).
- **Betriebsmodi:** Chat-Modus (Senden/Empfangen von Textnachrichten), Ping-Test und autonomer Beacon-Modus.
- **Status & Telemetrie:** Anzeige von Sende-/Empfangszählern, letztem RSSI (dBm) und SNR (dB).
- **Web-API:** Endpunkte zum Senden (`/api/lora/send`) und Abfragen (`/api/lora/status`).

### Modernes Web-Dashboard

Das Webinterface (`WebManager.cpp`) bietet ein responsives **Glassmorphism Dark Theme**:
- **System-Telemetrie in Echtzeit:**
  - CPU-Frequenz & Chip-Temperatur-Historie (bis zu 288 Messpunkte).
  - RAM- und PSRAM-Auslastung.
  - Adaptiver Akku-Lernalgorithmus: Ermittelt tatsächliche Entladeraten (%/h) für Normal- und Hacking-Betrieb sowie Laderaten beim Aufladen mit Restlaufzeit-Prognose.
  - Schrittzähler-Diagramm mit 24-Stunden-Verlauf und Zielerreichung.
- **Umfangreiche Konfiguration:**
  - Zifferblätter & AOD-Elemente (Position, Farbe, Größe, Einheiten).
  - Farbpalette (Hintergrund, Schrift, Akzente, Header, Batterie).
  - WLAN-Credentials, AP-Einstellungen, NTP-Server & Timezone.
  - 2FA TOTP-Konten und Secrets (Base32).
  - Mama & Papa Datensätze für den Elternvergleich.
  - Ducky-Skripte und Evil-Portal-HTML.
  - Wigle-kompatibler CSV-Export für Netzwerk-Scans.
- **Sicherheit & Komfort:**
  - Passwortschutz (HTTP-Basic-Auth) für Dashboard und OTA.
  - WLAN-Hotspot Auto-Off: Schaltet den Access Point nach 10 Minuten ohne verbundene Clients automatisch ab, um Akku zu sparen.
  - **Matrix-Easter-Egg:** Eingabe des Codes `6174` schaltet einen interaktiven Digital-Rain-Screensaver frei.

### Always-On-Display (AOD) & Power-Management

- **5-stufiges Power-Management:**
  1. *Aktiv* (volle Helligkeit, 240 MHz CPU)
  2. *Dimmen* (reduzierte Helligkeit)
  3. *AOD* (Display abgedunkelt mit konfigurierbaren Elementen, CPU auf 80 MHz gedrosselt)
  4. *Light-Sleep* (Display aus, Sensoren und Hintergrundtasks aktiv)
  5. *Deep-Sleep / Stealth* (maximaler Energiesparmodus)
- **Burn-In Schutz:** Symmetrischer Pixel-Shift (`random(-5, 6)`) bei jeder AOD-Aktualisierung.
- **Wrist-Raise-to-Wake:** Intelligentes Aufwecken bei Handgelenksdrehung über den BMA423-Sensor.

---

## Bedienung & Gesten

| Geste / Taste | Aktion |
|---|---|
| **Horizontal wischen (links/rechts)** | Zifferblatt wechseln (Seiten 0–3, 4, etc.) |
| **Vom Zifferblatt nach unten wischen** | Quick Settings / Control Center öffnen |
| **Vom Zifferblatt nach oben wischen** | App-Launcher öffnen |
| **In App nach oben wischen** | Aktive App sofort beenden (Not-Aus / Kill) |
| **In App nach links/rechts wischen** | Zurück zum Menü / Blättern |
| **Doppeltippen (bei AOD / Sleep)** | Uhr aufwecken |
| **Krone (Hardware-Taste) kurz drücken** | Ins AOD springen / Taschenlampe ausschalten |
| **Krone lange drücken** | Power-Off / Neustart |

---

## Bauen & Flashen

### 1. PlatformIO

Die Firmware ist vollständig für [PlatformIO](https://platformio.org/) vorkonfiguriert (`platformio.ini`):

```bash
# Kompilieren
pio run -e twatch-s3

# Per USB flashen
pio run -e twatch-s3 -t upload

# Serieller Monitor
pio device monitor -b 115200
```

### 2. Linux (arduino-cli & OTA-Skript)

Es stehen bequeme Shell-Skripte zur Verfügung:

```bash
# Firmware bauen (nutzt automatisch alle CPU-Kerne und Caching)
./build_firmware.sh

# Vollständiger Rebuild
./build_firmware.sh --clean

# Direkt über WLAN (OTA) auf die Uhr flashen:
# (Verbinde dich mit dem WLAN 'WatchOS' oder gib die IP an)
./flash_ota.sh 192.168.4.1
```

`flash_ota.sh` prüft automatisch, ob der Quellcode neuer als das Binary ist, baut bei Bedarf die Firmware und synchronisiert nach dem Neustart direkt die Zeit.

### 3. Windows (PowerShell)

Für Windows-Entwickler gibt es das PowerShell-Buildskript `build_firmware.ps1`:

```powershell
./build_firmware.ps1
```

### 4. Arduino IDE

- **Board:** `LilyGo T-Watch S3` (aus dem ESP32-Boardpaket)
- **Partitionsschema:** `app3M_fat9M_16MB` (3 MB App, 9 MB FAT-Dateisystem, 16 MB Flash)
- **Board-Optionen:**
  - `USB Mode: Hardware CDC and JTAG`
  - `CPU Frequency: 240MHz (WiFi)`
  - `PSRAM: OPI PSRAM`
  - `Flash Size: 16MB (128Mb)`
  - `Loop Core: Core 1`, `Events Core: Core 1`
  - `Revision: Radio_SX1262`

---

## Erste Inbetriebnahme

1. **Flashen:** Spiele die Firmware per USB (`./build_firmware.sh` / PlatformIO) auf die Watch.
2. **Boot:** Die Uhr startet mit der gewählten Retro-Animation (BIOS, C64, BSOD oder Quickstart).
3. **WLAN aktivieren:** Wische nach unten in die Quick Settings und tippe auf die WLAN-Kachel.
4. **Verbinden:**
   - Verbinde deinen PC/Smartphone mit dem Hotspot `WatchOS` (Standard-Passwort: `12345678`).
   - Öffne im Browser: **`http://watchos.local/`** oder **`http://192.168.4.1/`**.
5. **Konfiguration:**
   - Passe unter *Personal* Name, Gehalt, Kinder und Eltern (Mama & Papa) an.
   - Trage dein Heim-WLAN ein, damit sich die Uhr beim Start automatisch per NTP synchronisieren kann.
   - Richte unter *2FA* deine TOTP-Token ein.
   - Ändere die Standard-Passwörter für den Hotspot und aktiviere den Dashboard-Passwortschutz.

---

## Projektstruktur

```
WatchOS/
├── WatchOS.ino            # Setup, Loop, Core-Initialisierung & Dispatching
├── Globals.h              # Globale Konstanten, Page-Enums, Status-Variablen
├── HardwareConfig.h/.cpp  # AXP2101 PMU, BMA423, Audio-I2S, NTP-Sync & Sound-Synthese
├── Display.h/.cpp         # Rendering-Pipeline, Zifferblätter, Glassmorphism-UI, AOD
├── Logic.h/.cpp           # Touch-Steuerung, Gestenerkennung, Power-Management
├── AppManager.h/.cpp      # Modulares dynamisches App-Paketsystem & Lifecycle
├── TotpAuth.h/.cpp        # 2FA TOTP Authenticator (RFC 6238 Engine & NVS)
├── NotificationManager.*  # Benachrichtigungsverwaltung & Status-Badges
├── LoraManager.h/.cpp     # SX1262 LoRa Funkmanager (Chat, Beacon, Telemetrie)
├── SystemEvents.h/.cpp    # Systemweites Eventbus-System
├── GoodApps.h/.cpp        # Produktivitäts-Apps (Stoppuhr, Rechner, Wasserwaage etc.)
├── BadApps.h/.cpp         # Sicherheits-Werkzeuge (Deauther, Spammer, BadUSB etc.)
├── WebManager.h/.cpp      # Modernes Web-Dashboard, REST-API, Captive Portal
├── UpdateOTA.h/.cpp       # Authentifizierter OTA-Firmware-Update-Handler
├── platformio.ini         # PlatformIO-Projektkonfiguration
├── build_firmware.sh      # Linux Build-Skript mit Multi-Job-Unterstützung
├── build_firmware.ps1     # Windows PowerShell Build-Skript
├── flash_ota.sh           # Automatisches OTA-Flash- & Zeitsynchronisations-Skript
└── docs/                  # Detaillierte Dokumentation & Dashboard-Integrationsguides
```

---

## Konfiguration & Speicherung

- **NVS (`Preferences`, Namespace `clock`):** Speichert Zifferblatt-Auswahl, Farben, persönliche Daten, adaptive Akku-Lernraten, Schrittzähler-Stand, TOTP-Secrets und WLAN-Credentials.
- **FFat (`FFat` 9 MB Partition):** Speichert größere Dateien und HTML-Vorlagen für das Evil Portal, Ducky-Payloads für BadUSB und dynamische App-Paket-Dateien.

---

## Rechtliches & Ethik

Die in **Bad Apps** enthaltenen Funktionen dienen rein zu **Bildungs-, Forschungs- und Testzwecken** an Systemen, für die eine ausdrückliche Genehmigung des Eigentümers vorliegt.

Der unbefugte Betrieb von Deauthern, Signal-Spammern oder Captive Portals verstößt in vielen Ländern gegen Telekommunikations- und Strafgesetze (z. B. §§ 202a, 303b StGB in Deutschland). Die Entwickler übernehmen keine Haftung für Schäden oder missbräuchliche Verwendung.

---

## Version & Changelog

- **v1.3.0+ (Aktueller Stand):**
  - **AppManager:** Dynamisches Paket-System für modulare Apps und Dashboard-Paketverwaltung.
  - **2FA TOTP Authenticator:** Lokaler Token-Generator nach RFC 6238.
  - **LoRa Messenger:** SX1262 LoRa-Funkintegration für Textnachrichten und Beacons.
  - **Notification Center:** Systemweiter Benachrichtigungsdienst mit UI-Badge.
  - **Neue Zifferblätter:** Chronograph und Retro Casio Watchface.
  - **Modernes Dashboard:** Komplettes Redesign mit Glassmorphism-UI, Live-Telemetrie-Graphen und adaptivem Akkumodell.
  - **Plattform-Support:** PlatformIO-Unterstützung (`platformio.ini`) sowie optimierte Linux-Buildskripte (`build_firmware.sh`, `flash_ota.sh`).
  - **Pedometer & Wake:** 24h-Stundenanalyse für Schritte und Handgelenk-Drehgeste zum Aufwecken.
