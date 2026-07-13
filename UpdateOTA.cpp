// --- UpdateOTA.cpp ---
#include "UpdateOTA.h"
#include "WebManager.h"
#include "Globals.h"
#include "Display.h"
#include "Logic.h"
#include <Update.h>

unsigned long otaTotalSize = 0;

// Merkt sich, ob der laufende Upload authentifiziert ist. Ohne diese Sperre
// koennte jeder im Netz neue Firmware flashen, obwohl der Passwortschutz aktiv ist.
static bool otaAuthorized = false;

static bool otaAuthRequired() {
    return webAuthEnabled && webPass.length() > 0;
}

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
            // Wir holen uns die echte Dateigröße vom JavaScript, um die Prozente zu berechnen
            if (server.hasArg("size")) {
                otaTotalSize = server.arg("size").toInt();
            } else {
                otaTotalSize = 1500000; // Fallback (ca. 1.5MB)
            }
            
            // Alle Apps sauber stoppen — RAM & CPU freigeben für OTA
            killAllApps();
            if (evilActive) { evilActive = false; }
            matrixMode = false;
            
            // Uhr-Display übernehmen und Update-Screen vorbereiten
            setBrightness(255);
            tft.fillScreen(TFT_BLACK);
            tft.drawRoundRect(20, 100, 200, 40, 5, TFT_WHITE); // Der leere Rahmen
            tft.setTextDatum(MC_DATUM);
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            tft.drawString("System Update...", 120, 70, 4);

            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
            }
        } 
        // PHASE 2: WÄHREND DES SCHREIBENS (CHUNKS)
        else if (upload.status == UPLOAD_FILE_WRITE) {
            if (!otaAuthorized) return; // Nicht autorisiert -> Chunks verwerfen
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
            
            // Fortschrittsbalken auf der Uhr live zeichnen
            int pct = (upload.totalSize * 100) / otaTotalSize;
            if(pct > 100) pct = 100; // Absicherung
            
            // Füllt den Rahmen (Breite 196px bei 100%) in TFT_GREEN aus
            tft.fillRoundRect(22, 102, (196 * pct) / 100, 36, 3, TFT_GREEN);
            
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
    });
}