// --- UpdateOTA.cpp ---
#include "UpdateOTA.h"
#include "WebManager.h"
#include "Globals.h"
#include "Display.h"
#include "Logic.h"
#include "GoodApps.h"
#include <Update.h>

unsigned long otaTotalSize = 0;

// Merkt sich, ob der laufende Upload authentifiziert ist. Ohne diese Sperre
// koennte jeder im Netz neue Firmware flashen, obwohl der Passwortschutz aktiv ist.
static bool otaAuthorized = false;

static bool otaAuthRequired() {
    return webAuthEnabled && webPass.length() > 0;
}

#include <esp_wifi.h>

static int lastOtaPct = -1;

void setupOTARoutes() {
    server.on("/update", HTTP_POST, []() {
        // Abschluss-Handler: unautorisierte Uploads hier ablehnen (Flash wurde nie geschrieben).
        if (otaAuthRequired() && !otaAuthorized) {
            server.send(401, "text/plain", "Unauthorized");
            return;
        }
        server.sendHeader("Connection", "close");
        server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
        delay(500);
        ESP.restart(); // Nach erfolgreichem Update neustarten
    }, []() {
        HTTPUpload& upload = server.upload();

        // PHASE 1: START DES UPDATES
        if (upload.status == UPLOAD_FILE_START) {
            // Auth pruefen BEVOR irgendetwas in den Flash geschrieben wird.
            otaAuthorized = !otaAuthRequired() || server.authenticate(webUser.c_str(), webPass.c_str());
            if (!otaAuthorized) {
                Serial.println("[OTA] Abgelehnt: Authentifizierung fehlgeschlagen");
                return; // Kein Update.begin() -> es wird nichts geflasht
            }
            // Volle CPU-Leistung & kein WiFi-Modem-Sleep waehrend OTA fuer maximale Verbindungsstabilitaet
            powerState = 1;
            lastTouchTime = millis();
            if (ESP.getCpuFreqMHz() != 240) setCpuFrequencyMhz(240);
            esp_wifi_set_ps(WIFI_PS_NONE);
            lastOtaPct = -1;

            // Wir holen uns die echte Dateigröße vom JavaScript, um die Prozente zu berechnen
            if (server.hasArg("size")) {
                otaTotalSize = server.arg("size").toInt();
            } else {
                otaTotalSize = 2100000; // Fallback (ca. 2.1MB)
            }
            
            // Alle Apps sauber stoppen — RAM & CPU freigeben für OTA
            saveMotionPrefs();
            killAllApps();
            if (evilActive) { evilActive = false; }
            matrixMode = false;
            
            // Uhr-Display übernehmen und Update-Screen vorbereiten
            tft.writecommand(0x11);
            setBrightness(255);
            tft.fillScreen(TFT_BLACK);
            tft.drawRoundRect(20, 100, 200, 40, 5, TFT_WHITE); // Der leere Rahmen
            tft.setTextDatum(MC_DATUM);
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            tft.drawString("System Update...", 120, 70, 4);

            if (Update.isRunning()) {
                Update.abort();
            }
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
            }
        } 
        // PHASE 2: WÄHREND DES SCHREIBENS (CHUNKS)
        else if (upload.status == UPLOAD_FILE_WRITE) {
            if (!otaAuthorized) return; // Nicht autorisiert -> Chunks verwerfen
            lastTouchTime = millis();
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
            
            // Fortschrittsbalken auf der Uhr nur bei neuem Prozentwert zeichnen (entlastet SPI & TCP massiv)
            int pct = (upload.totalSize * 100) / max(1UL, otaTotalSize);
            if (pct > 100) pct = 100;
            if (pct != lastOtaPct) {
                lastOtaPct = pct;
                tft.fillRoundRect(22, 102, (196 * pct) / 100, 36, 3, TFT_GREEN);
            }
            
            yield(); // Verhindert, dass der Watchdog die Uhr währenddessen killt
        } 
        // PHASE 3: ENDE
        else if (upload.status == UPLOAD_FILE_END) {
            if (!otaAuthorized) return; // Nicht autorisiert -> kein Abschluss
            if (Update.end(true)) {
                tft.setTextColor(TFT_GREEN, TFT_BLACK);
                tft.drawString("Installation OK!", 120, 160, 4);
            } else {
                tft.setTextColor(TFT_RED, TFT_BLACK);
                tft.drawString("FEHLER!", 120, 160, 4);
            }
        }
        // PHASE 4: ABBRUCH (Wichtig: Update.abort() aufrufen, damit naechster Versuch sofort klappt!)
        else if (upload.status == UPLOAD_FILE_ABORTED) {
            Update.abort();
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("Abbruch - Retry...", 120, 160, 2);
            lastSecond = -1;
        }
    });
}