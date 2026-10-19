#include "UpdateOTA.h"
#include "WebManager.h"
#include "Globals.h"
#include "Display.h"
#include "Logic.h"
#include "SensorDriver.h"
#include "NotificationManager.h"
#include <Update.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <mbedtls/sha256.h>
#include <esp_wifi.h>

CloudOtaState cloudOta;
unsigned long otaTotalSize = 0;

// Merkt sich, ob der laufende Upload authentifiziert ist. Ohne diese Sperre
// koennte jeder im Netz neue Firmware flashen, obwohl der Passwortschutz aktiv ist.
static bool otaAuthorized = false;

static bool otaAuthRequired() {
    return webAuthEnabled && webPass.length() > 0;
}

static int lastOtaPct = -1;

// Helper zum Parsen von einfachen JSON-Strings
static String extractJsonField(const String &json, const String &key) {
    String search = "\"" + key + "\"";
    int idx = json.indexOf(search);
    if (idx < 0) return "";
    int colon = json.indexOf(':', idx + search.length());
    if (colon < 0) return "";
    int p = colon + 1;
    while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) p++;
    if (p >= (int)json.length()) return "";
    
    if (json[p] == '"') {
        int endQ = json.indexOf('"', p + 1);
        if (endQ > p) return json.substring(p + 1, endQ);
        return "";
    } else {
        int endVal = p;
        while (endVal < (int)json.length() && json[endVal] != ',' && json[endVal] != '}' && json[endVal] != '\r' && json[endVal] != '\n') {
            endVal++;
        }
        String val = json.substring(p, endVal);
        val.trim();
        return val;
    }
}

// Prüft das GitHub Repository auf neue Releases via raw.githubusercontent.com/version.json
bool checkGitHubUpdate(bool force) {
    if (!wlanActive || !wlanClientMode || WiFi.status() != WL_CONNECTED) {
        cloudOta.lastStatus = "WLAN nicht verbunden";
        return false;
    }

    if (!force && cloudOta.checked && (millis() - cloudOta.lastCheckMs < 60000UL)) {
        return true; // Kürzlich bereits geprüft
    }

    cloudOta.checking = true;
    Serial.println(F("[OTA] Prüfe GitHub Releases auf Updates..."));

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(8000);

    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(8000);

    const char* verUrl = "https://raw.githubusercontent.com/diggaknoxville/WatchOS/main/version.json";
    if (!http.begin(client, verUrl)) {
        cloudOta.checking = false;
        cloudOta.lastStatus = "HTTP-Client Init fehlgeschlagen";
        return false;
    }

    int httpCode = http.GET();
    if (httpCode == 200) {
        String payload = http.getString();
        http.end();

        String v = extractJsonField(payload, "version");
        String c = extractJsonField(payload, "commit");
        String bDate = extractJsonField(payload, "build_date");
        String szStr = extractJsonField(payload, "bin_size_bytes");
        String sha = extractJsonField(payload, "sha256");

        cloudOta.latestVersion = (v.length() > 0) ? v : String(WATCHOS_BUILD_VERSION);
        cloudOta.latestCommit = (c.length() > 0) ? c : "";
        cloudOta.buildDate = bDate;
        cloudOta.binSize = szStr.toInt();
        cloudOta.sha256 = sha;
        cloudOta.downloadUrl = "https://github.com/diggaknoxville/WatchOS/releases/latest/download/firmware.bin";
        cloudOta.checked = true;
        cloudOta.lastCheckMs = millis();

        // Zeitstempel formatieren
        time_t now; time(&now);
        struct tm tmInfo; localtime_r(&now, &tmInfo);
        char tBuf[32];
        snprintf(tBuf, sizeof(tBuf), "%02d.%02d. %02d:%02d", tmInfo.tm_mday, tmInfo.tm_mon + 1, tmInfo.tm_hour, tmInfo.tm_min);
        cloudOta.lastCheckTime = String(tBuf);

        // Vergleich: Ist es ein neuer Stand?
        String localCommit = String(WATCHOS_BUILD_COMMIT);
        String localVer = String(WATCHOS_BUILD_VERSION);
        bool commitDiffers = (cloudOta.latestCommit.length() > 0 && localCommit.length() > 0 && cloudOta.latestCommit != localCommit && localCommit != "dev");
        bool verDiffers = (cloudOta.latestVersion.length() > 0 && cloudOta.latestVersion != localVer);

        cloudOta.updateAvailable = commitDiffers || verDiffers;

        if (cloudOta.updateAvailable) {
            cloudOta.lastStatus = "Neu: v" + cloudOta.latestVersion + " (" + cloudOta.latestCommit + ")";
            Serial.printf("[OTA] Update verfügbar! Remote: %s (%s) vs Lokal: %s (%s)\n",
                          cloudOta.latestVersion.c_str(), cloudOta.latestCommit.c_str(),
                          localVer.c_str(), localCommit.c_str());
            NotificationManager::add("WatchOS Update", ("Neu verfügbar: v" + cloudOta.latestVersion + " (" + cloudOta.latestCommit + ")").c_str());
        } else {
            cloudOta.lastStatus = "Auf neuestem Stand (v" + localVer + " - " + localCommit + ")";
            Serial.println(F("[OTA] Firmware ist aktuell!"));
        }

        cloudOta.checking = false;
        return true;
    } else {
        cloudOta.lastStatus = "HTTP Fehler: " + String(httpCode);
        Serial.printf("[OTA] Check fehlgeschlagen, Code: %d\n", httpCode);
        http.end();
        cloudOta.checking = false;
        return false;
    }
}

