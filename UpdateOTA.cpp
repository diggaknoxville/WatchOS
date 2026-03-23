// --- UpdateOTA.cpp ---
#include "UpdateOTA.h"
#include "WebManager.h"
#include "Globals.h"
#include "Display.h"
#include <Update.h>

unsigned long otaTotalSize = 0;

void setupOTARoutes() {
    server.on("/update", HTTP_POST, []() {
        server.sendHeader("Connection", "close");
        server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
        delay(500);
        ESP.restart(); // Nach erfolgreichem Update neustarten
    }, []() {
        HTTPUpload& upload = server.upload();
        
        // PHASE 1: START DES UPDATES
        if (upload.status == UPLOAD_FILE_START) {
            // Wir holen uns die echte Dateigröße vom JavaScript, um die Prozente zu berechnen
            if (server.hasArg("size")) {
                otaTotalSize = server.arg("size").toInt();
            } else {
                otaTotalSize = 1500000; // Fallback (ca. 1.5MB)
            }
            
            // Alles abbrechen, was stören könnte
            matrixMode = false;
            jigglerActive = false;
            evilActive = false;
            
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