// Lädt die Firmware direkt via HTTPS auf der Uhr herunter und flasht sie autonom
bool performCloudOTA(String customUrl) {
    if (!wlanActive || !wlanClientMode || WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[OTA] Cloud-Update abgebrochen: Kein WLAN!"));
        return false;
    }

    Serial.println(F("[OTA] === Starte WatchOS Direct Cloud OTA ==="));
    powerState = 1;
    lastTouchTime = millis();
    if (ESP.getCpuFreqMHz() != 240) setCpuFrequencyMhz(240);
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Alle Hintergrundprozesse sauber stoppen
    saveMotionPrefs();
    killAllApps();
    if (evilActive) evilActive = false;
    matrixMode = false;
    freeDisplaySprites();

    // Display für Cloud-Update vorbereiten
    tft.writecommand(0x11);
    setBrightness(255);
    tft.fillScreen(TFT_BLACK);
    
    // Hightech Header & Rahmen
    tft.fillRect(0, 0, 240, 36, 0x0011);
    tft.drawFastHLine(0, 36, 240, TFT_CYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, 0x0011);
    tft.drawString("WATCHOS CLOUD UPDATE", 120, 18, 2);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Verbinde Server...", 120, 65, 2);

    // Fortschrittsbalken-Rahmen
    tft.drawRoundRect(15, 110, 210, 32, 6, 0x4A69);

    WiFiClientSecure secureClient;
    secureClient.setInsecure();
    secureClient.setTimeout(20000);
    WiFiClient plainClient;
    plainClient.setTimeout(20000);

    String targetUrl = (customUrl.length() > 0) ? customUrl : "https://github.com/diggaknoxville/WatchOS/releases/latest/download/firmware.bin";
    const char* headerKeys[] = {"Location", "Content-Length"};
    HTTPClient http;

    int hops = 0;
    int httpCode = 0;

    // Redirects (max. 4 Hops) sauber auflösen
    while (hops < 4) {
        hops++;
        if (targetUrl.startsWith("https://")) {
            http.begin(secureClient, targetUrl);
        } else {
            http.begin(plainClient, targetUrl);
        }
        http.collectHeaders(headerKeys, 2);
        http.setTimeout(20000);
        httpCode = http.GET();

        if (httpCode == 301 || httpCode == 302 || httpCode == 307 || httpCode == 308) {
            String newLoc = http.header("Location");
            http.end();
            if (newLoc.length() > 0) {
                if (newLoc.startsWith("/")) {
                    int idx = targetUrl.indexOf("://");
                    if (idx > 0) {
                        int slashIdx = targetUrl.indexOf('/', idx + 3);
                        newLoc = (slashIdx > 0 ? targetUrl.substring(0, slashIdx) : targetUrl) + newLoc;
                    }
                }
                targetUrl = newLoc;
                Serial.printf("[OTA] Redirect #%d zu: %s\n", hops, targetUrl.c_str());
                continue;
            } else {
                break;
            }
        }
        break;
    }

    if (httpCode != 200) {
        Serial.printf("[OTA] Download fehlgeschlagen: HTTP %d\n", httpCode);
        http.end();
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString("Download Fehler!", 120, 65, 2);
        char errBuf[32]; snprintf(errBuf, sizeof(errBuf), "HTTP Code: %d", httpCode);
        tft.drawString(errBuf, 120, 160, 2);
        delay(4000);
        return false;
    }

    int contentLength = http.getSize();
    Serial.printf("[OTA] Content-Length: %d Bytes\n", contentLength);

    if (contentLength <= 0 && cloudOta.binSize > 0) {
        contentLength = cloudOta.binSize;
    }

    tft.fillRect(0, 50, 240, 40, TFT_BLACK);
    tft.setTextColor(0x07FF, TFT_BLACK);
    tft.drawString("Flashe Firmware...", 120, 65, 2);

    if (Update.isRunning()) Update.abort();
    if (!Update.begin((contentLength > 0) ? contentLength : UPDATE_SIZE_UNKNOWN)) {
        Serial.println(F("[OTA] Update.begin fehlgeschlagen!"));
        http.end();
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString("Flash-Init Fehler!", 120, 160, 2);
        delay(4000);
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buff[4096];
    size_t written = 0;
    int lastPct = -1;
    unsigned long startTime = millis();

    // SHA-256 On-the-Fly Berechnung
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    while (http.connected() && (written < (size_t)contentLength || contentLength <= 0)) {
        size_t availableBytes = stream->available();
        if (availableBytes > 0) {
            int toRead = min(availableBytes, sizeof(buff));
            int bytesRead = stream->readBytes(buff, toRead);
            if (bytesRead > 0) {
                if (Update.write(buff, bytesRead) != (size_t)bytesRead) {
                    Serial.println(F("[OTA] Update.write Fehler!"));
                    Update.abort();
                    mbedtls_sha256_free(&sha_ctx);
                    http.end();
                    tft.setTextColor(TFT_RED, TFT_BLACK);
                    tft.drawString("Flash Schreibfehler!", 120, 160, 2);
                    delay(4000);
                    return false;
                }
                mbedtls_sha256_update(&sha_ctx, buff, bytesRead);
                written += bytesRead;

                int pct = (contentLength > 0) ? ((written * 100) / contentLength) : 0;
                if (pct != lastPct) {
                    lastPct = pct;
                    int barW = (204 * pct) / 100;
                    tft.fillRoundRect(18, 113, barW, 26, 4, TFT_GREEN);

                    tft.fillRect(0, 155, 240, 50, TFT_BLACK);
                    tft.setTextColor(TFT_WHITE, TFT_BLACK);
                    char statLine[48];
                    snprintf(statLine, sizeof(statLine), "%d%%  (%.1f / %.1f MB)", pct, written / 1048576.0f, contentLength / 1048576.0f);
                    tft.drawString(statLine, 120, 170, 2);
                }
            }
        } else {
            delay(5);
        }
        yield();
    }

    http.end();

    // Checksum auswerten
    uint8_t sha_out[32];
    mbedtls_sha256_finish(&sha_ctx, sha_out);
    mbedtls_sha256_free(&sha_ctx);

    char shaHex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(shaHex + (i * 2), "%02x", sha_out[i]);
    }
    shaHex[64] = 0;

    Serial.printf("[OTA] Download beendet. Geschrieben: %u Bytes. Berechneter SHA-256: %s\n", written, shaHex);

    if (cloudOta.sha256.length() == 64 && cloudOta.sha256.equalsIgnoreCase(String(shaHex)) == false) {
        Serial.println(F("[OTA] WARNUNG: SHA-256 Checksumme stimmt nicht mit version.json überein!"));
        // Bei Mismatch zur Sicherheit abbrechen
        Update.abort();
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString("Checksum Mismatch!", 120, 160, 2);
        delay(4000);
        return false;
    }

    if (Update.end(true)) {
        Serial.println(F("[OTA] Update erfolgreich geflasht!"));
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.drawString("UPDATE ERFOLGREICH!", 120, 90, 4);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString("Neustart in 2 Sek...", 120, 140, 2);
        delay(2000);
        ESP.restart();
        return true;
    } else {
        Serial.println(F("[OTA] Update.end() meldete Fehler!"));
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString("Update Fehlgeschlagen!", 120, 160, 4);
        delay(4000);
        return false;
    }
}

// Wird zyklisch in Logic.cpp aufgerufen, wenn Heim-WLAN verbunden ist
void checkOtaAutoOnWifi() {
    if (!otaAutoCheck) return;
    if (!wlanActive || !wlanClientMode || WiFi.status() != WL_CONNECTED) return;

    static unsigned long lastAutoCheck = 0;
    unsigned long now = millis();

    // Alle 6 Stunden oder beim allerersten Mal
    if (lastAutoCheck == 0 || (now - lastAutoCheck > 21600000UL)) {
        lastAutoCheck = now;
        checkGitHubUpdate(false);
    }
}

void setupOTARoutes() {
    // --- 1. STATUS & METADATA ---
    server.on("/api/ota/status", HTTP_GET, []() {
        String s = "{";
        s += "\"local_version\":\"" + String(WATCHOS_BUILD_VERSION) + "\",";
        s += "\"local_commit\":\"" + String(WATCHOS_BUILD_COMMIT) + "\",";
        s += "\"build_date\":\"" + String(WATCHOS_BUILD_DATE) + "\",";
        s += "\"wifi_connected\":" + String((WiFi.status() == WL_CONNECTED) ? "true" : "false") + ",";
        s += "\"remote_version\":\"" + cloudOta.latestVersion + "\",";
        s += "\"remote_commit\":\"" + cloudOta.latestCommit + "\",";
        s += "\"remote_build_date\":\"" + cloudOta.buildDate + "\",";
        s += "\"remote_bin_size\":" + String(cloudOta.binSize) + ",";
        s += "\"remote_sha256\":\"" + cloudOta.sha256 + "\",";
        s += "\"update_available\":" + String(cloudOta.updateAvailable ? "true" : "false") + ",";
        s += "\"last_status\":\"" + jsonEscape(cloudOta.lastStatus) + "\",";
        s += "\"last_check_time\":\"" + jsonEscape(cloudOta.lastCheckTime) + "\",";
        s += "\"auto_check\":" + String(otaAutoCheck ? "true" : "false");
        s += "}";
        server.send(200, "application/json", s);
    });

    // --- 2. MANUELLER CHECK ---
    server.on("/api/ota/check", HTTP_POST, []() {
        bool ok = checkGitHubUpdate(true);
        String s = "{";
        s += "\"success\":" + String(ok ? "true" : "false") + ",";
        s += "\"update_available\":" + String(cloudOta.updateAvailable ? "true" : "false") + ",";
        s += "\"remote_version\":\"" + cloudOta.latestVersion + "\",";
        s += "\"remote_commit\":\"" + cloudOta.latestCommit + "\",";
        s += "\"last_status\":\"" + jsonEscape(cloudOta.lastStatus) + "\"";
        s += "}";
        server.send(200, "application/json", s);
    });

    // --- 3. DIRECT CLOUD OTA TRIGGER ---
    server.on("/api/ota/cloud_update", HTTP_POST, []() {
        if (!otaAuthorized && otaAuthRequired() && !server.authenticate(webUser.c_str(), webPass.c_str())) {
            server.send(401, "text/plain", "Unauthorized");
            return;
        }
        String customUrl = server.hasArg("url") ? server.arg("url") : "";
        server.send(200, "application/json", "{\"status\":\"started\"}");
        delay(200);
        performCloudOTA(customUrl);
    });

    // --- 4. AUTO-CHECK EINSTELLUNG SPEICHERN ---
    server.on("/api/ota/settings", HTTP_POST, []() {
        if (server.hasArg("auto")) {
            otaAutoCheck = (server.arg("auto") == "1" || server.arg("auto") == "true");
            prefs.putBool("otaAuto", otaAutoCheck);
        }
        server.send(200, "application/json", "{\"status\":\"saved\"}");
    });

    // --- 5. BROWSER UPLOAD OTA (/update) ---
    server.on("/update", HTTP_POST, []() {
        if (!otaAuthorized) {
            server.send(evilActive ? 404 : 401, "text/plain", evilActive ? "Not Found" : "Unauthorized");
            return;
        }
        server.sendHeader("Connection", "close");
        server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
        delay(500);
        ESP.restart();
    }, []() {
        HTTPUpload& upload = server.upload();

        if (upload.status == UPLOAD_FILE_START) {
            otaAuthorized = !evilActive && (!otaAuthRequired() || server.authenticate(webUser.c_str(), webPass.c_str()));
            if (!otaAuthorized) {
                Serial.println("[OTA] Abgelehnt: Authentifizierung fehlgeschlagen oder Evil Portal aktiv");
                return;
            }
            powerState = 1;
            lastTouchTime = millis();
            if (ESP.getCpuFreqMHz() != 240) setCpuFrequencyMhz(240);
            esp_wifi_set_ps(WIFI_PS_NONE);
            lastOtaPct = -1;

            if (server.hasArg("size")) {
                otaTotalSize = server.arg("size").toInt();
            } else {
                otaTotalSize = 2200000;
            }
            
            saveMotionPrefs();
            killAllApps();
            if (evilActive) { evilActive = false; }
            matrixMode = false;
            
            tft.writecommand(0x11);
            setBrightness(255);
            tft.fillScreen(TFT_BLACK);
            tft.drawRoundRect(20, 100, 200, 40, 5, TFT_WHITE);
            tft.setTextDatum(MC_DATUM);
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            tft.drawString("System Update...", 120, 70, 4);

            if (Update.isRunning()) {
                Update.abort();
            }
            freeDisplaySprites();
            size_t updateSize = (otaTotalSize > 0) ? otaTotalSize : UPDATE_SIZE_UNKNOWN;
            if (!Update.begin(updateSize)) {
                Update.printError(Serial);
            }
        } 
        else if (upload.status == UPLOAD_FILE_WRITE) {
            if (!otaAuthorized) return;
            lastTouchTime = millis();
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
            
            int pct = (upload.totalSize * 100) / max(1UL, otaTotalSize);
            if (pct > 100) pct = 100;
            if (pct != lastOtaPct) {
                lastOtaPct = pct;
                tft.fillRoundRect(22, 102, (196 * pct) / 100, 36, 3, TFT_GREEN);
            }
            yield();
        } 
        else if (upload.status == UPLOAD_FILE_END) {
            if (!otaAuthorized) return;
            if (Update.end(true)) {
                tft.setTextColor(TFT_GREEN, TFT_BLACK);
                tft.drawString("Installation OK!", 120, 160, 4);
            } else {
                tft.setTextColor(TFT_RED, TFT_BLACK);
                tft.drawString("FEHLER!", 120, 160, 4);
            }
        }
        else if (upload.status == UPLOAD_FILE_ABORTED) {
            Update.abort();
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("Abbruch - Retry...", 120, 160, 2);
            lastSecond = -1;
        }
    });
}