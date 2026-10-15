// --- WebManager.cpp ---
#include <FS.h>         
#include <WebServer.h>  
#include <WiFi.h>
#include <esp_wifi.h> 
#include <DNSServer.h>
#include <ESPmDNS.h> 

#include "WebManager.h"
#include "Globals.h"
#include "Display.h"
#include "Logic.h"
#include "HardwareConfig.h" 
#include "UpdateOTA.h" 
#include "BadApps.h"
#include "TotpAuth.h"
#include "NotificationManager.h"
#include "SystemEvents.h"
#include "LoraManager.h"
#include "AppManager.h"
#include <FFat.h>
#include "nvs_flash.h"
#include "esp_system.h"
#include <esp_mac.h>
#include <BLEDevice.h>

const byte DNS_PORT = 53;

// Globale Variablen für den Client-Modus
bool wlanClientMode = false;
String wlanIP = "";

static String getHardwareMacAddress() {
    uint8_t baseMac[6];
    esp_read_mac(baseMac, ESP_MAC_WIFI_STA);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);
    return String(macStr);
}

static bool brandIconUploadOK = false;
static size_t brandIconUploadBytes = 0;

// Verbindungshistorie fuers Web-Dashboard: rein im RAM (kein NVS-Verschleiss durch minuetliche
// Schreibzugriffe), lebt daher nur bis zum naechsten Neustart - fuer den Zweck (Ueberblick waehrend
// laufendem Betrieb) ausreichend und bewusst so gewaehlt.
#define CONN_HIST_MAX 8
struct ConnHistEntry { String ssid; time_t startTime; time_t endTime; long rssiSum; int rssiSamples; bool ongoing; };
static ConnHistEntry connHist[CONN_HIST_MAX];
static int connHistCount = 0;
static String connHistLastKey = "";

static void updateConnectionHistory(const String &ssid, int rssi, bool wifiUp) {
    String key = wifiUp ? ssid : "";
    if (key != connHistLastKey) {
        if (connHistCount > 0 && connHist[connHistCount - 1].ongoing) {
            connHist[connHistCount - 1].ongoing = false;
            time(&connHist[connHistCount - 1].endTime);
        }
        connHistLastKey = key;
        if (key.length() > 0) {
            if (connHistCount >= CONN_HIST_MAX) {
                for (int i = 1; i < CONN_HIST_MAX; i++) connHist[i - 1] = connHist[i];
                connHistCount = CONN_HIST_MAX - 1;
            }
            ConnHistEntry e; e.ssid = ssid; time(&e.startTime); e.endTime = 0; e.rssiSum = 0; e.rssiSamples = 0; e.ongoing = true;
            connHist[connHistCount++] = e;
        }
    } else if (wifiUp && connHistCount > 0 && connHist[connHistCount - 1].ongoing && rssi < 0) {
        connHist[connHistCount - 1].rssiSum += rssi;
        connHist[connHistCount - 1].rssiSamples++;
    }
}

const char* tr(int lang, const char* de, const char* en, const char* fr) {
    if(lang == 1) return en;
    if(lang == 2) return fr;
    return de;
}

// Estimate minutes to empty/full from recent battery history samples.
// Returns -1 when there is not enough directional trend data.
static int estimateBatteryEtaMinutes(bool chargingNow, int currentPct) {
    if (historyCount < 2 || historyInterval <= 0) return -1;

    int start = max(0, historyCount - 24); // last up to ~24 samples
    float dirSum = 0.0f;
    int dirCnt = 0;

    auto decodePct = [](int raw) {
        int pct = raw % 100;
        if (raw >= 200 && pct == 0 && raw != 200) pct = 100;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        return pct;
    };

    int prev = decodePct(batHistoryPct[start]);
    for (int i = start + 1; i < historyCount; i++) {
        int cur = decodePct(batHistoryPct[i]);
        int d = cur - prev;
        prev = cur;

        // Ignore implausible jumps from sampling glitches.
        if (abs(d) > 20) continue;

        if (chargingNow) {
            if (d > 0) { dirSum += (float)d; dirCnt++; }
        } else {
            if (d < 0) { dirSum += (float)(-d); dirCnt++; }
        }
    }

    if (dirCnt == 0) return -1;

    float avgDeltaPerSample = dirSum / (float)dirCnt;
    float minsPerSample = (float)historyInterval / 60000.0f;
    if (avgDeltaPerSample <= 0.0001f || minsPerSample <= 0.0f) return -1;

    float pctPerMinute = avgDeltaPerSample / minsPerSample;
    if (pctPerMinute <= 0.0001f) return -1;

    float remainingPct = chargingNow ? (100.0f - (float)currentPct) : (float)currentPct;
    if (remainingPct <= 0.0f) return 0;

    int eta = (int)(remainingPct / pctPerMinute);
    if (eta < 0) eta = -1;
    if (eta > 10080) eta = 10080; // cap at 7 days
    return eta;
}

struct BattEtaResult { int etaMin; int confidencePct; float ratePctPerHour; bool learned; };

// Bevorzugt die gelernte %/h-Rate des aktuell aktiven Modus (Normal/Hacking/Laden), sobald
// genug Erfahrungswerte gesammelt wurden - sonst faellt sie auf die fensterbasierte Schaetzung zurueck.
static BattEtaResult computeBatteryEta(bool chargingNow, bool hackingNow, int currentPct) {
    BattEtaResult r; r.etaMin = -1; r.confidencePct = 0; r.ratePctPerHour = 0; r.learned = false;
    int mode = chargingNow ? 2 : (hackingNow ? 1 : 0);
    uint16_t samples = mode == 2 ? battSamplesCharging : (mode == 1 ? battSamplesHacking : battSamplesNormal);
    float rate = mode == 2 ? battRateCharging : (mode == 1 ? battRateHacking : battRateNormal);
    r.ratePctPerHour = rate;

    if (samples >= 3 && rate > 0.01f) {
        float remaining = chargingNow ? (100.0f - (float)currentPct) : (float)currentPct;
        r.etaMin = remaining <= 0.0f ? 0 : (int)(remaining / rate * 60.0f);
        if (r.etaMin > 10080) r.etaMin = 10080;
        r.confidencePct = battLearnConfidencePct(samples);
        r.learned = true;
        return r;
    }
    int fallbackEta = estimateBatteryEtaMinutes(chargingNow, currentPct);
    r.etaMin = fallbackEta;
    r.confidencePct = fallbackEta >= 0 ? 12 : 0; // noch kein Erfahrungswert - grobe Momentanschaetzung
    return r;
}

void handleApiStatus() {
    time_t jetzt; time(&jetzt); struct tm *aktuell = localtime(&jetzt);
    float dailyNet = (netSalary * 12.0) / 365.0; float fraction = (millis() % 1000) / 1000.0;
    long secToday = aktuell->tm_hour * 3600 + aktuell->tm_min * 60 + aktuell->tm_sec;
    float earnedToday = dailyNet * ((secToday + fraction) / 86400.0);
    // "Heute 7,6": derselbe Tagesnettobetrag, aber auf einen Arbeitstag statt den vollen Kalendertag bezogen -
    // erreicht den vollen Tagessatz daher schon nach workHoursPerDay Stunden statt erst nach 24h.
    float workDaySec = (workHoursPerDay > 0.1f ? workHoursPerDay : 7.6f) * 3600.0f;
    float earnedTodayWork = dailyNet * min(1.0f, (float)(secToday + fraction) / workDaySec);
    float earnedMonth = dailyNet * ((float)(aktuell->tm_mday - 1) + (secToday + fraction) / 86400.0f);
    float earnedYear = dailyNet * ((float)aktuell->tm_yday + (secToday + fraction) / 86400.0f);

    static unsigned long lastSensorSampleMs = 0;
    static float cachedCpuTemp = 0.0f;
    static int cachedRssi = -127;
    unsigned long nowMs = millis();
    if (lastSensorSampleMs == 0 || (nowMs - lastSensorSampleMs) > 1500) {
        lastSensorSampleMs = nowMs;
        cachedCpuTemp = temperatureRead();
        cachedRssi = WiFi.RSSI();
    }
    
    refreshPowerTelemetry(false);
    bool isCharging = isChargingCached(false);
    String currentSSID = ""; String currentIP = "";
    if (wlanActive) { if (wlanClientMode) { currentSSID = userSSID; currentIP = wlanIP; } else { currentSSID = apSSID; currentIP = "192.168.4.1"; } } 
    else if (nmapActive) { currentSSID = targetSSIDs[currentTargetNmap]; currentIP = WiFi.localIP().toString(); } 
    else if (evilActive) { currentSSID = evilSSID; currentIP = "192.168.4.1"; } 
    else if (deauthActive || wifiSpamActive) { currentSSID = "Hacking..."; currentIP = "RAW TX"; }
    
    // RSSI zu Prozent Umrechnung (-100dBm = 0%, -50dBm = 100%)
    int rssi = cachedRssi;
    int rssi_pct = 0;
    if (rssi <= -100) rssi_pct = 0;
    else if (rssi >= -50) rssi_pct = 100;
    else rssi_pct = 2 * (rssi + 100);

    updateConnectionHistory(currentSSID, rssi, wlanClientMode && wlanActive && currentSSID.length() > 0);

    int battPct = getBatteryCached(false);
    bool hacksRunningNow = (jigglerActive || badUsbActive || wifiSpamActive || bleSpamActive || deauthActive || nmapActive || evilActive || wlanActive || probeMonActive || bleScanActive);
    BattEtaResult eta = computeBatteryEta(isCharging, hacksRunningNow, battPct);
    const char* etaMode = "unknown";
    if (eta.etaMin >= 0) etaMode = isCharging ? "charge" : "discharge";

    static unsigned long lastStorageSampleMs = 0;
    static int cachedFlashFreeKB = 0, cachedFlashTotKB = 0, cachedNvsFree = 0, cachedNvsTot = 0;
    if (lastStorageSampleMs == 0 || (nowMs - lastStorageSampleMs) > 5000) {
        lastStorageSampleMs = nowMs;
        cachedFlashTotKB = littleFsReady ? (int)(FFat.totalBytes() / 1024) : 0;
        cachedFlashFreeKB = littleFsReady ? (int)((FFat.totalBytes() - FFat.usedBytes()) / 1024) : 0;
        nvs_stats_t nvsStats;
        if (nvs_get_stats(NULL, &nvsStats) == ESP_OK) { cachedNvsFree = nvsStats.free_entries; cachedNvsTot = nvsStats.total_entries; }
    }

    bool isApMode = !wlanClientMode;
    int apClients = (isApMode && wlanActive) ? WiFi.softAPgetStationNum() : 0;
    int apChan = WiFi.channel();
    String macAddr = getHardwareMacAddress();

    char json[1800];
    snprintf(json, sizeof(json), "{\"akku\":%d,\"volt\":\"%.2f\",\"cpu\":\"%.1f\",\"ram_free\":%d,\"ram_tot\":%d,\"geld\":\"%.4f\",\"geld_work\":\"%.4f\",\"geld_month\":\"%.4f\",\"geld_year\":\"%.4f\",\"wifi\":%s,\"wifi_mode\":\"%s\",\"clients\":%d,\"channel\":%d,\"mac\":\"%s\",\"charging\":%s,\"bt\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"cpufreq\":%d,\"uptime\":%lu,\"rssi\":%d,\"rssi_pct\":%d,\"eta_min\":%d,\"eta_mode\":\"%s\",\"eta_conf\":%d,\"steps\":%ld,\"step_goal\":%d,\"flash_free_kb\":%d,\"flash_tot_kb\":%d,\"nvs_free\":%d,\"nvs_tot\":%d,\"wf_mode\":%d,\"phone_mac\":\"%s\",\"phone_name\":\"%s\",\"phone_connected\":%s}",
             battPct, getBatteryVoltageCached(false), cachedCpuTemp,
             (int)(ESP.getFreeHeap() / 1024), (int)(ESP.getHeapSize() / 1024), earnedToday, earnedTodayWork, earnedMonth, earnedYear,
             (WiFi.status() == WL_CONNECTED || wlanActive || evilActive || wifiSpamActive || deauthActive || nmapActive) ? "true" : "false",
             isApMode ? "ap" : "sta", apClients, apChan, macAddr.c_str(),
             isCharging ? "true" : "false", btActive ? "true" : "false", jsonEscape(currentSSID).c_str(), jsonEscape(currentIP).c_str(),
             ESP.getCpuFreqMHz(), millis() / 1000, rssi, rssi_pct, eta.etaMin, etaMode, eta.confidencePct, stepCount, stepGoal,
             cachedFlashFreeKB, cachedFlashTotKB, cachedNvsFree, cachedNvsTot,
             watchfaceMode, jsonEscape(phoneMac).c_str(), jsonEscape(phoneName).c_str(), phoneConnected ? "true" : "false");
    server.send(200, "application/json", json);
}

String getBatteryHtml() {
    time_t jetzt; time(&jetzt); struct tm *aktuell = localtime(&jetzt); int curH = aktuell->tm_hour; int curM = aktuell->tm_min; String s; s.reserve(38000);

    const char* tTitle = tr(sysLang, "Akku Verlauf & Power Status", "Battery History & Power Status", "Historique Batterie & Etat Energie");
    const char* tBack = tr(sysLang, "Zurueck zum Dashboard", "Back to Dashboard", "Retour au Dashboard");
    const char* tFilterRange = tr(sysLang, "Zeitraum", "Time Range", "Periode");
    const char* tFilterMode = tr(sysLang, "Modus", "Mode", "Mode");
    const char* tAll = tr(sysLang, "Alles", "All", "Tout");
    const char* tLast6h = tr(sysLang, "Letzte 6h", "Last 6h", "Dernieres 6h");
    const char* tLast24h = tr(sysLang, "Letzte 24h", "Last 24h", "Dernieres 24h");
    const char* tLast72h = tr(sysLang, "Letzte 72h", "Last 72h", "Dernieres 72h");
    const char* tNormal = tr(sysLang, "Normal", "Normal", "Normal");
    const char* tHack = tr(sysLang, "Hacking", "Hacking", "Hacking");
    const char* tCharge = tr(sysLang, "Laden", "Charging", "Charge");
    const char* tResetHist = tr(sysLang, "Verlauf loeschen", "Reset history", "Reinitialiser l'historique");
    const char* tResetConfirm = tr(sysLang, "Akku-Verlauf wirklich loeschen?", "Really reset battery history?", "Reinitialiser l'historique batterie ?");

    s += F(R"rawliteral(<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>
    <style>
:root {
  --bg-primary: #0e1116;
  --bg-secondary: #161b22;
  --bg-tertiary: #1b2129;
  --surface-primary: #0e1116;
  --surface-secondary: #161b22;
  --border-subtle: #262d38;
  --border-default: #333c49;
  --text-primary: #e7eaf0;
  --text-secondary: #8b95a4;
  --text-muted: #69737f;
  --accent: #ffb020;
  --accent-2: #3dd0c4;
  --accent-3: #38d66b;
  --accent-danger: #ff5c5c;
  --accent-warning: #ffb020;
  --accent-info: #4aa8ff;
  --battery-full: #38d66b;
  --battery-good: #38d66b;
  --battery-medium: #ffb020;
  --battery-low: #ff5c5c;
  --shadow-sm: 0 1px 2px rgba(0, 0, 0, 0.4);
  --shadow-md: 0 3px 6px rgba(0, 0, 0, 0.3), 0 3px 6px rgba(0, 0, 0, 0.2);
  --shadow-lg: 0 1px 2px rgba(0, 0, 0, 0.4), 0 8px 26px rgba(0, 0, 0, 0.35);
  --transition-fast: 150ms cubic-bezier(0.4, 0, 0.2, 1);
  --transition-normal: 300ms cubic-bezier(0.4, 0, 0.2, 1);
  --transition-slow: 500ms cubic-bezier(0.4, 0, 0.2, 1);
  --radius-sm: 8px;
  --radius-md: 9px;
  --radius-lg: 12px;
  --radius-xl: 14px;
  --font-size-xs: 12px;
  --font-size-sm: 14px;
  --font-size-base: 16px;
  --font-size-lg: 18px;
  --font-size-xl: 20px;
  --font-size-2xl: 24px;
  --line-height-tight: 1.25;
  --line-height-normal: 1.5;
  --line-height-relaxed: 1.75;
}
@media (prefers-color-scheme: light) {
  :root {
    --bg-primary: #eceef2;
    --bg-secondary: #f4f6f9;
    --bg-tertiary: #ffffff;
    --surface-primary: #ffffff;
    --surface-secondary: #f4f6f9;
    --border-subtle: #dde1e9;
    --border-default: #c9cfda;
    --text-primary: #171c24;
    --text-secondary: #5b6675;
    --text-muted: #8a93a2;
    --accent: #c9770a;
    --accent-2: #0c9c90;
    --accent-3: #1a9d51;
    --accent-danger: #d94339;
    --accent-warning: #c9770a;
    --accent-info: #2b7fe0;
  }
}
body.high-contrast {
  --bg-primary: #000000;
  --bg-secondary: #000000;
  --bg-tertiary: #000000;
  --surface-primary: #000000;
  --surface-secondary: #000000;
  --border-subtle: #ffffff;
  --border-default: #ffffff;
  --text-primary: #ffffff;
  --text-secondary: #ffffff;
  --text-muted: #ffffff;
  --accent: #ffff00;
  --accent-2: #ffffff;
  --accent-3: #ffffff;
  --accent-danger: #ff0000;
  --shadow-sm: none;
  --shadow-md: none;
  --shadow-lg: none;
}
* {
  box-sizing: border-box;
}
html, body {
  margin: 0;
  padding: 0;
  height: 100%;
}
body {
  font-family: system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', 'Helvetica Neue', sans-serif;
  font-size: var(--font-size-base);
  line-height: var(--line-height-normal);
  background-color: var(--bg-primary);
  color: var(--text-primary);
  padding: 0;
  margin: 0;
  transition: background-color var(--transition-normal), color var(--transition-normal);
  -webkit-font-smoothing: antialiased;
  -moz-osx-font-smoothing: grayscale;
}
.wrap{max-width:1000px;margin:0 auto;padding:22px 16px 60px}
body.light-mode {
  --bg-primary: #eceef2;
  --bg-secondary: #f6f8fa;
  --bg-tertiary: #ffffff;
  --surface-primary: #ffffff;
  --surface-secondary: #f6f8fa;
  --border-subtle: #d8dee4;
  --border-default: #d0d7de;
  --text-primary: #0d1117;
  --text-secondary: #424a53;
  --text-muted: #656d76;
  --accent: #c9770a;
  --accent-2: #0969da;
  --accent-3: #1a7f0e;
  --accent-danger: #cf222e;
}
h1, h2, h3, h4, h5, h6 {
  margin: 0 0 8px 0;
  font-weight: 600;
  line-height: var(--line-height-tight);
  color: var(--text-primary);
}
h1 { font-size: 2rem; margin-bottom: 16px; }
h2 { font-size: 1.5rem; margin-bottom: 12px; }
h3 { font-size: 1.25rem; margin-bottom: 10px; }
h4 { font-size: 1.125rem; }
h5 { font-size: 1rem; }
h6 { font-size: 0.875rem; }
p {
  margin: 0 0 12px 0;
  color: var(--text-secondary);
}
b, strong {
  font-weight: 600;
  color: var(--text-primary);
}
a {
  color: var(--accent-2);
  text-decoration: none;
  transition: color var(--transition-fast);
}
a:hover {
  color: var(--accent);
  text-decoration: underline;
}
hr {
  border: none;
  border-top: 1px solid var(--border-subtle);
  margin: 16px 0;
  opacity: 0.5;
}
.header-container {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 20px;
  padding-bottom: 12px;
  border-bottom: 1px solid var(--border-subtle);
  flex-wrap: wrap;
  gap: 12px;
}
.header-title {
  font-size: var(--font-size-2xl);
  font-weight: 700;
  margin: 0;
  color: var(--text-primary);
}
.header-controls {
  display: flex;
  align-items: center;
  gap: 16px;
  flex-wrap: wrap;
}
.status-group {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 8px 12px;
  background-color: var(--bg-secondary);
  border-radius: var(--radius-lg);
  transition: background-color var(--transition-fast);
}
.status-group:hover {
  background-color: var(--bg-tertiary);
}
.status-text {
  font-size: var(--font-size-sm);
  color: var(--text-secondary);
  line-height: var(--line-height-tight);
}
.icon-button {
  background: none;
  border: none;
  color: var(--text-secondary);
  cursor: pointer;
  padding: 8px;
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: var(--radius-md);
  transition: background-color var(--transition-fast), color var(--transition-fast);
}
.icon-button:hover {
  background-color: var(--bg-secondary);
  color: var(--text-primary);
}
.icon-button svg {
  width: 24px;
  height: 24px;
  stroke: currentColor;
  stroke-width: 2;
  stroke-linecap: round;
  stroke-linejoin: round;
  fill: none;
}
.tabs {
  display: flex;
  background-color: var(--bg-secondary);
  border-radius: var(--radius-lg);
  margin-bottom: 20px;
  overflow: hidden;
  flex-wrap: wrap;
  border: 1px solid var(--border-subtle);
  transition: background-color var(--transition-normal);
  box-shadow: var(--shadow-sm);
}
.tab {
  flex: 1;
  padding: 12px 16px;
  text-align: center;
  cursor: pointer;
  color: var(--text-secondary);
  border-bottom: 2px solid transparent;
  font-size: var(--font-size-sm);
  font-weight: 500;
  min-width: 80px;
  transition: color var(--transition-fast), background-color var(--transition-fast), border-color var(--transition-fast);
  user-select: none;
}
.tab:hover {
  background-color: var(--bg-tertiary);
  color: var(--text-primary);
}
.tab.active {
  color: var(--text-primary);
  border-bottom-color: var(--accent);
  background-color: var(--bg-tertiary);
  font-weight: 600;
}
.hamburger {
  display: none;
  background-color: var(--bg-secondary);
  color: var(--text-primary);
  border: 1px solid var(--border-subtle);
  font-size: 24px;
  cursor: pointer;
  padding: 10px 16px;
  width: 100%;
  text-align: left;
  border-radius: var(--radius-lg);
  margin-bottom: 8px;
  transition: background-color var(--transition-fast), color var(--transition-fast);
  font-weight: 500;
}
.hamburger:hover {
  background-color: var(--bg-tertiary);
  border-color: var(--border-default);
}
@media (max-width: 640px) {
  .hamburger {
    display: block;
  }
  .tabs {
    display: none;
    flex-direction: column;
    margin-bottom: 12px;
  }
  .tabs.open {
    display: flex;
  }
  .tab {
    text-align: left;
    padding: 12px 16px;
    border-bottom: none;
    border-left: 3px solid transparent;
    margin: 4px 0;
  }
  .tab.active {
    border-left-color: var(--accent);
    border-bottom: none;
  }
}
.panel {
  display: none;
  background-color: var(--bg-secondary);
  padding: 20px;
  border-radius: var(--radius-lg);
  border: 1px solid var(--border-subtle);
  transition: background-color var(--transition-normal);
  box-shadow: var(--shadow-md);
}
.panel.active {
  display: block;
  animation: slideIn var(--transition-normal) ease-out;
}
@keyframes slideIn {
  from {
    opacity: 0;
    transform: translateY(-8px);
  }
  to {
    opacity: 1;
    transform: translateY(0);
  }
}
.box {
  background-color: var(--bg-tertiary);
  padding: 16px;
  border-radius: var(--radius-md);
  border: 1px solid var(--border-subtle);
  font-size: var(--font-size-sm);
  line-height: var(--line-height-normal);
  transition: background-color var(--transition-normal), box-shadow var(--transition-normal);
  box-shadow: var(--shadow-sm);
}
.box:hover {
  box-shadow: var(--shadow-md);
}
.box-title {
  color: var(--accent);
  margin: 0 0 12px 0;
  border-bottom: 1px solid var(--border-subtle);
  padding-bottom: 8px;
  font-weight: 600;
  font-size: var(--font-size-sm);
}
.card-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
  gap: 16px;
  margin: 16px 0;
}
.card {
  background-color: var(--bg-tertiary);
  padding: 16px;
  border-radius: var(--radius-md);
  border: 1px solid var(--border-subtle);
  box-shadow: var(--shadow-sm);
  transition: all var(--transition-normal);
  display: flex;
  flex-direction: column;
}
.card:hover {
  transform: translateY(-2px);
  box-shadow: var(--shadow-md);
  border-color: var(--border-default);
}
.card-title {
  font-size: var(--font-size-sm);
  color: var(--text-secondary);
  font-weight: 500;
  margin-bottom: 8px;
  text-transform: uppercase;
  letter-spacing: 0.5px;
}
.card-value {
  font-size: 1.75rem;
  font-weight: 700;
  color: var(--text-primary);
  font-family: 'Monaco', 'Menlo', 'Ubuntu Mono', monospace;
  margin-bottom: 4px;
}
.card-meta {
  font-size: var(--font-size-xs);
  color: var(--text-muted);
}
input[type="text"],
input[type="password"],
input[type="email"],
input[type="number"],
input[type="date"],
input[type="time"],
input[type="file"],
select,
textarea {
  display: block;
  width: 100%;
  padding: 10px 12px;
  margin: 8px 0;
  border-radius: var(--radius-md);
  border: 1px solid var(--border-default);
  background-color: var(--bg-tertiary);
  color: var(--text-primary);
  font-size: var(--font-size-sm);
  font-family: inherit;
  transition: border-color var(--transition-fast), box-shadow var(--transition-fast), background-color var(--transition-fast);
  box-sizing: border-box;
}
input[type="text"]::placeholder,
input[type="password"]::placeholder,
input[type="email"]::placeholder,
input[type="number"]::placeholder,
textarea::placeholder {
  color: var(--text-muted);
}
input[type="text"]:focus,
input[type="password"]:focus,
input[type="email"]:focus,
input[type="number"]:focus,
input[type="date"]:focus,
input[type="time"]:focus,
input[type="file"]:focus,
select:focus,
textarea:focus {
  outline: none;
  border-color: var(--accent);
  box-shadow: 0 0 0 3px rgba(255, 176, 32, 0.1);
}
input[type="color"] {
  padding: 2px;
  height: 44px;
  cursor: pointer;
  border-radius: var(--radius-md);
  border: 1px solid var(--border-default);
}
input[type="range"] {
  width: 100%;
  height: 6px;
  border-radius: 3px;
  background: var(--border-default);
  outline: none;
  -webkit-appearance: none;
  appearance: none;
}
input[type="range"]::-webkit-slider-thumb {
  -webkit-appearance: none;
  appearance: none;
  width: 18px;
  height: 18px;
  border-radius: 50%;
  background: var(--accent);
  cursor: pointer;
  box-shadow: var(--shadow-sm);
  transition: all var(--transition-fast);
}
input[type="range"]::-webkit-slider-thumb:hover {
  transform: scale(1.15);
  box-shadow: var(--shadow-md);
}
input[type="range"]::-moz-range-thumb {
  width: 18px;
  height: 18px;
  border-radius: 50%;
  background: var(--accent);
  cursor: pointer;
  border: none;
  box-shadow: var(--shadow-sm);
  transition: all var(--transition-fast);
}
input[type="range"]::-moz-range-thumb:hover {
  transform: scale(1.15);
  box-shadow: var(--shadow-md);
}
input[type="checkbox"],
input[type="radio"] {
  margin-right: 8px;
  cursor: pointer;
  width: 18px;
  height: 18px;
  accent-color: var(--accent);
}
select {
  cursor: pointer;
  appearance: none;
  background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 12 12'%3E%3Cpath fill='%238b949e' d='M6 9L1 4h10z'/%3E%3C/svg%3E");
  background-repeat: no-repeat;
  background-position: right 12px center;
  padding-right: 36px;
}
textarea {
  font-family: 'Monaco', 'Menlo', 'Ubuntu Mono', monospace;
  font-size: 12px;
  resize: vertical;
  min-height: 100px;
  line-height: var(--line-height-normal);
}
.cb-lbl {
  display: flex;
  align-items: center;
  gap: 10px;
  margin-bottom: 12px;
  font-size: var(--font-size-base);
  cursor: pointer;
  color: var(--text-primary);
  user-select: none;
  transition: color var(--transition-fast);
}
.cb-lbl:hover {
  color: var(--accent);
}
.cb-lbl input[type="checkbox"] {
  margin: 0;
  flex-shrink: 0;
}
.btn,
input[type="submit"],
input[type="button"],
button:not(.icon-button):not(.hamburger) {
  background-color: var(--accent-3);
  color: #ffffff;
  font-weight: 600;
  border: none;
  cursor: pointer;
  padding: 12px 16px;
  border-radius: var(--radius-md);
  width: 100%;
  margin-bottom: 10px;
  min-height: 44px;
  font-size: var(--font-size-sm);
  transition: all var(--transition-fast);
  box-shadow: var(--shadow-sm);
  user-select: none;
  font-family: inherit;
  display: inline-block;
  text-align: center;
}
.btn:hover,
input[type="submit"]:hover,
input[type="button"]:hover,
button:not(.icon-button):not(.hamburger):hover {
  background-color: var(--accent-3);
  opacity: 0.9;
  transform: translateY(-1px);
  box-shadow: var(--shadow-md);
}
.btn:active,
input[type="submit"]:active,
input[type="button"]:active,
button:not(.icon-button):not(.hamburger):active {
  transform: translateY(0);
  box-shadow: var(--shadow-sm);
}
.btn:disabled,
input[type="submit"]:disabled,
input[type="button"]:disabled,
button:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}
.btn-blue,
input[type="submit"] {
  background-color: var(--accent-2);
}
.btn-red,
.btn-danger {
  background-color: var(--accent-danger);
}
.btn-gray,
.btn-secondary {
  background-color: var(--border-default);
  color: var(--text-primary);
}
.btn-small {
  padding: 8px 12px;
  font-size: var(--font-size-xs);
  margin: 0;
  min-height: 32px;
  width: auto;
}
.toast {
  visibility: hidden;
  min-width: 250px;
  background-color: var(--accent-3);
  color: #ffffff;
  text-align: center;
  border-radius: var(--radius-md);
  padding: 16px;
  position: fixed;
  z-index: 1000;
  left: 50%;
  bottom: 30px;
  transform: translateX(-50%);
  box-shadow: var(--shadow-lg);
  font-weight: 500;
}
.toast.show {
  visibility: visible;
  animation: slideUp var(--transition-normal) ease-out, slideDown var(--transition-normal) ease-in 2.5s forwards;
}
@keyframes slideUp {
  from {
    opacity: 0;
    transform: translateX(-50%) translateY(20px);
  }
  to {
    opacity: 1;
    transform: translateX(-50%) translateY(0);
  }
}
@keyframes slideDown {
  from {
    opacity: 1;
    transform: translateX(-50%) translateY(0);
  }
  to {
    opacity: 0;
    transform: translateX(-50%) translateY(20px);
  }
}
.battery-indicator {
  display: flex;
  align-items: center;
  gap: 8px;
}
.battery-bar {
  display: inline-block;
  width: 32px;
  height: 16px;
  border: 2px solid var(--border-default);
  border-radius: 3px;
  position: relative;
  overflow: hidden;
}
.battery-bar::after {
  content: '';
  position: absolute;
  right: -4px;
  top: 3px;
  width: 2px;
  height: 8px;
  background-color: var(--border-default);
  border-radius: 0 2px 2px 0;
}
.battery-fill {
  background-color: var(--battery-good);
  width: 50%;
  height: 100%;
  transition: width var(--transition-normal), background-color var(--transition-normal);
}
.wifi-status,
.bt-status {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 6px 10px;
  background-color: var(--bg-tertiary);
  border-radius: var(--radius-md);
  font-size: var(--font-size-xs);
  font-weight: 500;
  color: var(--text-secondary);
  border: 1px solid var(--border-subtle);
}
.wifi-status.connected,
.bt-status.connected {
  color: var(--accent-3);
  border-color: var(--accent-3);
  background-color: rgba(63, 185, 80, 0.1);
}
.text-muted {
  color: var(--text-muted);
  font-size: var(--font-size-sm);
}
.text-center {
  text-align: center;
}
.text-right {
  text-align: right;
}
.mt-0 { margin-top: 0; }
.mt-2 { margin-top: 8px; }
.mt-4 { margin-top: 16px; }
.mb-0 { margin-bottom: 0; }
.mb-2 { margin-bottom: 8px; }
.mb-4 { margin-bottom: 16px; }
.gap-1 { gap: 4px; }
.gap-2 { gap: 8px; }
.gap-3 { gap: 12px; }
.gap-4 { gap: 16px; }
code, pre, .mono {
  font-family: 'Monaco', 'Menlo', 'Ubuntu Mono', 'Courier New', monospace;
  font-size: var(--font-size-xs);
  color: var(--text-primary);
}
pre {
  background-color: var(--bg-tertiary);
  border: 1px solid var(--border-subtle);
  padding: 12px;
  border-radius: var(--radius-md);
  overflow-x: auto;
  line-height: var(--line-height-normal);
  margin: 8px 0;
}
code {
  background-color: var(--bg-tertiary);
  padding: 2px 6px;
  border-radius: 3px;
  color: var(--accent);
}
.progress-container {
  display: block;
  background-color: var(--bg-tertiary);
  border-radius: var(--radius-md);
  height: 24px;
  width: 100%;
  overflow: hidden;
  border: 1px solid var(--border-subtle);
  margin: 12px 0;
}
.progress-bar {
  background-color: var(--accent-3);
  height: 100%;
  width: 50%;
  display: flex;
  align-items: center;
  justify-content: center;
  color: #ffffff;
  font-weight: 600;
  font-size: var(--font-size-xs);
  transition: width var(--transition-normal);
}
.progress-bar.warning {
  background-color: var(--accent-warning);
}
.progress-bar.danger {
  background-color: var(--accent-danger);
}
details {
  background-color: var(--bg-tertiary);
  padding: 12px;
  border-radius: var(--radius-md);
  border: 1px solid var(--border-subtle);
  margin-bottom: 12px;
  transition: background-color var(--transition-normal);
}
details:hover {
  background-color: var(--bg-secondary);
}
summary {
  cursor: pointer;
  font-weight: 600;
  color: var(--accent);
  user-select: none;
  padding: 8px 0;
  transition: color var(--transition-fast);
}
summary:hover {
  color: var(--accent-2);
}
details[open] summary {
  margin-bottom: 12px;
}
.data-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 12px 0;
  border-bottom: 1px solid var(--border-subtle);
}
.data-row:last-child {
  border-bottom: none;
}
.data-label {
  color: var(--text-secondary);
  font-weight: 500;
  font-size: var(--font-size-sm);
}
.data-value {
  color: var(--text-primary);
  font-weight: 600;
  font-family: 'Monaco', 'Menlo', 'Ubuntu Mono', monospace;
  font-size: var(--font-size-sm);
}
.grid-2 {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
  gap: 16px;
}
.grid-3 {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
  gap: 12px;
}
.grid-auto {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(120px, 1fr));
  gap: 12px;
}
@media (max-width: 768px) {
  .grid-2,
  .grid-3 {
    grid-template-columns: 1fr;
  }
}
.flex {
  display: flex;
}
.flex-center {
  display: flex;
  align-items: center;
  justify-content: center;
}
.flex-between {
  display: flex;
  align-items: center;
  justify-content: space-between;
}
.flex-col {
  display: flex;
  flex-direction: column;
}
.flex-wrap {
  flex-wrap: wrap;
}
.gap-8px {
  gap: 8px;
}
.gap-16px {
  gap: 16px;
}
@media (max-width: 1200px) {
  body {
    padding: 10px;
  }
  .card-grid {
    grid-template-columns: repeat(auto-fit, minmax(160px, 1fr));
  }
}
@media (max-width: 768px) {
  body {
    padding: 8px;
  }
  h1, h2 {
    font-size: 1.25rem;
  }
  .header-controls {
    width: 100%;
    gap: 8px;
  }
  .status-group {
    font-size: 12px;
  }
  .card-value {
    font-size: 1.5rem;
  }
  .btn {
    padding: 10px 12px;
    min-height: 40px;
    font-size: 14px;
  }
  input[type="text"],
  input[type="password"],
  input[type="email"],
  input[type="number"],
  input[type="date"],
  input[type="time"],
  select,
  textarea {
    font-size: 16px; }
  .grid-2,
  .grid-3,
  .grid-auto {
    grid-template-columns: 1fr;
  }
}
@media (max-width: 480px) {
  body {
    padding: 6px;
  }
  h1 {
    font-size: 1.125rem;
  }
  h2, h3 {
    font-size: 1rem;
  }
  .header-container {
    flex-direction: column;
    align-items: flex-start;
  }
  .header-title {
    width: 100%;
  }
  .header-controls {
    width: 100%;
    flex-wrap: wrap;
  }
  .tabs {
    margin-bottom: 8px;
  }
  .panel {
    padding: 12px;
  }
  .card {
    padding: 12px;
  }
  .card-grid {
    gap: 12px;
  }
  input[type="text"],
  input[type="password"],
  input[type="email"],
  input[type="number"],
  input[type="date"],
  input[type="time"],
  select,
  textarea {
    padding: 8px 10px;
    margin: 6px 0;
  }
  .btn {
    padding: 10px;
    margin-bottom: 8px;
  }
}
body.light-mode input[type="file"] {
  background-color: #f6f8fa;
}
body.light-mode pre {
  background-color: #f6f8fa;
  border-color: #d0d7de;
}
body.light-mode code {
  background-color: #f6f8fa;
  color: var(--accent);
}
body.high-contrast .tabs,
body.high-contrast .panel,
body.high-contrast .box,
body.high-contrast details {
  border: 2px solid var(--border-default) !important;
  box-shadow: none !important;
}
body.high-contrast input[type="text"],
body.high-contrast input[type="password"],
body.high-contrast input[type="email"],
body.high-contrast input[type="number"],
body.high-contrast input[type="date"],
body.high-contrast input[type="time"],
body.high-contrast select,
body.high-contrast textarea {
  border: 2px solid var(--text-primary) !important;
}
body.high-contrast .btn,
body.high-contrast input[type="submit"],
body.high-contrast input[type="button"],
body.high-contrast button {
  border: 2px solid var(--text-primary) !important;
}
@media print {
  body {
    background: white;
    color: black;
  }
  .btn,
  .hamburger,
  input[type="submit"],
  input[type="button"],
  button {
    display: none;
  }
  .panel {
    page-break-inside: avoid;
  }
}
@keyframes spin {
  from {
    transform: rotate(0deg);
  }
  to {
    transform: rotate(360deg);
  }
}
@keyframes pulse {
  0%, 100% {
    opacity: 1;
  }
  50% {
    opacity: 0.5;
  }
}
@keyframes fadeIn {
  from {
    opacity: 0;
  }
  to {
    opacity: 1;
  }
}
.animate-spin {
  animation: spin 1s linear infinite;
}
.animate-pulse {
  animation: pulse 2s cubic-bezier(0.4, 0, 0.6, 1) infinite;
}
.animate-fadeIn {
  animation: fadeIn var(--transition-normal);
}
</style>
    </head><body>)rawliteral");

    s += F("<div class='wrap'>");
    s += "<h2>" + String(tTitle) + "</h2>";
    int curPct = getBattery();
    float curVolt = getBatteryVoltageCached(false);
    bool isChgNow = isChargingCached(false);
    float activeRate = isChgNow ? battRateCharging : battRateNormal;

    s += F("<div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:12px;margin:12px 0 18px 0;'>");
    s += F("<div class='card' style='padding:12px 14px;background:var(--surface);border:1px solid var(--border);border-radius:10px;'>");
    s += F("<div style='font-size:11px;color:var(--text-dim);text-transform:uppercase;'>Akkustand &amp; Spannung</div>");
    s += "<div style='font-size:22px;font-weight:700;color:var(--text);margin-top:2px;'>" + String(curPct) + "% <span style='font-size:15px;color:#00e5ff;font-weight:400;'>" + String(curVolt, 2) + " V</span></div>";
    s += F("<div style='font-size:11px;color:var(--good);margin-top:2px;'>12-Pkt LiPo-Kurve</div></div>");

    s += F("<div class='card' style='padding:12px 14px;background:var(--surface);border:1px solid var(--border);border-radius:10px;'>");
    s += F("<div style='font-size:11px;color:var(--text-dim);text-transform:uppercase;'>Ladezustand</div>");
    s += "<div style='font-size:18px;font-weight:700;color:" + String(isChgNow ? "#2ecc71" : "#00e5ff") + ";margin-top:4px;'>" + String(isChgNow ? "⚡ Laden (USB-C)" : "🔋 Akku-Betrieb") + "</div>";
    s += F("<div style='font-size:11px;color:var(--text-dim);margin-top:2px;'>AXP2101 PMU</div></div>");

    s += F("<div class='card' style='padding:12px 14px;background:var(--surface);border:1px solid var(--border);border-radius:10px;'>");
    s += F("<div style='font-size:11px;color:var(--text-dim);text-transform:uppercase;'>Lern-Rate &amp; Prognose</div>");
    s += "<div style='font-size:18px;font-weight:700;color:#ffb74d;margin-top:4px;'>" + String(activeRate, 1) + "% / Std.</div>";
    s += "<div style='font-size:11px;color:var(--text-dim);margin-top:2px;'>" + String(isChgNow ? "Ladegeschwindigkeit" : "Entladetempo") + "</div></div>";

    s += F("<div class='card' style='padding:12px 14px;background:var(--surface);border:1px solid var(--border);border-radius:10px;'>");
    s += F("<div style='font-size:11px;color:var(--text-dim);text-transform:uppercase;'>CPU-Frequenz</div>");
    s += "<div style='font-size:18px;font-weight:700;color:#81c784;margin-top:4px;'>" + String(ESP.getCpuFreqMHz()) + " MHz</div>";
    s += F("<div style='font-size:11px;color:var(--text-dim);margin-top:2px;'>Dynamisch skaliert</div></div></div>");

    s += F("<div id='chart-container'>");
    s += F("<div class='filters'>");
    s += "<div class='fg grp'><span class='tag'>" + String(tFilterRange) + "</span>";
    s += "<button class='fbtn active' data-range='6h'>" + String(tLast6h) + "</button>";
    s += "<button class='fbtn' data-range='24h'>" + String(tLast24h) + "</button>";
    s += "<button class='fbtn' data-range='72h'>" + String(tLast72h) + "</button>";
    s += "<button class='fbtn' data-range='all'>" + String(tAll) + "</button></div>";
    s += "<div class='fg grp'><span class='tag'>" + String(tFilterMode) + "</span>";
    s += "<select id='modeFilter'><option value='all'>" + String(tAll) + "</option><option value='0'>" + String(tNormal) + "</option><option value='1'>" + String(tHack) + "</option><option value='2'>" + String(tCharge) + "</option></select></div>";
    s += F("</div><div id='tooltip'></div><canvas id='chart' width='860' height='360'></canvas>");
    s += F("<div class='legend' style='display:flex;flex-wrap:wrap;gap:14px;justify-content:center;padding:12px 16px;background:rgba(0,0,0,0.18);border-radius:8px;margin-top:10px;'>");
    s += "<div class='leg-item'><div class='dot' style='background:#4CAF50;'></div>" + String(tr(sysLang, "Normalbetrieb (Display & CPU 80-240 MHz)", "Normal operation (Display & CPU 80-240 MHz)", "Fonctionnement normal")) + "</div>";
    s += "<div class='leg-item'><div class='dot' style='background:#ff0000;'></div>" + String(tr(sysLang, "Hintergrund-Tools aktiv (RF/WiFi/BLE)", "Background tools active (RF/WiFi/BLE)", "Outils en arrière-plan")) + "</div>";
    s += "<div class='leg-item'><div class='dot' style='background:#00aaff;'></div>" + String(tr(sysLang, "Ladevorgang via USB (AXP2101)", "Charging via USB (AXP2101)", "Charge via USB")) + "</div>";
    s += "<div class='leg-item'><div class='dot' style='background:#ffb74d;'></div>" + String(tr(sysLang, "Entlade- & Spannungsverlauf", "Discharge & Voltage history", "Historique de décharge")) + "</div>";
    s += F("</div></div>");
    s += "<div style='display:flex; gap:10px; justify-content:center; flex-wrap:wrap;'>";
    s += "<a href='/' class='btn'>&#8592; " + String(tBack) + "</a>";
    s += "<form method='POST' action='/battery/reset' onsubmit='return confirm(\"" + String(tResetConfirm) + "\")' style='margin:0;'>";
    s += "<button type='submit' class='btn' style='background:#a33;'>" + String(tResetHist) + "</button></form></div>";
    s += F("</div>"); // .wrap
    s += F("<script>");

    s += F("var rawData = [");
    for(int i=0; i<historyCount; i++) { s += batHistoryPct[i]; if(i < historyCount - 1) s += F(","); }
    s += F("];\nvar intervalMins = "); s += (historyInterval / 60000); s += F(";\nvar curH = "); s += curH; s += F(";\nvar curM = "); s += curM; s += F(";\n");

    s += "var txBattery='" + String(tr(sysLang, "Akku", "Battery", "Batterie")) + "';";
    s += "var txStatus='" + String(tr(sysLang, "Status", "Status", "Statut")) + "';";
    s += "var txZone='" + String(tr(sysLang, "Krit. Zone", "Critical Zone", "Zone Critique")) + "';";
    s += "var zoneGreen='" + String(tr(sysLang, "Gruen", "Green", "Vert")) + "';";
    s += "var zoneYellow='" + String(tr(sysLang, "Gelb", "Yellow", "Jaune")) + "';";
    s += "var zoneOrange='" + String(tr(sysLang, "Orange", "Orange", "Orange")) + "';";
    s += "var zoneRed='" + String(tr(sysLang, "Rot", "Red", "Rouge")) + "';";
    s += "var stCharge='" + String(tr(sysLang, "Laden (USB)", "Charging (USB)", "Charge (USB)")) + "';";
    s += "var stHack='" + String(tr(sysLang, "Hacking aktiv", "Hacking active", "Hacking actif")) + "';";
    s += "var stNormal='" + String(tr(sysLang, "Normal", "Normal", "Normal")) + "';";

    s += F(R"rawliteral(
    var canvas = document.getElementById('chart'); var ctx = canvas.getContext('2d'); var tooltip = document.getElementById('tooltip');
    var w = canvas.width; var h = canvas.height; var padX = 50; var padY = 30; var chartW = w - padX * 2; var chartH = h - padY * 2;
    var curDate = new Date(); curDate.setHours(curH, curM, 0, 0);
    var dataRaw = [];
    for(var i=0; i<rawData.length; i++) {
        var v = rawData[i]; var pct = v % 100;
        if(v >= 200 && pct === 0 && v !== 200) pct = 100;
        var state = (v >= 200) ? 2 : (v >= 100 ? 1 : 0);
        var t = new Date(curDate.getTime() - (rawData.length - 1 - i) * intervalMins * 60000);
        dataRaw.push({ pct: pct, state: state, time: t });
    }

    var selectedRange = '6h';
    var data = [];

    function modeColor(state){ if(state === 2) return '#00aaff'; if(state === 1) return '#ff0000'; return '#4CAF50'; }
    function battFillColor(pct){ if(pct >= 75) return 'rgba(46,204,113,0.28)'; if(pct >= 50) return 'rgba(241,196,15,0.28)'; if(pct >= 25) return 'rgba(255,152,0,0.30)'; return 'rgba(229,57,53,0.34)'; }

    function inRange(t) {
        var diffH = (curDate.getTime() - t.getTime()) / 3600000;
        if(selectedRange === '6h') return diffH <= 6.0;
        if(selectedRange === '24h') return diffH <= 24.0;
        if(selectedRange === '72h') return diffH <= 72.0;
        return true;
    }

    function applyFilters() {
        var mode = document.getElementById('modeFilter').value;
        var filtered = [];
        for (var i = 0; i < dataRaw.length; i++) {
            if (!inRange(dataRaw[i].time)) continue;
            if (mode !== 'all' && dataRaw[i].state !== parseInt(mode)) continue;
            filtered.push(dataRaw[i]);
        }
        data = [];
        for (var j = 0; j < filtered.length; j++) {
            data.push({ x: padX + (j / Math.max(1, filtered.length - 1)) * chartW, y: padY + chartH - (filtered[j].pct / 100) * chartH, pct: filtered[j].pct, state: filtered[j].state, time: filtered[j].time });
        }
    }

    function drawBase() {
        ctx.clearRect(0,0,w,h); ctx.fillStyle = '#aaa'; ctx.strokeStyle = '#444'; ctx.lineWidth = 1; ctx.font = '12px sans-serif'; ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
        ctx.beginPath();
        for(var i=0; i<=4; i++){ var y = padY + chartH - (i/4)*chartH; ctx.moveTo(padX, y); ctx.lineTo(w-padX, y); ctx.fillText(i*25+'%', padX - 10, y); }
        ctx.stroke();

        ctx.textAlign = 'center'; ctx.textBaseline = 'top';
        if(data.length > 0) {
            var numLabels = 6;
            for(var i=0; i<=numLabels; i++) {
                var idx = Math.floor(i * (data.length-1) / numLabels);
                if(data[idx]) { var tx = data[idx].x; var tm = ('0'+data[idx].time.getHours()).slice(-2) + ':' + ('0'+data[idx].time.getMinutes()).slice(-2); ctx.fillText(tm, tx, h - padY + 10); }
            }
        }

        if(data.length > 1) {
            for(var i=1; i<data.length; i++) {
                var x1 = data[i-1].x, x2 = data[i].x, y1 = data[i-1].y, y2 = data[i].y;
                var midPct = Math.round((data[i-1].pct + data[i].pct) / 2);
                ctx.beginPath(); ctx.moveTo(x1, y1); ctx.lineTo(x2, y2); ctx.lineTo(x2, padY + chartH); ctx.lineTo(x1, padY + chartH); ctx.closePath();
                ctx.fillStyle = battFillColor(midPct); ctx.fill();
            }
        }

        if(data.length > 1) {
            ctx.lineWidth = 3;
            for(var i=1; i<data.length; i++) { ctx.beginPath(); ctx.moveTo(data[i-1].x, data[i-1].y); ctx.lineTo(data[i].x, data[i].y); ctx.strokeStyle = modeColor(data[i].state); ctx.stroke(); }
        }
    }

    function redraw() { applyFilters(); drawBase(); }

    document.querySelectorAll('[data-range]').forEach(function(btn){
        btn.addEventListener('click', function(){
            selectedRange = btn.getAttribute('data-range');
            document.querySelectorAll('[data-range]').forEach(function(b){ b.classList.remove('active'); });
            btn.classList.add('active');
            tooltip.style.display = 'none';
            redraw();
        });
    });

    document.getElementById('modeFilter').addEventListener('change', function(){ tooltip.style.display = 'none'; redraw(); });

    canvas.addEventListener('mousemove', function(e) {
        var rect = canvas.getBoundingClientRect(); var scaleX = canvas.width / rect.width; var scaleY = canvas.height / rect.height;
        var mouseX = (e.clientX - rect.left) * scaleX;
        if(data.length === 0 || mouseX < padX || mouseX > w - padX) { tooltip.style.display = 'none'; drawBase(); return; }

        var closest = data[0]; var minDist = Math.abs(mouseX - closest.x);
        for(var i=1; i<data.length; i++) { var d = Math.abs(mouseX - data[i].x); if(d < minDist) { minDist = d; closest = data[i]; } }

        drawBase();
        ctx.beginPath(); ctx.strokeStyle = '#fff'; ctx.lineWidth = 1; ctx.setLineDash([4, 4]); ctx.moveTo(closest.x, padY); ctx.lineTo(closest.x, padY + chartH); ctx.stroke(); ctx.setLineDash([]);
        ctx.beginPath(); ctx.arc(closest.x, closest.y, 6, 0, 2*Math.PI); ctx.fillStyle = modeColor(closest.state); ctx.fill(); ctx.lineWidth=2; ctx.strokeStyle='#fff'; ctx.stroke();

        var stateStr = closest.state === 2 ? stCharge : (closest.state === 1 ? stHack : stNormal);
        var krit = (closest.pct >= 75) ? zoneGreen : (closest.pct >= 50) ? zoneYellow : (closest.pct >= 25) ? zoneOrange : zoneRed;
        var timeStr = ('0'+closest.time.getHours()).slice(-2) + ':' + ('0'+closest.time.getMinutes()).slice(-2);
        tooltip.innerHTML = '<b style="color:#4CAF50;">' + timeStr + '</b><br><br><b>' + txBattery + ':</b> ' + closest.pct + '%<br><b>' + txStatus + ':</b> ' + stateStr + '<br><b>' + txZone + ':</b> ' + krit;
        tooltip.style.display = 'block';

        var tipX = (closest.x / scaleX);
        if(tipX + 120 > rect.width) tipX -= 50; else if(tipX - 120 < 0) tipX += 50;
        tooltip.style.left = tipX + 'px'; tooltip.style.top = (closest.y / scaleY) + 'px';
    });

    canvas.addEventListener('mouseleave', function() { tooltip.style.display = 'none'; drawBase(); });
    redraw();
    </script></body></html>)rawliteral");
    return s;
}

String htmlEscape(String str) { str.replace("&", "&amp;"); str.replace("<", "&lt;"); str.replace(">", "&gt;"); str.replace("\"", "&quot;"); return str; }

// Berechnet, an welchem Kalenderdatum "andere" Person (Elternteil oder Kind) genau so alt war/sein wird
// wie man selbst gerade ist - und wie alt man dabei damals/dann ist. Gemeinsame Formel fuer
// Mama/Papa (Vergangenheit) und Kinder (i.d.R. Zukunft), da beide nach demselben Prinzip funktionieren.
static String computeAnalogDate(time_t uTime, time_t jetzt, const String &otherDate, const String &otherTime, int sysLang, bool futureContext = false) {
    time_t oTime = stringToTime(otherDate, otherTime);
    if (uTime == 0 || oTime == 0) return tr(sysLang, "Daten fehlen", "No Data", "Pas de données");
    int64_t myAgeSec = (int64_t)jetzt - (int64_t)uTime;
    time_t targetDate = (time_t)((int64_t)oTime + myAgeSec);
    struct tm tgt;
    localtime_r(&targetDate, &tgt);
    char dBuf[30];
    snprintf(dBuf, sizeof(dBuf), "%02d.%02d.%04d", tgt.tm_mday, tgt.tm_mon + 1, tgt.tm_year + 1900);
    struct tm uTm;
    localtime_r(&uTime, &uTm);
    int yearsThen = (tgt.tm_year + 1900) - (uTm.tm_year + 1900);
    if (tgt.tm_mon < uTm.tm_mon || (tgt.tm_mon == uTm.tm_mon && tgt.tm_mday < uTm.tm_mday)) {
        yearsThen--;
    }
    if (yearsThen < 0) return String(dBuf) + " (" + tr(sysLang, "Noch nicht geboren", "Not born yet", "Pas encore né") + ")";
    if (futureContext) {
        return String(dBuf) + " (" + tr(sysLang, "Mein Alter dann: ", "My age then: ", "Mon âge alors: ") + String(yearsThen) + " " + tr(sysLang, "Jahre", "years", "ans") + ")";
    }
    return String(dBuf) + " (" + tr(sysLang, "Mein Alter damals: ", "My age then: ", "Mon âge alors: ") + String(yearsThen) + " " + tr(sysLang, "Jahre", "years", "ans") + ")";
}

static const char* resetReasonText(int sysLang) {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return tr(sysLang, "Power-On", "Power-on", "Mise sous tension");
        case ESP_RST_SW: return tr(sysLang, "Software (Neustart)", "Software (reboot)", "Logiciel (redémarrage)");
        case ESP_RST_PANIC: return tr(sysLang, "Absturz (Panic)", "Crash (panic)", "Plantage (panique)");
        case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return tr(sysLang, "Watchdog", "Watchdog", "Watchdog");
        case ESP_RST_DEEPSLEEP: return tr(sysLang, "Deep-Sleep-Aufwachen", "Deep-sleep wake", "Réveil deep-sleep");
        case ESP_RST_BROWNOUT: return tr(sysLang, "Unterspannung (Brownout)", "Brownout", "Sous-tension");
        default: return tr(sysLang, "Unbekannt", "Unknown", "Inconnu");
    }
}

String getHtmlContent() {
    String s; s.reserve(40000);
    
    char sysD[32]; char sysT[16]; char bdayStr[12]; char btimeStr[6];
    
    time_t jetzt; time(&jetzt); struct tm *aktuell = localtime(&jetzt);
    snprintf(sysD, sizeof(sysD), "%04d-%02d-%02d", aktuell->tm_year+1900, aktuell->tm_mon+1, aktuell->tm_mday); 
    snprintf(sysT, sizeof(sysT), "%02d:%02d", aktuell->tm_hour, aktuell->tm_min);
    
    long diffLife = mktime(&zielZeit) - jetzt; if(diffLife < 0) diffLife = 0;
    int rJ = diffLife / 31536000; int rM = (diffLife % 31536000) / 2592000; int rT = (diffLife % 2592000) / 86400;
    
    snprintf(bdayStr, sizeof(bdayStr), "%04d-%02d-%02d", gJahr, gMonat, gTag); 
    snprintf(btimeStr, sizeof(btimeStr), "%02d:%02d", gStunde, gMinute);
    time_t uTime = stringToTime(String(bdayStr), String(btimeStr));

    int lifePct = 0;
    if (uTime != 0) {
        // Als double statt long rechnen: elapsedLifeSec*100 uebersteigt bei mehrjaehrigem
        // Alter locker den 32-Bit-long-Bereich (~2,1 Mrd.) und lief bisher als stiller
        // Ueberlauf, was zu einem falschen/zufaelligen Prozentwert fuehrte.
        double totalLifeSec = (double)(mktime(&zielZeit) - uTime);
        double elapsedLifeSec = (double)(jetzt - uTime);
        if (totalLifeSec > 0) lifePct = (int)(elapsedLifeSec * 100.0 / totalLifeSec);
        if (lifePct < 0) lifePct = 0; if (lifePct > 100) lifePct = 100;
    }

    String parentStr = computeAnalogDate(uTime, jetzt, parentDate, parentTime, sysLang);
    String papaStr = computeAnalogDate(uTime, jetzt, papaDate, papaTime, sysLang);

    s += F(R"rawliteral(<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>
<style>
:root{
  --bg:#0e1116; --surface:#161b22; --surface-2:#1b2129; --elev:#1e242d;
  --border:#262d38; --border-strong:#333c49;
  --text:#e7eaf0; --muted:#8b95a4;
  --accent:#ffb020; --accent-2:#3dd0c4;
  --good:#38d66b; --warn:#ffb020; --crit:#ff5c5c; --charge:#4aa8ff;
  --shadow:0 1px 2px rgba(0,0,0,.4),0 8px 26px rgba(0,0,0,.35);
  --r:12px; --r-sm:8px;
  --font-ui:system-ui,-apple-system,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;
}
*{box-sizing:border-box}
body{font-family:var(--font-ui);background:var(--bg);color:var(--text);padding:10px;margin:0; transition: background 0.3s, color 0.3s;}
.tabs{display:flex;background:var(--surface);border:1px solid var(--border);border-radius:var(--r);margin-bottom:15px;overflow:hidden;flex-wrap:wrap;box-shadow:var(--shadow); transition: background 0.3s, border-color 0.3s;}
.tab{flex:1;padding:12px;text-align:center;cursor:pointer;color:var(--muted);border-bottom:3px solid transparent;font-size:14px;min-width:70px; transition: color 0.2s, background 0.2s;}
.tab.active{color:var(--text);border-bottom:3px solid var(--accent);background:var(--surface-2);}
.panel{display:none;background:var(--surface);padding:20px;border-radius:var(--r);border:1px solid var(--border);box-shadow:var(--shadow); transition: background 0.3s, border-color 0.3s;}
.panel.active{display:block;}
input,select,textarea{padding:10px;margin:8px 0;border-radius:var(--r-sm);border:1px solid var(--border-strong);width:100%;box-sizing:border-box;background:var(--surface-2);color:var(--text);font-family:inherit; transition: background 0.3s, color 0.3s, border-color 0.15s;}
input:focus,select:focus,textarea:focus{outline:2px solid var(--accent-2);outline-offset:1px;border-color:var(--accent-2);}
input[type='color']{padding:0;height:40px;cursor:pointer;}
input[type='checkbox']{width:auto;accent-color:var(--accent);}
input[type='range']{width:100%; cursor:pointer;accent-color:var(--accent);}
.btn{background:var(--accent);color:#1a1406;font-weight:650;border:none;cursor:pointer;padding:12px;border-radius:var(--r-sm);width:100%;margin-bottom:10px;min-height:44px; transition: filter 0.15s, transform 0.08s;}
.btn:hover{filter:brightness(1.08);}
.btn:active{transform:translateY(1px);}
.btn-blue{background:var(--charge);color:#fff;} .btn-red{background:var(--crit);color:#fff;} .btn-gray{background:#4b5563;color:#fff;}
.toast{visibility:hidden;min-width:250px;background-color:var(--good);color:#fff;text-align:center;border-radius:8px;padding:16px;position:fixed;z-index:1;left:50%;bottom:30px;transform:translateX(-50%);box-shadow:var(--shadow);}
.toast.show{visibility:visible;animation:fadein 0.5s, fadeout 0.5s 2.5s;}
@keyframes fadein{from{bottom:0;opacity:0;}to{bottom:30px;opacity:1;}} @keyframes fadeout{from{bottom:30px;opacity:1;}to{bottom:0;opacity:0;}}
h3{margin-top:0;} .cb-lbl{display:flex; align-items:center; gap:10px; margin-bottom:10px; font-size:16px;}
.box{background:var(--surface-2); padding:12px; border-radius:var(--r-sm); font-size:14px; line-height:1.6; border:1px solid var(--border); transition: background 0.3s, border-color 0.3s;}
.box-title{color:var(--accent); margin:0 0 8px 0; border-bottom:1px solid var(--border); padding-bottom:5px; font-weight:600;}

/* Dashboard-Tab: Karten, Ringe, Kacheln, Modale (Entwurf 3) */
.grid{display:grid;gap:16px}
.hero{grid-template-columns:minmax(0,240px) minmax(0,1fr);align-items:stretch}
.cols2{grid-template-columns:repeat(2,minmax(0,1fr))}
@media (max-width:720px){.hero,.cols2{grid-template-columns:1fr}}
.card{background:var(--surface);border:1px solid var(--border);border-radius:var(--r);box-shadow:var(--shadow);padding:16px}
.card h2{margin:0 0 2px;font-size:12px;font-weight:600;letter-spacing:.6px;text-transform:uppercase;color:var(--muted);display:flex;align-items:center;gap:7px}
.card h2 svg{width:14px;height:14px;color:var(--accent)}
.card .hint{font-size:12px;color:var(--muted);margin:2px 0 12px}
.batt-card{display:flex;flex-direction:column;align-items:center;justify-content:center;text-align:center;gap:5px}
.ring-wrap{position:relative;width:150px;height:150px}
.ring-wrap svg{transform:rotate(-90deg);display:block}
.ring-wrap.clickable,.tile.clickable,.barw.clickable{cursor:pointer}
.barw.clickable:hover{border-color:var(--accent)}
.ring-wrap.clickable:focus-visible{outline:2px solid var(--accent-2);outline-offset:4px;border-radius:99px}
.tile.clickable:hover{border-color:var(--accent)}
.tile.clickable:focus-visible{outline:2px solid var(--accent-2);outline-offset:2px}
.ring-track{fill:none;stroke:var(--surface-2);stroke-width:11}
.ring-val{fill:none;stroke-width:11;stroke-linecap:round;transition:stroke-dashoffset .8s cubic-bezier(.4,0,.2,1),stroke .4s}
.ring-center{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:1px}
.ring-center .pct{font-size:32px;font-weight:600;font-family:var(--font-mono,monospace);letter-spacing:-1px;line-height:1}
.ring-center .pct span{font-size:15px;color:var(--muted)}
.ring-center .state{font-size:11px;color:var(--muted);display:flex;align-items:center;gap:4px}
.ring-center .state.charging{color:var(--charge)}
.eta{font-size:12px;color:var(--muted);margin-top:2px}
.eta b{color:var(--text)}
.volt-row{font-size:12px;color:var(--muted)}
.volt-row b{color:var(--text)}
.confidence{display:inline-flex;align-items:center;gap:5px;font-size:10px;color:var(--muted);background:var(--surface-2);border:1px solid var(--border);border-radius:99px;padding:3px 9px;margin-top:7px;cursor:help}
.confidence b{color:var(--accent)}
.confidence svg{width:10px;height:10px;color:var(--accent)}
.ring-row{display:flex;justify-content:space-around;gap:8px;flex-wrap:wrap;margin-top:4px}
.ring-mini{display:flex;flex-direction:column;align-items:center;gap:5px;flex:1;min-width:80px}
.ring-mini .rwrap{position:relative;width:72px;height:72px}
.ring-mini svg{transform:rotate(-90deg);display:block}
.ring-mini .track{fill:none;stroke:var(--surface-2);stroke-width:7}
.ring-mini .val{fill:none;stroke-width:7;stroke-linecap:round;transition:stroke-dashoffset .8s cubic-bezier(.4,0,.2,1)}
.ring-mini .rc{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;font-size:12.5px;font-weight:600}
.ring-mini .lbl{font-size:10px;letter-spacing:.3px;text-transform:uppercase;color:var(--muted);text-align:center}
.ring-mini .sub{font-size:10px;color:var(--muted);text-align:center}
.details-btn{margin-top:12px;width:100%;font-family:var(--font-ui);font-size:12.5px;font-weight:600;cursor:pointer;padding:9px 12px;border-radius:var(--r-sm);border:1px solid var(--border-strong);background:var(--surface-2);color:var(--text)}
.details-btn:hover{border-color:var(--accent)}
.chart-blk+.chart-blk{margin-top:12px}
.chart-blk .chead{display:flex;align-items:baseline;justify-content:space-between;margin-bottom:3px}
.chart-blk .chead .t{font-size:11px;color:var(--muted)}
.chart-blk .chead .now{font-size:13px;font-weight:600}
canvas.spark{width:100%;height:38px;display:block}
.tile-row{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-bottom:9px}
.tile-row3{grid-template-columns:1.3fr 1fr 1fr}
@media (max-width:520px){.tile-row3{grid-template-columns:1fr 1fr}}
.tile{background:var(--surface-2);border:1px solid var(--border);border-radius:var(--r-sm);padding:10px 11px;display:flex;flex-direction:column;gap:3px;min-width:0}
.tile .lbl{font-size:10px;letter-spacing:.4px;text-transform:uppercase;color:var(--muted)}
.tile .v{font-size:16px;font-weight:600}
.tile .v u{font-size:10.5px;text-decoration:none;color:var(--muted);margin-left:2px}
.wifi-line{display:flex;align-items:center;gap:7px}
.wifi-bars{display:flex;align-items:flex-end;gap:2px;height:13px}
.wifi-bars i{width:3px;background:var(--border-strong);border-radius:1px;display:block}
.wifi-bars i:nth-child(1){height:25%} .wifi-bars i:nth-child(2){height:50%}
.wifi-bars i:nth-child(3){height:75%} .wifi-bars i:nth-child(4){height:100%}
.wifi-bars i.on{background:var(--good)}
.pill{display:inline-flex;align-items:center;gap:6px;font-size:11px;padding:3px 8px;border-radius:99px;border:1px solid var(--border)}
.pill.on{color:var(--good);border-color:var(--good);background:color-mix(in srgb,var(--good) 12%,transparent)}
.pill .dot{width:6px;height:6px;border-radius:99px;background:currentColor}
.goal-row{display:flex;gap:8px;align-items:center;margin-top:10px}
.goal-row input{width:90px;flex:0 0 auto;margin:0}
.goal-row button{flex:0 0 auto;font-family:var(--font-ui);font-size:12px;font-weight:600;cursor:pointer;padding:8px 12px;border-radius:var(--r-sm);border:1px solid var(--border-strong);background:var(--surface-2);color:var(--text)}
.goal-row button:hover{border-color:var(--accent)}
.goal-bar{height:7px;border-radius:99px;background:var(--surface-2);border:1px solid var(--border);overflow:hidden;margin-top:8px}
.goal-bar i{display:block;height:100%;border-radius:99px;background:var(--good)}
.goal-txt{font-size:11px;color:var(--muted);margin-top:5px}
.veil{position:fixed;inset:0;background:rgba(10,13,18,.55);display:flex;align-items:center;justify-content:center;padding:18px;z-index:80;opacity:0;pointer-events:none;transition:opacity .18s}
.veil.show{opacity:1;pointer-events:auto}
.modal{background:var(--elev);border:1px solid var(--border-strong);border-radius:14px;box-shadow:var(--shadow);max-width:520px;width:100%;max-height:82vh;overflow:auto;padding:20px 20px 18px;position:relative;transform:translateY(6px);transition:transform .18s}
.veil.show .modal{transform:translateY(0)}
.modal h3{margin:0 0 2px;font-size:15px}
.modal .hint{font-size:12px;color:var(--muted);margin:2px 0 14px}
.modal .close{position:absolute;top:14px;right:14px;width:34px;height:34px;border-radius:8px;border:1px solid var(--border);background:var(--surface-2);color:var(--text);cursor:pointer}
.seg-title{font-size:10.5px;font-weight:700;letter-spacing:.5px;text-transform:uppercase;color:var(--accent);margin:16px 0 8px}
.seg-title:first-of-type{margin-top:0}
.dl .row,.frow{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:8px 0;border-top:1px solid var(--border)}
.dl .row:first-child,.frow:first-child{border-top:0}
.dl .k{font-size:12px;color:var(--muted)}
.dl .val{font-size:12.5px;text-align:right}
.frow{flex-wrap:wrap}
.frow .fname{font-family:monospace;font-size:12px}
.frow .fsize{font-size:11px;color:var(--muted)}
.fedit-btn{background:none;border:1px solid var(--border-strong);border-radius:6px;color:var(--muted);cursor:pointer;padding:3px 7px;font-size:10.5px}
.fedit-btn:hover{border-color:var(--accent);color:var(--text)}
.editor{display:none;margin:8px 0 2px;padding:10px;background:var(--surface-2);border:1px solid var(--border);border-radius:var(--r-sm);width:100%}
.editor.open{display:block}
.editor textarea{width:100%;min-height:100px;font-family:monospace;font-size:11.5px;background:var(--surface);border:1px solid var(--border-strong);border-radius:6px;padding:7px;resize:vertical;margin:0}
.editor .erow{display:flex;gap:8px;margin-top:7px;justify-content:flex-end}
.editor .erow button{font-family:var(--font-ui);font-size:11.5px;font-weight:600;cursor:pointer;padding:6px 11px;border-radius:6px;border:1px solid var(--border-strong);background:var(--surface);color:var(--text)}
.editor .erow button.primary{background:var(--accent);border-color:var(--accent);color:#1a1406}
.hist-row{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:7px 0;border-top:1px solid var(--border);font-size:12px}
.hist-row:first-child{border-top:0}
.hist-row .hmeta{color:var(--muted);font-size:11px}

/* Zentrierter Wrap statt voller Displaybreite (Entwurf 4) */
.wrap{max-width:1000px;margin:0 auto;padding:22px 4px 0}
header.dashhdr{display:flex;align-items:center;gap:14px;flex-wrap:wrap;padding:4px 2px 18px}
.brand{display:flex;align-items:center;gap:12px;margin-right:auto;min-width:0}
.logo{width:38px;height:38px;border-radius:11px;display:grid;place-items:center;flex:0 0 auto;position:relative;cursor:pointer;overflow:hidden;
  background:linear-gradient(150deg,var(--accent),color-mix(in srgb,var(--accent) 55%,var(--accent-2)));
  color:#12161c;font-weight:800;font-family:var(--font-ui);font-size:17px;
  box-shadow:0 4px 14px color-mix(in srgb,var(--accent) 40%,transparent)}
.logo img{width:100%;height:100%;object-fit:cover}
.brand-txt{min-width:0}
.brand-txt h1{font-size:16px;margin:0;font-weight:650;letter-spacing:.2px;cursor:text;border-radius:5px;padding:1px 4px;margin-left:-4px;color:var(--text)}
.brand-txt h1:hover,.brand-txt h1:focus{background:var(--surface-2);outline:none}
.icon-row{display:flex;gap:8px}
.icon-btn{width:36px;height:36px;border-radius:10px;border:1px solid var(--border);background:var(--surface);color:var(--text);cursor:pointer;display:grid;place-items:center;transition:border-color .15s,transform .1s;flex:0 0 auto}
.icon-btn:hover{border-color:var(--border-strong)}
.icon-btn:active{transform:translateY(1px)}
.icon-btn.hc{font-weight:700;font-size:11px;font-family:var(--font-ui)}
.icon-btn.danger{color:var(--crit)}
.icon-btn.danger:hover{border-color:var(--crit)}

/* Verdienst/Opportunitaet: 2x2-Kachelmatrix */
.earn-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:2px}
.earn-cell{background:var(--surface-2);border:1px solid var(--border);border-radius:var(--r-sm);padding:9px 10px;text-align:center}
.earn-cell .t{font-size:9.5px;text-transform:uppercase;letter-spacing:.4px;color:var(--faint)}
.earn-cell .v{font-size:14px;font-weight:700;color:var(--accent);margin-top:3px}

/* Fortschrittsbalken mit eingebettetem Text (Restzeit / Schritte+Ziel) */
.barw{position:relative;height:32px;border-radius:9px;background:var(--surface-2);border:1px solid var(--border);overflow:hidden;display:flex;align-items:center}
.barw .fill{position:absolute;left:0;top:0;bottom:0;border-radius:9px 0 0 9px;background:linear-gradient(90deg,var(--accent-2),var(--accent));transition:width .5s}
.barw .lead{position:relative;z-index:1;padding-left:10px;font-size:12px;font-weight:700;color:#141a10;white-space:nowrap}
.barw .trail{position:relative;z-index:1;margin-left:auto;padding-right:10px;font-size:11.5px;font-weight:600;color:var(--text);white-space:nowrap}

/* Klickbare Charts + Systemverlauf-Modal */
.chart-blk.clickable{cursor:pointer;border-radius:8px;padding:4px;margin:-4px -4px 8px;transition:background .15s}
.chart-blk.clickable:hover{background:var(--surface-2)}
.seg-toggle{display:flex;gap:6px;margin-bottom:12px}
.seg-toggle button{flex:1;font-family:var(--font-ui);font-size:12px;font-weight:600;cursor:pointer;padding:8px;border-radius:var(--r-sm);border:1px solid var(--border-strong);background:var(--surface-2);color:var(--muted)}
.seg-toggle button.active{background:var(--accent);border-color:var(--accent);color:#1a1406}
.range-row{display:flex;gap:6px;margin-bottom:10px;flex-wrap:wrap}
.range-row button{font-family:var(--font-ui);font-size:11.5px;font-weight:600;cursor:pointer;padding:6px 11px;border-radius:99px;border:1px solid var(--border-strong);background:var(--surface-2);color:var(--muted)}
.range-row button.active{background:var(--accent-soft,var(--surface-2));border-color:var(--accent);color:var(--text)}
.stat-row{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;margin-top:12px}
.stat-row .cell{background:var(--surface-2);border:1px solid var(--border);border-radius:var(--r-sm);padding:8px;text-align:center}
.stat-row .cell .t{font-size:9.5px;text-transform:uppercase;color:var(--faint)}
.stat-row .cell .v{font-size:14px;font-weight:700;margin-top:2px}
.hbar-chart{display:flex;align-items:flex-end;gap:2px;height:140px;padding:8px 4px 0;border-bottom:1px solid var(--border)}
.hbar-chart .col{flex:1;display:flex;flex-direction:column;align-items:center;justify-content:flex-end;height:100%;min-width:0}
.hbar-chart .col i{display:block;width:100%;background:linear-gradient(180deg,var(--accent-2),var(--good));border-radius:2px 2px 0 0;min-height:1px}
.hbar-chart .col span{font-size:8px;color:var(--faint);margin-top:4px;writing-mode:vertical-rl;transform:rotate(180deg);height:14px}

/* Speicher-Modal: NVS-Kategorien */
.catlist{display:flex;flex-direction:column;gap:0;margin-top:8px}
.catrow{display:flex;align-items:center;gap:10px;padding:6px 0}
.catrow .cname{font-size:12.5px;flex:0 0 128px}
.catrow .cbar{flex:1;height:6px;border-radius:99px;background:var(--surface-2);overflow:hidden}
.catrow .cbar i{display:block;height:100%;border-radius:99px;background:var(--accent)}
.catrow .ccount{font-size:11px;color:var(--muted);flex:0 0 60px;text-align:right}
.link-out{display:inline-flex;align-items:center;gap:6px;font-size:12.5px;text-decoration:none;margin-top:14px;font-weight:600;
  padding:9px 14px;border-radius:var(--r-sm);border:1px solid var(--border-strong);background:var(--surface-2);color:var(--text)}
.link-out:hover{border-color:var(--accent-2);color:var(--accent-2)}

/* Matrix-Easter-Egg: Code-Gate + Digital-Rain-Overlay */
.code-veil{position:fixed;inset:0;background:rgba(10,13,18,.55);display:flex;align-items:center;justify-content:center;padding:18px;z-index:150;opacity:0;pointer-events:none;transition:opacity .18s}
.code-veil.show{opacity:1;pointer-events:auto}
.code-modal{background:var(--elev);border:1px solid var(--border-strong);border-radius:14px;box-shadow:var(--shadow);max-width:280px;width:100%;padding:20px;text-align:center}
.code-modal h3{margin:0 0 4px;font-size:14px;color:var(--text)}
.code-modal .hint{font-size:11.5px;color:var(--muted);margin:0 0 12px}
.code-modal input{width:100%;font-family:monospace;font-size:22px;letter-spacing:6px;text-align:center;color:var(--text);background:var(--surface-2);border:1px solid var(--border-strong);border-radius:var(--r-sm);padding:10px;box-sizing:border-box}
.code-modal input.err{border-color:var(--crit);animation:shakeCode .3s}
@keyframes shakeCode{0%,100%{transform:translateX(0)}25%{transform:translateX(-6px)}75%{transform:translateX(6px)}}
.code-modal .row2{display:flex;gap:8px;margin-top:12px}
.code-modal .row2 button{flex:1;font-family:var(--font-ui);font-size:13px;font-weight:600;cursor:pointer;padding:9px;border-radius:var(--r-sm);border:1px solid var(--border-strong);background:var(--surface-2);color:var(--text)}
.code-modal .row2 button.primary{background:var(--accent);border-color:var(--accent);color:#1a1406}
#matrixOverlay{position:fixed;inset:0;background:#000;z-index:200;display:none}
#matrixOverlay.show{display:block}
#matrixCanvas{width:100%;height:100%;display:block}
#matrixQuote{position:absolute;left:50%;top:50%;transform:translate(-50%,-50%);color:#8aff8a;font-family:monospace;font-size:22px;text-align:center;
  text-shadow:0 0 12px #0f0,0 0 28px #0a0;opacity:0;transition:opacity 1.2s;letter-spacing:.5px;padding:0 20px;max-width:90vw}
#matrixQuote.show{opacity:.92}
#matrixExit{position:absolute;top:16px;right:16px;color:#6f6;background:rgba(0,20,0,.5);border:1px solid #0f0;border-radius:8px;padding:8px 12px;font-family:monospace;font-size:12px;cursor:pointer}

/* LIGHT MODE CSS */
body.light-mode { --bg:#eceef2; --surface:#ffffff; --surface-2:#f4f6f9; --elev:#ffffff; --border:#dde1e9; --border-strong:#c9cfda; --text:#171c24; --muted:#5b6675; --accent:#c9770a; --accent-2:#0c9c90; --good:#1a9d51; --warn:#c9770a; --crit:#d94339; --charge:#2b7fe0; --shadow:0 1px 2px rgba(20,28,40,.06),0 6px 20px rgba(20,28,40,.06); }
body.light-mode b[style*="color:#fff"] { color: #000 !important; }
body.light-mode pre { background: #f0f0f0 !important; color: #000 !important; border: 1px solid #ccc; }
/* ACCESSIBILITY: high contrast mode */
body.high-contrast { background:#000 !important; color:#fff !important; }
body.high-contrast .tabs, body.high-contrast .panel, body.high-contrast .box { background:#000 !important; border:2px solid #fff !important; box-shadow:none !important; }
body.high-contrast .tab { color:#fff !important; }
body.high-contrast .tab.active { background:#111 !important; border-bottom:3px solid #ffea00 !important; color:#ffea00 !important; }
body.high-contrast input, body.high-contrast select, body.high-contrast textarea { background:#000 !important; color:#fff !important; border:2px solid #fff !important; }
body.high-contrast .box-title { color:#ffea00 !important; border-bottom:1px solid #ffea00 !important; }
body.high-contrast .btn { border:2px solid #fff !important; }
body.high-contrast .card, body.high-contrast .tile, body.high-contrast .modal, body.high-contrast .editor { background:#000 !important; border:2px solid #fff !important; box-shadow:none !important; }
body.high-contrast .ring-track, body.high-contrast .track { stroke:#333 !important; }
body.high-contrast .card h2, body.high-contrast .seg-title, body.high-contrast .confidence b { color:#ffea00 !important; }
body.high-contrast .details-btn, body.high-contrast .goal-row button, body.high-contrast .fedit-btn { border:2px solid #fff !important; }
body.high-contrast .icon-btn, body.high-contrast .code-modal { border:2px solid #fff !important; background:#000 !important; }
body.high-contrast .catrow .cbar, body.high-contrast .barw { background:#111 !important; }
/* RESPONSIVE: hamburger for small screens */
.hamburger{display:none;background:var(--surface);color:var(--text);border:1px solid var(--border);font-size:28px;cursor:pointer;padding:10px 16px;width:100%;text-align:left;border-radius:var(--r-sm);margin-bottom:4px;}
@media(max-width:520px){.hamburger{display:block;} .tabs{display:none;flex-direction:column;} .tabs.open{display:flex;} .tab{text-align:left;padding:14px 18px;border-bottom:none;border-left:3px solid transparent;} .tab.active{border-left:3px solid var(--accent);border-bottom:none;}}
</style>
<script>
const I18N = {
    selectFirmware: { de:'Bitte Firmware-Datei waehlen!', en:'Please select a firmware file!', fr:'Veuillez selectionner un fichier firmware !' },
    otaSuccess: { de:'Erfolgreich! Neustart...', en:'Success! Rebooting...', fr:'Succes ! Redemarrage...' },
    otaError: { de:'Fehler!', en:'Error!', fr:'Erreur !' },
    fileVersion: { de:'Datei-Version', en:'File version', fr:'Version fichier' },
    current: { de:'Aktuell', en:'Current', fr:'Actuelle' },
    unknown: { de:'unbekannt', en:'unknown', fr:'inconnue' },
    notAvailable: { de:'nicht verfuegbar', en:'not available', fr:'indisponible' },
    stopBadAppsOk: { de:'Security Apps gestoppt.', en:'Security apps stopped.', fr:'Apps securite arretees.' },
    stopBadAppsFail: { de:'Stop fehlgeschlagen', en:'Stop failed', fr:'Echec de l\'arret' },
    scanWifiBusy: { de:'Scanne... (kann dauern)', en:'Scanning... (may take a while)', fr:'Scan en cours... (cela peut prendre du temps)' },
    scanWifiIdle: { de:'WLANs scannen', en:'Scan WiFis', fr:'Scanner les WiFis' },
    scanFoundNetworks: { de:'Netzwerke gefunden!', en:'networks found!', fr:'reseaux trouves !' },
    scanError: { de:'Fehler beim Scannen', en:'Error during scan', fr:'Erreur pendant le scan' },
    scanRouterBusy: { de:'Scanne Router...', en:'Scanning routers...', fr:'Scan des routeurs...' },
    scanRouterIdle: { de:'Router scannen', en:'Scan routers', fr:'Scanner les routeurs' },
    deauthConfirmPrefix: { de:'Deauth auf ', en:'Prepare deauth for ', fr:'Preparer deauth pour ' },
    deauthConfirmSuffix: { de:'? Danach 3s Safety-Countdown.', en:'? Then 3s safety countdown.', fr:' ? Puis compte a rebours securite de 3s.' },
    deauthStartedPrefix: { de:'Deauth Angriff auf ', en:'Deauth attack started on ', fr:'Attaque deauth lancee sur ' },
    chargingShort: { de:'Laedt', en:'Charging', fr:'Charge' },
    etaUnknown: { de:'--', en:'--', fr:'--' },
    etaToEmpty: { de:'bis leer', en:'to empty', fr:'jusqu\'a vide' },
    etaToFull: { de:'bis voll', en:'to full', fr:'jusqu\'a plein' },
    aodSaveError: { de:'AOD Speicherfehler: HTTP ', en:'AOD save error: HTTP ', fr:'Erreur de sauvegarde AOD : HTTP ' },
    aodNetError: { de:'AOD Netzwerkfehler: ', en:'AOD network error: ', fr:'Erreur reseau AOD : ' },
    learnSamples: { de:'gelernte Messungen', en:'learned samples', fr:'mesures apprises' },
    connModeClient: { de:'Client (Heim-WLAN)', en:'Client (home WiFi)', fr:'Client (WiFi domestique)' },
    connModeHotspot: { de:'Hotspot (eigene Uhr)', en:'Hotspot (own watch)', fr:'Point d\'accès (montre)' },
    connOngoing: { de:'jetzt', en:'now', fr:'maintenant' },
    connNoHistory: { de:'Noch keine Verbindungshistorie seit dem letzten Neustart.', en:'No connection history yet since last reboot.', fr:'Aucun historique de connexion depuis le dernier redémarrage.' },
    btActive: { de:'Aktiv', en:'Active', fr:'Actif' },
    btInactive: { de:'Inaktiv', en:'Inactive', fr:'Inactif' },
    btAdvOn: { de:'Sichtbar (Advertising)', en:'Visible (advertising)', fr:'Visible (annonce)' },
    btAdvOff: { de:'Nicht sichtbar', en:'Not visible', fr:'Non visible' },
    free: { de:'frei', en:'free', fr:'libre' },
    slotsFree: { de:'Slots frei', en:'slots free', fr:'emplacements libres' },
    edit: { de:'Bearbeiten', en:'Edit', fr:'Modifier' },
    cancel: { de:'Abbrechen', en:'Cancel', fr:'Annuler' },
    save: { de:'Speichern', en:'Save', fr:'Enregistrer' },
    saved: { de:'Gespeichert ✓', en:'Saved ✓', fr:'Enregistré ✓' },
    loading: { de:'Lädt…', en:'Loading…', fr:'Chargement…' },
    error: { de:'Fehler', en:'Error', fr:'Erreur' },
    etaLabelCharging: { de:'Voll in', en:'Full in', fr:'Plein dans' },
    etaLabelDischarging: { de:'Noch für', en:'Remaining', fr:'Restant' },
    battStateNormal: { de:'Akku', en:'Battery', fr:'Batterie' },
    stepsUnit: { de:'Schritte', en:'steps', fr:'pas' },
    of: { de:'von', en:'of', fr:'sur' },
    noFiles: { de:'Keine Dateien gefunden.', en:'No files found.', fr:'Aucun fichier trouvé.' },
    loadError: { de:'Fehler beim Laden — bitte erneut versuchen.', en:'Error loading — please try again.', fr:'Erreur de chargement — réessayez.' }
};
function t(k){
    var l=(window.UI_LANG===undefined?0:window.UI_LANG);
    var e=I18N[k];
    if(!e) return k;
    return l===1?e.en:(l===2?e.fr:e.de);
}
function i18n3(de,en,fr){ return (window.UI_LANG===1?en:(window.UI_LANG===2?fr:de)); }
function showTab(id, el){ document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active')); document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active')); document.getElementById(id).classList.add('active'); el.classList.add('active'); } 
function toggleTheme() { 
    document.body.classList.toggle('light-mode'); 
    var isLight = document.body.classList.contains('light-mode'); 
    localStorage.setItem('lightMode', isLight);
    document.getElementById('moon_icon').style.display = isLight ? 'none' : 'block';
    document.getElementById('sun_icon').style.display = isLight ? 'block' : 'none';
}
function toggleContrast() {
    document.body.classList.toggle('high-contrast');
    localStorage.setItem('highContrast', document.body.classList.contains('high-contrast'));
}
function applyThemePreset() { 
    var v = document.getElementById('themeSel').value; 
    if(v=='night') { setC('#000000','#ffffff','#ffea00','#00ff00','#333333','#111111','#ffffff','#222222','#ff0000'); } 
    if(v=='day') { setC('#ffffff','#000000','#0000ff','#ff0000','#cccccc','#eeeeee','#000000','#dddddd','#ff0000'); } 
    if(v=='cyber') { setC('#09090f','#00ffcc','#ff00ff','#00ffff','#112233','#050508','#ff00ff','#1a1a2e','#ff3333'); } 
    if(v=='hacker') { setC('#000000','#00ff00','#00ff00','#ffffff','#004400','#001100','#00ff00','#002200','#ff0000'); } 
} 
function setC(bg,fg,a1,a2,bat, hbg, hfg, abg, wrn) { 
    document.getElementsByName('cBG')[0].value=bg; document.getElementsByName('cFG')[0].value=fg; document.getElementsByName('cAcc1')[0].value=a1; document.getElementsByName('cAcc2')[0].value=a2; document.getElementsByName('cBat')[0].value=bat;
    document.getElementsByName('cHBG')[0].value=hbg; document.getElementsByName('cHFG')[0].value=hfg; document.getElementsByName('cABG')[0].value=abg; document.getElementsByName('cWrn')[0].value=wrn; 
} 
function saveForm(formId){ var data = new URLSearchParams(new FormData(document.getElementById(formId))); data.append('_form', formId); fetch('/save', {method:'POST', body:data}).then(()=>{ var t = document.getElementById('toast'); t.className = 'toast show'; setTimeout(function(){ t.className = t.className.replace('show', ''); }, 3000); }); return false; }
function saveTime(){ var data = new URLSearchParams(new FormData(document.getElementById('fTime'))); fetch('/settime', {method:'POST', body:data}).then(()=>{ var t = document.getElementById('toast'); t.className = 'toast show'; setTimeout(function(){ t.className = t.className.replace('show', ''); }, 3000); }); return false; }
function syncBrowserTime(){ var now = Math.floor(Date.now() / 1000); fetch('/api/sync_time?epoch=' + now, {method:'POST'}).then(function(r){ return r.json(); }).then(function(){ var t = document.getElementById('toast'); if(t){ t.className = 'toast show'; setTimeout(function(){ t.className = t.className.replace('show', ''); }, 3000); } }).catch(function(){}); }
window.addEventListener('DOMContentLoaded', function(){ setTimeout(syncBrowserTime, 800); });
function previewHTML() { var w = window.open('', '_blank'); w.document.write(document.getElementsByName('evHtml')[0].value); w.document.close(); }
async function fileSHA256(file) {
    var buf = await file.arrayBuffer();
    var hash = await crypto.subtle.digest('SHA-256', buf);
    var bytes = Array.from(new Uint8Array(hash));
    return bytes.map(function(b){ return b.toString(16).padStart(2, '0'); }).join('');
}
function parseVersionFromFilename(name) {
    var m = name.match(/v?(\d+\.\d+\.\d+)/i);
    return m ? m[1] : null;
}
function updateOtaMeta() {
    var file = document.getElementById('otafile').files[0];
    if(!file) return;
    var cur = document.getElementById('ota_cur_ver') ? document.getElementById('ota_cur_ver').innerText : '';
    var found = parseVersionFromFilename(file.name);
    var meta = document.getElementById('ota_meta');
    if(meta) meta.innerText = found
        ? (t('fileVersion') + ': ' + found + ' | ' + t('current') + ': ' + cur)
        : (t('fileVersion') + ': ' + t('unknown') + ' | ' + t('current') + ': ' + cur);
    fileSHA256(file).then(function(h){ var out = document.getElementById('ota_sha'); if(out) out.innerText = 'SHA-256: ' + h; }).catch(function(){ var out = document.getElementById('ota_sha'); if(out) out.innerText = 'SHA-256: ' + t('notAvailable'); });
}
function uploadOTA() {
    var file = document.getElementById('otafile').files[0];
    if(!file) { alert(t('selectFirmware')); return; }
    var fd = new FormData(); fd.append('update', file);
    var xhr = new XMLHttpRequest();
    document.getElementById('otaprog-container').style.display = 'block';
    xhr.upload.addEventListener('progress', function(e) {
        if(e.lengthComputable) {
            var pct = Math.round((e.loaded / e.total) * 100);
            document.getElementById('otaprog').style.width = pct + '%';
            document.getElementById('otaprog').innerHTML = pct + '%';
        }
    }, false);
    xhr.addEventListener('load', function() {
        if(xhr.status == 200) {
            document.getElementById('otaprog').innerHTML = t('otaSuccess');
            setTimeout(function(){ location.reload(); }, 6000);
        } else {
            document.getElementById('otaprog').innerHTML = t('otaError');
            document.getElementById('otaprog').style.background = 'red';
        }
    }, false);
    xhr.open('POST', '/update?size=' + file.size, true);
    xhr.send(fd);
}

function applyPortPreset(val) {
    var out = document.getElementsByName('nmapPorts')[0];
    if(!out) return;
    var map = {
        quick: '80,443',
        webdev: '80,443,3000,5173,8080,8443',
        full: '21,22,23,25,53,80,110,139,143,443,445,3389,5900,8080'
    };
    if(map[val]) out.value = map[val];
}

function saveAndRememberHacker() {
    var f = document.getElementById('fHacker1');
    if(f) {
        var data = {
            tgS0: (f.querySelector('[name=tgS0]')||{}).value || '',
            tgS1: (f.querySelector('[name=tgS1]')||{}).value || '',
            tgS2: (f.querySelector('[name=tgS2]')||{}).value || '',
            nmapIP: (f.querySelector('[name=nmapIP]')||{}).value || '',
            nmapPorts: (f.querySelector('[name=nmapPorts]')||{}).value || ''
        };
        localStorage.setItem('hackerRecent', JSON.stringify(data));
    }
    return saveForm('fHacker1');
}

function loadHackerRecent() {
    var raw = localStorage.getItem('hackerRecent');
    if(!raw) return;
    try {
        var d = JSON.parse(raw);
        var f = document.getElementById('fHacker1');
        if(!f) return;
        ['tgS0','tgS1','tgS2','nmapIP','nmapPorts'].forEach(function(k){
            var el = f.querySelector('[name='+k+']');
            if(el && d[k] && el.value.trim() === '') el.value = d[k];
        });
    } catch(e) {}
}

function stopBadApps() {
    fetch('/stopbad', {method:'POST'}).then(function(){ alert(t('stopBadAppsOk')); }).catch(function(){ alert(t('stopBadAppsFail')); });
}

function scanWifi() {
    var btn = document.getElementById('scanBtn'); btn.value = t('scanWifiBusy');
    fetch('/wifiscan').then(r=>r.json()).then(d=>{
        btn.value = t('scanWifiIdle');
        var dl = document.getElementById('scannedWifis'); dl.innerHTML = '';
        var unique = []; d.forEach(ssid => { if(unique.indexOf(ssid) === -1 && ssid.length > 0) { unique.push(ssid); var opt = document.createElement('option'); opt.value = ssid; dl.appendChild(opt); } });
        alert(unique.length + ' ' + t('scanFoundNetworks'));
    }).catch(e=>{ btn.value = t('scanWifiIdle'); alert(t('scanError')); });
}

function scanRouters() {
    var btn = document.getElementById('scanRBtn'); btn.value = t('scanRouterBusy');
    fetch('/routerscan').then(r=>r.json()).then(d=>{
        btn.value = t('scanRouterIdle');
        var t = document.getElementById('routerTable'); t.innerHTML = '';
        d.forEach(n => {
            var div = document.createElement('div');
            div.style = 'background:transparent; padding:10px; margin-bottom:5px; border:1px solid #555; border-radius:5px; display:flex; justify-content:space-between; align-items:center;';
            div.innerHTML = '<div><b>'+n.ssid+'</b><br><span style="font-size:11px;color:#aaa;">'+n.bssid+' (Ch: '+n.ch+')</span></div><button type="button" class="btn btn-red" style="margin:0; width:auto; padding:8px 15px;" onclick="deauthMac(\''+n.bssid+'\')">Deauth</button>';
            t.appendChild(div);
        });
    });
}
function deauthMac(mac) {
    var c = 3;
    var ok = confirm(t('deauthConfirmPrefix') + mac + t('deauthConfirmSuffix'));
    if(!ok) return;
    var iv = setInterval(function(){
        c--;
        if(c <= 0) {
            clearInterval(iv);
            fetch('/deauth?mac='+mac);
            alert(t('deauthStartedPrefix') + mac + '!');
        }
    }, 1000);
}

let liveInFlight = false;
let liveTimer = null;
function scheduleLive(ms){ if(liveTimer) clearTimeout(liveTimer); liveTimer = setTimeout(updateLive, ms); }
function setHtmlIfChanged(id, html){ var el=document.getElementById(id); if(el && el.innerHTML!==html) el.innerHTML=html; }
function setTextIfChanged(id, txt){ var el=document.getElementById(id); if(el && el.innerText!==txt) el.innerText=txt; }

function fmtEta(mins, mode){
    if(mins === undefined || mins === null || mins < 0) return t('etaUnknown');
    var h = Math.floor(mins / 60);
    var m = mins % 60;
    var base = h > 0 ? (h + 'h ' + m + 'm') : (m + 'm');
    if(mode === 'charge') return base + ' ' + t('etaToFull');
    if(mode === 'discharge') return base + ' ' + t('etaToEmpty');
    return base;
}

// ---------- Dashboard-Tab: Ring/Sparkline-Helfer (Entwurf 3) ----------
var ringC = null, ringRamC = null, ringFlashC = null, ringNvsC = null;
function ringSetup(){
    var ring=document.getElementById('ring'); if(ring){ ringC=2*Math.PI*64; ring.style.strokeDasharray=ringC; }
    var rr=document.getElementById('ringRam'); if(rr){ ringRamC=2*Math.PI*30; rr.style.strokeDasharray=ringRamC; }
    var rf=document.getElementById('ringFlash'); if(rf){ ringFlashC=2*Math.PI*30; rf.style.strokeDasharray=ringFlashC; }
    var rn=document.getElementById('ringNvs'); if(rn){ ringNvsC=2*Math.PI*30; rn.style.strokeDasharray=ringNvsC; }
}
function setRing(id, c, pct){ var el=document.getElementById(id); if(el && c) el.style.strokeDashoffset = c*(1-Math.max(0,Math.min(100,pct))/100); }
function ringColorFor(p, ch){
    var s=getComputedStyle(document.documentElement);
    if(ch) return s.getPropertyValue('--charge');
    if(p>50) return s.getPropertyValue('--good');
    if(p>20) return s.getPropertyValue('--warn');
    return s.getPropertyValue('--crit');
}

var histFreq=[], histTemp=[], sparkFit=false;
function drawSpark(cv,hist,minV,maxV){
    if(!cv) return; var ctx=cv.getContext('2d');
    var s=getComputedStyle(document.documentElement);
    var w=cv.width,h=cv.height,pad=5;
    ctx.clearRect(0,0,w,h);
    ctx.strokeStyle=s.getPropertyValue('--border'); ctx.lineWidth=1;
    for(var g=0;g<=2;g++){ var y=pad+(h-2*pad)*g/2; ctx.beginPath(); ctx.moveTo(0,y); ctx.lineTo(w,y); ctx.stroke(); }
    if(hist.length<2) return;
    var n=hist.length, step=w/(n-1);
    function X(i){return i*step;} function Y(v){ var p=(v-minV)/(maxV-minV); return pad+(h-2*pad)*(1-Math.max(0,Math.min(1,p))); }
    var acc=s.getPropertyValue('--accent-2').trim();
    function hexA(hex,a){ hex=hex.trim(); if(!hex||hex[0]!=='#')return 'rgba(150,150,150,'+a+')'; var r=parseInt(hex.slice(1,3),16),gg=parseInt(hex.slice(3,5),16),b=parseInt(hex.slice(5,7),16); return 'rgba('+r+','+gg+','+b+','+a+')'; }
    var grad=ctx.createLinearGradient(0,0,0,h);
    grad.addColorStop(0, hexA(acc,.30)); grad.addColorStop(1, hexA(acc,0));
    ctx.beginPath(); ctx.moveTo(0,h);
    for(var i=0;i<n;i++){ ctx.lineTo(X(i),Y(hist[i])); }
    ctx.lineTo(w,h); ctx.closePath(); ctx.fillStyle=grad; ctx.fill();
    ctx.beginPath(); for(var j=0;j<n;j++){ var x=X(j),y=Y(hist[j]); j?ctx.lineTo(x,y):ctx.moveTo(x,y); }
    ctx.strokeStyle=acc; ctx.lineWidth=2; ctx.lineJoin='round'; ctx.stroke();
    ctx.beginPath(); ctx.arc(X(n-1),Y(hist[n-1]),3,0,7); ctx.fillStyle=acc; ctx.fill();
}
function fitSparks(){
    ['sparkFreq','sparkTemp'].forEach(function(id){ var cv=document.getElementById(id); if(!cv) return; var r=cv.getBoundingClientRect(); if(r.width>0){ cv.width=Math.round(r.width*2); cv.height=76; } });
    drawSpark(document.getElementById('sparkFreq'), histFreq, 60, 260);
    drawSpark(document.getElementById('sparkTemp'), histTemp, 30, 55);
    sparkFit = true;
}
addEventListener('resize', function(){ if(document.getElementById('sparkFreq')) fitSparks(); });

// ---------- generische Modal-Helfer ----------
function wireModal(veilId, openIds, closeId, onOpen){
    var veil=document.getElementById(veilId); if(!veil) return;
    function openM(){ veil.classList.add('show'); if(onOpen) onOpen(); }
    function closeM(){ veil.classList.remove('show'); }
    openIds.forEach(function(id){
        var el=document.getElementById(id); if(!el) return;
        el.addEventListener('click', openM);
        el.addEventListener('keydown', function(e){ if(e.key==='Enter'||e.key===' '){ e.preventDefault(); openM(); } });
    });
    var cb=document.getElementById(closeId); if(cb) cb.addEventListener('click', closeM);
    veil.addEventListener('click', function(e){ if(e.target===veil) closeM(); });
    modalClosers.push(closeM);
}
var modalClosers=[];
document.addEventListener('keydown', function(e){ if(e.key==='Escape') modalClosers.forEach(function(c){c();}); });

function loadBatteryDetail(){
    fetch('/api/battery/detail', {cache:'no-store'}).then(r=>r.json()).then(function(d){
        setTextIfChanged('mdVolt', d.volt + ' V');
        setTextIfChanged('mdEta', fmtEta(d.eta_min, d.eta_mode));
        setTextIfChanged('mdConf', d.eta_conf + '% (' + (d.samples_normal+d.samples_hacking+d.samples_charging) + ' ' + t('learnSamples') + ')');
        setTextIfChanged('mdRateNormal', '-' + d.rate_normal.toFixed(1) + '%/h');
        setTextIfChanged('mdRateHacking', '-' + d.rate_hacking.toFixed(1) + '%/h');
        setTextIfChanged('mdRateCharging', '+' + d.rate_charging.toFixed(1) + '%/h');
    }).catch(function(){});
}
function loadWifiDetail(){
    fetch('/api/wifi/detail', {cache:'no-store'}).then(r=>r.json()).then(function(d){
        setTextIfChanged('cnMode', d.mode==='client' ? t('connModeClient') : t('connModeHotspot'));
        setTextIfChanged('cnSsid', d.ssid);
        setTextIfChanged('cnIp', d.ip);
        var list=document.getElementById('cnHistList'); if(!list) return;
        list.innerHTML='';
        for(var i=d.history.length-1;i>=0;i--){
            var e=d.history[i];
            var dur=Math.max(0, e.end-e.start); var durMin=Math.round(dur/60);
            var row=document.createElement('div'); row.className='hist-row';
            row.innerHTML='<div><b>'+e.ssid+'</b><div class="hmeta">'+new Date(e.start*1000).toLocaleString()+(e.ongoing?(' \u2013 '+t('connOngoing')):'')+'</div></div><div>'+(durMin<1?'<1':durMin)+'min \u00B7 \u00D8'+e.avg_pct+'%</div>';
            list.appendChild(row);
        }
        if(d.history.length===0) list.innerHTML='<div class="hist-row hmeta">'+t('connNoHistory')+'</div>';
    }).catch(function(){});
}
function toggleWifiApi(){
    fetch('/api/wifi/toggle', {method:'POST'}).then(r=>r.json()).then(function(d){
        alert(d.wifi ? 'WLAN aktiviert!' : 'WLAN deaktiviert!');
        loadWifiDetail();
    }).catch(function(e){ alert('Fehler beim Umschalten'); });
}
function switchWifiModeApi(){
    fetch('/api/wifi/mode', {method:'POST'}).then(r=>r.json()).then(function(d){
        alert('Neuer Modus: ' + d.mode);
        setTimeout(function(){ location.reload(); }, 1500);
    }).catch(function(e){ alert('Fehler beim Moduswechsel'); });
}
function scanWifiApi(){
    var resDiv = document.getElementById('wifiScanResult');
    if(!resDiv) return;
    resDiv.innerHTML = '<div class="hint" style="margin:8px 0;">Scanne Netzwerke...</div>';
    fetch('/api/wifi/scan').then(r=>r.json()).then(function(d){
        if(!d.networks || d.networks.length === 0){
            resDiv.innerHTML = '<div class="hint" style="margin:8px 0;">Keine Netzwerke gefunden.</div>';
            return;
        }
        var h = '<div class="dl" style="max-height:160px; overflow-y:auto; margin:8px 0;">';
        d.networks.forEach(function(n){
            h += '<div class="row"><span class="k"><b>' + n.ssid + '</b> <span style="font-size:11px;color:var(--muted);">(Ch ' + n.chan + ')</span></span><span class="val mono">' + n.rssi + ' dBm</span></div>';
        });
        h += '</div>';
        resDiv.innerHTML = h;
    }).catch(function(e){ resDiv.innerHTML = '<div class="hint" style="color:var(--crit);margin:8px 0;">Scan-Fehler</div>'; });
}
function loadBtDetail(){
    fetch('/api/bt/detail', {cache:'no-store'}).then(r=>r.json()).then(function(d){
        setTextIfChanged('btmStatus', d.active ? t('btActive') : t('btInactive'));
        setTextIfChanged('btmAdv', d.advertising ? t('btAdvOn') : t('btAdvOff'));
        setTextIfChanged('btmName', d.device_name);
    }).catch(function(){});
    fetch('/api/status', {cache:'no-store'}).then(r=>r.json()).then(function(s){
        if(s.phone_name !== undefined){
            setTextIfChanged('phoneDevName', s.phone_name.length > 0 ? s.phone_name : 'Kein Smartphone gekoppelt');
            setTextIfChanged('phoneDevMac', s.phone_mac.length > 0 ? s.phone_mac : '--:--:--:--:--:--');
            var stEl = document.getElementById('phoneDevStatus');
            if(stEl) stEl.innerHTML = s.phone_connected ? '<span style="color:var(--good);font-weight:600;">Verbunden ✓</span>' : '<span style="color:var(--muted);">Getrennt</span>';
        }
    }).catch(function(){});
}
function toggleBtApi(){
    fetch('/api/bt/toggle', {method:'POST'}).then(r=>r.json()).then(function(d){
        alert(d.bt ? 'Bluetooth aktiviert!' : 'Bluetooth deaktiviert!');
        loadBtDetail();
    }).catch(function(e){ alert('Fehler beim Umschalten'); });
}
function scanBleApi(){
    var resDiv = document.getElementById('bleScanResult');
    if(!resDiv) return;
    resDiv.innerHTML = '<div class="hint" style="margin:8px 0;">Scanne BLE-Geräte...</div>';
    fetch('/api/bt/scan').then(r=>r.json()).then(function(d){
        if(!d.devices || d.devices.length === 0){
            resDiv.innerHTML = '<div class="hint" style="margin:8px 0;">Keine BLE-Geräte gefunden.</div>';
            return;
        }
        var h = '<div class="dl" style="max-height:220px; overflow-y:auto; margin:8px 0;">';
        d.devices.forEach(function(dev){
            var nm = dev.name && dev.name.length > 0 ? dev.name : 'Unbekanntes BLE-Gerät';
            h += '<div class="row" style="align-items:center;padding:6px 0;"><span class="k"><b>' + nm + '</b><br><small class="mono">' + dev.mac + ' (' + dev.rssi + ' dBm)</small></span>' +
                 '<span class="val"><button type="button" class="btn" style="padding:4px 8px;font-size:11px;margin:0;background:var(--accent);color:#fff;border-radius:6px;" onclick="pairPhoneApi(\'' + dev.mac + '\', \'' + encodeURIComponent(nm) + '\')">Als Smartphone koppeln</button></span></div>';
        });
        h += '</div>';
        resDiv.innerHTML = h;
    }).catch(function(e){ resDiv.innerHTML = '<div class="hint" style="color:var(--crit);margin:8px 0;">Scan-Fehler</div>'; });
}
function pairPhoneApi(mac, nameEnc){
    var name = decodeURIComponent(nameEnc);
    fetch('/api/bt/pair', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'mac=' + encodeURIComponent(mac) + '&name=' + encodeURIComponent(name)
    }).then(r=>r.json()).then(function(d){
        alert('Smartphone gekoppelt: ' + name);
        loadBtDetail();
    }).catch(function(e){ alert('Fehler beim Koppeln'); });
}
function unpairPhoneApi(){
    if(!confirm('Smartphone wirklich entkoppeln?')) return;
    fetch('/api/bt/unpair', {method:'POST'}).then(r=>r.json()).then(function(d){
        alert('Smartphone entkoppelt.');
        loadBtDetail();
    }).catch(function(e){ alert('Fehler beim Entkoppeln'); });
}
function pingPhoneApi(){
    fetch('/api/notify', {
        method:'POST',
        headers:{'Content-Type':'application/x-www-form-urlencoded'},
        body:'title=WatchOS+Ping&msg=Smartphone-Verbindung+aktiv'
    }).then(r=>r.json()).then(function(d){
        alert('Ping gesendet!');
    }).catch(function(){ alert('Ping gesendet!'); });
}
function connectWebBle(){
    if(!navigator.bluetooth){
        alert('Web Bluetooth wird von diesem Browser nicht unterstützt. Bitte Chrome, MS Edge oder Bluefy auf iOS/Android nutzen!');
        return;
    }
    var st=document.getElementById('webBleStatus');
    if(st) st.innerHTML='<span style="color:var(--accent);">Suche WatchOS BLE...</span>';
    navigator.bluetooth.requestDevice({
        acceptAllDevices: true,
        optionalServices: ['battery_service', 'device_information']
    }).then(function(dev){
        if(st) st.innerHTML='<span style="color:var(--accent);">Verbinde mit ' + dev.name + '...</span>';
        return dev.gatt.connect();
    }).then(function(srv){
        alert('Erfolgreich per Web-Bluetooth verbunden mit: ' + (srv.device.name || 'WatchOS'));
        if(st) st.innerHTML='<span style="color:var(--good);font-weight:600;">✓ BLE Aktiv (' + (srv.device.name || 'WatchOS') + ')</span>';
    }).catch(function(e){
        if(e.name !== 'NotFoundError'){
            alert('Bluetooth Verbindung abgebrochen: ' + e.message);
            if(st) st.innerHTML='<span style="color:var(--crit);">' + e.message + '</span>';
        } else {
            if(st) st.innerHTML='';
        }
    });
}
var curWfMode = 0;
var wfNames = ["Ultra Rings", "Classic Chrono", "Retro Casio", "3D Lebensuhr"];
function updateWfUI(mode){
    curWfMode = mode;
    var bName = document.getElementById('activeWfName');
    if (bName) bName.textContent = wfNames[mode] || ("Modus " + mode);
    var p2Name = document.getElementById('p2WfName');
    if (p2Name) p2Name.textContent = wfNames[mode] || ("Modus " + mode);
    for (var i = 0; i < 4; i++) {
        var c = document.getElementById('wfCard' + i);
        var b = document.getElementById('wfBadge' + i);
        var btn = document.getElementById('wfBtn' + i);
        if (!c) continue;
        if (i === mode) {
            c.style.borderColor = 'var(--accent)';
            c.style.background = 'rgba(59,130,246,0.12)';
            if (b) { b.textContent = 'AKTIV'; b.style.background = 'var(--accent)'; b.style.color = '#fff'; }
            if (btn) { btn.textContent = 'Aktiv ✓'; btn.style.background = 'var(--accent)'; btn.style.color = '#fff'; }
        } else {
            c.style.borderColor = 'var(--border)';
            c.style.background = 'var(--surface-2)';
            if (b) { b.textContent = 'Bereit'; b.style.background = 'var(--surface)'; b.style.color = 'var(--muted)'; }
            if (btn) { btn.textContent = 'Aktivieren'; btn.style.background = 'var(--surface-2)'; btn.style.color = 'var(--text)'; }
        }
    }
}
function selectWatchface(mode){
    updateWfUI(mode);
    fetch('/api/watchface', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'mode=' + mode
    }).then(r => r.json()).then(d => {
        if (d.mode !== undefined) updateWfUI(d.mode);
        showToastMsg && showToastMsg('Watchface: ' + (wfNames[mode] || mode));
    }).catch(function(e){ console.log('WF err', e); });
}
function fsSizeStr(b){
    if(b < 1024) return b + ' B';
    if(b < 1024*1024) return (b/1024).toFixed(1) + ' KB';
    return (b/(1024*1024)).toFixed(2) + ' MB';
}
function newFilePrompt(){
    var p = prompt('Dateiname mit fuehrendem Slash angeben (z.B. /notizen.txt):', '/');
    if(!p || p.length < 2 || p[0] !== '/') return;
    var c = prompt('Optionaler Textinhalt fuer ' + p + ':', '');
    if(c === null) return;
    var body = 'path=' + encodeURIComponent(p) + '&content=' + encodeURIComponent(c);
    fetch('/api/fs/create', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:body})
    .then(r => r.json()).then(function(d){
        alert('Datei erstellt: ' + p);
        loadStorageDetail();
    }).catch(function(e){ alert('Fehler beim Erstellen der Datei'); });
}
function loadStorageDetail(){
    var list=document.getElementById('ffatList');
    if(list) list.innerHTML='<div class="hint">'+t('loading')+'</div>';
    fetch('/api/fs/list', {cache:'no-store'}).then(function(r){
        if(!r.ok) throw new Error('HTTP '+r.status);
        return r.json();
    }).then(function(d){
        var pct = d.flash_tot_kb>0 ? Math.round((d.flash_tot_kb-d.flash_free_kb)/d.flash_tot_kb*100) : 0;
        var bar=document.getElementById('stFlashUsedBar'); if(bar) bar.style.width=pct+'%';
        setTextIfChanged('stFlashTxt', fsSizeStr(d.flash_free_kb*1024) + ' ' + t('free') + ' / ' + fsSizeStr(d.flash_tot_kb*1024));
        var nvsPct = d.nvs_tot>0 ? Math.round((d.nvs_tot-d.nvs_free)/d.nvs_tot*100) : 0;
        var nbar=document.getElementById('stNvsUsedBar'); if(nbar) nbar.style.width=nvsPct+'%';
        setTextIfChanged('stNvsTxt', d.nvs_free + ' / ' + d.nvs_tot + ' ' + t('slotsFree'));
        var catList=document.getElementById('nvsCatList');
        if(catList && d.categories){
            catList.innerHTML='';
            d.categories.forEach(function(c){
                var cpct = c.total>0 ? Math.round(c.count/c.total*100) : 0;
                var row=document.createElement('div'); row.className='catrow';
                row.innerHTML='<span class="cname">'+c.name+'</span><div class="cbar"><i style="width:'+cpct+'%"></i></div><span class="ccount">'+c.count+'/'+c.total+'</span>';
                catList.appendChild(row);
            });
        }
        if(!list) return;
        list.innerHTML='';
        if(!d.files || d.files.length===0){ list.innerHTML='<div class="hint">'+t('noFiles')+'</div>'; return; }
        d.files.forEach(function(f){
            var row=document.createElement('div'); row.className='frow'; row.setAttribute('data-file', f.name);
            row.innerHTML='<div class="frow-main" style="display:flex;align-items:center;justify-content:space-between;gap:8px;width:100%;flex-wrap:wrap;">'+
                '<span class="fname" style="font-weight:600;color:var(--text);">' + f.name + '</span>'+
                '<div style="display:flex;align-items:center;gap:6px;">'+
                '<span class="fsize mono" style="font-size:11px;color:var(--muted);">' + fsSizeStr(f.size) + '</span>'+
                '<a href="/api/fs/read?path=' + encodeURIComponent(f.name) + '" download style="padding:3px 8px;font-size:11px;background:var(--surface-2);border:1px solid var(--border);color:var(--text);border-radius:6px;text-decoration:none;">Download</a>'+
                '<button type="button" class="fedit-btn" style="padding:3px 8px;font-size:11px;border-radius:6px;">' + t('edit') + '</button>'+
                '<button type="button" class="fdel-btn" style="padding:3px 8px;font-size:11px;background:#ef444422;border:1px solid #ef4444;color:#ef4444;border-radius:6px;cursor:pointer;">Löschen</button>'+
                '</div></div>'+
                '<div class="editor"><textarea></textarea><div class="erow"><button type="button" class="cancel">'+t('cancel')+'</button><button type="button" class="primary save">'+t('save')+'</button></div></div>';
            list.appendChild(row);
            var btn=row.querySelector('.fedit-btn'), delBtn=row.querySelector('.fdel-btn'), ed=row.querySelector('.editor'), ta=row.querySelector('textarea');
            btn.addEventListener('click', function(){
                if(ed.classList.contains('open')){ ed.classList.remove('open'); return; }
                ta.value=t('loading');
                fetch('/api/fs/read?path='+encodeURIComponent(f.name), {cache:'no-store'}).then(function(r){ return r.text(); }).then(function(txt){ ta.value=txt; ed.classList.add('open'); }).catch(function(){ ta.value=t('error'); });
            });
            delBtn.addEventListener('click', function(){
                if(!confirm('Datei wirklich löschen: ' + f.name + ' ?')) return;
                fetch('/api/fs/delete', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'path='+encodeURIComponent(f.name)})
                .then(r=>r.json()).then(function(d){
                    loadStorageDetail();
                }).catch(function(e){ alert('Löschen fehlgeschlagen'); });
            });
            row.querySelector('.cancel').addEventListener('click', function(){ ed.classList.remove('open'); });
            row.querySelector('.save').addEventListener('click', function(){
                var body='path='+encodeURIComponent(f.name)+'&content='+encodeURIComponent(ta.value);
                fetch('/api/fs/write', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:body}).then(function(r){
                    var old=btn.textContent; btn.textContent = r.ok ? t('saved') : t('error');
                    setTimeout(function(){ btn.textContent=old; }, 1800);
                    if(r.ok) ed.classList.remove('open');
                }).catch(function(){ btn.textContent=t('error'); setTimeout(function(){ btn.textContent=t('edit'); },1800); });
            });
        });
    }).catch(function(e){
        if(list) list.innerHTML='<div class="hint" style="color:var(--crit)">'+t('loadError')+'</div>';
    });
}

// ---------- Systemverlauf (echte Ringpuffer-Historie ueber /api/history/system) ----------
var sysHistMode='freq', sysHistRange='24h';
function renderSysHist(){
    document.getElementById('sysTabFreq').classList.toggle('active', sysHistMode==='freq');
    document.getElementById('sysTabTemp').classList.toggle('active', sysHistMode==='temp');
    var n = sysHistRange==='6h'?24 : sysHistRange==='24h'?96 : sysHistRange==='72h'?288 : 0; // 0 = alle vorhandenen (15min-Takt)
    fetch('/api/history/system?n='+n, {cache:'no-store'}).then(r=>r.json()).then(function(d){
        var hist = sysHistMode==='freq' ? d.freq : d.temp;
        if(!hist || hist.length===0) hist=[0];
        var minV = sysHistMode==='freq'?60:30, maxV = sysHistMode==='freq'?260:52;
        var cv=document.getElementById('sparkSysHistBig');
        var r=cv.getBoundingClientRect(); cv.width=Math.round(r.width*2); cv.height=180;
        drawSpark(cv, hist, minV, maxV);
        var mn=Math.min.apply(null,hist), mx=Math.max.apply(null,hist), avg=hist.reduce(function(a,b){return a+b;},0)/hist.length;
        var unit = sysHistMode==='freq' ? ' MHz' : ' °C';
        var fmt = function(v){ return sysHistMode==='freq' ? Math.round(v) : v.toFixed(1).replace('.',','); };
        document.getElementById('sysHistMin').textContent=fmt(mn)+unit;
        document.getElementById('sysHistAvg').textContent=fmt(avg)+unit;
        document.getElementById('sysHistMax').textContent=fmt(mx)+unit;
    }).catch(function(){});
}
function initSysHist(){
    document.getElementById('sysTabFreq').addEventListener('click', function(){ sysHistMode='freq'; renderSysHist(); });
    document.getElementById('sysTabTemp').addEventListener('click', function(){ sysHistMode='temp'; renderSysHist(); });
    document.querySelectorAll('#sysHistVeil .range-row button').forEach(function(b){
        b.addEventListener('click', function(){
            document.querySelectorAll('#sysHistVeil .range-row button').forEach(function(x){x.classList.remove('active');});
            b.classList.add('active'); sysHistRange=b.getAttribute('data-range'); renderSysHist();
        });
    });
}

// ---------- Schritt-Verlauf: Balkendiagramm je Stunde (echte Ringpuffer-Daten) ----------
function renderStepHist(){
    var chart=document.getElementById('stepHistChart');
    chart.innerHTML='<div class="hint">'+t('loading')+'</div>';
    fetch('/api/history/steps', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(d){
        var hourly=d.hourly||[]; var max=Math.max.apply(null,hourly.concat([1]));
        var total=hourly.reduce(function(a,b){return a+b;},0);
        var peakIdx=hourly.indexOf(Math.max.apply(null,hourly));
        chart.innerHTML='';
        hourly.forEach(function(v,h){
            var col=document.createElement('div'); col.className='col';
            var pct = max>0 ? Math.round(v/max*100) : 0;
            col.innerHTML='<i style="height:'+Math.max(pct,v>0?3:0)+'%" title="'+h+':00 – '+v+' '+t('stepsUnit')+'"></i><span>'+(h%3===0?h:'')+'</span>';
            chart.appendChild(col);
        });
        setTextIfChanged('stepHistTotal', total.toLocaleString('de-DE'));
        setTextIfChanged('stepHistPeak', total>0 ? (peakIdx+':00 (' + hourly[peakIdx] + ')') : '--');
    }).catch(function(){ chart.innerHTML='<div class="hint" style="color:var(--crit)">'+t('loadError')+'</div>'; });
}

// ---------- Branding: Name umbenennen + eigenes Icon hochladen ----------
function initBranding(){
    var nameEl=document.getElementById('brandName');
    nameEl.addEventListener('blur', function(){
        var v=nameEl.textContent.trim(); if(!v) { nameEl.textContent='WatchOS'; v='WatchOS'; }
        fetch('/save', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'_form=fBrand&brandName='+encodeURIComponent(v)});
    });
    nameEl.addEventListener('keydown', function(e){ if(e.key==='Enter'){ e.preventDefault(); nameEl.blur(); } });
    document.getElementById('brandLogo').addEventListener('click', function(e){
        if(e.detail===0) return; // Tastatur-Fokus soll nicht sofort den Dateidialog oeffnen
        document.getElementById('brandFile').click();
    });
    document.getElementById('brandFile').addEventListener('change', function(e){
        var f=e.target.files[0]; if(!f) return;
        var fd=new FormData(); fd.append('icon', f, f.name);
        fetch('/api/brand/icon', {method:'POST', body:fd}).then(function(r){
            if(r.ok){ var img=document.getElementById('brandIconImg'); img.src='/api/brand/icon?_=' + Date.now(); }
        });
    });
}

// ---------- Matrix-Easter-Egg: "M"-Button + Code-Gate (6174) + Digital-Rain mit Zitaten ----------
function initMatrixEgg(){
    var codeVeil=document.getElementById('codeVeil'), codeInput=document.getElementById('codeInput');
    document.getElementById('matrixBtn').addEventListener('click', function(){
        codeInput.value=''; codeInput.classList.remove('err'); codeVeil.classList.add('show'); codeInput.focus();
    });
    document.getElementById('codeCancel').addEventListener('click', function(){ codeVeil.classList.remove('show'); });
    codeVeil.addEventListener('click', function(e){ if(e.target===codeVeil) codeVeil.classList.remove('show'); });
    function tryCode(){
        if(codeInput.value.trim()==='6174'){
            codeVeil.classList.remove('show');
            fetch('/matrix', {method:'POST'}); // toggelt zusaetzlich den echten Matrix-Modus auf dem Uhren-Display
            openMatrix();
        } else {
            codeInput.classList.add('err'); setTimeout(function(){codeInput.classList.remove('err');},350); codeInput.value=''; codeInput.focus();
        }
    }
    document.getElementById('codeOk').addEventListener('click', tryCode);
    codeInput.addEventListener('keydown', function(e){ if(e.key==='Enter') tryCode(); });

    var matrixQuotes=[
        "Wake up, Neo…","The Matrix has you.","Follow the white rabbit.","Knock, knock, Neo.",
        "There is no spoon.","Free your mind.","I know kung fu.","Welcome to the desert of the real.",
        "What is real? How do you define real?","Unfortunately, no one can be told what the Matrix is."
    ];
    var matrixRunning=false, matrixRAF=null;
    function openMatrix(){ document.getElementById('matrixOverlay').classList.add('show'); matrixRunning=true; startRain(); cycleQuotes(); }
    function closeMatrix(){
        matrixRunning=false; if(matrixRAF) cancelAnimationFrame(matrixRAF);
        document.getElementById('matrixOverlay').classList.remove('show');
        document.getElementById('matrixQuote').classList.remove('show');
    }
    document.getElementById('matrixExit').addEventListener('click', closeMatrix);
    document.addEventListener('keydown', function(e){ if(e.key==='Escape' && matrixRunning) closeMatrix(); });

    var glyphs='アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲン0123456789'.split('');
    function startRain(){
        var cv=document.getElementById('matrixCanvas'), ctx=cv.getContext('2d');
        function fit(){ cv.width=innerWidth; cv.height=innerHeight; }
        fit(); addEventListener('resize', fit);
        var fontSize=16, cols=Math.floor(cv.width/fontSize);
        var drops=new Array(cols).fill(0).map(function(){return Math.random()*-40;});
        var speeds=new Array(cols).fill(0).map(function(){return 0.25+Math.random()*0.35;});
        var lastT=0;
        function frame(tms){
            if(!matrixRunning) return;
            if(tms-lastT>40){
                lastT=tms;
                ctx.fillStyle='rgba(0,0,0,0.09)'; ctx.fillRect(0,0,cv.width,cv.height);
                for(var i=0;i<cols;i++){
                    var x=i*fontSize, y=drops[i]*fontSize;
                    var ch=glyphs[Math.floor(Math.random()*glyphs.length)];
                    ctx.fillStyle='#e8fff0'; ctx.font=fontSize+'px monospace'; ctx.fillText(ch,x,y);
                    ctx.fillStyle='rgba(60,220,110,'+(0.35+Math.random()*0.4)+')';
                    ctx.fillText(glyphs[Math.floor(Math.random()*glyphs.length)], x, y-fontSize);
                    drops[i]+=speeds[i];
                    if(y>cv.height && Math.random()>0.975) drops[i]=Math.random()*-20;
                }
            }
            matrixRAF=requestAnimationFrame(frame);
        }
        matrixRAF=requestAnimationFrame(frame);
    }
    function cycleQuotes(){
        var q=document.getElementById('matrixQuote'); var idx=0;
        function show(){
            if(!matrixRunning) return;
            q.textContent=matrixQuotes[idx%matrixQuotes.length]; idx++;
            q.classList.add('show');
            setTimeout(function(){ if(matrixRunning) q.classList.remove('show'); }, 3400);
            setTimeout(function(){ if(matrixRunning) show(); }, 6000);
        }
        setTimeout(show, 1200);
    }
}

function initDashboardModals(){
    ringSetup();
    wireModal('battVeil', ['battRingBtn'], 'closeBatt', loadBatteryDetail);
    wireModal('connVeil', ['wifiTile'], 'closeConn', loadWifiDetail);
    wireModal('btVeil', ['btTile'], 'closeBt', loadBtDetail);
    wireModal('storageVeil', ['openStorage'], 'closeStorage', loadStorageDetail);
    wireModal('sysHistVeil', ['freqBlk'], 'closeSysHist', function(){ sysHistMode='freq'; renderSysHist(); });
    wireModal('sysHistVeil', ['tempBlk'], 'closeSysHist', function(){ sysHistMode='temp'; renderSysHist(); });
    wireModal('stepHistVeil', ['stepBarBtn'], 'closeStepHist', renderStepHist);
    initSysHist();
    initBranding();
    initMatrixEgg();

    var stepGoalCache = parseInt(document.getElementById('goalInput') ? document.getElementById('goalInput').value : '8000', 10);
    document.getElementById('goalSave') && document.getElementById('goalSave').addEventListener('click', function(){
        var v=parseInt(document.getElementById('goalInput').value,10);
        if(!v || v<500) return;
        stepGoalCache=v;
        fetch('/save', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'_form=fStepGoal&stepGoal='+v}).then(function(){
            var el=document.getElementById('goalSavedTxt'); el.textContent=t('saved'); setTimeout(function(){el.textContent='';},2200);
        });
    });
    window.__stepGoal = function(){ return stepGoalCache; };
}

function updateLive() {
    if (liveInFlight) return;
    liveInFlight = true;
    fetch('/api/status', { cache: 'no-store' }).then(r=>r.json()).then(d=>{
        requestAnimationFrame(function(){
            function eur(v){ return (Math.round(v*100)/100).toLocaleString('de-DE',{minimumFractionDigits:2,maximumFractionDigits:2}) + ' €'; }
            if(document.getElementById('earnToday24')){
                setTextIfChanged('earnToday24', eur(d.geld));
                setTextIfChanged('earnTodayWork', eur(d.geld_work));
                setTextIfChanged('earnMonth', eur(d.geld_month));
                setTextIfChanged('earnYear', eur(d.geld_year));
            }

            if(document.getElementById('battPct')){
                if(!ringC) ringSetup();
                setTextIfChanged('battPct', d.akku);
                var ring=document.getElementById('ring'); if(ring){ ring.style.strokeDashoffset = ringC*(1-Math.max(0,Math.min(100,d.akku))/100); ring.style.stroke = ringColorFor(d.akku, d.charging); }
                var st=document.getElementById('battState'); if(st){ st.classList.toggle('charging', d.charging); st.innerHTML = d.charging ? ('&#9889; ' + t('chargingShort')) : t('battStateNormal'); }
                setTextIfChanged('etaLabel', d.charging ? t('etaLabelCharging') : t('etaLabelDischarging'));
                setHtmlIfChanged('eta', fmtEta(d.eta_min, d.eta_mode));
                setHtmlIfChanged('voltVal', d.volt + ' V');
                setTextIfChanged('confVal', d.eta_conf + '%');
            }

            if(document.getElementById('ringRam')){
                var rPct = Math.round((d.ram_free / d.ram_tot) * 100);
                setRing('ringRam', ringRamC, rPct); setTextIfChanged('ringRamTxt', rPct+'%');
                setTextIfChanged('ramSub', d.ram_free + ' KB / ' + d.ram_tot + ' KB');
                var fPct = d.flash_tot_kb>0 ? Math.round((d.flash_free_kb/d.flash_tot_kb)*100) : 0;
                setRing('ringFlash', ringFlashC, fPct); setTextIfChanged('ringFlashTxt', fPct+'%');
                setTextIfChanged('flashSub', (d.flash_free_kb/1024).toFixed(1) + ' / ' + (d.flash_tot_kb/1024).toFixed(1) + ' MB');
                var nPct = d.nvs_tot>0 ? Math.round((d.nvs_free/d.nvs_tot)*100) : 0;
                setRing('ringNvs', ringNvsC, nPct); setTextIfChanged('ringNvsTxt', nPct+'%');
                setTextIfChanged('nvsSub', d.nvs_free + ' / ' + d.nvs_tot + ' ' + t('slotsFree'));
            }

            if(document.getElementById('sparkFreq')){
                histFreq.push(d.cpufreq); if(histFreq.length>40) histFreq.shift();
                histTemp.push(parseFloat(d.cpu)); if(histTemp.length>40) histTemp.shift();
                if(!sparkFit) fitSparks(); else { drawSpark(document.getElementById('sparkFreq'), histFreq, 60, 260); drawSpark(document.getElementById('sparkTemp'), histTemp, 30, 55); }
                setHtmlIfChanged('freqNow', d.cpufreq + ' <span style="color:var(--muted);font-size:11px">MHz</span>');
                setHtmlIfChanged('tempNow', d.cpu + ' <span style="color:var(--muted);font-size:11px">&deg;C</span>');
            }

            if(document.getElementById('wifiVal')){
                if (d.wifi_mode === 'ap' || (!d.wifi_client && d.wifi && d.rssi >= 0)) {
                    var cl = (d.clients || 0);
                    var clTxt = cl + ' Client' + (cl === 1 ? '' : 's');
                    setHtmlIfChanged('wifiVal', 'Hotspot \u00B7 Ch ' + (d.channel || 1) + ' \u00B7 ' + clTxt);
                    var bars = document.querySelectorAll('#wifiBars i');
                    bars.forEach(function(b, i){ b.classList.toggle('on', true); });
                } else {
                    setHtmlIfChanged('wifiVal', (d.rssi < 0 ? (d.rssi + ' <u style="text-decoration:none;color:var(--muted);font-size:11px">dBm</u> \u00B7 ' + d.rssi_pct + ' <u style="text-decoration:none;color:var(--muted);font-size:11px">%</u>') : (d.wifi ? 'Aktiv' : '--')));
                    var bars = document.querySelectorAll('#wifiBars i');
                    var n = Math.ceil(d.rssi_pct / 25);
                    bars.forEach(function(b, i){ b.classList.toggle('on', d.rssi < 0 && i < n); });
                }
            }
            if(document.getElementById('macVal') && d.mac){
                setTextIfChanged('macVal', d.mac);
            }
            if(document.getElementById('btPill')){
                var bp=document.getElementById('btPill');
                bp.classList.toggle('on', d.bt);
                bp.innerHTML='<span class="dot"></span> ' + (d.bt ? t('btActive') : t('btInactive'));
            }
            if(document.getElementById('d_up')) {
                let u = d.uptime; let h = Math.floor(u/3600); let m = Math.floor((u%3600)/60);
                setHtmlIfChanged('d_up', h+'h '+m+'m '+(u%60)+'s');
            }
            if(document.getElementById('stepFill')){
                var goal = d.step_goal || 8000;
                var gi=document.getElementById('goalInput'); if(gi && document.activeElement!==gi) gi.value=goal;
                var pct=Math.min(100, Math.round(d.steps/goal*100));
                document.getElementById('stepFill').style.width=pct+'%';
                setTextIfChanged('stepLead', d.steps.toLocaleString('de-DE'));
                setTextIfChanged('stepTrail', pct + '% ' + t('of') + ' ' + goal.toLocaleString('de-DE'));
            }

            if(d.wf_mode !== undefined && d.wf_mode !== curWfMode) {
                updateWfUI(d.wf_mode);
            }
            if(d.phone_name !== undefined) {
                setTextIfChanged('phoneDevName', d.phone_name.length > 0 ? d.phone_name : 'Kein Smartphone gekoppelt');
                setTextIfChanged('phoneDevMac', d.phone_mac.length > 0 ? d.phone_mac : '--:--:--:--:--:--');
                var stEl = document.getElementById('phoneDevStatus');
                if (stEl) stEl.innerHTML = d.phone_connected ? '<span style="color:var(--good);font-weight:600;">Verbunden ✓</span>' : '<span style="color:var(--muted);">Getrennt</span>';
            }

        });

        liveInFlight = false;
        scheduleLive(document.hidden ? 2500 : 700);
    }).catch(function(){
        liveInFlight = false;
        scheduleLive(2500);
    });
}
window.onload = function() {
    if(localStorage.getItem('lightMode') === 'true') { toggleTheme(); }
    if(localStorage.getItem('highContrast') === 'true') { document.body.classList.add('high-contrast'); }
    loadHackerRecent();
    if(document.getElementById('battRingBtn')) initDashboardModals();
    updateLive();
    loadNotifList();
    loadTotpList();
    refreshLoraApi();
    loadPackagesList();
    fetch('/api/watchface').then(r=>r.json()).then(d=>{ if(d.mode!==undefined) updateWfUI(d.mode); }).catch(function(){});
};
function syncTitle(id, val) { document.getElementById(id).innerText = val; }
function updateVal(id, val) { document.getElementById(id).innerText = val + '%'; }
function sendNotifApi() {
    var title = document.getElementById('notifTitle').value;
    var msg = document.getElementById('notifMsg').value;
    fetch('/api/notify?title=' + encodeURIComponent(title) + '&msg=' + encodeURIComponent(msg), {method:'POST'})
    .then(r => r.json())
    .then(d => { showToastMsg('Mitteilung gesendet!'); loadNotifList(); });
}
function loadNotifList() {
    fetch('/api/notify')
    .then(r => r.json())
    .then(d => {
        var el = document.getElementById('notifList');
        if (!el) return;
        if (!d.items || d.items.length === 0) { el.innerHTML = '<i>Keine Mitteilungen vorhanden</i>'; return; }
        var h = '<div style="display:flex;flex-direction:column;gap:6px;">';
        d.items.forEach(function(n) {
            h += '<div style="background:var(--surface-2);border-radius:6px;padding:6px 10px;"><b>' + n.title + '</b>: ' + n.msg + '</div>';
        });
        h += '</div>';
        el.innerHTML = h;
    });
}
function loadTotpList() {
    fetch('/api/totp')
    .then(r => r.json())
    .then(d => {
        var el = document.getElementById('totpList');
        if (!el) return;
        if (!d.accounts || d.accounts.length === 0) { el.innerHTML = '<i>Keine Konten eingetragen</i>'; return; }
        var h = '<div style="display:flex;flex-direction:column;gap:8px;">';
        d.accounts.forEach(function(a) {
            h += '<div style="display:flex;justify-content:space-between;align-items:center;background:var(--surface-2);border-radius:6px;padding:8px 12px;">';
            h += '<div><b>' + a.name + '</b><br><span style="font-family:monospace;font-size:18px;color:var(--accent);letter-spacing:2px;">' + a.code + '</span></div>';
            h += '<button type="button" class="btn btn-red" style="margin:0;padding:4px 8px;font-size:11px;" onclick="delTotpApi(' + a.idx + ')">Loeschen</button>';
            h += '</div>';
        });
        h += '</div><div style="font-size:12px;color:var(--muted);margin-top:6px;">Code wechselt in: <b>' + d.remaining + 's</b></div>';
        el.innerHTML = h;
    });
}
function addTotpApi() {
    var name = document.getElementById('newTotpName').value;
    var sec = document.getElementById('newTotpSec').value;
    if (!name || !sec) { alert('Name und Secret benoetigt'); return; }
    fetch('/api/totp/set?name=' + encodeURIComponent(name) + '&secret=' + encodeURIComponent(sec), {method:'POST'})
    .then(r => r.json())
    .then(d => {
        showToastMsg('2FA Konto gespeichert');
        document.getElementById('newTotpName').value = '';
        document.getElementById('newTotpSec').value = '';
        loadTotpList();
    });
}
function delTotpApi(idx) {
    if (!confirm('Konto wirklich loeschen?')) return;
    fetch('/api/totp/del?idx=' + idx, {method:'POST'})
    .then(r => r.json())
    .then(d => { loadTotpList(); });
}
function showToastMsg(txt) {
    var toast = document.getElementById('toast');
    if (toast) {
        toast.innerText = txt;
        toast.className = 'toast show';
        setTimeout(function(){ toast.className = toast.className.replace('show', ''); }, 3000);
    }
}
var loraBeaconRunning = false;
function refreshLoraApi() {
    fetch('/api/lora/status')
    .then(r => r.json())
    .then(d => {
        var badge = document.getElementById('loraReadyBadge');
        if (badge) {
            badge.innerText = d.ready ? 'BEREIT (SX1262 OK)' : 'NICHT BEREIT';
            badge.style.background = d.ready ? 'var(--good)' : 'var(--danger)';
            badge.style.color = '#fff';
        }
        setTextIfChanged('loraTxCount', d.tx !== undefined ? d.tx : 0);
        setTextIfChanged('loraRxCount', d.rx !== undefined ? d.rx : 0);
        var rVal = (d.last_rssi !== undefined && d.last_rssi !== 0) ? d.last_rssi : ((d.rssi !== undefined && d.rssi !== 0) ? d.rssi : null);
        var sVal = (d.last_snr !== undefined && d.last_snr !== 0) ? d.last_snr : ((d.snr !== undefined && d.snr !== 0) ? d.snr : null);
        setTextIfChanged('loraLastRssi', rVal !== null ? (rVal.toFixed(1) + ' dBm') : '-- dBm');
        setTextIfChanged('loraLastSnr', sVal !== null ? (sVal.toFixed(1) + ' dB') : '-- dB');
        
        loraBeaconRunning = !!d.beacon;
        var bSt = document.getElementById('loraBeaconSt');
        if (bSt) {
            bSt.innerText = loraBeaconRunning ? 'Aktiv (5s)' : 'Aus';
            bSt.style.color = loraBeaconRunning ? '#00ff88' : 'var(--muted)';
        }
        var bBtn = document.getElementById('loraBeaconBtn');
        if (bBtn) {
            bBtn.innerText = loraBeaconRunning ? 'Beacon stoppen' : 'Beacon Ping (5s)';
            bBtn.className = loraBeaconRunning ? 'btn btn-red' : 'btn btn-gray';
        }

        var feed = document.getElementById('loraMsgFeed');
        if (feed && d.messages) {
            if (d.messages.length === 0) {
                feed.innerHTML = '<i>Keine LoRa-Nachrichten vorhanden</i>';
            } else {
                var html = '';
                for (var i = 0; i < d.messages.length; i++) {
                    var m = d.messages[i];
                    var isOut = m.out;
                    var col = isOut ? '#00e5ff' : '#00ff88';
                    var dir = isOut ? 'TX \u2192' : 'RX \u2190';
                    var meta = isOut ? '' : (' <span style="color:#aaa;font-size:10px;">(' + (m.rssi ? m.rssi.toFixed(0) : 0) + 'dBm / SNR ' + (m.snr ? m.snr.toFixed(1) : 0) + ')</span>');
                    html += '<div style="margin-bottom:4px; border-bottom:1px solid #222; padding-bottom:3px;">';
                    html += '<span style="color:#777;font-size:10px;">[' + m.time + ']</span> ';
                    html += '<b style="color:' + col + ';">' + dir + ' ' + m.sender + ':</b> ';
                    html += '<span style="color:#eee;">' + m.text + '</span>' + meta;
                    html += '</div>';
                }
                feed.innerHTML = html;
                feed.scrollTop = feed.scrollHeight;
            }
        }
    }).catch(function(){});
}

function sendLoraMsgApi() {
    var msgInput = document.getElementById('loraMsgInput');
    var sndInput = document.getElementById('loraSenderInput');
    if (!msgInput || !msgInput.value.trim()) return;
    var msg = msgInput.value.trim();
    var sender = (sndInput && sndInput.value.trim()) ? sndInput.value.trim() : 'WebDash';
    fetch('/api/lora/send', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'msg=' + encodeURIComponent(msg) + '&sender=' + encodeURIComponent(sender)
    }).then(r => r.json()).then(d => {
        if (d.status === 'ok') {
            msgInput.value = '';
            showToastMsg('LoRa Nachricht gesendet!');
            refreshLoraApi();
        } else {
            alert('LoRa TX Fehler: ' + (d.msg || 'unbekannt'));
        }
    }).catch(function(){ alert('Netzwerkfehler beim LoRa Senden'); });
}

function quickLoraMsg(txt) {
    var msgInput = document.getElementById('loraMsgInput');
    if (msgInput) { msgInput.value = txt; sendLoraMsgApi(); }
}

function sendLoraPingNow() {
    quickLoraMsg('PING');
}

function toggleLoraBeaconApi() {
    var nextState = !loraBeaconRunning;
    fetch('/api/lora/beacon', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'enable=' + (nextState ? '1' : '0') + '&interval=5'
    }).then(r => r.json()).then(d => {
        showToastMsg(d.beacon ? 'LoRa Beacon aktiv!' : 'LoRa Beacon gestoppt');
        refreshLoraApi();
    }).catch(function(){ alert('Fehler bei Beacon Steuerung'); });
}

function clearLoraLogApi() {
    fetch('/api/lora/clear', { method: 'POST' }).then(() => {
        showToastMsg('LoRa Verlauf geleert');
        refreshLoraApi();
    });
}

var pendingAppPackage = null;
var pendingAppPayload = "";

function handleAppFileSelect(input) {
    if (!input.files || input.files.length === 0) return;
    var file = input.files[0];
    var reader = new FileReader();
    reader.onload = function(e) {
        var text = e.target.result;
        try {
            var data = JSON.parse(text);
            if (!data.id || !data.name) {
                alert("Ungueltiges App-Paket: 'id' oder 'name' fehlt in der Datei.");
                input.value = "";
                return;
            }
            pendingAppPackage = data;
            pendingAppPayload = text;
            
            var iconEl = document.getElementById('aiIcon');
            var nameEl = document.getElementById('aiName');
            var idEl = document.getElementById('aiId');
            var verEl = document.getElementById('aiVer');
            var catEl = document.getElementById('aiCat');
            var authorEl = document.getElementById('aiAuthor');
            var descEl = document.getElementById('aiDesc');
            var pageEl = document.getElementById('aiPage');
            
            var col = data.color || '#00e5ff';
            if (iconEl) {
                iconEl.innerText = (data.icon || data.id.substring(0, 3).toUpperCase());
                iconEl.style.color = col;
                iconEl.style.borderColor = col;
            }
            if (nameEl) nameEl.innerText = data.name;
            if (idEl) idEl.innerText = data.id;
            if (verEl) verEl.innerText = data.version || '1.0.0';
            var isSec = (data.category === 'security' || data.cat === 1);
            var catName = isSec ? 'Security' : 'Productivity';
            if (catEl) catEl.innerHTML = '<span style="color:' + (isSec ? 'var(--crit)' : '#00ff88') + ';font-weight:600;">' + catName + '</span>';
            if (authorEl) authorEl.innerText = data.author || 'Community';
            if (descEl) descEl.innerText = data.description || 'Keine Beschreibung';
            if (pageEl) pageEl.innerText = (data.ui ? 'Dynamische UI-App' : 'Natives Paket');
            
            var v = document.getElementById('appInstallVeil');
            if (v) v.classList.add('show');
        } catch(err) {
            alert("Fehler beim Lesen der Datei: " + err);
        }
        input.value = "";
    };
    reader.readAsText(file);
}

function closeAppInstallModal() {
    var v = document.getElementById('appInstallVeil');
    if (v) v.classList.remove('show');
    pendingAppPackage = null;
    pendingAppPayload = "";
}

function confirmAppInstall() {
    if (!pendingAppPayload) return;
    var btn = document.getElementById('btnDoInstall');
    if (btn) { btn.disabled = true; btn.innerText = 'Installiere...'; }
    
    fetch('/api/apps/install', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: pendingAppPayload
    })
    .then(r => r.json())
    .then(d => {
        if (btn) { btn.disabled = false; btn.innerText = '🚀 Jetzt installieren'; }
        if (d.status === 'ok') {
            closeAppInstallModal();
            showToastMsg('App "' + (pendingAppPackage ? pendingAppPackage.name : '') + '" erfolgreich installiert!');
            loadPackagesList();
        } else {
            alert('Fehler bei der Installation: ' + (d.msg || 'Unbekannter Fehler'));
        }
    })
    .catch(err => {
        if (btn) { btn.disabled = false; btn.innerText = '🚀 Jetzt installieren'; }
        alert('Netzwerkfehler: ' + err);
    });
}

function uninstallPackageApi(id, name) {
    if (!confirm('Moechten Sie die App "' + name + '" wirklich deinstallieren?\n\nDie App wird vollstaendig von der Uhr entfernt. Um sie wieder zu nutzen, muss die .app-Datei erneut hochgeladen werden.')) {
        return;
    }
    fetch('/api/apps/uninstall', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'id=' + encodeURIComponent(id)
    })
    .then(r => r.json())
    .then(d => {
        if (d.status === 'ok') {
            showToastMsg('App "' + name + '" deinstalliert');
            loadPackagesList();
        } else {
            alert('Fehler beim Deinstallieren: ' + (d.msg || ''));
        }
    })
    .catch(err => {
        alert('Fehler: ' + err);
    });
}

function loadPackagesList() {
    fetch('/api/apps/list')
    .then(r => r.json())
    .then(d => {
        if (!d.packages) return;
        var elGood = document.getElementById('pkgListGood');
        var elBad = document.getElementById('pkgListBad');
        var cntP = document.getElementById('cntProd');
        var cntS = document.getElementById('cntSec');
        if (!elGood || !elBad) return;

        var hGood = '', hBad = '';
        var numP = 0, numS = 0;
        d.packages.forEach(function(pkg) {
            var isRunning = pkg.running;
            var isEn = pkg.enabled;
            var isCustom = pkg.is_custom;
            var badgeCol = pkg.color || '#00e5ff';
            var runBadge = isRunning ? ' <span style="background:var(--good);color:#000;font-size:10px;padding:2px 6px;border-radius:3px;font-weight:700;">LAEUFT</span>' : '';
            var customBadge = isCustom ? ' <span style="background:var(--accent);color:#000;font-size:10px;padding:2px 5px;border-radius:3px;font-weight:700;">CUSTOM</span>' : '';
            
            var row = '<div style="display:flex; justify-content:space-between; align-items:center; background:var(--surface-2); padding:10px 12px; border-radius:6px; border-left:4px solid ' + badgeCol + '; margin-bottom:8px; flex-wrap:wrap; gap:8px;">';
            row += '<div style="display:flex; align-items:center; gap:10px; min-width:200px;">';
            row += '<span style="background:#222; color:' + badgeCol + '; font-family:monospace; font-weight:bold; padding:6px 8px; border-radius:4px; font-size:12px; border:1px solid ' + badgeCol + '44;">' + (pkg.icon || 'APP') + '</span>';
            row += '<div><b>' + pkg.name + '</b>' + runBadge + customBadge + '<br><span style="font-size:11px;color:var(--muted);">' + pkg.id + ' · v' + (pkg.version || '1.0') + (pkg.is_custom ? ' · Dynamische App' : ' · System') + '</span></div>';
            row += '</div>';
            row += '<div style="display:flex; align-items:center; gap:6px; flex-wrap:wrap;">';
            
            row += '<button type="button" class="btn ' + (isEn ? 'btn-green' : 'btn-gray') + '" style="margin:0; width:auto; padding:5px 10px; font-size:11px;" onclick="togglePackageApi(\'' + pkg.id + '\', ' + (!isEn) + ')">' + (isEn ? 'Aktiviert ✓' : 'Deaktiviert') + '</button>';
            row += '<a href="/api/apps/export?id=' + encodeURIComponent(pkg.id) + '" class="btn btn-blue" style="margin:0; width:auto; padding:5px 10px; font-size:11px; text-decoration:none; display:inline-block;" title="Paketdatei herunterladen">📥 Export</a>';
            row += '<button type="button" class="btn" style="margin:0; width:auto; padding:5px 10px; font-size:11px; background:#ef444422; border:1px solid #ef4444; color:#ef4444;" onclick="uninstallPackageApi(\'' + pkg.id + '\', \'' + pkg.name.replace(/'/g, "\\'") + '\')" title="Vollstaendig deinstallieren">🗑️ Deinstallieren</button>';
            row += '</div>';
            row += '</div>';

            if (pkg.cat === 0) { hGood += row; numP++; }
            else if (pkg.cat === 1) { hBad += row; numS++; }
        });

        elGood.innerHTML = hGood || '<i>Keine Apps in dieser Kategorie</i>';
        elBad.innerHTML = hBad || '<i>Keine Apps in dieser Kategorie</i>';
        if (cntP) cntP.innerText = numP + ' Apps';
        if (cntS) cntS.innerText = numS + ' Apps';
    }).catch(function(){});
}

function togglePackageApi(id, enable) {
    fetch('/api/apps/toggle', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'id=' + encodeURIComponent(id) + '&enable=' + (enable ? '1' : '0')
    }).then(r => r.json()).then(d => {
        showToastMsg('App ' + (enable ? 'aktiviert' : 'deaktiviert'));
        loadPackagesList();
    });
}

function resetPackagesApi() {
    if (!confirm('Alle Pakete auf Werkseinstellung zuruecksetzen?')) return;
    fetch('/api/apps/reset', { method: 'POST' }).then(() => {
        showToastMsg('Pakete zurueckgesetzt');
        loadPackagesList();
    });
}
</script></head><body>)rawliteral");

    // WRAP: zentrierter Container mit Raendern statt voller Displaybreite
    s += F("<div class='wrap'>");

    // HEADER: editierbares Branding (Name + eigenes Icon) + genau 4 Icon-Buttons (Matrix/HC/Theme/Reboot)
    s += F("<header class='dashhdr'><div class='brand'>");
    s += F("<input type='file' id='brandFile' accept='image/png,image/jpeg' style='display:none'>");
    s += F("<div class='logo' id='brandLogo' title='"); s += tr(sysLang, "Klicken zum Hochladen eines eigenen 25×25 Icons", "Click to upload your own 25×25 icon", "Cliquer pour charger votre propre icône 25×25"); s += F("'>");
    s += F("<span id='logoLetter'>"); { String bn = brandName.length() > 0 ? brandName : String("W"); s += htmlEscape(bn.substring(0, 1)); } s += F("</span>");
    s += F("<img id='brandIconImg' alt='Icon' style='display:none' src='/api/brand/icon' onload=\"this.style.display='block';document.getElementById('logoLetter').style.display='none';\" onerror=\"this.style.display='none';\">");
    s += F("</div>");
    s += F("<div class='brand-txt'><h1 id='brandName' contenteditable='true' spellcheck='false' title='"); s += tr(sysLang, "Klicken zum Umbenennen", "Click to rename", "Cliquer pour renommer"); s += F("'>"); s += htmlEscape(brandName); s += F("</h1></div>");
    s += F("</div>"); // .brand

    s += F("<div class='icon-row'>");
    s += F("<button type='button' class='icon-btn' id='bleConnectBtn' onclick='connectWebBle()' title='");
    s += tr(sysLang, "Mit Uhr per Bluetooth (BLE) verbinden (sparsam für Mobiltelefone)", "Connect via Bluetooth (BLE) (battery-saving for mobile phones)", "Connexion via Bluetooth (BLE)");
    s += F("' style='width:auto;padding:0 8px;font-size:12px;font-weight:600;display:flex;align-items:center;gap:4px;color:#00e5ff;'><svg width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M6.5 6.5l11 11L12 23V1l5.5 5.5-11 11'/></svg> BLE</button>");
    s += F("<button type='button' class='icon-btn hc' id='matrixBtn' title='???'>M</button>");
    s += F("<button type='button' class='icon-btn hc' onclick='toggleContrast()' title='"); s += tr(sysLang, "Hoher Kontrast", "High Contrast", "Haut contraste"); s += F("'>HC</button>");
    s += F("<button type='button' class='icon-btn' onclick='toggleTheme()' title='"); s += tr(sysLang, "Hell / Dunkel", "Light / Dark", "Clair / Sombre"); s += F("'>");
    s += F("<svg id='moon_icon' width='18' height='18' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z'></path></svg>");
    s += F("<svg id='sun_icon' width='18' height='18' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round' style='display:none;'><circle cx='12' cy='12' r='5'></circle><line x1='12' y1='1' x2='12' y2='3'></line><line x1='12' y1='21' x2='12' y2='23'></line><line x1='4.22' y1='4.22' x2='5.64' y2='5.64'></line><line x1='18.36' y1='18.36' x2='19.78' y2='19.78'></line><line x1='1' y1='12' x2='3' y2='12'></line><line x1='21' y1='12' x2='23' y2='12'></line><line x1='4.22' y1='19.78' x2='5.64' y2='18.36'></line><line x1='18.36' y1='5.64' x2='19.78' y2='4.22'></line></svg>");
    s += F("</button>");
    s += F("<form action='/reboot' method='POST' style='margin:0;' onsubmit='return confirm(\""); s += tr(sysLang, "Uhr wirklich neustarten?", "Really reboot watch?", "Redemarrer la montre ?"); s += F("\");'><button type='submit' class='icon-btn danger' title='"); s += tr(sysLang, "Neustart", "Reboot", "Redemarrer"); s += F("'><svg width='18' height='18' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M18.36 6.64a9 9 0 1 1-12.73 0'></path><line x1='12' y1='2' x2='12' y2='12'></line></svg></button></form>");
    s += F("</div>"); // .icon-row
    s += F("</header>");

    s += F("<script>window.UI_LANG="); s += String(sysLang); s += F(";</script>");

    // TABS (Mit Übersetzung)
    s += F("<button class='hamburger' onclick='var t=document.querySelector(\".tabs\");t.classList.toggle(\"open\");'>&#9776; Menu</button>");
    s += F("<div class='tabs'><div class='tab active' onclick='showTab(\"p1\", this);document.querySelector(\".tabs\").classList.remove(\"open\")'>Home</div><div class='tab' onclick='showTab(\"p2\", this);document.querySelector(\".tabs\").classList.remove(\"open\")'>");
    s += tr(sysLang, "Anzeige", "Display", "Affichage");
    s += F("</div><div class='tab' onclick='showTab(\"p3\", this);document.querySelector(\".tabs\").classList.remove(\"open\")'>");
    s += tr(sysLang, "System", "System", "Système");
    s += F("</div><div class='tab' onclick='showTab(\"p5\", this);document.querySelector(\".tabs\").classList.remove(\"open\")'>");
    s += tr(sysLang, "Persoenlich", "Personal", "Personnel");
    s += F("</div><div class='tab' onclick='showTab(\"p8\", this);document.querySelector(\".tabs\").classList.remove(\"open\")'>");
    s += tr(sysLang, "Hacker Tools", "Hacker Tools", "Outils Hacker");
    s += F("</div><div class='tab' onclick='showTab(\"p9\", this);document.querySelector(\".tabs\").classList.remove(\"open\")'>");
    s += tr(sysLang, "2FA & Mitteilungen", "2FA & Alerts", "2FA & Notifs");
    s += F("</div><div class='tab' onclick='showTab(\"p_apps\", this);document.querySelector(\".tabs\").classList.remove(\"open\");loadPackagesList();'>");
    s += tr(sysLang, "Pakete & Apps", "Packages & Apps", "Paquets & Apps");
    s += F("</div><div class='tab' onclick='showTab(\"p6\", this);document.querySelector(\".tabs\").classList.remove(\"open\")'>");
    s += tr(sysLang, "Update", "Update", "Mise a jour");
    s += F("</div></div>");

    // 1. DASHBOARD
    s += F("<div id='p1' class='panel active'>");
    s += F("<div class='grid hero'>");

    // Akku-Ring (klickbar -> Akku-Detailfenster)
    s += F("<div class='card batt-card'><div class='ring-wrap clickable' id='battRingBtn' role='button' tabindex='0' title='");
    s += tr(sysLang, "Klicken für Akku-Details", "Click for battery details", "Cliquer pour les détails batterie");
    s += F("'><svg width='150' height='150' viewBox='0 0 150 150'><circle class='ring-track' cx='75' cy='75' r='64'></circle><circle class='ring-val' id='ring' cx='75' cy='75' r='64'></circle></svg>");
    s += F("<div class='ring-center'><div class='pct'><span id='battPct'>--</span><span>%</span></div><div class='state' id='battState'>--</div></div></div>");
    s += F("<div class='eta'><span id='etaLabel'>--</span> <b id='eta'>--</b></div>");
    s += F("<div class='volt-row'><b id='voltVal'>-- V</b></div>");
    s += F("<div class='confidence' title='");
    s += tr(sysLang, "Die Uhr sammelt fortlaufend Lade-/Entladedaten je Modus (Normal/Hacking/Laden) und verfeinert daraus die Restdauer-Schätzung.", "The watch continuously collects charge/discharge data per mode (normal/hacking/charging) and refines the remaining-time estimate from it.", "La montre collecte en continu des données de charge/décharge par mode (normal/hacking/charge) et affine son estimation.");
    s += F("'><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round'><path d='M12 2v4M12 18v4M4.9 4.9l2.8 2.8M16.3 16.3l2.8 2.8M2 12h4M18 12h4M4.9 19.1l2.8-2.8M16.3 7.7l2.8-2.8'/></svg>");
    s += tr(sysLang, "Erfahrungswert", "Learned estimate", "Valeur apprise"); s += F(" · <b id='confVal'>--%</b> ");
    s += tr(sysLang, "sicher", "confident", "sûr"); s += F("</div></div>");

    // Speicher-Übersicht (RAM/Flash/NVS als Ringe + Details-Button -> Speicher-Detailfenster)
    s += F("<div class='card'><h2><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round'><rect x='3' y='3' width='7' height='7' rx='1.5'/><rect x='14' y='3' width='7' height='7' rx='1.5'/><rect x='3' y='14' width='7' height='7' rx='1.5'/><rect x='14' y='14' width='7' height='7' rx='1.5'/></svg> ");
    s += tr(sysLang, "Speicher-Übersicht", "Storage overview", "Aperçu du stockage"); s += F("</h2><div class='hint'>");
    s += tr(sysLang, "RAM, Flash (FFat) und Einstellungs-Speicher (NVS)", "RAM, flash (FFat) and settings storage (NVS)", "RAM, flash (FFat) et stockage des réglages (NVS)"); s += F("</div>");
    s += F("<div class='ring-row'>");
    s += F("<div class='ring-mini'><div class='rwrap'><svg width='72' height='72' viewBox='0 0 72 72'><circle class='track' cx='36' cy='36' r='30'></circle><circle class='val' id='ringRam' cx='36' cy='36' r='30' style='stroke:var(--accent-2)'></circle></svg><div class='rc' id='ringRamTxt'>--%</div></div><div class='lbl'>"); s += tr(sysLang, "RAM frei", "RAM free", "RAM libre"); s += F("</div><div class='sub' id='ramSub'>--</div></div>");
    s += F("<div class='ring-mini'><div class='rwrap'><svg width='72' height='72' viewBox='0 0 72 72'><circle class='track' cx='36' cy='36' r='30'></circle><circle class='val' id='ringFlash' cx='36' cy='36' r='30' style='stroke:var(--good)'></circle></svg><div class='rc' id='ringFlashTxt'>--%</div></div><div class='lbl'>"); s += tr(sysLang, "Flash frei", "Flash free", "Flash libre"); s += F("</div><div class='sub' id='flashSub'>--</div></div>");
    s += F("<div class='ring-mini'><div class='rwrap'><svg width='72' height='72' viewBox='0 0 72 72'><circle class='track' cx='36' cy='36' r='30'></circle><circle class='val' id='ringNvs' cx='36' cy='36' r='30' style='stroke:var(--accent)'></circle></svg><div class='rc' id='ringNvsTxt'>--%</div></div><div class='lbl'>"); s += tr(sysLang, "NVS frei", "NVS free", "NVS libre"); s += F("</div><div class='sub' id='nvsSub'>--</div></div>");
    s += F("</div><button type='button' class='details-btn' id='openStorage'>"); s += tr(sysLang, "Speicherinhalt anzeigen…", "Show storage contents…", "Afficher le contenu…"); s += F("</button></div>");
    s += F("</div>"); // .hero

    // Watchface Galerie
    s += F("<div class='card' style='margin-top:16px;'>");
    s += F("<div style='display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:8px;margin-bottom:12px;'>");
    s += F("<div><h2 style='margin:0;'><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' style='width:20px;height:20px;vertical-align:middle;margin-right:6px;'><circle cx='12' cy='12' r='10'></circle><polyline points='12 6 12 12 16 14'></polyline></svg> ");
    s += tr(sysLang, "Watchface Galerie", "Watchface Gallery", "Galerie de cadrans");
    s += F("</h2><div class='hint'>");
    s += tr(sysLang, "Aktives Zifferblatt auf der Uhr bequem per Klick auswählen", "Select active watchface on watch with a click", "Sélectionnez le cadran actif en un clic");
    s += F("</div></div><span class='pill' id='activeWfBadge' style='background:rgba(59,130,246,0.15);color:var(--accent);border:1px solid var(--accent);font-weight:600;'>");
    s += tr(sysLang, "Aktiv: ", "Active: ", "Actif: ");
    s += F("<b id='activeWfName'>--</b></span></div>");
    s += F("<div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px;'>");

    // Card 0: Ultra Rings
    s += F("<div class='wf-card' id='wfCard0' onclick='selectWatchface(0)' style='background:var(--surface-2);border:2px solid var(--border);border-radius:12px;padding:12px;cursor:pointer;transition:all .2s ease;'>");
    s += F("<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:6px;'><span style='font-size:18px;'>⚡</span><span class='badge' id='wfBadge0' style='font-size:10px;padding:2px 6px;border-radius:4px;'>--</span></div>");
    s += F("<div style='font-weight:700;font-size:14px;color:var(--text);'>Ultra Rings</div>");
    s += F("<div class='hint' style='font-size:11px;margin-top:3px;'>");
    s += tr(sysLang, "Cyberpunk Aktivitätsringe & Glowing Complications", "Cyberpunk activity rings & glowing complications", "Anneaux d'activité cyberpunk et complications");
    s += F("</div><button type='button' class='btn' style='margin-top:8px;padding:5px 10px;font-size:12px;width:100%;' id='wfBtn0'>");
    s += tr(sysLang, "Aktivieren", "Activate", "Activer");
    s += F("</button></div>");

    // Card 1: Classic Chrono
    s += F("<div class='wf-card' id='wfCard1' onclick='selectWatchface(1)' style='background:var(--surface-2);border:2px solid var(--border);border-radius:12px;padding:12px;cursor:pointer;transition:all .2s ease;'>");
    s += F("<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:6px;'><span style='font-size:18px;'>⏱️</span><span class='badge' id='wfBadge1' style='font-size:10px;padding:2px 6px;border-radius:4px;'>--</span></div>");
    s += F("<div style='font-weight:700;font-size:14px;color:var(--text);'>Classic Chrono</div>");
    s += F("<div class='hint' style='font-size:11px;margin-top:3px;'>");
    s += tr(sysLang, "Schweizer Chronograph mit 3 Hilfszifferblättern", "Swiss chronograph with 3 sub-dials & sword hands", "Chronographe suisse avec 3 sous-cadrans");
    s += F("</div><button type='button' class='btn' style='margin-top:8px;padding:5px 10px;font-size:12px;width:100%;' id='wfBtn1'>");
    s += tr(sysLang, "Aktivieren", "Activate", "Activer");
    s += F("</button></div>");

    // Card 2: Retro Casio
    s += F("<div class='wf-card' id='wfCard2' onclick='selectWatchface(2)' style='background:var(--surface-2);border:2px solid var(--border);border-radius:12px;padding:12px;cursor:pointer;transition:all .2s ease;'>");
    s += F("<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:6px;'><span style='font-size:18px;'>📟</span><span class='badge' id='wfBadge2' style='font-size:10px;padding:2px 6px;border-radius:4px;'>--</span></div>");
    s += F("<div style='font-weight:700;font-size:14px;color:var(--text);'>Retro Casio</div>");
    s += F("<div class='hint' style='font-size:11px;margin-top:3px;'>");
    s += tr(sysLang, "Kultiges 80er Olive LCD mit Wochentagsmatrix", "Iconic 80s olive LCD with weekday matrix", "LCD olive emblématique des années 80");
    s += F("</div><button type='button' class='btn' style='margin-top:8px;padding:5px 10px;font-size:12px;width:100%;' id='wfBtn2'>");
    s += tr(sysLang, "Aktivieren", "Activate", "Activer");
    s += F("</button></div>");

    // Card 3: 3D Lebensuhr
    s += F("<div class='wf-card' id='wfCard3' onclick='selectWatchface(3)' style='background:var(--surface-2);border:2px solid var(--border);border-radius:12px;padding:12px;cursor:pointer;transition:all .2s ease;'>");
    s += F("<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:6px;'><span style='font-size:18px;'>⏳</span><span class='badge' id='wfBadge3' style='font-size:10px;padding:2px 6px;border-radius:4px;'>--</span></div>");
    s += F("<div style='font-weight:700;font-size:14px;color:var(--text);'>3D Lebensuhr</div>");
    s += F("<div class='hint' style='font-size:11px;margin-top:3px;'>");
    s += tr(sysLang, "Restlebenszeit-Countdown mit 3D Glass Cards", "Life countdown with 3D glass cards", "Compte à rebours de vie avec cartes en verre 3D");
    s += F("</div><button type='button' class='btn' style='margin-top:8px;padding:5px 10px;font-size:12px;width:100%;' id='wfBtn3'>");
    s += tr(sysLang, "Aktivieren", "Activate", "Activer");
    s += F("</button></div>");

    s += F("</div></div>");

    s += F("<div class='grid cols2' style='margin-top:16px'>");

    // Systemdaten: CPU-Takt/-Temp als klickbare Liniendiagramme, WLAN (dBm+%), Bluetooth, Uptime + Hardware-Fakten
    s += F("<div class='card'><h2><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round'><rect x='4' y='4' width='16' height='16' rx='2'/><path d='M9 9h6v6H9z'/></svg> ");
    s += tr(sysLang, "Systemdaten", "System data", "Données système"); s += F("</h2><div class='hint'>");
    s += tr(sysLang, "Technische Live-Telemetrie der Hardware", "Live hardware telemetry", "Télémétrie matérielle en direct"); s += F("</div>");
    s += F("<div class='chart-blk clickable' id='freqBlk' role='button' tabindex='0' title='"); s += tr(sysLang, "Klicken für größere Detailansicht über längere Zeiträume", "Click for a larger view over longer time ranges", "Cliquer pour une vue détaillée sur des périodes plus longues"); s += F("'><div class='chead'><span class='t'>"); s += tr(sysLang, "CPU-Takt", "CPU clock", "Fréquence CPU"); s += F("</span><span class='now' id='freqNow'>--</span></div><canvas class='spark' id='sparkFreq' width='640' height='76'></canvas></div>");
    s += F("<div class='chart-blk clickable' id='tempBlk' role='button' tabindex='0' title='"); s += tr(sysLang, "Klicken für größere Detailansicht über längere Zeiträume", "Click for a larger view over longer time ranges", "Cliquer pour une vue détaillée sur des périodes plus longues"); s += F("'><div class='chead'><span class='t'>"); s += tr(sysLang, "CPU-Temperatur", "CPU temperature", "Température CPU"); s += F("</span><span class='now' id='tempNow'>--</span></div><canvas class='spark' id='sparkTemp' width='640' height='76'></canvas></div>");
    s += F("<div class='tile-row tile-row3' style='margin-top:12px'>");
    s += F("<div class='tile clickable' id='wifiTile' role='button' tabindex='0' title='"); s += tr(sysLang, "Klicken für Verbindungsdetails", "Click for connection details", "Cliquer pour les détails de connexion"); s += F("'><div class='lbl'>"); s += tr(sysLang, "WLAN-Signal", "WiFi signal", "Signal WiFi"); s += F("</div><div class='wifi-line'><div class='wifi-bars' id='wifiBars'><i></i><i></i><i></i><i></i></div><div class='v' id='wifiVal' style='font-size:14px'>--</div></div></div>");
    s += F("<div class='tile clickable' id='btTile' role='button' tabindex='0' title='"); s += tr(sysLang, "Klicken für Bluetooth-Details", "Click for Bluetooth details", "Cliquer pour les détails Bluetooth"); s += F("'><div class='lbl'>Bluetooth</div><span class='pill' id='btPill' style='margin-top:2px'><span class='dot'></span> --</span></div>");
    s += F("<div class='tile'><div class='lbl'>Uptime</div><div class='v' id='d_up'>--</div></div>");
    s += F("</div>");
    s += F("<div class='tile-row tile-row3' style='margin-top:9px'>");
    s += F("<div class='tile' style='cursor:default'><div class='lbl'>Firmware</div><div class='v' style='font-size:13.5px'>v"); s += WATCHOS_VERSION; s += F("</div></div>");
    s += F("<div class='tile' style='cursor:default'><div class='lbl'>Chip</div><div class='v' style='font-size:13.5px'>"); s += String(ESP.getChipModel()); s += F(" Rev"); s += String(ESP.getChipRevision()); s += F("</div></div>");
    s += F("<div class='tile' style='cursor:default'><div class='lbl'>"); s += tr(sysLang, "Modus", "Mode", "Mode"); s += F("</div><div class='v' style='font-size:13.5px'>"); s += (wlanClientMode ? tr(sysLang,"Client","Client","Client") : tr(sysLang,"Hotspot","Hotspot","Point d'accès")); s += F("</div></div>");
    s += F("</div>");
    s += F("<div class='tile-row tile-row3' style='margin-top:9px'>");
    s += F("<div class='tile' style='cursor:default'><div class='lbl'>MAC</div><div class='v mono' id='macVal' style='font-size:12.5px'>"); s += getHardwareMacAddress(); s += F("</div></div>");
    s += F("<div class='tile' style='cursor:default;grid-column:span 2' title='"); s += tr(sysLang, "Grund des letzten Neustarts", "Reason for the last reboot", "Raison du dernier redémarrage"); s += F("'><div class='lbl'>"); s += tr(sysLang, "Letzter Neustart", "Last reboot", "Dernier redémarrage"); s += F("</div><div class='v' style='font-size:13.5px'>"); s += resetReasonText(sysLang); s += F("</div></div>");
    s += F("</div></div>");

    // Persönliche Zähler: Restzeit-Balken, Opportunität (2x2), Schritte+Ziel-Balken, Mama/Papa/Kinder
    s += F("<div class='card'><h2><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round'><path d='M12 1v22M17 5H9.5a3.5 3.5 0 0 0 0 7h5a3.5 3.5 0 0 1 0 7H6'/></svg> ");
    s += tr(sysLang, "Persönliche Zähler", "Personal counters", "Compteurs personnels"); s += F("</h2>");

    s += F("<div class='hint' style='margin:0 0 5px;font-size:11px;text-transform:uppercase;letter-spacing:.4px;color:var(--faint)'>"); s += tr(sysLang, "Restzeit", "Remaining time", "Temps restant"); s += F("</div>");
    s += F("<div class='barw'><div class='fill' id='lifeFill' style='width:"); s += String(lifePct); s += F("%'></div><span class='lead'>"); s += String(lifePct); s += F("%</span><span class='trail' id='lifeRemain'>"); s += String(rJ); s += F("J "); s += String(rM); s += F("M "); s += String(rT); s += F("T</span></div>");

    s += F("<div class='hint' style='margin:14px 0 4px'>"); s += tr(sysLang, "Opportunität", "Opportunity", "Opportunité"); s += F("</div>");
    s += F("<div class='earn-grid'>");
    s += F("<div class='earn-cell'><div class='t'>"); s += tr(sysLang, "Heute 24", "Today 24", "Aujourd'hui 24"); s += F("</div><div class='v' id='earnToday24'>-- &euro;</div></div>");
    s += F("<div class='earn-cell'><div class='t'>"); s += tr(sysLang, "Heute 7,6", "Today 7.6", "Aujourd'hui 7,6"); s += F("</div><div class='v' id='earnTodayWork'>-- &euro;</div></div>");
    s += F("<div class='earn-cell'><div class='t'>"); s += tr(sysLang, "Monat", "Month", "Mois"); s += F("</div><div class='v' id='earnMonth'>-- &euro;</div></div>");
    s += F("<div class='earn-cell'><div class='t'>"); s += tr(sysLang, "Jahr", "Year", "Année"); s += F("</div><div class='v' id='earnYear'>-- &euro;</div></div>");
    s += F("</div>");

    s += F("<div class='hint' style='margin:14px 0 5px'>"); s += tr(sysLang, "Schritte &amp; Ziel", "Steps &amp; goal", "Pas &amp; objectif"); s += F("</div>");
    s += F("<div class='barw clickable' id='stepBarBtn' role='button' tabindex='0' title='"); s += tr(sysLang, "Klicken für Verlauf nach Stunden", "Click for hourly history", "Cliquer pour l'historique par heure"); s += F("'><div class='fill' id='stepFill' style='width:0%;background:linear-gradient(90deg,var(--good),var(--accent-2))'></div><span class='lead' id='stepLead'>--</span><span class='trail' id='stepTrail'>--</span></div>");
    s += F("<div class='goal-row'><input type='number' id='goalInput' value='"); s += String(stepGoal); s += F("' min='500' max='100000' step='500'><button type='button' id='goalSave'>"); s += tr(sysLang, "Ziel speichern", "Save goal", "Enregistrer l'objectif"); s += F("</button><span class='v good' style='font-size:12px' id='goalSavedTxt'></span></div>");

    s += F("<div class='dl' style='margin-top:14px'>");
    s += F("<div class='row'><span class='k'>"); s += htmlEscape(parentName.length() > 0 ? parentName : String("Mama")); s += F("</span><span class='val'>"); s += parentStr; s += F("</span></div>");
    s += F("<div class='row'><span class='k'>"); s += htmlEscape(papaName.length() > 0 ? papaName : String("Papa")); s += F("</span><span class='val'>"); s += papaStr; s += F("</span></div>");
    if (child1Name != "") { s += F("<div class='row'><span class='k'>"); s += htmlEscape(child1Name); s += F("</span><span class='val'>"); s += computeAnalogDate(uTime, jetzt, child1Date, child1Time, sysLang, true); s += F("</span></div>"); }
    if (child2Name != "") { s += F("<div class='row'><span class='k'>"); s += htmlEscape(child2Name); s += F("</span><span class='val'>"); s += computeAnalogDate(uTime, jetzt, child2Date, child2Time, sysLang, true); s += F("</span></div>"); }
    if (child3Name != "") { s += F("<div class='row'><span class='k'>"); s += htmlEscape(child3Name); s += F("</span><span class='val'>"); s += computeAnalogDate(uTime, jetzt, child3Date, child3Time, sysLang, true); s += F("</span></div>"); }
    s += F("</div>");
    s += F("</div>");

    s += F("</div>"); // .cols2
    s += F("</div>"); // #p1
    
    // 2. ANZEIGE (9 Farben im Grid)
    s += F("<div id='p2' class='panel'>");
    s += F("<div style='background:var(--surface);border:1px solid var(--border);border-radius:8px;padding:12px;margin-bottom:14px;'>");
    s += F("<div style='display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px;'>");
    s += F("<b>"); s += tr(sysLang, "Aktives Watchface:", "Active Watchface:", "Cadran actif:"); s += F(" <span id='p2WfName' style='color:var(--accent);font-weight:700;'>--</span></b>");
    s += F("<button type='button' class='btn' style='margin:0;padding:5px 12px;font-size:12px;background:var(--accent);color:#fff;' onclick='showTab(\"p1\", document.querySelector(\".tabs .tab\"))'>");
    s += tr(sysLang, "In Watchface Galerie wechseln →", "Go to Watchface Gallery →", "Aller à la Galerie →");
    s += F("</button></div></div>");
    s += F("<form id='fViews' onsubmit='return saveForm(\"fViews\")'><h3>"); s += tr(sysLang, "Sichten (Gesten)", "Views (Gestures)", "Vues (Gestes)"); s += F("</h3>");
    s += F("<div style='display:flex; flex-direction:column; gap:5px; margin-bottom:15px;'>");
    s += F("<label class='cb-lbl'><input type='checkbox' style='width:auto;' name='enP0' value='1' "); if(enP0) s += F("checked"); s += F("> 1. Dashboard</label>");
    s += F("<label class='cb-lbl'><input type='checkbox' style='width:auto;' name='enP1' value='1' "); if(enP1) s += F("checked"); s += F("> 2. "); s += tr(sysLang, "Opportunitaet", "Opportunity", "Opportunite"); s += F("</label>");
    s += F("<label class='cb-lbl'><input type='checkbox' style='width:auto;' name='enP2' value='1' "); if(enP2) s += F("checked"); s += F("> 3. Kinder</label>");
    s += F("<label class='cb-lbl'><input type='checkbox' style='width:auto;' name='enP3' value='1' "); if(enP3) s += F("checked"); s += F("> 4. Eltern</label></div>");
    s += F("<input type='submit' class='btn' value='"); s += tr(sysLang, "Sichten speichern", "Save views", "Enregistrer vues"); s += F("'></form><hr>");

    s += F("<form id='fDesign' onsubmit='return saveForm(\"fDesign\")'><h3>"); s += tr(sysLang, "Optik & Verhalten", "Look & Behavior", "Apparence & Comportement"); s += F("</h3>");
    s += F("<b>"); s += tr(sysLang, "Boot-Animation", "Boot animation", "Animation de demarrage"); s += F(":</b><br><select name='bm'><option value='0'"); if(bootMode==0) s += F(" selected"); s += F(">MS-DOS (486)</option><option value='1'"); if(bootMode==1) s += F(" selected"); s += F(">C64</option><option value='3'"); if(bootMode==3) s += F(" selected"); s += F(">Win98</option><option value='4'"); if(bootMode==4) s += F(" selected"); s += F(">Fast Boot</option></select><hr>");
    
    s += F(R"rawliteral(<b>Farb-Presets:</b><br><select id='themeSel' onchange='applyThemePreset()'><option value=''>- Eigene / Manuell -</option><option value='night'>Nightlife</option><option value='day'>Daybreak</option><option value='cyber'>Cyberpunk</option><option value='hacker'>Hacker Matrix</option></select><br>)rawliteral");
    
    s += F("<div style='display:grid; grid-template-columns:1fr 1fr 1fr; gap:10px; margin-top:10px; font-size:12px;'>");
    s += F("<div><b>"); s += tr(sysLang, "Hintergrund", "Background", "Arriere-plan"); s += F(":</b><input name='cBG' type='color' value='"); s += hexBG; s += F("'></div>");
    s += F("<div><b>"); s += tr(sysLang, "Standard Text", "Default text", "Texte par defaut"); s += F(":</b><input name='cFG' type='color' value='"); s += hexFG; s += F("'></div>");
    s += F("<div><b>"); s += tr(sysLang, "Akzent 1", "Accent 1", "Accent 1"); s += F(":</b><input name='cAcc1' type='color' value='"); s += hexAcc1; s += F("'></div>");
    s += F("<div><b>"); s += tr(sysLang, "Akzent 2", "Accent 2", "Accent 2"); s += F(":</b><input name='cAcc2' type='color' value='"); s += hexAcc2; s += F("'></div>");
    s += F("<div><b>"); s += tr(sysLang, "Batterie", "Battery", "Batterie"); s += F(":</b><input name='cBat' type='color' value='"); s += hexBat; s += F("'></div>");
    s += F("<div><b>"); s += tr(sysLang, "Kopfzeile BG", "Header BG", "Fond en-tete"); s += F(":</b><input name='cHBG' type='color' value='"); s += hexHeadBG; s += F("'></div>");
    s += F("<div><b>"); s += tr(sysLang, "Kopfzeile Text", "Header text", "Texte en-tete"); s += F(":</b><input name='cHFG' type='color' value='"); s += hexHeadFG; s += F("'></div>");
    s += F("<div><b>"); s += tr(sysLang, "App Button BG", "App button BG", "Fond bouton app"); s += F(":</b><input name='cABG' type='color' value='"); s += hexAppBG; s += F("'></div>");
    s += F("<div><b>"); s += tr(sysLang, "Warnung (Rot)", "Warning (red)", "Alerte (rouge)"); s += F(":</b><input name='cWrn' type='color' value='"); s += hexWarn; s += F("'></div>");
    s += F("</div><br><input type='submit' class='btn' value='"); s += tr(sysLang, "Design anwenden", "Apply design", "Appliquer le design"); s += F("'></form><hr>");

    // === AOD MANAGER (ehemals Watchface Designer, jetzt im Anzeige-Tab) ===
    s += F("<form id='fWatchface'><h3>"); s += tr(sysLang, "AOD Manager", "AOD Manager", "Gestionnaire AOD"); s += F("</h3>");
    s += F("<p style='color:#aaa;font-size:13px;'>"); s += tr(sysLang, "Ziehe die Elemente auf dem Canvas an die gewuenschte Position (X+Y). Waehle ein Element aus und aendere die Skalierung stufenlos mit dem Slider.", "Drag elements on the canvas to the desired position (X+Y). Select an element and adjust scale smoothly with the slider.", "Faites glisser les elements sur le canevas a la position souhaitee (X+Y). Selectionnez un element et ajustez l'echelle avec le curseur."); s += F("</p>");

    s += F("<div style='display:flex; gap:15px; flex-wrap:wrap; align-items:flex-start;'>");

    // Canvas (240x240 -> 280x280)
    s += F("<div style='position:relative;'><canvas id='wfCanvas' width='280' height='280' style='border:2px solid #555; border-radius:14px; background:#000; cursor:move;'></canvas></div>");

    // Settings panel right
    s += F("<div style='flex:1; min-width:220px;'>");

    // Standard-Elemente
    s += F("<div class='box' style='margin-bottom:8px;'><div class='box-title'>"); s += tr(sysLang, "Standard Elemente", "Standard elements", "Elements standards"); s += F("</div>");
    s += F("<label class='cb-lbl'><input type='checkbox' style='width:auto;' id='wfT_cb' onchange='wfUpdate()' "); if(wfShowTime) s += F("checked"); s += F("> "); s += tr(sysLang, "Uhrzeit", "Time", "Heure"); s += F("</label>");
    s += F("<label class='cb-lbl'><input type='checkbox' style='width:auto;' id='wfD_cb' onchange='wfUpdate()' "); if(wfShowDate) s += F("checked"); s += F("> "); s += tr(sysLang, "Datum", "Date", "Date"); s += F("</label>");
    s += F("<label class='cb-lbl'><input type='checkbox' style='width:auto;' id='wfB_cb' onchange='wfUpdate()' "); if(wfShowBatt) s += F("checked"); s += F("> "); s += tr(sysLang, "Akku", "Battery", "Batterie"); s += F("</label>");
    s += F("<label class='cb-lbl'><input type='checkbox' style='width:auto;' id='wfS_cb' onchange='wfUpdate()' "); if(wfShowSteps) s += F("checked"); s += F("> "); s += tr(sysLang, "Schritte", "Steps", "Pas"); s += F("</label></div>");

    // AOD Elemente
    s += F("<div class='box' style='margin-bottom:8px;'><div class='box-title'>"); s += tr(sysLang, "AOD Elemente", "AOD elements", "Elements AOD"); s += F("</div>");
    s += F("<label class='cb-lbl'><input type='checkbox' style='width:auto;' id='wfWC_cb' onchange='wfUpdate()' "); if(wfShowWordClock) s += F("checked"); s += F("> "); s += tr(sysLang, "Wortuhr", "Word clock", "Horloge en mots"); s += F("</label>");
    s += F("<label class='cb-lbl'><input type='checkbox' style='width:auto;' id='wfBC_cb' onchange='wfUpdate()' "); if(wfShowBinClock) s += F("checked"); s += F("> "); s += tr(sysLang, "Binaer Uhr", "Binary clock", "Horloge binaire"); s += F("</label>");
    s += F("<label class='cb-lbl'><input type='checkbox' style='width:auto;' id='wfAn_cb' onchange='wfUpdate()' "); if(wfShowAnalog) s += F("checked"); s += F("> "); s += tr(sysLang, "Analoguhr", "Analog clock", "Horloge analogique"); s += F("</label>");
    s += F("<label class='cb-lbl'><input type='checkbox' style='width:auto;' id='wfLn_cb' onchange='wfUpdate()' "); if(wfShowLine) s += F("checked"); s += F("> "); s += tr(sysLang, "Trennlinie", "Divider", "Ligne separatrice"); s += F("</label>");
    s += F("<label class='cb-lbl'><input type='checkbox' style='width:auto;' id='wfSA_cb' onchange='wfUpdate()' "); if(wfShowSecArc) s += F("checked"); s += F("> "); s += tr(sysLang, "Minuten-Bogen", "Minute arc", "Arc des minutes"); s += F("</label>");
    s += F("<label class='cb-lbl'><input type='checkbox' style='width:auto;' id='wfWd_cb' onchange='wfUpdate()' "); if(wfShowWeekday) s += F("checked"); s += F("> "); s += tr(sysLang, "Wochentag", "Weekday", "Jour de la semaine"); s += F("</label>");
    s += F("<span id='wfAnStyleBox' style='display:"); s += (wfShowAnalog ? "block" : "none"); s += F(";'><b>"); s += tr(sysLang, "Analog Stil", "Analog style", "Style analogique"); s += F(":</b><select id='wfAnSt_sel' onchange='wfUpdate()'><option value='0'"); if(wfAnalogStyle==0) s += F(" selected"); s += F(">"); s += tr(sysLang, "Normal", "Normal", "Normal"); s += F("</option><option value='1'"); if(wfAnalogStyle==1) s += F(" selected"); s += F(">"); s += tr(sysLang, "Binaer", "Binary", "Binaire"); s += F("</option><option value='2'"); if(wfAnalogStyle==2) s += F(" selected"); s += F(">Hex</option></select></span></div>");

    // AOD-Farben
    s += F("<div class='box' style='margin-bottom:8px;'><div class='box-title'>AOD Farben</div>");
    s += F("<label>Hauptfarbe: <input type='color' id='aodFG_cp' name='aodFG' value='"); s += hexAodFG; s += F("' oninput='wfDraw()'></label><br>");
    s += F("<label>Akzentfarbe: <input type='color' id='aodAcc_cp' name='aodAcc' value='"); s += hexAodAcc; s += F("' oninput='wfDraw()'></label></div>");

    // Selected element info
    s += F("<div class='box' id='wfSelBox' style='display:none;'><div class='box-title' id='wfSelTitle'>-</div>");
    s += F("<span id='wfOptTime' style='display:none;'><b>Format:</b><select id='wfTFm_sel' name='wfTFm' onchange='wfUpdate()'><option value='0'>Digital</option><option value='1'>7-Segment</option></select></span>");
    s += F("<b>Skalierung:</b> <span id='wfScVal'>1.0</span><br><input type='range' id='wfSc_sl' min='0.3' max='3.0' step='0.1' value='1.0' oninput='wfScaleChange(this.value)'>");
    s += F("<div style='margin-top:8px; color:#aaa; font-size:12px;'>X: <span id='wfXVal'>-</span> | Y: <span id='wfYVal'>-</span></div></div>");

    // Hidden fields for positions + scales
    s += F("<input type='hidden' id='wfTY_h' name='wfTY' value='"); s += wfTimeY; s += F("'>");
    s += F("<input type='hidden' id='wfDY_h' name='wfDY' value='"); s += wfDateY; s += F("'>");
    s += F("<input type='hidden' id='wfBY_h' name='wfBY' value='"); s += wfBattY; s += F("'>");
    s += F("<input type='hidden' id='wfSY_h' name='wfSY' value='"); s += wfStepsY; s += F("'>");
    s += F("<input type='hidden' id='wfTX_h' name='wfTX' value='"); s += wfTimeX; s += F("'>");
    s += F("<input type='hidden' id='wfDX_h' name='wfDX' value='"); s += wfDateX; s += F("'>");
    s += F("<input type='hidden' id='wfBX_h' name='wfBX' value='"); s += wfBattX; s += F("'>");
    s += F("<input type='hidden' id='wfSX_h' name='wfSX' value='"); s += wfStepsX; s += F("'>");
    s += F("<input type='hidden' id='wfTSc_h' name='wfTSc' value='"); s += String(wfTimeScale, 1); s += F("'>");
    s += F("<input type='hidden' id='wfDSc_h' name='wfDSc' value='"); s += String(wfDateScale, 1); s += F("'>");
    s += F("<input type='hidden' id='wfBSc_h' name='wfBSc' value='"); s += String(wfBattScale, 1); s += F("'>");
    s += F("<input type='hidden' id='wfSSc_h' name='wfSSc' value='"); s += String(wfStepsScale, 1); s += F("'>");
    // AOD element hidden fields
    s += F("<input type='hidden' id='wfWCY_h' name='wfWCY' value='"); s += wfWordClockY; s += F("'>");
    s += F("<input type='hidden' id='wfWCX_h' name='wfWCX' value='"); s += wfWordClockX; s += F("'>");
    s += F("<input type='hidden' id='wfBCY_h' name='wfBCY' value='"); s += wfBinClockY; s += F("'>");
    s += F("<input type='hidden' id='wfBCX_h' name='wfBCX' value='"); s += wfBinClockX; s += F("'>");
    s += F("<input type='hidden' id='wfAnY_h' name='wfAnY' value='"); s += wfAnalogY; s += F("'>");
    s += F("<input type='hidden' id='wfAnX_h' name='wfAnX' value='"); s += wfAnalogX; s += F("'>");
    s += F("<input type='hidden' id='wfWCSc_h' name='wfWCSc' value='"); s += String(wfWordClockScale, 1); s += F("'>");
    s += F("<input type='hidden' id='wfBCSc_h' name='wfBCSc' value='"); s += String(wfBinClockScale, 1); s += F("'>");
    s += F("<input type='hidden' id='wfAnSc_h' name='wfAnSc' value='"); s += String(wfAnalogScale, 1); s += F("'>");
    // New AOD elements hidden fields
    s += F("<input type='hidden' id='wfLnY_h' name='wfLnY' value='"); s += wfLineY; s += F("'>");
    s += F("<input type='hidden' id='wfLnX_h' name='wfLnX' value='"); s += wfLineX; s += F("'>");
    s += F("<input type='hidden' id='wfSAY_h' name='wfSAY' value='"); s += wfSecArcY; s += F("'>");
    s += F("<input type='hidden' id='wfSAX_h' name='wfSAX' value='"); s += wfSecArcX; s += F("'>");
    s += F("<input type='hidden' id='wfWdY_h' name='wfWdY' value='"); s += wfWeekdayY; s += F("'>");
    s += F("<input type='hidden' id='wfWdX_h' name='wfWdX' value='"); s += wfWeekdayX; s += F("'>");
    s += F("<input type='hidden' id='wfLnSc_h' name='wfLnSc' value='"); s += String(wfLineScale, 1); s += F("'>");
    s += F("<input type='hidden' id='wfSASc_h' name='wfSASc' value='"); s += String(wfSecArcScale, 1); s += F("'>");
    s += F("<input type='hidden' id='wfWdSc_h' name='wfWdSc' value='"); s += String(wfWeekdayScale, 1); s += F("'>");
    // Hidden fields for checkboxes (always sent as 0/1)
    s += F("<input type='hidden' id='wfT_h' name='wfT' value='"); s += (wfShowTime ? "1" : "0"); s += F("'>");
    s += F("<input type='hidden' id='wfD_h' name='wfD' value='"); s += (wfShowDate ? "1" : "0"); s += F("'>");
    s += F("<input type='hidden' id='wfB_h' name='wfB' value='"); s += (wfShowBatt ? "1" : "0"); s += F("'>");
    s += F("<input type='hidden' id='wfS_h' name='wfS' value='"); s += (wfShowSteps ? "1" : "0"); s += F("'>");
    s += F("<input type='hidden' id='wfWC_h' name='wfWC' value='"); s += (wfShowWordClock ? "1" : "0"); s += F("'>");
    s += F("<input type='hidden' id='wfBC_h' name='wfBC' value='"); s += (wfShowBinClock ? "1" : "0"); s += F("'>");
    s += F("<input type='hidden' id='wfAn_h' name='wfAn' value='"); s += (wfShowAnalog ? "1" : "0"); s += F("'>");
    s += F("<input type='hidden' id='wfLn_h' name='wfLn' value='"); s += (wfShowLine ? "1" : "0"); s += F("'>");
    s += F("<input type='hidden' id='wfSA_h' name='wfSA' value='"); s += (wfShowSecArc ? "1" : "0"); s += F("'>");
    s += F("<input type='hidden' id='wfWd_h' name='wfWd' value='"); s += (wfShowWeekday ? "1" : "0"); s += F("'>");
    s += F("<input type='hidden' id='wfAnSt_h' name='wfAnSt' value='"); s += wfAnalogStyle; s += F("'>");

    s += F("</div></div>");

    // === AOD Manager JavaScript ===
    s += F("<script>\nvar wfScale=280/240;\n");
    s += F("var wfElems=[\n");
    s += F("  {id:'time',label:'Uhrzeit',x:"); s += wfTimeX; s += F(",y:"); s += wfTimeY; s += F(",show:"); s += (wfShowTime?"true":"false"); s += F(",sc:"); s += String(wfTimeScale,1); s += F(",fmt:"); s += wfTimeFmt; s += F("},\n");
    s += F("  {id:'date',label:'Datum',x:"); s += wfDateX; s += F(",y:"); s += wfDateY; s += F(",show:"); s += (wfShowDate?"true":"false"); s += F(",sc:"); s += String(wfDateScale,1); s += F("},\n");
    s += F("  {id:'batt',label:'Akku',x:"); s += wfBattX; s += F(",y:"); s += wfBattY; s += F(",show:"); s += (wfShowBatt?"true":"false"); s += F(",sc:"); s += String(wfBattScale,1); s += F("},\n");
    s += F("  {id:'steps',label:'Schritte',x:"); s += wfStepsX; s += F(",y:"); s += wfStepsY; s += F(",show:"); s += (wfShowSteps?"true":"false"); s += F(",sc:"); s += String(wfStepsScale,1); s += F("},\n");
    s += F("  {id:'wordclock',label:'Wortuhr',x:"); s += wfWordClockX; s += F(",y:"); s += wfWordClockY; s += F(",show:"); s += (wfShowWordClock?"true":"false"); s += F(",sc:"); s += String(wfWordClockScale,1); s += F("},\n");
    s += F("  {id:'binclock',label:'Binaer Uhr',x:"); s += wfBinClockX; s += F(",y:"); s += wfBinClockY; s += F(",show:"); s += (wfShowBinClock?"true":"false"); s += F(",sc:"); s += String(wfBinClockScale,1); s += F("},\n");
    s += F("  {id:'analog',label:'Analoguhr',x:"); s += wfAnalogX; s += F(",y:"); s += wfAnalogY; s += F(",show:"); s += (wfShowAnalog?"true":"false"); s += F(",sc:"); s += String(wfAnalogScale,1); s += F(",ast:"); s += wfAnalogStyle; s += F("},\n");
    s += F("  {id:'line',label:'Trennlinie',x:"); s += wfLineX; s += F(",y:"); s += wfLineY; s += F(",show:"); s += (wfShowLine?"true":"false"); s += F(",sc:"); s += String(wfLineScale,1); s += F("},\n");
    s += F("  {id:'secarc',label:'Minuten-Bogen',x:"); s += wfSecArcX; s += F(",y:"); s += wfSecArcY; s += F(",show:"); s += (wfShowSecArc?"true":"false"); s += F(",sc:"); s += String(wfSecArcScale,1); s += F("},\n");
    s += F("  {id:'weekday',label:'Wochentag',x:"); s += wfWeekdayX; s += F(",y:"); s += wfWeekdayY; s += F(",show:"); s += (wfShowWeekday?"true":"false"); s += F(",sc:"); s += String(wfWeekdayScale,1); s += F("}\n];\n");

    s += F("var wfCvs=document.getElementById('wfCanvas');");
    s += F("var wfCtx=wfCvs.getContext('2d');");
    s += F("var wfSel=-1,wfDragging=false,wfDragOffX=0,wfDragOffY=0;");

    // Draw function
    s += F("function fontH(e){");
    s += F("var base=14;");
    s += F("if(e.id=='time')base=36;");
    s += F("else if(e.id=='date')base=16;");
    s += F("else if(e.id=='batt')base=14;");
    s += F("else if(e.id=='steps')base=14;");
    s += F("else if(e.id=='wordclock')base=100;");
    s += F("else if(e.id=='binclock')base=30;");
    s += F("else if(e.id=='analog')base=100;");
    s += F("else if(e.id=='line')base=8;");
    s += F("else if(e.id=='secarc')base=100;");
    s += F("else if(e.id=='weekday')base=24;");
    s += F("return Math.round(base*e.sc);}");

    s += F("function wfDraw(){");
    s += F("var cFG=document.getElementById('aodFG_cp').value;");
    s += F("var cAcc=document.getElementById('aodAcc_cp').value;");
    s += F("var cDim=cFG+'99';");
    s += F("wfCtx.fillStyle='#000';wfCtx.fillRect(0,0,280,280);");

    s += F("for(var i=0;i<wfElems.length;i++){");
    s += F("var e=wfElems[i];if(!e.show)continue;");
    s += F("var cx=Math.round(e.x*wfScale);var cy=Math.round(e.y*wfScale);");
    s += F("var fh=Math.round(fontH(e)*wfScale);");
    s += F("var sel=(wfSel==i);");

    // Render based on type
    s += F("if(e.id=='time'){");
    s += F("var fs=Math.round(36*e.sc*wfScale);");
    s += F("wfCtx.font='bold '+fs+'px '+(e.fmt==1?'monospace':'sans-serif');");
    s += F("wfCtx.textAlign='center';wfCtx.textBaseline='middle';");
    s += F("wfCtx.fillStyle=sel?'#ffea00':cFG;");
    s += F("wfCtx.fillText('12:34',cx,cy);");
    s += F("}else if(e.id=='date'){");
    s += F("var fs=Math.round(16*e.sc*wfScale);");
    s += F("wfCtx.font=fs+'px sans-serif';wfCtx.textAlign='center';wfCtx.textBaseline='middle';");
    s += F("wfCtx.fillStyle=sel?'#ffea00':cDim;");
    s += F("wfCtx.fillText('Mo 25.03.',cx,cy);");
    s += F("}else if(e.id=='batt'){");
    s += F("var fs=Math.round(14*e.sc*wfScale);");
    s += F("wfCtx.font=fs+'px sans-serif';wfCtx.textAlign='center';wfCtx.textBaseline='middle';");
    s += F("wfCtx.fillStyle=sel?'#ffea00':cAcc;");
    s += F("wfCtx.fillText('87%',cx,cy);");
    s += F("}else if(e.id=='steps'){");
    s += F("var fs=Math.round(14*e.sc*wfScale);");
    s += F("wfCtx.font=fs+'px sans-serif';wfCtx.textAlign='center';wfCtx.textBaseline='middle';");
    s += F("wfCtx.fillStyle=sel?'#ffea00':cDim;");
    s += F("wfCtx.fillText('Schritte: --',cx,cy);");

    s += F("}else if(e.id=='wordclock'){");
    s += F("var grid=['ESKISTAFUNF','ZEHNZWANZIG','DREIVIERTEL','TGNACHVORJM','HALBXZWOLFP','ZWEINSIEBEN','KDREIRHFUNF','ELFNEUNVIER','WACHTZEHNRS','BSECHSFUHRX'];");
    s += F("var cs=Math.round(10*e.sc*wfScale);var sx=cx-Math.round(55*e.sc*wfScale);var sy=cy-Math.round(50*e.sc*wfScale);");
    s += F("wfCtx.font=cs+'px monospace';wfCtx.textAlign='left';wfCtx.textBaseline='top';");
    s += F("for(var r=0;r<10;r++){for(var c=0;c<11;c++){");
    s += F("wfCtx.fillStyle=(r<3)?");
    s += F("(sel?'#ffea00':cAcc):(sel?'#776600':'#333');");
    s += F("wfCtx.fillText(grid[r][c],sx+c*cs,sy+r*cs);}}");

    s += F("}else if(e.id=='binclock'){");
    s += F("var fs=Math.round(16*e.sc*wfScale);var gap=Math.round(20*e.sc*wfScale);");
    s += F("wfCtx.font='bold '+fs+'px monospace';wfCtx.textAlign='center';wfCtx.textBaseline='middle';");
    s += F("wfCtx.fillStyle=sel?'#ffea00':cAcc;");
    s += F("wfCtx.fillText('001100',cx,cy-gap/2);wfCtx.fillText('100010',cx,cy+gap/2);");

    s += F("}else if(e.id=='analog'){");
    s += F("var rad=Math.round(50*e.sc*wfScale);");
    s += F("wfCtx.strokeStyle=sel?'#ffea00':cDim;wfCtx.lineWidth=1;");
    s += F("wfCtx.beginPath();wfCtx.arc(cx,cy,rad,0,Math.PI*2);wfCtx.stroke();");
    s += F("for(var n=1;n<=12;n++){var a=n*30*Math.PI/180;var nx=cx+Math.round((rad-12)*Math.sin(a));var ny=cy-Math.round((rad-12)*Math.cos(a));");
    s += F("wfCtx.font=Math.round(9*e.sc*wfScale)+'px sans-serif';wfCtx.textAlign='center';wfCtx.textBaseline='middle';");
    s += F("wfCtx.fillStyle=sel?'#ffea00':cFG;");
    s += F("var lbl='';var ast=e.ast||0;");
    s += F("if(ast==1){var b=n;var bs='';for(var bi=0;bi<4;bi++){bs=(b%2)+bs;b=Math.floor(b/2);}lbl=bs;}");
    s += F("else if(ast==2){lbl=n.toString(16).toUpperCase();}else{lbl=''+n;}");
    s += F("wfCtx.fillText(lbl,nx,ny);}");
    // Hour hand (pointing ~10:10)
    s += F("var hA=310*Math.PI/180;wfCtx.strokeStyle=sel?'#ffea00':cFG;wfCtx.lineWidth=2;");
    s += F("wfCtx.beginPath();wfCtx.moveTo(cx,cy);wfCtx.lineTo(cx+Math.round(rad*0.5*Math.sin(hA)),cy-Math.round(rad*0.5*Math.cos(hA)));wfCtx.stroke();");
    // Minute hand
    s += F("var mA=60*Math.PI/180;wfCtx.lineWidth=1;wfCtx.strokeStyle=sel?'#ffea00':cAcc;");
    s += F("wfCtx.beginPath();wfCtx.moveTo(cx,cy);wfCtx.lineTo(cx+Math.round(rad*0.75*Math.sin(mA)),cy-Math.round(rad*0.75*Math.cos(mA)));wfCtx.stroke();");
    s += F("wfCtx.fillStyle=sel?'#ffea00':cFG;wfCtx.beginPath();wfCtx.arc(cx,cy,3,0,Math.PI*2);wfCtx.fill();");
    s += F("}");

    // Trennlinie
    s += F("else if(e.id=='line'){");
    s += F("var lw=Math.round(120*e.sc*wfScale);var lh=Math.max(1,Math.round(2*wfScale));");
    s += F("wfCtx.fillStyle=sel?'#ffea00':cDim;");
    s += F("wfCtx.fillRect(cx-lw/2,cy-lh/2,lw,lh);");
    s += F("}");

    // Minuten-Bogen (arc showing ~37 minutes as preview)
    s += F("else if(e.id=='secarc'){");
    s += F("var rad=Math.round(50*e.sc*wfScale);");
    s += F("wfCtx.strokeStyle=sel?'#776600':'#333';wfCtx.lineWidth=Math.max(2,Math.round(4*e.sc*wfScale));");
    s += F("wfCtx.beginPath();wfCtx.arc(cx,cy,rad,-Math.PI/2,Math.PI*2-Math.PI/2);wfCtx.stroke();");
    s += F("wfCtx.strokeStyle=sel?'#ffea00':cAcc;");
    s += F("wfCtx.beginPath();wfCtx.arc(cx,cy,rad,-Math.PI/2,-Math.PI/2+Math.PI*2*37/60);wfCtx.stroke();");
    s += F("}");

    // Wochentag
    s += F("else if(e.id=='weekday'){");
    s += F("var fs=Math.round(24*e.sc*wfScale);");
    s += F("wfCtx.font='bold '+fs+'px sans-serif';wfCtx.textAlign='center';wfCtx.textBaseline='middle';");
    s += F("wfCtx.fillStyle=sel?'#ffea00':cAcc;");
    s += F("wfCtx.fillText('MONTAG',cx,cy);");
    s += F("}");

    // Selection border
    s += F("if(sel){wfCtx.strokeStyle='#ffea00';wfCtx.lineWidth=1;wfCtx.setLineDash([4,4]);");
    s += F("var bx=fh+10;");
    s += F("if(e.id=='wordclock')bx=Math.round(115*e.sc*wfScale);");
    s += F("if(e.id=='analog')bx=Math.round(105*e.sc*wfScale);");
    s += F("if(e.id=='secarc')bx=Math.round(105*e.sc*wfScale);");
    s += F("if(e.id=='line'){var lw=Math.round(120*e.sc*wfScale)+10;wfCtx.strokeRect(cx-lw/2,cy-8,lw,16);wfCtx.setLineDash([]);}else{");
    s += F("wfCtx.strokeRect(cx-bx/2,cy-bx/2,bx,bx);wfCtx.setLineDash([]);}}");
    s += F("}}");

    // Hit test: check proximity to element center
    s += F("function wfHitTest(mx,my){");
    s += F("for(var i=wfElems.length-1;i>=0;i--){var e=wfElems[i];if(!e.show)continue;");
    s += F("var cx=Math.round(e.x*wfScale);var cy=Math.round(e.y*wfScale);");
    s += F("var r=Math.round(fontH(e)*wfScale/2)+10;");
    s += F("if(e.id=='wordclock'||e.id=='analog'||e.id=='secarc')r=Math.round(60*e.sc*wfScale);");
    s += F("if(e.id=='line'){r=Math.round(60*e.sc*wfScale);var ry=10;if(Math.abs(mx-cx)<r&&Math.abs(my-cy)<ry)return i;continue;}");
    s += F("if(Math.abs(mx-cx)<r&&Math.abs(my-cy)<r)return i;}return -1;}");

    // Mouse events: drag X+Y
    s += F("wfCvs.addEventListener('mousedown',function(ev){");
    s += F("var rect=wfCvs.getBoundingClientRect();");
    s += F("var mx=(ev.clientX-rect.left)*(280/rect.width);");
    s += F("var my=(ev.clientY-rect.top)*(280/rect.height);");
    s += F("var hit=wfHitTest(mx,my);wfSel=hit;");
    s += F("if(hit>=0){wfDragging=true;wfDragOffX=mx-Math.round(wfElems[hit].x*wfScale);wfDragOffY=my-Math.round(wfElems[hit].y*wfScale);}");
    s += F("wfShowSelOpts();wfDraw();});");

    s += F("wfCvs.addEventListener('mousemove',function(ev){");
    s += F("if(wfSel<0||!wfDragging)return;var rect=wfCvs.getBoundingClientRect();");
    s += F("var mx=(ev.clientX-rect.left)*(280/rect.width);");
    s += F("var my=(ev.clientY-rect.top)*(280/rect.height);");
    s += F("var nx=Math.round((mx-wfDragOffX)/wfScale);var ny=Math.round((my-wfDragOffY)/wfScale);");
    s += F("if(nx<0)nx=0;if(nx>240)nx=240;if(ny<0)ny=0;if(ny>240)ny=240;");
    s += F("wfElems[wfSel].x=nx;wfElems[wfSel].y=ny;wfSyncHidden();");
    s += F("document.getElementById('wfXVal').innerText=nx;document.getElementById('wfYVal').innerText=ny;wfDraw();});");

    s += F("wfCvs.addEventListener('mouseup',function(){wfDragging=false;});");
    s += F("wfCvs.addEventListener('mouseleave',function(){wfDragging=false;});");

    // Touch events: drag X+Y
    s += F("wfCvs.addEventListener('touchstart',function(ev){ev.preventDefault();");
    s += F("var t=ev.touches[0];var rect=wfCvs.getBoundingClientRect();");
    s += F("var mx=(t.clientX-rect.left)*(280/rect.width);");
    s += F("var my=(t.clientY-rect.top)*(280/rect.height);");
    s += F("var hit=wfHitTest(mx,my);wfSel=hit;");
    s += F("if(hit>=0){wfDragging=true;wfDragOffX=mx-Math.round(wfElems[hit].x*wfScale);wfDragOffY=my-Math.round(wfElems[hit].y*wfScale);}");
    s += F("wfShowSelOpts();wfDraw();},{passive:false});");

    s += F("wfCvs.addEventListener('touchmove',function(ev){ev.preventDefault();");
    s += F("if(wfSel<0||!wfDragging)return;var t=ev.touches[0];var rect=wfCvs.getBoundingClientRect();");
    s += F("var mx=(t.clientX-rect.left)*(280/rect.width);");
    s += F("var my=(t.clientY-rect.top)*(280/rect.height);");
    s += F("var nx=Math.round((mx-wfDragOffX)/wfScale);var ny=Math.round((my-wfDragOffY)/wfScale);");
    s += F("if(nx<0)nx=0;if(nx>240)nx=240;if(ny<0)ny=0;if(ny>240)ny=240;");
    s += F("wfElems[wfSel].x=nx;wfElems[wfSel].y=ny;wfSyncHidden();");
    s += F("document.getElementById('wfXVal').innerText=nx;document.getElementById('wfYVal').innerText=ny;wfDraw();},{passive:false});");

    s += F("wfCvs.addEventListener('touchend',function(){wfDragging=false;});");

    // Sync hidden fields
    s += F("function wfSyncHidden(){");
    s += F("var ids=['T','D','B','S','WC','BC','An','Ln','SA','Wd'];var keys=['time','date','batt','steps','wordclock','binclock','analog','line','secarc','weekday'];");
    s += F("for(var i=0;i<wfElems.length;i++){var p=ids[i];");
    s += F("document.getElementById('wf'+p+'Y_h').value=wfElems[i].y;");
    s += F("document.getElementById('wf'+p+'X_h').value=wfElems[i].x;");
    s += F("document.getElementById('wf'+p+'Sc_h').value=wfElems[i].sc.toFixed(1);");
    s += F("document.getElementById('wf'+p+'_h').value=wfElems[i].show?'1':'0';}");
    s += F("document.getElementById('wfAnSt_h').value=wfElems[6].ast||0;}");
    // Show options for selected element
    s += F("function wfShowSelOpts(){var box=document.getElementById('wfSelBox');");
    s += F("document.getElementById('wfOptTime').style.display='none';");
    s += F("if(wfSel<0){box.style.display='none';return;}");
    s += F("box.style.display='block';");
    s += F("document.getElementById('wfSelTitle').innerText=wfElems[wfSel].label;");
    s += F("document.getElementById('wfXVal').innerText=wfElems[wfSel].x;");
    s += F("document.getElementById('wfYVal').innerText=wfElems[wfSel].y;");
    s += F("document.getElementById('wfSc_sl').value=wfElems[wfSel].sc;");
    s += F("document.getElementById('wfScVal').innerText=wfElems[wfSel].sc.toFixed(1);");
    s += F("if(wfSel==0){document.getElementById('wfOptTime').style.display='';document.getElementById('wfTFm_sel').value=wfElems[0].fmt||0;}}");

    // Scale slider changed
    s += F("function wfScaleChange(v){");
    s += F("if(wfSel<0)return;var fv=parseFloat(v);");
    s += F("wfElems[wfSel].sc=fv;document.getElementById('wfScVal').innerText=fv.toFixed(1);");
    s += F("wfSyncHidden();wfDraw();}");

    // Update from checkboxes
    s += F("function wfUpdate(){");
    s += F("wfElems[0].show=document.getElementById('wfT_cb').checked;");
    s += F("wfElems[1].show=document.getElementById('wfD_cb').checked;");
    s += F("wfElems[2].show=document.getElementById('wfB_cb').checked;");
    s += F("wfElems[3].show=document.getElementById('wfS_cb').checked;");
    s += F("wfElems[4].show=document.getElementById('wfWC_cb').checked;");
    s += F("wfElems[5].show=document.getElementById('wfBC_cb').checked;");
    s += F("wfElems[6].show=document.getElementById('wfAn_cb').checked;");
    s += F("wfElems[7].show=document.getElementById('wfLn_cb').checked;");
    s += F("wfElems[8].show=document.getElementById('wfSA_cb').checked;");
    s += F("wfElems[9].show=document.getElementById('wfWd_cb').checked;");
    s += F("if(wfSel==0)wfElems[0].fmt=parseInt(document.getElementById('wfTFm_sel').value);");
    s += F("wfElems[6].ast=parseInt(document.getElementById('wfAnSt_sel').value);");
    s += F("document.getElementById('wfAnStyleBox').style.display=wfElems[6].show?'block':'none';");
    s += F("wfSyncHidden();wfDraw();}");  // Sync hidden fields on every checkbox change
    
    // JSON-gebündeltes Speichern
    s += F("function doSaveAOD(){");
    s += F("  try { wfSyncHidden(); } catch(e) { console.error('wfSync err', e); }");
    s += F("  var f = document.getElementById('fWatchface');");
    s += F("  var inp = f.querySelectorAll('input[name],select[name]');");
    s += F("  var dataObj = {};");
    s += F("  for(var i = 0; i < inp.length; i++) {");
    s += F("    var val = inp[i].value;");
    s += F("    if(!isNaN(val) && val !== '') dataObj[inp[i].name] = Number(val);");
    s += F("    else dataObj[inp[i].name] = val;");
    s += F("  }");
    s += F("  var params = '_form=fWatchface&aodData=' + encodeURIComponent(JSON.stringify(dataObj));");
    s += F("  fetch('/save', {");
    s += F("    method: 'POST',");
    s += F("    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },");
    s += F("    body: params");
    s += F("  }).then(function(r) {");
    s += F("    if(r.ok) {");
    s += F("      var t = document.getElementById('toast');");
    s += F("      t.className = 'toast show';");
    s += F("      setTimeout(function(){ t.className = t.className.replace('show',''); }, 3000);");
    s += F("    } else { alert(i18n3('AOD Speicherfehler: HTTP ', 'AOD save error: HTTP ', 'Erreur de sauvegarde AOD : HTTP ') + r.status); }");
    s += F("  }).catch(function(e){ alert(i18n3('AOD Netzwerkfehler: ', 'AOD network error: ', 'Erreur reseau AOD : ') + e); });");
    s += F("}");
    
    s += F("wfDraw();\n</script>");
    s += F("<button type='button' class='btn' onclick='doSaveAOD()'>"); s += tr(sysLang, "AOD speichern", "Save AOD", "Enregistrer AOD"); s += F("</button></form></div>");

    // 3. SYSTEM
    s += F("<div id='p3' class='panel'>");
    s += F("<form id='fSys' onsubmit='return saveForm(\"fSys\")'>");
    
    s += F("<h3>"); s += tr(sysLang, "System Sprache", "System Language", "Langue Systeme"); s += F("</h3>");
    s += F("<select name='sysLang'><option value='0'"); if(sysLang==0) s += F(" selected"); s += F(">Deutsch</option><option value='1'"); if(sysLang==1) s += F(" selected"); s += F(">English</option><option value='2'"); if(sysLang==2) s += F(" selected"); s += F(">Français</option></select><hr>");

    s += F("<h3>"); s += tr(sysLang, "WLAN Konfiguration", "WiFi Configuration", "Configuration WiFi"); s += F("</h3><div style='display:grid; grid-template-columns:1fr 1fr; gap:10px;'>");
    s += F("<div class='box'><div class='box-title'>"); s += tr(sysLang, "Hotspot (Uhr)", "Hotspot (watch)", "Hotspot (montre)"); s += F("</div><b>SSID:</b><input name='apSsid' type='text' value='"); s += apSSID; s += F("'><b>"); s += tr(sysLang, "Passwort", "Password", "Mot de passe"); s += F(":</b><input name='apPwd' type='text' value='"); s += apPWD; s += F("'></div>");
    s += F("<div class='box'><div class='box-title'>"); s += tr(sysLang, "Client (Heim/Handy)", "Client (home/phone)", "Client (maison/telephone)"); s += F("</div><b>SSID:</b><input name='ssid' type='text' value='"); s += userSSID; s += F("'><b>"); s += tr(sysLang, "Passwort", "Password", "Mot de passe"); s += F(":</b><input name='pwd' type='password' value='"); s += userPWD; s += F("'></div></div><hr>");
    
    int valMax = (maxBright * 100) / 255; int valMin = (minBright * 100) / 255;
    s += F("<h3>"); s += tr(sysLang, "Helligkeit & Timer", "Brightness & Timer", "Luminosite & Minuteur"); s += F("</h3>");
    s += F("<b>Helligkeit Aktiv: </b><span id='slM_v' style='color:#ffb020;'>"); s += valMax; s += F("%</span><br><input name='maxb_pct' type='range' min='5' max='100' value='"); s += valMax; s += F("' oninput='updateVal(\"slM_v\", this.value)'>");
    s += F("<b>Helligkeit AOD: </b><span id='slA_v' style='color:#ffb020;'>"); s += valMin; s += F("%</span><br><input name='minb_pct' type='range' min='0' max='100' value='"); s += valMin; s += F("' oninput='updateVal(\"slA_v\", this.value)'><br><br>");
    
    s += F("<div style='display:grid; grid-template-columns:1fr 1fr 1fr; gap:5px; text-align:center;'><div><b>Dimmen (s)</b><br><input name='tdim' type='number' min='5' max='120' value='"); s += (time_dim_start/1000); s += F("'></div>");
    s += F("<div><b>AOD (s)</b><br><input name='taod' type='number' min='10' max='300' value='"); s += (time_aod_start/1000); s += F("'></div>");
    s += F("<div><b>Sleep (s)</b><br><input name='tslp' type='number' min='20' max='600' value='"); s += (time_sleep_start/1000); s += F("'></div></div><br>");
    
    s += F("<b>Timer Alarm Typ:</b><br><select name='tAlarm'><option value='0'"); if(timerAlarmMode==0) s += F(" selected"); s += F(">Nur Haptisch</option><option value='1'"); if(timerAlarmMode==1) s += F(" selected"); s += F(">Nur Akustisch</option><option value='2'"); if(timerAlarmMode==2) s += F(" selected"); s += F(">Beides</option></select><br><br>");
    
    s += F("<b>Haptic Waveform (1-123): </b><span id='hap_v' style='color:#ffb020;'>"); s += hapticWaveform; s += F("</span><br><input name='hapW' type='range' min='1' max='123' value='"); s += hapticWaveform; s += F("' oninput='updateVal(\"hap_v\", this.value)'><hr>");
    
    s += F("<h3>"); s += tr(sysLang, "Web Dashboard Schutz", "Web Dashboard Protection", "Protection Dashboard Web"); s += F("</h3>");
    s += F("<label class='cb-lbl'><input type='checkbox' name='webAuth' value='1' "); if(webAuthEnabled) s += F("checked"); s += F("> "); s += tr(sysLang, "Passwort-Schutz aktivieren", "Enable password protection", "Activer protection par mot de passe"); s += F("</label>");
    s += F("<b>"); s += tr(sysLang, "Benutzername", "Username", "Nom d'utilisateur"); s += F(":</b><input name='webUsr' type='text' value='"); s += htmlEscape(webUser); s += F("'>");
    s += F("<b>"); s += tr(sysLang, "Passwort", "Password", "Mot de passe"); s += F(":</b><input name='webPwd' type='password' value='"); s += htmlEscape(webPass); s += F("'><hr>");
    
    s += F("<input type='submit' class='btn' value='"); s += tr(sysLang, "System speichern", "Save system", "Enregistrer systeme"); s += F("'></form><hr>");

    s += F("<form id='fTime' onsubmit='return saveTime()'><h3>"); s += tr(sysLang, "Uhrzeit & Zeitzone", "Time & Timezone", "Heure & Fuseau horaire"); s += F("</h3>");
    s += F("<div style='display:grid; grid-template-columns:1fr 1fr; gap:10px; margin-bottom:10px;'><div><b>"); s += tr(sysLang, "Datum", "Date", "Date"); s += F(":</b><br><input type='date' name='md' value='"); s += sysD; s += F("'></div><div><b>"); s += tr(sysLang, "Uhrzeit", "Time", "Heure"); s += F(":</b><br><input type='time' name='mt' value='"); s += sysT; s += F("'></div></div>");
    s += F("<div style='display:grid; grid-template-columns:1fr 1fr; gap:10px; align-items:center;'><div><b>"); s += tr(sysLang, "Zeitzone", "Timezone", "Fuseau horaire"); s += F(":</b><br><select name='ntpz'><option value='CET-1CEST,M3.5.0,M10.5.0/3'"); if(ntpZone == "CET-1CEST,M3.5.0,M10.5.0/3") s += F(" selected"); s += F(">Berlin</option><option value='GMT0BST,M3.5.0/1,M10.5.0'"); if(ntpZone == "GMT0BST,M3.5.0/1,M10.5.0") s += F(" selected"); s += F(">London</option></select></div><div><br><label class='cb-lbl'><input type='checkbox' name='dst' value='1' "); if(aktuell->tm_isdst > 0) s += F("checked"); s += F("> "); s += tr(sysLang, "Sommerzeit", "Daylight saving", "Heure d'ete"); s += F("</label></div></div>");
    s += F("<div style='display:flex; gap:10px; margin-top:10px;'><input type='submit' class='btn btn-blue' value='"); s += tr(sysLang, "Manuell setzen", "Set manually", "Regler manuellement"); s += F("' style='margin:0;'><button type='button' class='btn btn-gray' style='margin:0;' onclick='syncBrowserTime()'>&#9200; Browser-Zeit</button><button type='button' class='btn btn-gray' style='margin:0;' onclick='fetch(\"/sync\", {method:\"POST\"}).then(()=>setTimeout(()=>location.reload(), 4000))'>NTP Sync</button></div></form>");
    // Export / Import Settings
    s += F("<hr><h3>"); s += tr(sysLang, "Einstellungen sichern / laden", "Backup / restore settings", "Sauvegarder / charger les reglages"); s += F("</h3>");
    s += F("<div style='display:flex; gap:10px; flex-wrap:wrap;'>");
    s += F("<a class='btn btn-blue' style='text-decoration:none;text-align:center;flex:1;' href='/export' download='watchos_settings.json'>&#128190; Export (JSON)</a>");
    s += F("<button class='btn btn-gray' style='flex:1;' onclick='document.getElementById(\"impFile\").click()'>&#128194; Import (JSON)</button>");
    s += F("<input type='file' id='impFile' accept='.json' style='display:none' onchange='importSettings(this.files[0])'></div>");
    s += F("<script>function importSettings(f){if(!f)return;var r=new FileReader();r.onload=function(e){fetch(\"/import\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:e.target.result}).then(r=>r.text()).then(t=>{var toast=document.getElementById(\"toast\");toast.className=\"toast show\";setTimeout(()=>{toast.className=toast.className.replace(\"show\",\"\");},3000);setTimeout(()=>location.reload(),3500);});};r.readAsText(f);}</script>");
    s += F("</div>");

    // 4. PERSÖNLICH
    s += F("<div id='p5' class='panel'><form id='fPers' onsubmit='return saveForm(\"fPers\")'><h3>"); s += tr(sysLang, "Persoenliche Daten", "Personal Data", "Donnees personnelles"); s += F("</h3>");
    s += F("<b>"); s += tr(sysLang, "Dein Geburtsdatum & Zeit", "Your birth date & time", "Votre date et heure de naissance"); s += F(":</b><br><input name='bday' type='date' value='"); s += bdayStr; s += F("' style='width:60%;display:inline-block;'><input name='btime' type='time' value='"); s += btimeStr; s += F("' style='width:35%;display:inline-block;float:right;'>");
    s += F("<b>Ziel-Alter:</b><input name='a' type='number' min='1' max='120' value='"); s += zielAlter; s += F("'><hr>");
    s += F("<b>"); s += tr(sysLang, "Mama Name", "Mom's name", "Nom de maman"); s += F(":</b><input name='pN' type='text' value='"); s += htmlEscape(parentName); s += F("'>");
    s += F("<b>"); s += tr(sysLang, "Geburt & Zeit", "Birth & time", "Naissance & heure"); s += F(":</b><br><input name='pD' type='date' value='"); s += parentDate; s += F("' style='width:60%;display:inline-block;'><input name='pT' type='time' value='"); s += parentTime; s += F("' style='width:35%;display:inline-block;float:right;'><hr>");
    s += F("<b>"); s += tr(sysLang, "Papa Name", "Dad's name", "Nom de papa"); s += F(":</b><input name='ppN' type='text' value='"); s += htmlEscape(papaName); s += F("'>");
    s += F("<b>"); s += tr(sysLang, "Geburt & Zeit", "Birth & time", "Naissance & heure"); s += F(":</b><br><input name='ppD' type='date' value='"); s += papaDate; s += F("' style='width:60%;display:inline-block;'><input name='ppT' type='time' value='"); s += papaTime; s += F("' style='width:35%;display:inline-block;float:right;'><hr>");
    s += F("<b>Netto-Monatslohn (E):</b><input name='netS' type='number' step='0.01' value='"); s += netSalary; s += F("'><b>Arbeitsstunden / Tag (100%):</b><input name='wh' type='number' step='0.1' value='"); s += workHoursPerDay; s += F("'><b>Teilzeit (%):</b><input name='pt' type='number' min='1' max='100' value='"); s += partTimePct; s += F("'><b>Regel-Arbeitstage / Jahr:</b><input name='wd' type='number' value='"); s += workDaysYear; s += F("'><b>Urlaubstage:</b><input name='vac' type='number' value='"); s += vacationDays; s += F("'><b>Krankheitstage (dieses Jahr):</b><input name='sick' type='number' value='"); s += sickDays; s += F("'><hr>");
    s += F("<b>Kind 1 Name:</b><input name='c1n' type='text' value='"); s += child1Name; s += F("'>");
    s += F("<b>Kind 1 Geburtsdatum & Zeit:</b><br><input name='c1d' type='date' value='"); s += child1Date; s += F("' style='width:60%;display:inline-block;'><input name='c1t' type='time' value='"); s += child1Time; s += F("' style='width:35%;display:inline-block;float:right;'><br>");
    s += F("<b>Kind 2 Name:</b><input name='c2n' type='text' value='"); s += child2Name; s += F("'>");
    s += F("<b>Kind 2 Geburtsdatum & Zeit:</b><br><input name='c2d' type='date' value='"); s += child2Date; s += F("' style='width:60%;display:inline-block;'><input name='c2t' type='time' value='"); s += child2Time; s += F("' style='width:35%;display:inline-block;float:right;'><br>");
    s += F("<b>Kind 3 Name:</b><input name='c3n' type='text' value='"); s += child3Name; s += F("'>");
    s += F("<b>Kind 3 Geburtsdatum & Zeit:</b><br><input name='c3d' type='date' value='"); s += child3Date; s += F("' style='width:60%;display:inline-block;'><input name='c3t' type='time' value='"); s += child3Time; s += F("' style='width:35%;display:inline-block;float:right;'><br>");
    s += F("<input type='submit' class='btn' value='"); s += tr(sysLang, "Daten speichern", "Save data", "Enregistrer donnees"); s += F("'></form></div>");

    // 5. HACKER TOOLS
    s += F("<div id='p8' class='panel'>");
    s += F("<div style='position:sticky; top:6px; z-index:5; margin-bottom:10px;'><button type='button' class='btn btn-red' onclick='stopBadApps()' style='margin:0;'>");
    s += tr(sysLang, "Not-Aus: Security Apps stoppen", "Emergency Stop: Stop Security Apps", "Arret d'urgence: Stopper Security Apps");
    s += F("</button></div>");

    // LoRa SX1262 Off-Grid Funk Card
    s += F("<div class='card' style='margin-bottom:15px; background:var(--surface-2); padding:12px; border-radius:8px;'>");
    s += F("<div style='display:flex; justify-content:space-between; align-items:center; margin-bottom:8px;'>");
    s += F("<div><b style='font-size:14px;'>SX1262 LoRa Off-Grid Funk (868 MHz)</b><br><span id='loraReadyBadge' style='padding:2px 6px; border-radius:4px; font-size:11px; font-weight:600; background:#333; color:#aaa;'>Wird geladen...</span></div>");
    s += F("<button type='button' class='btn btn-blue' style='margin:0; padding:4px 10px; font-size:11px; width:auto;' onclick='refreshLoraApi()'>"); s += tr(sysLang, "Aktualisieren", "Refresh", "Actualiser"); s += F("</button>");
    s += F("</div>");
    s += F("<div style='display:flex; gap:12px; flex-wrap:wrap; font-size:12px; margin-bottom:10px;'>");
    s += F("<div>TX: <b id='loraTxCount' style='color:#00e5ff;'>0</b></div>");
    s += F("<div>RX: <b id='loraRxCount' style='color:#00ff88;'>0</b></div>");
    s += F("<div>RSSI: <b id='loraLastRssi' style='color:#ffb020;'>-- dBm</b></div>");
    s += F("<div>SNR: <b id='loraLastSnr' style='color:#ffb020;'>-- dB</b></div>");
    s += F("<div>Beacon: <b id='loraBeaconSt' style='color:#aaa;'>Aus</b></div>");
    s += F("</div>");
    s += F("<b>"); s += tr(sysLang, "Off-Grid Chat-Verlauf:", "Off-Grid Chat Feed:", "Fil de discussion Off-Grid :"); s += F("</b>");
    s += F("<div id='loraMsgFeed' style='background:#111; border:1px solid #444; border-radius:6px; padding:8px; min-height:80px; max-height:180px; overflow-y:auto; font-family:monospace; font-size:12px; margin-top:4px;'><i>"); s += tr(sysLang, "Keine LoRa-Nachrichten", "No LoRa messages", "Aucun message LoRa"); s += F("</i></div>");
    s += F("<div style='display:flex; gap:6px; margin:8px 0;'>");
    s += F("<input type='text' id='loraSenderInput' placeholder='"); s += tr(sysLang, "Absender", "Sender", "Expéditeur"); s += F("' value='WebDash' style='width:25%; font-size:12px;'>");
    s += F("<input type='text' id='loraMsgInput' placeholder='"); s += tr(sysLang, "Nachricht...", "Message...", "Message..."); s += F("' maxlength='64' style='flex:1; font-size:12px;' onkeydown='if(event.key===\"Enter\")sendLoraMsgApi()'>");
    s += F("<button type='button' class='btn btn-blue' style='margin:0; width:auto; padding:6px 14px;' onclick='sendLoraMsgApi()'>"); s += tr(sysLang, "Senden", "Send", "Envoyer"); s += F("</button>");
    s += F("</div>");
    s += F("<div style='display:flex; gap:6px; flex-wrap:wrap; margin-bottom:10px;'>");
    s += F("<button type='button' class='btn btn-gray' style='margin:0; padding:4px 8px; font-size:11px; width:auto;' onclick='quickLoraMsg(\"Hallo!\")'>Hallo!</button>");
    s += F("<button type='button' class='btn btn-gray' style='margin:0; padding:4px 8px; font-size:11px; width:auto;' onclick='quickLoraMsg(\"OK\")'>OK</button>");
    s += F("<button type='button' class='btn btn-gray' style='margin:0; padding:4px 8px; font-size:11px; width:auto;' onclick='quickLoraMsg(\"Ping?\")'>Ping?</button>");
    s += F("<button type='button' class='btn btn-gray' style='margin:0; padding:4px 8px; font-size:11px; width:auto;' onclick='quickLoraMsg(\"Hilfe!\")'>Hilfe!</button>");
    s += F("<button type='button' class='btn btn-gray' style='margin:0; padding:4px 8px; font-size:11px; width:auto;' onclick='quickLoraMsg(\"SOS 48.77N 9.18E\")'>SOS</button>");
    s += F("<button type='button' class='btn btn-gray' style='margin:0; padding:4px 8px; font-size:11px; width:auto;' onclick='quickLoraMsg(\"73 de WatchOS\")'>73 de Watch</button>");
    s += F("</div>");
    s += F("<div style='display:flex; gap:8px; flex-wrap:wrap; align-items:center;'>");
    s += F("<button type='button' id='loraBeaconBtn' class='btn btn-gray' style='margin:0; width:auto; padding:6px 12px; font-size:12px;' onclick='toggleLoraBeaconApi()'>Beacon Ping (5s)</button>");
    s += F("<button type='button' class='btn btn-gray' style='margin:0; width:auto; padding:6px 12px; font-size:12px;' onclick='sendLoraPingNow()'>Einzel-Ping</button>");
    s += F("<button type='button' class='btn btn-red' style='margin:0; width:auto; padding:6px 10px; font-size:11px;' onclick='clearLoraLogApi()'>"); s += tr(sysLang, "Verlauf leeren", "Clear log", "Vider"); s += F("</button>");
    s += F("</div></div><hr>");

    s += F("<h3>"); s += tr(sysLang, "Nmap Portscanner (Netzwerke)", "Nmap Port Scanner (Networks)", "Scanner Nmap (Reseaux)"); s += F("</h3>");
    s += F("<datalist id='scannedWifis'></datalist>");
    s += F("<input type='button' id='scanBtn' class='btn btn-blue' value='"); s += tr(sysLang, "WLANs scannen", "Scan WiFis", "Scanner les WiFis"); s += F("' onclick='scanWifi()'><br><br>");
    s += F("<form id='fHacker1' onsubmit='return saveAndRememberHacker()'>");
    for(int i=0; i<3; i++) {
        s += F("<b>Slot "); s += (i+1); s += F(":</b><div style='display:flex; gap:5px; margin-bottom:10px;'>");
        s += F("<input name='tgS"); s += i; s += F("' type='text' list='scannedWifis' placeholder='SSID' value='"); s += htmlEscape(targetSSIDs[i]); s += F("'>");
        s += F("<input name='tgP"); s += i; s += F("' type='text' placeholder='"); s += tr(sysLang, "Passwort", "Password", "Mot de passe"); s += F("' value='"); s += htmlEscape(targetPWDs[i]); s += F("'>");
        s += F("</div>");
    }
    s += F("<b>Basis-IP:</b><input name='nmapIP' type='text' value='"); s += htmlEscape(nmapBaseIP); s += F("'>");
    s += F("<b>Ports (kommagetrennt):</b><input name='nmapPorts' type='text' value='"); s += htmlEscape(nmapPorts); s += F("'>");
    s += F("<div style='display:flex; gap:8px; flex-wrap:wrap; margin:6px 0 12px 0;'>");
    s += F("<button type='button' class='btn btn-gray' style='margin:0; width:auto; padding:8px 10px;' onclick=\"applyPortPreset('quick')\">Quick</button>");
    s += F("<button type='button' class='btn btn-gray' style='margin:0; width:auto; padding:8px 10px;' onclick=\"applyPortPreset('webdev')\">WebDev</button>");
    s += F("<button type='button' class='btn btn-gray' style='margin:0; width:auto; padding:8px 10px;' onclick=\"applyPortPreset('full')\">Full</button>");
    s += F("</div>");
    s += F("<input type='submit' class='btn btn-red' style='margin-bottom:20px;' value='"); s += tr(sysLang, "Nmap Ziele speichern", "Save Nmap targets", "Enregistrer cibles Nmap"); s += F("'></form>");
    
    s += F("<b>Nmap Scan Ergebnisse:</b>");
    s += F("<pre style='background:#111; color:#0f0; padding:10px; border-radius:5px; min-height:50px; max-height:200px; overflow-y:auto; font-size:12px;'>");
    s += (nmapLog.length() > 0 ? htmlEscape(nmapLog) : String(tr(sysLang, "Bisher keine Scans...", "No scans yet...", "Aucun scan pour l'instant...")));
    s += F("</pre>");

    s += F("<hr><h3>"); s += tr(sysLang, "WiFi Deauther (Kicker)", "WiFi Deauther", "Deauther WiFi"); s += F("</h3>");
    s += F("<input type='button' id='scanRBtn' class='btn btn-blue' value='"); s += tr(sysLang, "Router scannen", "Scan routers", "Scanner les routeurs"); s += F("' onclick='scanRouters()'>");
    s += F("<div id='routerTable'></div>");

    s += F("<hr><h3>BadUSB / Rubber Ducky</h3><form id='fHacker2' onsubmit='return saveForm(\"fHacker2\")'>");
    s += F("<b>Tastatur-Layout:</b><br><select name='buLay'><option value='0'");
    if(badUsbLayout==0) s += F(" selected");
    s += F(">US (QWERTY)</option><option value='1'");
    if(badUsbLayout==1) s += F(" selected");
    s += F(">DE (QWERTZ)</option></select><br><br>");
    
    s += F("<details style='background:#222;padding:10px;border-radius:6px;margin-bottom:8px;'><summary style='cursor:pointer;color:#ffb020;font-weight:bold;'>"); s += tr(sysLang, "Skript 1", "Script 1", "Script 1"); s += F(": <span id='sN0'>"); s += htmlEscape(badUsbNames[0]); s += F("</span></summary>");
    s += F("Name: <input name='buN0' type='text' value='"); s += htmlEscape(badUsbNames[0]); s += F("' oninput='syncTitle(\"sN0\", this.value)'><br>");
    s += F("Code: <textarea name='buS0' rows='4' style='font-family:monospace; font-size:11px;'>"); s += htmlEscape(badUsbScripts[0]); s += F("</textarea></details>");

    s += F("<details style='background:#222;padding:10px;border-radius:6px;margin-bottom:8px;'><summary style='cursor:pointer;color:#ffb020;font-weight:bold;'>"); s += tr(sysLang, "Skript 2", "Script 2", "Script 2"); s += F(": <span id='sN1'>"); s += htmlEscape(badUsbNames[1]); s += F("</span></summary>");
    s += F("Name: <input name='buN1' type='text' value='"); s += htmlEscape(badUsbNames[1]); s += F("' oninput='syncTitle(\"sN1\", this.value)'><br>");
    s += F("Code: <textarea name='buS1' rows='4' style='font-family:monospace; font-size:11px;'>"); s += htmlEscape(badUsbScripts[1]); s += F("</textarea></details>");

    s += F("<details style='background:#222;padding:10px;border-radius:6px;margin-bottom:8px;'><summary style='cursor:pointer;color:#ffb020;font-weight:bold;'>"); s += tr(sysLang, "Skript 3", "Script 3", "Script 3"); s += F(": <span id='sN2'>"); s += htmlEscape(badUsbNames[2]); s += F("</span></summary>");
    s += F("Name: <input name='buN2' type='text' value='"); s += htmlEscape(badUsbNames[2]); s += F("' oninput='syncTitle(\"sN2\", this.value)'><br>");
    s += F("Code: <textarea name='buS2' rows='4' style='font-family:monospace; font-size:11px;'>"); s += htmlEscape(badUsbScripts[2]); s += F("</textarea></details>");
    
    s += F("<hr><h3>WiFi Spam (Beacons)</h3>");
    s += F("<input name='spamSsid' type='text' value='"); s += htmlEscape(spamSSIDs); s += F("'>");
    s += F("<br><br><input type='submit' class='btn btn-red' value='USB/Spam Tools Speichern'></form>");
    
    s += F("<hr><form id='fEvil' onsubmit='return saveForm(\"fEvil\")'>");
    s += F("<h3>Evil Portal Setup</h3>");
    s += F("<b>WLAN Name (SSID):</b><input name='evSsid' type='text' value='"); s += evilSSID; s += F("'><br><br>");
    s += F("<b>HTML Code für die Startseite:</b><br>");
    s += F("<textarea name='evHtml' rows='12' style='font-family:monospace; font-size:11px;'>"); s += htmlEscape(evilHTML); s += F("</textarea>");
    s += F("<div style='display:flex;gap:10px;'><button type='button' class='btn btn-blue' onclick='previewHTML()'>HTML Vorschau</button><input type='submit' class='btn btn-red' value='Evil Portal Speichern'></div></form>");
    
    s += F("<hr><h3>"); s += tr(sysLang, "Phishing Logbuch", "Phishing logbook", "Journal phishing"); s += F("</h3><div style='background:#111;color:#0f0;padding:10px;text-align:left;font-family:monospace;min-height:100px;max-height:300px;overflow-y:auto;'>");
    s += (evilLog.length() > 0 ? evilLog : String(tr(sysLang, "Bisher keine Phishing-Opfer...", "No phishing victims yet...", "Aucune victime de phishing pour l'instant...")));
    s += F("</div><br><form action='/clearlog' method='POST'><input type='submit' class='btn btn-gray' value='"); s += tr(sysLang, "Logbuch leeren", "Clear logbook", "Vider le journal"); s += F("'></form></div>");

    // 6. UPDATE
    s += F("<div id='p6' class='panel'><h3>"); s += tr(sysLang, "Firmware Update", "Firmware Update", "Mise a jour Firmware"); s += F("</h3>");
    s += F("<p>"); s += tr(sysLang, "WatchOS Version", "WatchOS Version", "Version WatchOS"); s += F(": <b id='ota_cur_ver' style='color:#ffb020;'>"); s += WATCHOS_VERSION; s += F("</b><br>Build: <b style='color:#ffb020;'>");
    s += String(__DATE__) + " - " + String(__TIME__);
    s += F("</b></p><hr><p>"); s += tr(sysLang, "Waehle die kompilierte .bin Datei aus.", "Select the compiled .bin file.", "Selectionnez le fichier .bin compile."); s += F("</p>");
    s += F("<input type='file' id='otafile' accept='.bin' onchange='updateOtaMeta()' style='background:#333;color:#fff;padding:10px;margin:8px 0;border-radius:6px;width:100%;box-sizing:border-box;'><br>");
    s += F("<div id='ota_meta' style='font-size:12px; color:#bbb; margin:4px 0;'></div>");
    s += F("<div id='ota_sha' style='font-family:monospace; font-size:11px; color:#9fd; margin:4px 0 10px 0; word-break:break-all;'></div>");
    s += F("<button type='button' class='btn btn-red' onclick='uploadOTA()'>"); s += tr(sysLang, "Firmware flashen", "Flash firmware", "Flasher le firmware"); s += F("</button>");
    s += F("<div id='otaprog-container' style='display:none;background:#222;border-radius:4px;margin-top:10px;height:20px;width:100%;overflow:hidden;'><div id='otaprog' style='background:#4CAF50;height:100%;width:0%;text-align:center;font-size:12px;line-height:20px;'>0%</div></div></div>");

    // 7. 2FA & MITTEILUNGEN (Panel 9)
    s += F("<div id='p9' class='panel'>");
    s += F("<h3>"); s += tr(sysLang, "2FA Authenticator &amp; Mitteilungen", "2FA Authenticator &amp; Notifications", "2FA Authenticator &amp; Notifications"); s += F("</h3>");
    
    // Notification Card
    s += F("<div style='background:var(--surface);border:1px solid var(--border);border-radius:8px;padding:14px;margin-bottom:16px;'>");
    s += F("<h4 style='margin:0 0 10px 0;color:var(--accent-2);'>"); s += tr(sysLang, "Push-Mitteilung an die Watch senden", "Send Push Notification to Watch", "Envoyer Notification à la Montre"); s += F("</h4>");
    s += F("<div style='display:flex;gap:10px;margin-bottom:8px;'><input type='text' id='notifTitle' placeholder='Titel (z.B. Alarm / Kalender)' style='flex:1;' value='Alert'><input type='text' id='notifMsg' placeholder='Nachricht' style='flex:2;' value='Hallo von WatchOS!'></div>");
    s += F("<div style='display:flex;gap:10px;'><button type='button' class='btn btn-blue' style='margin:0;' onclick='sendNotifApi()'>"); s += tr(sysLang, "Sofort an Watch senden", "Send to Watch Now", "Envoyer à la Montre"); s += F("</button><button type='button' class='btn btn-gray' style='margin:0;' onclick='fetch(\"/api/notify/clear\",{method:\"POST\"}).then(()=>loadNotifList())'>"); s += tr(sysLang, "Alle Mitteilungen löschen", "Clear All Notifications", "Tout effacer"); s += F("</button></div>");
    s += F("<div id='notifList' style='margin-top:10px;font-size:12px;color:var(--muted);'></div>");
    s += F("</div>");

    // 2FA TOTP Card
    s += F("<div style='background:var(--surface);border:1px solid var(--border);border-radius:8px;padding:14px;margin-bottom:16px;'>");
    s += F("<h4 style='margin:0 0 10px 0;color:var(--accent);'>"); s += tr(sysLang, "2FA TOTP Konten (RFC 6238)", "2FA TOTP Accounts (RFC 6238)", "Comptes 2FA TOTP (RFC 6238)"); s += F("</h4>");
    s += F("<div id='totpList'></div>");
    s += F("<hr style='border:0;border-top:1px solid var(--border);margin:14px 0;'>");
    s += F("<h5 style='margin:0 0 8px 0;'>Neues 2FA-Konto hinzufuegen:</h5>");
    s += F("<div style='display:flex;gap:10px;margin-bottom:8px;'><input type='text' id='newTotpName' placeholder='Name (z.B. GitHub)' style='flex:1;'><input type='text' id='newTotpSec' placeholder='Geheimschluessel (Base32, z.B. JBSWY3DPEHPK3PXP)' style='flex:2;'></div>");
    s += F("<button type='button' class='btn btn-green' style='margin:0;' onclick='addTotpApi()'>"); s += tr(sysLang, "2FA Konto speichern", "Save 2FA Account", "Enregistrer Compte 2FA"); s += F("</button>");
    s += F("</div>");

    // Wardriving & Recon Export Card
    s += F("<div style='background:var(--surface);border:1px solid var(--border);border-radius:8px;padding:14px;margin-bottom:16px;'>");
    s += F("<h4 style='margin:0 0 10px 0;color:var(--good);'>"); s += tr(sysLang, "Wardriving &amp; RF Recon Export", "Wardriving &amp; RF Recon Export", "Export Wardriving &amp; Recon"); s += F("</h4>");
    s += F("<p style='font-size:12px;color:var(--muted);margin:0 0 10px 0;'>"); s += tr(sysLang, "Exportiert alle gescannten Access Points und BLE Beacons als standardisierte WiGLE CSV-Datei zur Analyse.", "Exports all scanned APs and BLE beacons as standard WiGLE CSV.", "Exporte tous les AP et balises BLE scannés au format CSV WiGLE."); s += F("</p>");
    s += F("<a href='/api/recon/export' class='btn btn-blue' style='text-decoration:none;display:inline-block;padding:8px 16px;'>📥 "); s += tr(sysLang, "WiGLE CSV Export herunterladen", "Download WiGLE CSV", "Télécharger CSV WiGLE"); s += F("</a>");
    s += F("</div>");

    s += F("</div>"); // #p9

    // 8. MODULARE PAKETE & APPS (Panel p_apps)
    s += F("<div id='p_apps' class='panel'>");
    s += F("<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:12px;flex-wrap:wrap;gap:8px;'>");
    s += F("<div><h3 style='margin:0;'>"); s += tr(sysLang, "📦 Modulare Pakete &amp; App-Manager", "📦 Modular Packages &amp; App Manager", "📦 Paquets &amp; Gestionnaire d'Apps"); s += F("</h3>");
    s += F("<div class='hint'>"); s += tr(sysLang, "Aktiviere oder deaktiviere Anwendungen. Deaktivierte Apps werden sofort von der Uhr ausgeblendet.", "Enable or disable applications. Disabled apps are instantly hidden from the watch.", "Activez ou désactivez les applications. Les apps désactivées sont masquées sur la montre."); s += F("</div></div>");
    s += F("<button type='button' class='btn btn-gray' style='margin:0;width:auto;padding:6px 14px;font-size:12px;' onclick='resetPackagesApi()'>"); s += tr(sysLang, "Standard wiederherstellen", "Reset Defaults", "Rétablir valeurs par défaut"); s += F("</button>");
    s += F("</div>");

    // Upload Card
    s += F("<div style='background:var(--surface);border:1px dashed var(--accent);border-radius:8px;padding:16px;margin-bottom:16px;'>");
    s += F("<div style='display:flex;align-items:center;gap:12px;margin-bottom:10px;'>");
    s += F("<div style='font-size:24px;'>📦</div><div>");
    s += F("<h4 style='margin:0;color:var(--accent);'>"); s += tr(sysLang, "App-Paket installieren (.app)", "Install App Package (.app)", "Installer le paquet d'app (.app)"); s += F("</h4>");
    s += F("<div style='font-size:12px;color:var(--muted);'>"); s += tr(sysLang, "Lade ein App-Paket hoch. Die Watch prueft das Paket und fragt vor der Aktivierung nach Bestaetigung.", "Upload an app package. The watch verifies it and prompts for confirmation.", "Televersez un paquet. La montre le verifie et demande confirmation."); s += F("</div>");
    s += F("</div></div>");
    s += F("<div style='display:flex;gap:10px;align-items:center;flex-wrap:wrap;'>");
    s += F("<input type='file' id='appFileInput' accept='.app,.json' style='display:none;' onchange='handleAppFileSelect(this)'>");
    s += F("<button type='button' class='btn btn-blue' style='width:auto;margin:0;padding:8px 18px;font-size:13px;' onclick='document.getElementById(\"appFileInput\").click()'>📁 ");
    s += tr(sysLang, "App-Paket (.app) auswaehlen...", "Select App Package (.app)...", "Selectionner paquet (.app)...");
    s += F("</button>");
    s += F("</div></div>");

    // Produktivitaet
    s += F("<div style='background:var(--surface);border:1px solid var(--border);border-radius:8px;padding:14px;margin-bottom:16px;'>");
    s += F("<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:12px;'>");
    s += F("<h4 style='margin:0;color:#00ff88;'>🛠️ "); s += tr(sysLang, "Produktivitaet (Productivity)", "Productivity", "Productivite"); s += F("</h4>");
    s += F("<span class='badge' id='cntProd' style='background:var(--surface-2);color:#00ff88;padding:2px 8px;border-radius:4px;font-size:11px;'></span>");
    s += F("</div>");
    s += F("<div id='pkgListGood'><i>Lade Pakete...</i></div>");
    s += F("</div>");

    // Security & Hacker Tools
    s += F("<div style='background:var(--surface);border:1px solid var(--border);border-radius:8px;padding:14px;margin-bottom:16px;'>");
    s += F("<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:12px;'>");
    s += F("<h4 style='margin:0;color:var(--crit);'>⚡ "); s += tr(sysLang, "Security &amp; Hacker Tools (Security)", "Security &amp; Hacker Tools (Security)", "Securite &amp; Outils Hacker (Security)"); s += F("</h4>");
    s += F("<span class='badge' id='cntSec' style='background:var(--surface-2);color:var(--crit);padding:2px 8px;border-radius:4px;font-size:11px;'></span>");
    s += F("</div>");
    s += F("<div id='pkgListBad'><i>Lade Pakete...</i></div>");
    s += F("</div>");
    s += F("</div>"); // #p_apps

    s += F("</div>"); // .wrap

    // --- Dashboard-Detailfenster (Entwurf 3): Akku, WLAN, Bluetooth, Speicher ---
    s += F("<div class='veil' id='battVeil'><div class='modal' role='dialog' aria-modal='true'>");
    s += F("<button type='button' class='close' id='closeBatt' aria-label='"); s += tr(sysLang,"Schließen","Close","Fermer"); s += F("'>&times;</button>");
    s += F("<h3>"); s += tr(sysLang, "Akku-Details", "Battery details", "Détails batterie"); s += F("</h3><div class='hint'>");
    s += tr(sysLang, "Spannung, Modus-Mittelwerte &amp; Konfidenz der Schätzung", "Voltage, per-mode averages &amp; estimate confidence", "Tension, moyennes par mode &amp; confiance de l'estimation"); s += F("</div>");
    s += F("<div class='dl'>");
    s += F("<div class='row'><span class='k'>"); s += tr(sysLang,"Spannung","Voltage","Tension"); s += F("</span><span class='val' id='mdVolt'>--</span></div>");
    s += F("<div class='row'><span class='k'>"); s += tr(sysLang,"Restdauer","Remaining time","Durée restante"); s += F("</span><span class='val' id='mdEta'>--</span></div>");
    s += F("<div class='row'><span class='k'>"); s += tr(sysLang,"Konfidenz","Confidence","Confiance"); s += F("</span><span class='val' id='mdConf'>--</span></div>");
    s += F("</div><div class='seg-title'>"); s += tr(sysLang,"Gelernte Mittelwerte je Modus","Learned per-mode averages","Moyennes apprises par mode"); s += F("</div><div class='dl'>");
    s += F("<div class='row'><span class='k'>"); s += tr(sysLang,"Normal","Normal","Normal"); s += F("</span><span class='val' id='mdRateNormal'>--</span></div>");
    s += F("<div class='row'><span class='k'>"); s += tr(sysLang,"Hacking-Tools aktiv","Hacking tools active","Outils hacking actifs"); s += F("</span><span class='val' id='mdRateHacking' style='color:var(--crit)'>--</span></div>");
    s += F("<div class='row'><span class='k'>"); s += tr(sysLang,"Laden (USB)","Charging (USB)","Charge (USB)"); s += F("</span><span class='val' id='mdRateCharging' style='color:var(--charge)'>--</span></div>");
    s += F("</div><a href='/battery' class='link-out'>");
    s += tr(sysLang, "Vollständigen Verlauf öffnen →", "Open full history →", "Ouvrir l'historique complet →"); s += F("</a></div></div>");

    s += F("<div class='veil' id='connVeil'><div class='modal' role='dialog' aria-modal='true'>");
    s += F("<button type='button' class='close' id='closeConn' aria-label='"); s += tr(sysLang,"Schließen","Close","Fermer"); s += F("'>&times;</button>");
    s += F("<h3>"); s += tr(sysLang, "Verbindungsdetails &amp; Steuerung", "Connection &amp; WiFi Control", "Détails &amp; Contrôle WiFi"); s += F("</h3><div class='hint'>");
    s += tr(sysLang, "Aktuelle WLAN-Verbindung, Steuerung &amp; Historie", "Current WiFi connection, control &amp; history", "Connexion WiFi, contrôle &amp; historique"); s += F("</div>");
    s += F("<div class='dl'>");
    s += F("<div class='row'><span class='k'>"); s += tr(sysLang,"Modus","Mode","Mode"); s += F("</span><span class='val' id='cnMode'>--</span></div>");
    s += F("<div class='row'><span class='k'>SSID</span><span class='val' id='cnSsid'>--</span></div>");
    s += F("<div class='row'><span class='k'>IP</span><span class='val' id='cnIp'>--</span></div>");
    s += F("</div>");
    s += F("<div class='seg-title'>"); s += tr(sysLang, "WLAN Steuerung", "WiFi Controls", "Contrôles WiFi"); s += F("</div>");
    s += F("<div style='display:flex;gap:8px;flex-wrap:wrap;margin:10px 0;'>");
    s += F("<button type='button' class='btn btn-blue' style='flex:1;min-width:110px;margin:0;' onclick='toggleWifiApi()'>"); s += tr(sysLang, "WLAN Umschalten", "Toggle WiFi", "Basculer WiFi"); s += F("</button>");
    s += F("<button type='button' class='btn btn-gray' style='flex:1;min-width:110px;margin:0;' onclick='switchWifiModeApi()'>"); s += tr(sysLang, "Modus AP/Client", "Switch AP/Client", "Changer AP/Client"); s += F("</button>");
    s += F("<button type='button' class='btn' style='flex:1;min-width:110px;margin:0;' onclick='scanWifiApi()'>"); s += tr(sysLang, "Netze scannen", "Scan Networks", "Scanner"); s += F("</button>");
    s += F("</div><div id='wifiScanResult'></div>");
    s += F("<div class='seg-title'>"); s += tr(sysLang,"Verbindungshistorie","Connection history","Historique de connexion"); s += F("</div><div id='cnHistList'></div></div></div>");

    s += F("<div class='veil' id='btVeil'><div class='modal' role='dialog' aria-modal='true'>");
    s += F("<button type='button' class='close' id='closeBt' aria-label='"); s += tr(sysLang,"Schließen","Close","Fermer"); s += F("'>&times;</button>");
    s += F("<h3>"); s += tr(sysLang, "Bluetooth-Details &amp; Steuerung", "Bluetooth Details &amp; Control", "Détails &amp; Contrôle Bluetooth"); s += F("</h3><div class='hint'>");
    s += tr(sysLang, "Status, Steuerung &amp; Sichtbarkeit (BLE)", "Status, control &amp; visibility (BLE)", "Statut, contrôle &amp; visibilité (BLE)"); s += F("</div>");
    s += F("<div class='dl'>");
    s += F("<div class='row'><span class='k'>"); s += tr(sysLang,"Status","Status","Statut"); s += F("</span><span class='val' id='btmStatus'>--</span></div>");
    s += F("<div class='row'><span class='k'>"); s += tr(sysLang,"Sichtbarkeit","Visibility","Visibilité"); s += F("</span><span class='val' id='btmAdv'>--</span></div>");
    s += F("<div class='row'><span class='k'>"); s += tr(sysLang,"Gerätename","Device name","Nom de l'appareil"); s += F("</span><span class='val' id='btmName'>--</span></div>");
    s += F("</div>");

    s += F("<div class='seg-title'>"); s += tr(sysLang, "Gekoppeltes Smartphone", "Paired Smartphone", "Smartphone couplé"); s += F("</div>");
    s += F("<div class='dl' id='phonePairCard'>");
    s += F("<div class='row'><span class='k'>"); s += tr(sysLang, "Gerät", "Device", "Appareil"); s += F("</span><span class='val' id='phoneDevName'>");
    s += (phoneName.length() > 0 ? htmlEscape(phoneName) : F("Kein Smartphone gekoppelt"));
    s += F("</span></div>");
    s += F("<div class='row'><span class='k'>MAC</span><span class='val mono' id='phoneDevMac'>");
    s += (phoneMac.length() > 0 ? htmlEscape(phoneMac) : F("--:--:--:--:--:--"));
    s += F("</span></div>");
    s += F("<div class='row'><span class='k'>"); s += tr(sysLang, "Verbindung", "Connection", "Connexion"); s += F("</span><span class='val' id='phoneDevStatus'>");
    s += (phoneConnected ? F("<span style='color:var(--good);font-weight:600;'>Verbunden ✓</span>") : F("<span style='color:var(--muted);'>Getrennt</span>"));
    s += F("</span></div></div>");
    s += F("<div style='display:flex;gap:8px;margin-top:8px;'>");
    s += F("<button type='button' class='btn' style='flex:1;margin:0;font-size:12px;background:#ef444422;border:1px solid #ef4444;color:#ef4444;' onclick='unpairPhoneApi()'>"); s += tr(sysLang, "Entkoppeln", "Unpair", "Dissocier"); s += F("</button>");
    s += F("<button type='button' class='btn' style='flex:1;margin:0;font-size:12px;background:var(--surface-2);' onclick='pingPhoneApi()'>"); s += tr(sysLang, "Ping / Testen", "Ping / Test", "Tester"); s += F("</button>");
    s += F("</div>");

    s += F("<div class='seg-title'>"); s += tr(sysLang, "Web-Bluetooth (Direktzugriff fürs Smartphone)", "Web Bluetooth (Direct Phone Access)", "Web Bluetooth (Accès direct smartphone)"); s += F("</div>");
    s += F("<div class='hint' style='margin:4px 0 8px;'>");
    s += tr(sysLang, "Akkuschonende Direktverbindung über BLE im mobilen Browser (z.B. Chrome/Bluefy) ohne aktiven WLAN-Hotspot.", "Battery-saving direct BLE connection via mobile browser without WiFi hotspot.", "Connexion BLE directe économe en batterie via le navigateur mobile sans hotspot WiFi.");
    s += F("</div>");
    s += F("<button type='button' class='btn' style='width:100%;margin:0;background:linear-gradient(135deg,#2563eb,#3b82f6);color:#fff;font-weight:600;padding:9px;' onclick='connectWebBle()'>");
    s += tr(sysLang, "⚡ Per Web-Bluetooth verbinden", "⚡ Connect via Web-Bluetooth", "⚡ Se connecter via Web-Bluetooth");
    s += F("</button><div id='webBleStatus' style='margin-top:6px;font-size:12px;text-align:center;'></div>");

    s += F("<div class='seg-title'>"); s += tr(sysLang, "Bluetooth Steuerung", "Bluetooth Controls", "Contrôles Bluetooth"); s += F("</div>");
    s += F("<div style='display:flex;gap:8px;flex-wrap:wrap;margin:10px 0;'>");
    s += F("<button type='button' class='btn btn-blue' style='flex:1;min-width:120px;margin:0;' onclick='toggleBtApi()'>"); s += tr(sysLang, "Bluetooth Umschalten", "Toggle Bluetooth", "Basculer Bluetooth"); s += F("</button>");
    s += F("<button type='button' class='btn' style='flex:1;min-width:120px;margin:0;background:#8b5cf6;' onclick='scanBleApi()'>"); s += tr(sysLang, "BLE Geräte scannen", "Scan BLE Devices", "Scanner BLE"); s += F("</button>");
    s += F("</div><div id='bleScanResult'></div>");
    s += F("<div class='hint' style='margin-top:14px'>"); s += tr(sysLang, "Hinweis: Ermöglicht das Steuern und Scannen von BLE-Geräten in Reichweite direkt aus dem Dashboard.", "Note: Allows direct control and scanning of nearby BLE devices from this dashboard.", "Remarque : Permet le contrôle et le scan d'appareils BLE directement depuis le tableau de bord."); s += F("</div></div></div>");

    s += F("<div class='veil' id='storageVeil'><div class='modal' role='dialog' aria-modal='true'>");
    s += F("<button type='button' class='close' id='closeStorage' aria-label='"); s += tr(sysLang,"Schließen","Close","Fermer"); s += F("'>&times;</button>");
    s += F("<h3>"); s += tr(sysLang, "Speicher &amp; Dateimanager", "Storage &amp; File Manager", "Stockage &amp; Gestionnaire de fichiers"); s += F("</h3><div class='hint'>");
    s += tr(sysLang, "Was auf der Uhr abgelegt ist — und wie viel noch reinpasst", "What's stored on the watch — and how much room is left", "Ce qui est stocké sur la montre — et l'espace restant"); s += F("</div>");
    s += F("<div class='seg-title'>NVS · "); s += tr(sysLang,"Einstellungen","Settings","Réglages"); s += F(" <span class='cnt' id='stNvsTxt'>--</span></div><div class='sbar' style='height:8px;border-radius:99px;background:var(--surface-2);overflow:hidden'><i id='stNvsUsedBar' style='display:block;height:100%;background:var(--accent);width:0%'></i></div><div id='nvsCatList' class='catlist'></div>");
    s += F("<div class='seg-title'>FFat · "); s += tr(sysLang,"Dateien","Files","Fichiers"); s += F(" <span class='cnt' id='stFlashTxt'>--</span></div><div class='sbar' style='height:8px;border-radius:99px;background:var(--surface-2);overflow:hidden'><i id='stFlashUsedBar' style='display:block;height:100%;background:var(--good);width:0%'></i></div>");
    s += F("<div style='display:flex;justify-content:space-between;align-items:center;margin:12px 0 8px;gap:8px;'>");
    s += F("<span style='font-size:12px;font-weight:600;color:var(--text);'>"); s += tr(sysLang, "Dateimanager (FFat)", "File Manager (FFat)", "Gestionnaire de fichiers (FFat)"); s += F("</span>");
    s += F("<div style='display:flex;gap:6px;'>");
    s += F("<button type='button' class='btn' style='margin:0;padding:4px 9px;font-size:11.5px;background:var(--good);color:#fff;' onclick='newFilePrompt()'>+ "); s += tr(sysLang, "Neue Datei", "New File", "Nouveau fichier"); s += F("</button>");
    s += F("<button type='button' class='btn' style='margin:0;padding:4px 9px;font-size:11.5px;background:var(--surface-2);' onclick='loadStorageDetail()'>↻</button>");
    s += F("</div></div>");
    s += F("<div id='ffatList'></div></div></div>");

    // System-Verlauf (CPU-Takt/-Temperatur ueber laengere Zeitraeume, Klick auf die Charts)
    s += F("<div class='veil' id='sysHistVeil'><div class='modal' role='dialog' aria-modal='true'>");
    s += F("<button type='button' class='close' id='closeSysHist' aria-label='"); s += tr(sysLang,"Schließen","Close","Fermer"); s += F("'>&times;</button>");
    s += F("<h3>"); s += tr(sysLang, "Systemverlauf", "System history", "Historique système"); s += F("</h3><div class='hint'>"); s += tr(sysLang, "CPU-Takt &amp; -Temperatur über längere Zeiträume", "CPU clock &amp; temperature over longer periods", "Fréquence et température CPU sur des périodes plus longues"); s += F("</div>");
    s += F("<div class='seg-toggle'><button type='button' class='active' id='sysTabFreq'>"); s += tr(sysLang,"CPU-Takt","CPU clock","Fréquence CPU"); s += F("</button><button type='button' id='sysTabTemp'>"); s += tr(sysLang,"CPU-Temperatur","CPU temperature","Température CPU"); s += F("</button></div>");
    s += F("<div class='range-row'><button type='button' data-range='6h'>6h</button><button type='button' data-range='24h' class='active'>24h</button><button type='button' data-range='72h'>72h</button><button type='button' data-range='all'>"); s += tr(sysLang,"Alle","All","Tout"); s += F("</button></div>");
    s += F("<canvas class='spark' id='sparkSysHistBig' width='620' height='180' style='height:180px'></canvas>");
    s += F("<div class='stat-row'><div class='cell'><div class='t'>Min</div><div class='v' id='sysHistMin'>--</div></div><div class='cell'><div class='t'>Ø</div><div class='v' id='sysHistAvg'>--</div></div><div class='cell'><div class='t'>Max</div><div class='v' id='sysHistMax'>--</div></div></div>");
    s += F("</div></div>");

    // Schritt-Verlauf: Balkendiagramm je Stunde (Klick auf Schritte&Ziel-Balken)
    s += F("<div class='veil' id='stepHistVeil'><div class='modal' role='dialog' aria-modal='true'>");
    s += F("<button type='button' class='close' id='closeStepHist' aria-label='"); s += tr(sysLang,"Schließen","Close","Fermer"); s += F("'>&times;</button>");
    s += F("<h3>"); s += tr(sysLang, "Schrittverlauf heute", "Today's step history", "Historique des pas aujourd'hui"); s += F("</h3><div class='hint'>"); s += tr(sysLang, "Schritte je Stunde seit Mitternacht", "Steps per hour since midnight", "Pas par heure depuis minuit"); s += F("</div>");
    s += F("<div class='hbar-chart' id='stepHistChart'></div>");
    s += F("<div class='stat-row'><div class='cell'><div class='t'>"); s += tr(sysLang,"Gesamt heute","Total today","Total aujourd'hui"); s += F("</div><div class='v' id='stepHistTotal'>--</div></div><div class='cell'><div class='t'>"); s += tr(sysLang,"Aktivste Stunde","Busiest hour","Heure la plus active"); s += F("</div><div class='v' id='stepHistPeak'>--</div></div></div>");
    s += F("</div></div>");

    // App-Installation Bestaetigungs-Modal
    s += F("<div class='veil' id='appInstallVeil'><div class='modal' role='dialog' aria-modal='true' style='max-width:440px;'>");
    s += F("<button type='button' class='close' onclick='closeAppInstallModal()' aria-label='"); s += tr(sysLang,"Schließen","Close","Fermer"); s += F("'>&times;</button>");
    s += F("<h3>📦 "); s += tr(sysLang, "App-Installation", "App Installation", "Installation d'app"); s += F("</h3><div class='hint' style='margin-bottom:14px;'>");
    s += tr(sysLang, "Die Watch hat ein gueltiges App-Paket erkannt. Moechten Sie dieses Paket jetzt installieren?", "The watch detected a valid app package. Do you want to install it now?", "La montre a detecte un paquet valide. Voulez-vous l'installer maintenant ?");
    s += F("</div>");
    s += F("<div style='display:flex;gap:12px;align-items:center;background:var(--surface-2);padding:12px;border-radius:8px;margin-bottom:14px;border:1px solid var(--border);'>");
    s += F("<div id='aiIcon' style='font-family:monospace;font-weight:bold;font-size:18px;padding:10px 14px;border-radius:6px;background:#222;color:#00e5ff;border:1px solid #00e5ff44;'>APP</div>");
    s += F("<div><div id='aiName' style='font-weight:700;font-size:16px;'>--</div>");
    s += F("<div style='font-size:12px;color:var(--muted);'>ID: <span id='aiId' style='font-family:monospace;'>--</span> · v<span id='aiVer'>--</span></div></div></div>");
    s += F("<div class='dl' style='margin-bottom:16px;'>");
    s += F("<div class='row'><span class='k'>"); s += tr(sysLang, "Kategorie", "Category", "Categorie"); s += F("</span><span class='val' id='aiCat'>--</span></div>");
    s += F("<div class='row'><span class='k'>"); s += tr(sysLang, "Entwickler", "Author", "Auteur"); s += F("</span><span class='val' id='aiAuthor'>--</span></div>");
    s += F("<div class='row'><span class='k'>"); s += tr(sysLang, "Beschreibung", "Description", "Description"); s += F("</span><span class='val' id='aiDesc' style='text-align:right;max-width:220px;'>--</span></div>");
    s += F("<div class='row'><span class='k'>"); s += tr(sysLang, "App-Typ", "App type", "Type d'app"); s += F("</span><span class='val' id='aiPage'>--</span></div>");
    s += F("</div>");
    s += F("<div style='display:flex;gap:10px;'>");
    s += F("<button type='button' class='btn btn-gray' style='flex:1;margin:0;' onclick='closeAppInstallModal()'>"); s += tr(sysLang, "✕ Abbrechen", "✕ Cancel", "✕ Annuler"); s += F("</button>");
    s += F("<button type='button' class='btn btn-green' id='btnDoInstall' style='flex:2;margin:0;font-weight:700;' onclick='confirmAppInstall()'>"); s += tr(sysLang, "🚀 Jetzt installieren", "🚀 Install now", "🚀 Installer maintenant"); s += F("</button>");
    s += F("</div></div></div>");

    // Matrix-Easter-Egg: Code-Gate (nur bei Eingabe von 6174) + Digital-Rain-Overlay mit Zitaten
    s += F("<div class='code-veil' id='codeVeil'><div class='code-modal'><h3>???</h3><div class='hint'>"); s += tr(sysLang, "Code eingeben", "Enter code", "Entrer le code"); s += F("</div>");
    s += F("<input type='text' id='codeInput' inputmode='numeric' maxlength='4' autocomplete='off'>");
    s += F("<div class='row2'><button type='button' id='codeCancel'>"); s += tr(sysLang,"Abbrechen","Cancel","Annuler"); s += F("</button><button type='button' class='primary' id='codeOk'>OK</button></div></div></div>");
    s += F("<div id='matrixOverlay'><canvas id='matrixCanvas'></canvas><div id='matrixQuote'></div><button id='matrixExit' type='button'>[ ESC ]</button></div>");

    s += F("<div id='toast' class='toast'>"); s += tr(sysLang, "Gespeichert", "Saved", "Enregistre"); s += F("! &#10004;</div></body></html>");
    return s;
}

void handleRoot() { 
    if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
    String html = getHtmlContent();
    server.setContentLength(html.length());
    server.send(200, "text/html", "");
    // Chunked senden um Heap-Spike zu vermeiden
    int pos = 0;
    while (pos < (int)html.length()) {
        int chunk = min(2048, (int)html.length() - pos);
        server.sendContent(html.substring(pos, pos + chunk));
        pos += chunk;
    }
    server.sendContent("");
}

void handleSave() {
    if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
    // Formulare identifizieren: Jedes Formular sendet ein hidden "_form" Feld
    String formId = server.hasArg("_form") ? server.arg("_form") : "";
    
    // DEBUG: Alle empfangenen POST-Parameter auflisten
    Serial.printf("[SAVE] formId='%s', args=%d\n", formId.c_str(), server.args());
    for (int i = 0; i < server.args(); i++) {
        Serial.printf("  [%d] %s = '%s'\n", i, server.argName(i).c_str(), server.arg(i).substring(0, 60).c_str());
    }
    
    // Variablen nur setzen wenn im POST enthalten
    if (server.hasArg("ssid")) userSSID = server.arg("ssid"); if (server.hasArg("pwd")) userPWD = server.arg("pwd"); if (server.hasArg("ntpz")) ntpZone = server.arg("ntpz");
    if (server.hasArg("apSsid")) apSSID = server.arg("apSsid"); if (server.hasArg("apPwd")) apPWD = server.arg("apPwd");
    if (server.hasArg("bm")) bootMode = server.arg("bm").toInt(); if (server.hasArg("aodM")) aodMode = server.arg("aodM").toInt();
    if (server.hasArg("tdim")) time_dim_start = server.arg("tdim").toInt() * 1000; if (server.hasArg("taod")) time_aod_start = server.arg("taod").toInt() * 1000; if (server.hasArg("tslp")) time_sleep_start = server.arg("tslp").toInt() * 1000;
    
    if (server.hasArg("maxb_pct")) maxBright = (server.arg("maxb_pct").toInt() * 255) / 100;
    if (server.hasArg("minb_pct")) minBright = (server.arg("minb_pct").toInt() * 255) / 100;
    if (server.hasArg("stepGoal")) { int g = server.arg("stepGoal").toInt(); if (g >= 500 && g <= 100000) stepGoal = g; }
    
    if (server.hasArg("cBG")) hexBG = server.arg("cBG"); if (server.hasArg("cFG")) hexFG = server.arg("cFG"); 
    if (server.hasArg("cAcc1")) hexAcc1 = server.arg("cAcc1"); if (server.hasArg("cAcc2")) hexAcc2 = server.arg("cAcc2"); 
    if (server.hasArg("cBat")) hexBat = server.arg("cBat");
    if (server.hasArg("cHBG")) hexHeadBG = server.arg("cHBG"); if (server.hasArg("cHFG")) hexHeadFG = server.arg("cHFG"); 
    if (server.hasArg("cABG")) hexAppBG = server.arg("cABG"); if (server.hasArg("cWrn")) hexWarn = server.arg("cWrn"); 
    
    if (server.hasArg("sysLang")) sysLang = server.arg("sysLang").toInt();
    
    if (server.hasArg("evSsid")) evilSSID = server.arg("evSsid"); if (server.hasArg("evHtml")) evilHTML = server.arg("evHtml"); 
    
    if (server.hasArg("a")) zielAlter = server.arg("a").toInt();
    if (server.hasArg("bday")) { String bday = server.arg("bday"); if(bday.length() == 10) { gJahr = bday.substring(0, 4).toInt(); gMonat = bday.substring(5, 7).toInt(); gTag = bday.substring(8, 10).toInt(); } }
    if (server.hasArg("btime")) { String btime = server.arg("btime"); if(btime.length() == 5) { gStunde = btime.substring(0,2).toInt(); gMinute = btime.substring(3,5).toInt(); } }
    if (server.hasArg("pN")) parentName = server.arg("pN"); if (server.hasArg("pD")) parentDate = server.arg("pD"); if (server.hasArg("pT")) parentTime = server.arg("pT");
    if (server.hasArg("ppN")) papaName = server.arg("ppN"); if (server.hasArg("ppD")) papaDate = server.arg("ppD"); if (server.hasArg("ppT")) papaTime = server.arg("ppT");
    if (server.hasArg("brandName")) { String bn = server.arg("brandName"); if (bn.length() > 0 && bn.length() <= 40) brandName = htmlEscape(bn); }
    if (server.hasArg("netS")) netSalary = server.arg("netS").toFloat(); if (server.hasArg("pt")) partTimePct = server.arg("pt").toInt(); if (server.hasArg("vac")) vacationDays = server.arg("vac").toInt(); if (server.hasArg("sick")) sickDays = server.arg("sick").toInt(); if (server.hasArg("wh")) workHoursPerDay = server.arg("wh").toFloat(); if (server.hasArg("wd")) workDaysYear = server.arg("wd").toInt();
    if (server.hasArg("c1n")) child1Name = server.arg("c1n"); if (server.hasArg("c1d")) child1Date = server.arg("c1d"); if (server.hasArg("c1t")) child1Time = server.arg("c1t");
    if (server.hasArg("c2n")) child2Name = server.arg("c2n"); if (server.hasArg("c2d")) child2Date = server.arg("c2d"); if (server.hasArg("c2t")) child2Time = server.arg("c2t");
    if (server.hasArg("c3n")) child3Name = server.arg("c3n"); if (server.hasArg("c3d")) child3Date = server.arg("c3d"); if (server.hasArg("c3t")) child3Time = server.arg("c3t");

    if (server.hasArg("buN0")) badUsbNames[0] = server.arg("buN0"); if (server.hasArg("buS0")) badUsbScripts[0] = server.arg("buS0");
    if (server.hasArg("buN1")) badUsbNames[1] = server.arg("buN1"); if (server.hasArg("buS1")) badUsbScripts[1] = server.arg("buS1");
    if (server.hasArg("buN2")) badUsbNames[2] = server.arg("buN2"); if (server.hasArg("buS2")) badUsbScripts[2] = server.arg("buS2");

    if (server.hasArg("tgS0")) targetSSIDs[0] = server.arg("tgS0"); if (server.hasArg("tgP0")) targetPWDs[0] = server.arg("tgP0");
    if (server.hasArg("tgS1")) targetSSIDs[1] = server.arg("tgS1"); if (server.hasArg("tgP1")) targetPWDs[1] = server.arg("tgP1");
    if (server.hasArg("tgS2")) targetSSIDs[2] = server.arg("tgS2"); if (server.hasArg("tgP2")) targetPWDs[2] = server.arg("tgP2");

    if (server.hasArg("spamSsid")) spamSSIDs = server.arg("spamSsid"); 
    if (server.hasArg("buLay")) badUsbLayout = server.arg("buLay").toInt(); 
    if (server.hasArg("nmapIP")) nmapBaseIP = server.arg("nmapIP"); 
    if (server.hasArg("nmapPorts")) nmapPorts = server.arg("nmapPorts");
    if (server.hasArg("hapW")) hapticWaveform = server.arg("hapW").toInt();
    if (server.hasArg("webUsr")) webUser = server.arg("webUsr");
    if (server.hasArg("webPwd")) webPass = server.arg("webPwd");

    // AOD Manager — Variablen aus POST lesen (gleiche Mechanik wie fDesign)
    if (server.hasArg("wfT"))  wfShowTime  = server.arg("wfT") == "1";
    if (server.hasArg("wfD"))  wfShowDate  = server.arg("wfD") == "1";
    if (server.hasArg("wfB"))  wfShowBatt  = server.arg("wfB") == "1";
    if (server.hasArg("wfS"))  wfShowSteps = server.arg("wfS") == "1";
    if (server.hasArg("wfWC")) wfShowWordClock = server.arg("wfWC") == "1";
    if (server.hasArg("wfBC")) wfShowBinClock  = server.arg("wfBC") == "1";
    if (server.hasArg("wfAn")) wfShowAnalog    = server.arg("wfAn") == "1";
    if (server.hasArg("wfLn")) wfShowLine      = server.arg("wfLn") == "1";
    if (server.hasArg("wfSA")) wfShowSecArc    = server.arg("wfSA") == "1";
    if (server.hasArg("wfWd")) wfShowWeekday   = server.arg("wfWd") == "1";
    if (server.hasArg("wfTY")) wfTimeY = server.arg("wfTY").toInt();
    if (server.hasArg("wfDY")) wfDateY = server.arg("wfDY").toInt();
    if (server.hasArg("wfBY")) wfBattY = server.arg("wfBY").toInt();
    if (server.hasArg("wfSY")) wfStepsY = server.arg("wfSY").toInt();
    if (server.hasArg("wfTX")) wfTimeX = server.arg("wfTX").toInt();
    if (server.hasArg("wfDX")) wfDateX = server.arg("wfDX").toInt();
    if (server.hasArg("wfBX")) wfBattX = server.arg("wfBX").toInt();
    if (server.hasArg("wfSX")) wfStepsX = server.arg("wfSX").toInt();
    if (server.hasArg("wfTSc")) wfTimeScale = server.arg("wfTSc").toFloat();
    if (server.hasArg("wfDSc")) wfDateScale = server.arg("wfDSc").toFloat();
    if (server.hasArg("wfBSc")) wfBattScale = server.arg("wfBSc").toFloat();
    if (server.hasArg("wfSSc")) wfStepsScale = server.arg("wfSSc").toFloat();
    if (server.hasArg("wfTFm")) wfTimeFmt = server.arg("wfTFm").toInt();
    if (server.hasArg("wfWCY")) wfWordClockY = server.arg("wfWCY").toInt();
    if (server.hasArg("wfWCX")) wfWordClockX = server.arg("wfWCX").toInt();
    if (server.hasArg("wfBCY")) wfBinClockY = server.arg("wfBCY").toInt();
    if (server.hasArg("wfBCX")) wfBinClockX = server.arg("wfBCX").toInt();
    if (server.hasArg("wfAnY")) wfAnalogY = server.arg("wfAnY").toInt();
    if (server.hasArg("wfAnX")) wfAnalogX = server.arg("wfAnX").toInt();
    if (server.hasArg("wfWCSc")) wfWordClockScale = server.arg("wfWCSc").toFloat();
    if (server.hasArg("wfBCSc")) wfBinClockScale = server.arg("wfBCSc").toFloat();
    if (server.hasArg("wfAnSc")) wfAnalogScale = server.arg("wfAnSc").toFloat();
    if (server.hasArg("wfAnSt")) wfAnalogStyle = server.arg("wfAnSt").toInt();
    if (server.hasArg("wfLnY")) wfLineY = server.arg("wfLnY").toInt();
    if (server.hasArg("wfLnX")) wfLineX = server.arg("wfLnX").toInt();
    if (server.hasArg("wfSAY")) wfSecArcY = server.arg("wfSAY").toInt();
    if (server.hasArg("wfSAX")) wfSecArcX = server.arg("wfSAX").toInt();
    if (server.hasArg("wfWdY")) wfWeekdayY = server.arg("wfWdY").toInt();
    if (server.hasArg("wfWdX")) wfWeekdayX = server.arg("wfWdX").toInt();
    if (server.hasArg("wfLnSc")) wfLineScale = server.arg("wfLnSc").toFloat();
    if (server.hasArg("wfSASc")) wfSecArcScale = server.arg("wfSASc").toFloat();
    if (server.hasArg("wfWdSc")) wfWeekdayScale = server.arg("wfWdSc").toFloat();
    if (server.hasArg("aodFG")) hexAodFG = server.arg("aodFG");
    if (server.hasArg("aodAcc")) hexAodAcc = server.arg("aodAcc");
    
    // Checkboxen NUR aktualisieren wenn das Views-Formular gesendet wurde
    if (formId == "fViews") {
        enP0 = server.hasArg("enP0"); enP1 = server.hasArg("enP1"); enP2 = server.hasArg("enP2"); enP3 = server.hasArg("enP3");
    }
    if (formId == "fSys") {
        webAuthEnabled = server.hasArg("webAuth");
    }
    if (server.hasArg("tAlarm")) timerAlarmMode = server.arg("tAlarm").toInt();

    // NUR die Preferences speichern die zum jeweiligen Formular gehören
    Serial.printf("[SAVE] formId='%s' - NVS oeffnen...\n", formId.c_str());
    bool nvsOK = prefs.begin("clock", false);
    Serial.printf("[SAVE] NVS open=%d freeEntries=%d\n", nvsOK, prefs.freeEntries());
    if (!nvsOK) { Serial.println("[SAVE] FEHLER: NVS open failed!"); server.send(500, "text/plain", "NVS Error"); return; }
    
    if (formId == "fDesign") {
        Serial.printf("[SAVE] Design: cBG=%s cHBG=%s cHFG=%s bootM=%d aodM=%d\n", hexBG.c_str(), hexHeadBG.c_str(), hexHeadFG.c_str(), bootMode, aodMode);
        prefs.putInt("bootM", bootMode); prefs.putInt("aodM", aodMode);
        prefs.putString("cBG", hexBG); prefs.putString("cFG", hexFG); prefs.putString("cAcc1", hexAcc1); prefs.putString("cAcc2", hexAcc2); prefs.putString("cBat", hexBat);
        prefs.putString("cHBG", hexHeadBG); prefs.putString("cHFG", hexHeadFG); prefs.putString("cABG", hexAppBG); prefs.putString("cWrn", hexWarn);
    }
    else if (formId == "fViews") {
        prefs.putBool("enP0", enP0); prefs.putBool("enP1", enP1); prefs.putBool("enP2", enP2); prefs.putBool("enP3", enP3);
    }
    else if (formId == "fSys") {
        prefs.putString("ssid", userSSID); prefs.putString("pwd", userPWD); prefs.putString("ntpZ", ntpZone); prefs.putString("apS", apSSID); prefs.putString("apP", apPWD);
        prefs.putULong("tDim", time_dim_start); prefs.putULong("tAod", time_aod_start); prefs.putULong("tSlp", time_sleep_start);
        prefs.putInt("maxB", maxBright); prefs.putInt("minB", minBright);
        prefs.putInt("lang", sysLang);
        prefs.putInt("tAlarm", timerAlarmMode);
        prefs.putInt("hapW", hapticWaveform);
        prefs.putString("webUsr", webUser); prefs.putString("webPwd", webPass); prefs.putBool("webAuth", webAuthEnabled);
    }
    else if (formId == "fPers") {
        prefs.putInt("tag", gTag); prefs.putInt("monat", gMonat); prefs.putInt("jahr", gJahr); prefs.putInt("std", gStunde); prefs.putInt("min", gMinute); prefs.putInt("alter", zielAlter);
        prefs.putString("pN", parentName); prefs.putString("pD", parentDate); prefs.putString("pT", parentTime);
        prefs.putString("ppN", papaName); prefs.putString("ppD", papaDate); prefs.putString("ppT", papaTime);
        prefs.putFloat("netS", netSalary); prefs.putInt("pt", partTimePct); prefs.putInt("vac", vacationDays); prefs.putInt("sick", sickDays); prefs.putFloat("wh", workHoursPerDay); prefs.putInt("wd", workDaysYear);
        prefs.putString("c1n", child1Name); prefs.putString("c1d", child1Date); prefs.putString("c1t", child1Time); prefs.putString("c2n", child2Name); prefs.putString("c2d", child2Date); prefs.putString("c2t", child2Time); prefs.putString("c3n", child3Name); prefs.putString("c3d", child3Date); prefs.putString("c3t", child3Time);
    }
    else if (formId == "fBrand") {
        prefs.putString("brandName", brandName);
    }
    else if (formId == "fStepGoal") {
        prefs.putInt("stepGoal", stepGoal);
    }
    else if (formId == "fHacker1") {
        prefs.putString("spamSsid", spamSSIDs); prefs.putInt("buLay", badUsbLayout); prefs.putString("nmapIP", nmapBaseIP);
        prefs.putString("nmapPorts", nmapPorts);
        prefs.putString("tgS0", targetSSIDs[0]); prefs.putString("tgP0", targetPWDs[0]); prefs.putString("tgS1", targetSSIDs[1]); prefs.putString("tgP1", targetPWDs[1]); prefs.putString("tgS2", targetSSIDs[2]); prefs.putString("tgP2", targetPWDs[2]);
    }
    else if (formId == "fHacker2") {
        prefs.putString("buN0", badUsbNames[0]); prefs.putString("buS0", badUsbScripts[0]); prefs.putString("buN1", badUsbNames[1]); prefs.putString("buS1", badUsbScripts[1]); prefs.putString("buN2", badUsbNames[2]); prefs.putString("buS2", badUsbScripts[2]);
        // Also save large scripts to FFat
        if (littleFsReady) { for(int i=0; i<3; i++) writeFileToFS("/badusb" + String(i) + ".txt", badUsbScripts[i]); }
    }
    else if (formId == "fEvil") {
        prefs.putString("evSsid", evilSSID); prefs.putString("evHtml", evilHTML);
        // Also save large HTML to FFat
        if (littleFsReady) writeFileToFS("/evil.html", evilHTML);
    }
    else if (formId == "fWatchface") {
        Serial.println("[SAVE-AOD] Verarbeite gebuendeltes JSON aus AOD Manager...");

        // Prüfen, ob unser zusammengefasstes JSON-Paket angekommen ist
        if (server.hasArg("aodData")) {
            String body = server.arg("aodData");
            
            // Dein bewährter Minimal-JSON-Parser
            auto jStr = [&](const String& key) -> String { int i = body.indexOf("\"" + key + "\":\""); if(i<0) return ""; i += key.length()+4; int e = body.indexOf("\"", i); return (e>i) ? body.substring(i, e) : ""; };
            auto jInt = [&](const String& key, int def) -> int { int i = body.indexOf("\"" + key + "\":"); if(i<0) return def; i += key.length()+3; int e = i; while(e < (int)body.length() && (isdigit(body[e]) || body[e]=='-')) e++; return (e>i) ? body.substring(i, e).toInt() : def; };
            auto jFloat = [&](const String& key, float def) -> float { int i = body.indexOf("\"" + key + "\":"); if(i<0) return def; i += key.length()+3; int e = i; while(e < (int)body.length() && (isdigit(body[e]) || body[e]=='.' || body[e]=='-')) e++; return (e>i) ? body.substring(i, e).toFloat() : def; };

            // Variablen aus dem JSON parsen
            wfShowTime = jInt("wfT", wfShowTime); wfShowDate = jInt("wfD", wfShowDate); wfShowBatt = jInt("wfB", wfShowBatt); wfShowSteps = jInt("wfS", wfShowSteps);
            wfTimeY = jInt("wfTY", wfTimeY); wfDateY = jInt("wfDY", wfDateY); wfBattY = jInt("wfBY", wfBattY); wfStepsY = jInt("wfSY", wfStepsY);
            wfTimeX = jInt("wfTX", wfTimeX); wfDateX = jInt("wfDX", wfDateX); wfBattX = jInt("wfBX", wfBattX); wfStepsX = jInt("wfSX", wfStepsX);
            wfTimeScale = jFloat("wfTSc", wfTimeScale); wfDateScale = jFloat("wfDSc", wfDateScale); wfBattScale = jFloat("wfBSc", wfBattScale); wfStepsScale = jFloat("wfSSc", wfStepsScale);
            wfTimeFmt = jInt("wfTFm", wfTimeFmt);

            wfShowWordClock = jInt("wfWC", wfShowWordClock); wfShowBinClock = jInt("wfBC", wfShowBinClock); wfShowAnalog = jInt("wfAn", wfShowAnalog);
            wfWordClockY = jInt("wfWCY", wfWordClockY); wfWordClockX = jInt("wfWCX", wfWordClockX);
            wfBinClockY = jInt("wfBCY", wfBinClockY); wfBinClockX = jInt("wfBCX", wfBinClockX);
            wfAnalogY = jInt("wfAnY", wfAnalogY); wfAnalogX = jInt("wfAnX", wfAnalogX);
            wfWordClockScale = jFloat("wfWCSc", wfWordClockScale); wfBinClockScale = jFloat("wfBCSc", wfBinClockScale); wfAnalogScale = jFloat("wfAnSc", wfAnalogScale);
            wfAnalogStyle = jInt("wfAnSt", wfAnalogStyle);

            wfShowLine = jInt("wfLn", wfShowLine); wfShowSecArc = jInt("wfSA", wfShowSecArc); wfShowWeekday = jInt("wfWd", wfShowWeekday);
            wfLineY = jInt("wfLnY", wfLineY); wfLineX = jInt("wfLnX", wfLineX);
            wfSecArcY = jInt("wfSAY", wfSecArcY); wfSecArcX = jInt("wfSAX", wfSecArcX);
            wfWeekdayY = jInt("wfWdY", wfWeekdayY); wfWeekdayX = jInt("wfWdX", wfWeekdayX);
            wfLineScale = jFloat("wfLnSc", wfLineScale); wfSecArcScale = jFloat("wfSASc", wfSecArcScale); wfWeekdayScale = jFloat("wfWdSc", wfWeekdayScale);

            // Farben extrahieren
            String tFG = jStr("aodFG"); if(tFG != "") hexAodFG = tFG;
            String tAc = jStr("aodAcc"); if(tAc != "") hexAodAcc = tAc;
        }

        // Alles ins NVS abspeichern
        prefs.putBool("wfT", wfShowTime); prefs.putBool("wfD", wfShowDate); prefs.putBool("wfB", wfShowBatt); prefs.putBool("wfS", wfShowSteps);
        prefs.putInt("wfTY", wfTimeY); prefs.putInt("wfDY", wfDateY); prefs.putInt("wfBY", wfBattY); prefs.putInt("wfSY", wfStepsY);
        prefs.putInt("wfTX", wfTimeX); prefs.putInt("wfDX", wfDateX); prefs.putInt("wfBX", wfBattX); prefs.putInt("wfSX", wfStepsX);
        prefs.putFloat("wfTSc", wfTimeScale); prefs.putFloat("wfDSc", wfDateScale); prefs.putFloat("wfBSc", wfBattScale); prefs.putFloat("wfSSc", wfStepsScale);
        prefs.putInt("wfTFm", wfTimeFmt);
        prefs.putBool("wfWC", wfShowWordClock); prefs.putBool("wfBC", wfShowBinClock); prefs.putBool("wfAn", wfShowAnalog);
        prefs.putInt("wfWCY", wfWordClockY); prefs.putInt("wfWCX", wfWordClockX);
        prefs.putInt("wfBCY", wfBinClockY); prefs.putInt("wfBCX", wfBinClockX);
        prefs.putInt("wfAnY", wfAnalogY); prefs.putInt("wfAnX", wfAnalogX);
        prefs.putFloat("wfWCSc", wfWordClockScale); prefs.putFloat("wfBCSc", wfBinClockScale); prefs.putFloat("wfAnSc", wfAnalogScale);
        prefs.putInt("wfAnSt", wfAnalogStyle);
        prefs.putBool("wfLn", wfShowLine); prefs.putBool("wfSA", wfShowSecArc); prefs.putBool("wfWd", wfShowWeekday);
        prefs.putInt("wfLnY", wfLineY); prefs.putInt("wfLnX", wfLineX);
        prefs.putInt("wfSAY", wfSecArcY); prefs.putInt("wfSAX", wfSecArcX);
        prefs.putInt("wfWdY", wfWeekdayY); prefs.putInt("wfWdX", wfWeekdayX);
        prefs.putFloat("wfLnSc", wfLineScale); prefs.putFloat("wfSASc", wfSecArcScale); prefs.putFloat("wfWdSc", wfWeekdayScale);
        prefs.putString("hAodFG", hexAodFG); prefs.putString("hAodAc", hexAodAcc);
    }
    else {
        // Fallback: Alles speichern (kompatibilität mit alten Aufrufen ohne _form)
        prefs.putString("ssid", userSSID); prefs.putString("pwd", userPWD); prefs.putString("ntpZ", ntpZone); prefs.putString("apS", apSSID); prefs.putString("apP", apPWD);
        prefs.putInt("tag", gTag); prefs.putInt("monat", gMonat); prefs.putInt("jahr", gJahr); prefs.putInt("std", gStunde); prefs.putInt("min", gMinute); prefs.putInt("alter", zielAlter);
        prefs.putInt("bootM", bootMode); prefs.putInt("aodM", aodMode);
        prefs.putULong("tDim", time_dim_start); prefs.putULong("tAod", time_aod_start); prefs.putULong("tSlp", time_sleep_start);
        prefs.putInt("maxB", maxBright); prefs.putInt("minB", minBright);
        prefs.putString("cBG", hexBG); prefs.putString("cFG", hexFG); prefs.putString("cAcc1", hexAcc1); prefs.putString("cAcc2", hexAcc2); prefs.putString("cBat", hexBat);
        prefs.putString("cHBG", hexHeadBG); prefs.putString("cHFG", hexHeadFG); prefs.putString("cABG", hexAppBG); prefs.putString("cWrn", hexWarn);
        prefs.putInt("lang", sysLang);
        prefs.putString("pN", parentName); prefs.putString("pD", parentDate); prefs.putString("pT", parentTime);
        prefs.putString("ppN", papaName); prefs.putString("ppD", papaDate); prefs.putString("ppT", papaTime);
        prefs.putString("brandName", brandName);
        prefs.putFloat("netS", netSalary); prefs.putInt("pt", partTimePct); prefs.putInt("vac", vacationDays); prefs.putInt("sick", sickDays); prefs.putFloat("wh", workHoursPerDay); prefs.putInt("wd", workDaysYear);
        prefs.putString("c1n", child1Name); prefs.putString("c1d", child1Date); prefs.putString("c1t", child1Time); prefs.putString("c2n", child2Name); prefs.putString("c2d", child2Date); prefs.putString("c2t", child2Time); prefs.putString("c3n", child3Name); prefs.putString("c3d", child3Date); prefs.putString("c3t", child3Time);
        prefs.putString("evSsid", evilSSID); prefs.putString("evHtml", evilHTML);
        prefs.putString("buN0", badUsbNames[0]); prefs.putString("buS0", badUsbScripts[0]); prefs.putString("buN1", badUsbNames[1]); prefs.putString("buS1", badUsbScripts[1]); prefs.putString("buN2", badUsbNames[2]); prefs.putString("buS2", badUsbScripts[2]);
        prefs.putString("tgS0", targetSSIDs[0]); prefs.putString("tgP0", targetPWDs[0]); prefs.putString("tgS1", targetSSIDs[1]); prefs.putString("tgP1", targetPWDs[1]); prefs.putString("tgS2", targetSSIDs[2]); prefs.putString("tgP2", targetPWDs[2]);
        prefs.putString("spamSsid", spamSSIDs); prefs.putInt("buLay", badUsbLayout); prefs.putString("nmapIP", nmapBaseIP);
        prefs.putBool("enP0", enP0); prefs.putBool("enP1", enP1); prefs.putBool("enP2", enP2); prefs.putBool("enP3", enP3);
        prefs.putInt("tAlarm", timerAlarmMode);
    }
    
    Serial.printf("[SAVE] NVS freeEntries nach Speichern=%d\n", prefs.freeEntries());
    prefs.end();
    
    updateZielZeit(); applyTheme(); lastSecond = -1; tft.fillScreen(cBG); setBrightness(maxBright); 
    Serial.println("[SAVE] Fertig - OK gesendet.");
    server.send(200, "text/plain", "OK");
}

void handleSetTime() {
    if(server.hasArg("md") && server.hasArg("mt")) {
        String d = server.arg("md"); String t = server.arg("mt");
        if(d.length() == 10 && t.length() == 5) {
            struct tm tm_new;
            tm_new.tm_year = d.substring(0, 4).toInt() - 1900; tm_new.tm_mon = d.substring(5, 7).toInt() - 1; tm_new.tm_mday = d.substring(8, 10).toInt();
            tm_new.tm_hour = t.substring(0,2).toInt(); tm_new.tm_min = t.substring(3,5).toInt(); tm_new.tm_sec = 0; tm_new.tm_isdst = server.hasArg("dst") ? 1 : 0;
            time_t t_of_day = mktime(&tm_new); struct timeval tv; tv.tv_sec = t_of_day; tv.tv_usec = 0; settimeofday(&tv, NULL); lastSecond = -1;
        }
    }
    server.send(200, "text/plain", "OK");
}

void handleSyncTimeApi() {
    long epoch = 0;
    if (server.hasArg("epoch")) {
        epoch = server.arg("epoch").toInt();
    } else if (server.hasArg("plain")) {
        String p = server.arg("plain");
        int idx = p.indexOf("epoch");
        if (idx >= 0) {
            epoch = p.substring(idx + 7).toInt();
        }
    }
    if (epoch > 1700000000L) {
        if (ntpZone.length() == 0) ntpZone = "CET-1CEST,M3.5.0,M10.5.0/3";
        setenv("TZ", ntpZone.c_str(), 1);
        tzset();
        struct timeval tv;
        tv.tv_sec = (time_t)epoch;
        tv.tv_usec = 0;
        settimeofday(&tv, NULL);
        lastSecond = -1;
        Serial.printf("[TIME] Uhrzeit via API synchronisiert: Epoch=%ld (TZ=%s)\n", epoch, ntpZone.c_str());
        showToastColor("Zeit synchronisiert 🕒", 3000, 0x07E0);
        server.send(200, "application/json", "{\"status\":\"ok\",\"epoch\":" + String(epoch) + "}");
        return;
    }
    server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"Ungueltige Epoch\"}");
}

void setupServerRoutes() {
    server.on("/", []() { if (evilActive) handleEvilRoot(); else handleRoot(); });
    server.onNotFound([]() { if (evilActive) server.sendHeader("Location", "http://hotspot.bund/", true); else server.sendHeader("Location", "/", true); server.send(302, "text/plain", ""); });
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/sync_time", HTTP_POST, handleSyncTimeApi);
    server.on("/api/sync_time", HTTP_GET, handleSyncTimeApi);

    // --- WLAN & Bluetooth Steuerung & Scans ---
    server.on("/api/wifi/toggle", HTTP_POST, []() {
        toggleWLAN();
        server.send(200, "application/json", "{\"status\":\"ok\",\"wifi\":" + String(wlanActive ? "true" : "false") + ",\"mode\":\"" + String(wlanClientMode ? "client" : "ap") + "\"}");
    });
    server.on("/api/wifi/toggle", HTTP_GET, []() {
        toggleWLAN();
        server.send(200, "application/json", "{\"status\":\"ok\",\"wifi\":" + String(wlanActive ? "true" : "false") + ",\"mode\":\"" + String(wlanClientMode ? "client" : "ap") + "\"}");
    });

    server.on("/api/wifi/mode", HTTP_POST, []() {
        if (!wlanActive) {
            startHotspotAP();
        } else {
            toggleWLAN();
        }
        server.send(200, "application/json", "{\"status\":\"ok\",\"wifi\":" + String(wlanActive ? "true" : "false") + ",\"mode\":\"" + String(wlanClientMode ? "client" : "ap") + "\"}");
    });
    server.on("/api/wifi/mode", HTTP_GET, []() {
        if (!wlanActive) {
            startHotspotAP();
        } else {
            toggleWLAN();
        }
        server.send(200, "application/json", "{\"status\":\"ok\",\"wifi\":" + String(wlanActive ? "true" : "false") + ",\"mode\":\"" + String(wlanClientMode ? "client" : "ap") + "\"}");
    });

    server.on("/api/wifi/scan", HTTP_GET, []() {
        int n = WiFi.scanNetworks(false, true);
        String j = "{\"networks\":[";
        for (int i = 0; i < n; i++) {
            if (i > 0) j += ",";
            j += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"chan\":" + String(WiFi.channel(i)) + ",\"secure\":" + String((int)WiFi.encryptionType(i)) + "}";
        }
        j += "]}";
        WiFi.scanDelete();
        server.send(200, "application/json", j);
    });

    server.on("/api/bt/toggle", HTTP_POST, []() {
        toggleBT();
        server.send(200, "application/json", "{\"status\":\"ok\",\"bt\":" + String(btActive ? "true" : "false") + "}");
    });
    server.on("/api/bt/toggle", HTTP_GET, []() {
        toggleBT();
        server.send(200, "application/json", "{\"status\":\"ok\",\"bt\":" + String(btActive ? "true" : "false") + "}");
    });

    server.on("/api/bt/scan", HTTP_GET, []() {
        if (!bleInitDone) {
            BLEDevice::init("T-Watch S3");
            bleInitDone = true;
        }
        BLEScan* pBLEScan = BLEDevice::getScan();
        pBLEScan->setActiveScan(true);
        BLEScanResults* foundDevices = pBLEScan->start(2, false);
        int count = foundDevices ? foundDevices->getCount() : 0;
        String j = "{\"devices\":[";
        for (int i = 0; i < count; i++) {
            BLEAdvertisedDevice d = foundDevices->getDevice(i);
            if (i > 0) j += ",";
            String dName = d.haveName() ? String(d.getName().c_str()) : "";
            j += "{\"name\":\"" + jsonEscape(dName) + "\",\"mac\":\"" + String(d.getAddress().toString().c_str()) + "\",\"rssi\":" + String(d.getRSSI()) + "}";
        }
        j += "]}";
        pBLEScan->clearResults();
        server.send(200, "application/json", j);
    });

    // --- Watchface Gallery API ---
    server.on("/api/watchface", HTTP_GET, []() {
        const char* wfNames[] = {"Ultra Rings", "Chronograph", "Retro Casio", "Lebensuhr"};
        String j = "{\"mode\":" + String(watchfaceMode) + ",\"name\":\"" + String(wfNames[watchfaceMode % 4]) + "\"}";
        server.send(200, "application/json", j);
    });
    server.on("/api/watchface", HTTP_POST, []() {
        int m = -1;
        if (server.hasArg("mode")) m = server.arg("mode").toInt();
        else if (server.hasArg("plain")) {
            String p = server.arg("plain");
            int idx = p.indexOf("\"mode\":");
            if (idx >= 0) m = p.substring(idx + 7).toInt();
        }
        if (m >= 0 && m <= 3) {
            watchfaceMode = m;
            prefs.begin("clock", false);
            prefs.putInt("wfMode", watchfaceMode);
            prefs.end();
            const char* wfNames[] = {"Ultra Rings", "Chronograph", "Retro Casio", "Lebensuhr"};
            showToastColor(wfNames[watchfaceMode], 2000, 0x07FF);
            triggerVibration();
            lastSecond = -1;
            server.send(200, "application/json", "{\"status\":\"ok\",\"mode\":" + String(watchfaceMode) + ",\"name\":\"" + String(wfNames[watchfaceMode]) + "\"}");
        } else {
            server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"Ungueltiger Modus (0..3)\"}");
        }
    });

    // --- Smartphone BLE Pairing API ---
    server.on("/api/bt/pair", HTTP_POST, []() {
        String mac = server.hasArg("mac") ? server.arg("mac") : "";
        String name = server.hasArg("name") ? server.arg("name") : "Smartphone";
        if (server.hasArg("plain")) {
            String p = server.arg("plain");
            int mIdx = p.indexOf("\"mac\":");
            if (mIdx >= 0) {
                int q1 = p.indexOf("\"", mIdx + 6);
                int q2 = p.indexOf("\"", q1 + 1);
                if (q1 >= 0 && q2 > q1) mac = p.substring(q1 + 1, q2);
            }
            int nIdx = p.indexOf("\"name\":");
            if (nIdx >= 0) {
                int q1 = p.indexOf("\"", nIdx + 7);
                int q2 = p.indexOf("\"", q1 + 1);
                if (q1 >= 0 && q2 > q1) name = p.substring(q1 + 1, q2);
            }
        }
        if (mac.length() >= 8) {
            phoneMac = mac;
            phoneName = name;
            phoneConnected = true;
            prefs.begin("clock", false);
            prefs.putString("phoneMac", phoneMac);
            prefs.putString("phoneName", phoneName);
            prefs.end();
            showToastColor("Phone: " + phoneName, 3000, 0x07E0);
            triggerVibration();
            lastSecond = -1;
            server.send(200, "application/json", "{\"status\":\"ok\",\"paired\":true,\"phone_mac\":\"" + jsonEscape(phoneMac) + "\",\"phone_name\":\"" + jsonEscape(phoneName) + "\"}");
        } else {
            server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"Ungueltige MAC\"}");
        }
    });

    server.on("/api/bt/unpair", HTTP_POST, []() {
        phoneMac = "";
        phoneName = "";
        phoneConnected = false;
        prefs.begin("clock", false);
        prefs.remove("phoneMac");
        prefs.remove("phoneName");
        prefs.end();
        showToastColor("Phone entkoppelt", 2500, 0x4208);
        triggerVibration();
        lastSecond = -1;
        server.send(200, "application/json", "{\"status\":\"ok\",\"paired\":false}");
    });

    // --- File Manager Delete & Create API ---
    server.on("/api/fs/delete", HTTP_POST, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        String path = server.hasArg("path") ? server.arg("path") : "";
        if (path.length() < 2 || path[0] != '/' || path.indexOf("..") >= 0 || !littleFsReady) {
            server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"Ungueltiger Pfad\"}");
            return;
        }
        bool ok = FFat.remove(path);
        server.send(200, "application/json", "{\"status\":\"" + String(ok ? "ok" : "fail") + "\",\"deleted\":\"" + jsonEscape(path) + "\"}");
    });

    server.on("/api/fs/create", HTTP_POST, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        String path = server.hasArg("path") ? server.arg("path") : "";
        String content = server.hasArg("content") ? server.arg("content") : "";
        if (path.length() < 2 || path[0] != '/' || path.indexOf("..") >= 0 || !littleFsReady) {
            server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"Ungueltiger Pfad\"}");
            return;
        }
        writeFileToFS(path, content);
        server.send(200, "application/json", "{\"status\":\"ok\",\"created\":\"" + jsonEscape(path) + "\"}");
    });

    // --- Push Notifications API ---
    server.on("/api/notify", HTTP_POST, []() {
        String title = server.hasArg("title") ? server.arg("title") : "Mitteilung";
        String msg = server.hasArg("msg") ? server.arg("msg") : (server.hasArg("message") ? server.arg("message") : "");
        if (msg.length() == 0 && server.hasArg("plain")) msg = server.arg("plain");
        if (msg.length() == 0) msg = "Hallo von WebOS!";
        NotificationManager::add(title.c_str(), msg.c_str());
        server.send(200, "application/json", "{\"status\":\"ok\",\"unread\":" + String(NotificationManager::getUnreadCount()) + "}");
    });
    server.on("/api/notify", HTTP_GET, []() {
        server.send(200, "application/json", NotificationManager::toJson());
    });
    server.on("/api/notify/clear", HTTP_POST, []() {
        NotificationManager::clear();
        server.send(200, "application/json", "{\"status\":\"ok\",\"cleared\":true}");
    });

    // --- 2FA TOTP Authenticator API ---
    server.on("/api/totp", HTTP_GET, []() {
        time_t nowTime; time(&nowTime);
        int count = TotpAuth::getAccountCount();
        int remSec = TotpAuth::getRemainingSeconds(nowTime);
        String j = "{\"count\":" + String(count) + ",\"remaining\":" + String(remSec) + ",\"accounts\":[";
        for (int i = 0; i < count; i++) {
            TotpAccount acc;
            TotpAuth::getAccount(i, acc);
            uint32_t code = TotpAuth::generateCode(i, nowTime);
            char codeBuf[12];
            snprintf(codeBuf, sizeof(codeBuf), "%06u", (unsigned int)code);
            if (i > 0) j += ",";
            j += "{\"idx\":" + String(i) + ",\"name\":\"" + jsonEscape(String(acc.name)) + "\",\"code\":\"" + String(codeBuf) + "\"}";
        }
        j += "]}";
        server.send(200, "application/json", j);
    });
    server.on("/api/totp/set", HTTP_POST, []() {
        if (!server.hasArg("name") || !server.hasArg("secret")) {
            server.send(400, "application/json", "{\"error\":\"name and secret required\"}");
            return;
        }
        int idx = server.hasArg("idx") ? server.arg("idx").toInt() : TotpAuth::getAccountCount();
        TotpAuth::setAccount(idx, server.arg("name").c_str(), server.arg("secret").c_str());
        showToastColor("2FA Konto gespeichert", 2500, TFT_GREEN);
        server.send(200, "application/json", "{\"status\":\"ok\"}");
    });
    server.on("/api/totp/del", HTTP_POST, []() {
        int idx = server.hasArg("idx") ? server.arg("idx").toInt() : -1;
        if (idx >= 0 && TotpAuth::removeAccount(idx)) {
            server.send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
            server.send(400, "application/json", "{\"error\":\"invalid index\"}");
        }
    });

    // --- Wardriving / RF Recon Export API ---
    server.on("/api/recon/export", HTTP_GET, []() {
        String csv = "WigleWifi-1.4,appRelease=WatchOS,model=T-WatchS3,release=1.0,device=ESP32S3,display=WatchOS,board=T-WatchS3,brand=LilyGO\n";
        csv += "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type\n";
        time_t nowTime; time(&nowTime);
        char timeBuf[30];
        struct tm *tmInfo = gmtime(&nowTime);
        snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02d %02d:%02d:%02d", tmInfo->tm_year+1900, tmInfo->tm_mon+1, tmInfo->tm_mday, tmInfo->tm_hour, tmInfo->tm_min, tmInfo->tm_sec);
        for (int i = 0; i < probeCount; i++) {
            csv += probeMacs[i] + "," + probeSSIDs[i] + ",[WPA2-PSK],\"" + String(timeBuf) + "\",1," + String(probeRSSI[i]) + ",0,0,0,0,WIFI\n";
        }
        for (int i = 0; i < bleScanCount; i++) {
            csv += bleScanMacs[i] + "," + bleScanNames[i] + ",[BLE],\"" + String(timeBuf) + "\",0," + String(bleScanRssi[i]) + ",0,0,0,0,BLE\n";
        }
        server.sendHeader("Content-Disposition", "attachment; filename=\"watchos_wardriving.csv\"");
        server.send(200, "text/csv", csv);
    });

    // Akku-Detailfenster: Spannung + gelernte Modus-Raten + Konfidenz (Klick auf den Akkuring)
    server.on("/api/battery/detail", HTTP_GET, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        bool isCharging = isChargingCached(false);
        bool hacksRunningNow = (jigglerActive || badUsbActive || wifiSpamActive || bleSpamActive || deauthActive || nmapActive || evilActive || wlanActive || probeMonActive || bleScanActive);
        BattEtaResult eta = computeBatteryEta(isCharging, hacksRunningNow, getBatteryCached(false));
        char json[420];
        snprintf(json, sizeof(json),
            "{\"volt\":\"%.2f\",\"eta_min\":%d,\"eta_mode\":\"%s\",\"eta_conf\":%d,"
            "\"rate_normal\":%.2f,\"rate_hacking\":%.2f,\"rate_charging\":%.2f,"
            "\"samples_normal\":%u,\"samples_hacking\":%u,\"samples_charging\":%u}",
            getBatteryVoltageCached(false), eta.etaMin, isCharging ? "charge" : "discharge", eta.confidencePct,
            battRateNormal, battRateHacking, battRateCharging,
            battSamplesNormal, battSamplesHacking, battSamplesCharging);
        server.send(200, "application/json", json);
    });

    // WLAN-Detailfenster: aktuelle Verbindung + Verlauf seit letztem Neustart (Klick auf WLAN-Kachel)
    server.on("/api/wifi/detail", HTTP_GET, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        time_t now; time(&now);
        String json = "{\"mode\":\"" + String(wlanClientMode ? "client" : "hotspot") + "\",";
        json += "\"ssid\":\"" + jsonEscape(wlanClientMode ? userSSID : apSSID) + "\",";
        json += "\"ip\":\"" + jsonEscape(wlanClientMode ? wlanIP : String("192.168.4.1")) + "\",";
        json += "\"history\":[";
        for (int i = 0; i < connHistCount; i++) {
            if (i > 0) json += ",";
            ConnHistEntry &e = connHist[i];
            int avgRssi = e.rssiSamples > 0 ? (int)(e.rssiSum / e.rssiSamples) : 0;
            int avgPct = avgRssi == 0 ? 0 : (avgRssi <= -100 ? 0 : (avgRssi >= -50 ? 100 : 2 * (avgRssi + 100)));
            json += "{\"ssid\":\"" + jsonEscape(e.ssid) + "\",\"start\":" + String((long)e.startTime) + ",\"end\":" + String((long)(e.ongoing ? now : e.endTime)) + ",\"ongoing\":" + String(e.ongoing ? "true" : "false") + ",\"avg_pct\":" + String(avgPct) + "}";
        }
        json += "]}";
        server.send(200, "application/json", json);
    });

    // Bluetooth-Detailfenster (Klick auf BT-Kachel). Die verwendete BLE-Lib fuehrt kein
    // Bonding-Verzeichnis, das sich hier auslesen liesse - daher nur echte Status/Advertising-Felder,
    // keine erfundene Geraeteliste.
    server.on("/api/bt/detail", HTTP_GET, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        String json = "{\"active\":" + String(btActive ? "true" : "false") + ",\"advertising\":" + String(bleInitDone ? "true" : "false") + ",\"device_name\":\"T-Watch S3\"}";
        server.send(200, "application/json", json);
    });

    // Speicher-Detailfenster: FFat-Dateiliste (Klick auf "Speicherinhalt anzeigen")
    server.on("/api/fs/list", HTTP_GET, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        String json = "{\"flash_free_kb\":" + String(littleFsReady ? (int)((FFat.totalBytes() - FFat.usedBytes()) / 1024) : 0);
        json += ",\"flash_tot_kb\":" + String(littleFsReady ? (int)(FFat.totalBytes() / 1024) : 0);
        nvs_stats_t nvsStats; nvsStats.free_entries = 0; nvsStats.total_entries = 0;
        nvs_get_stats(NULL, &nvsStats);
        json += ",\"nvs_free\":" + String((int)nvsStats.free_entries) + ",\"nvs_tot\":" + String((int)nvsStats.total_entries);

        // Echte Kategorien-Zaehlung: prueft je bekannter Schluessel-Gruppe, wie viele davon
        // je tatsaechlich in NVS geschrieben wurden (isKey), statt erfundener Platzhalterwerte.
        {
            static const char* catPersoenlich[] = {"tag","monat","jahr","std","min","alter","netS","pt","vac","sick","wh","wd","c1n","c1d","c1t","c2n","c2d","c2t","c3n","c3d","c3t","pN","pD","pT","ppN","ppD","ppT","stepGoal"};
            static const char* catWlan[] = {"ssid","pwd","ntpZ","apS","apP"};
            static const char* catDesign[] = {"cBG","cFG","cAcc1","cAcc2","cBat","cHBG","cHFG","cABG","cWrn","hAodFG","hAodAc"};
            static const char* catAod[] = {"wfT","wfD","wfB","wfS","wfTY","wfDY","wfBY","wfSY","wfTX","wfDX","wfBX","wfSX","wfTFm","wfTSc","wfDSc","wfBSc","wfSSc","wfWC","wfBC","wfAn","wfWCY","wfWCX","wfBCY","wfBCX","wfAnY","wfAnX","wfWCSc","wfBCSc","wfAnSc","wfAnSt","wfLn","wfSA","wfWd","wfLnY","wfLnX","wfSAY","wfSAX","wfWdY","wfWdX","wfLnSc","wfSASc","wfWdSc","aodM"};
            static const char* catHacker[] = {"spamSsid","buLay","nmapIP","nmapPorts","tgS0","tgP0","tgS1","tgP1","tgS2","tgP2","buN0","buS0","buN1","buS1","buN2","buS2","evSsid","evHtml"};
            static const char* catSystem[] = {"bootM","tDim","tAod","tSlp","maxB","minB","lang","tAlarm","hapW","webUsr","webPwd","webAuth","enP0","enP1","enP2","enP3","hCount","hFmt","bRateN","bRateH","bRateC","bSampN","bSampH","bSampC","brandName","usbHid"};
            struct NvsCat { const char* name; const char** keys; int n; };
            NvsCat cats[] = {
                {"Persönliche Daten", catPersoenlich, sizeof(catPersoenlich)/sizeof(char*)},
                {"WLAN-Zugang", catWlan, sizeof(catWlan)/sizeof(char*)},
                {"Design & Farben", catDesign, sizeof(catDesign)/sizeof(char*)},
                {"AOD-Layout", catAod, sizeof(catAod)/sizeof(char*)},
                {"Hacker-Tools", catHacker, sizeof(catHacker)/sizeof(char*)},
                {"System", catSystem, sizeof(catSystem)/sizeof(char*)}
            };
            prefs.begin("clock", true);
            json += ",\"categories\":[";
            for (size_t ci = 0; ci < sizeof(cats)/sizeof(NvsCat); ci++) {
                int cnt = 0;
                for (int ki = 0; ki < cats[ci].n; ki++) if (prefs.isKey(cats[ci].keys[ki])) cnt++;
                if (ci > 0) json += ",";
                json += "{\"name\":\"" + jsonEscape(String(cats[ci].name)) + "\",\"count\":" + String(cnt) + ",\"total\":" + String(cats[ci].n) + "}";
            }
            prefs.end();
            json += "]";
        }

        json += ",\"files\":[";
        bool first = true;
        if (littleFsReady) {
            fs::File root = FFat.open("/");
            if (root) {
                fs::File f = root.openNextFile();
                while (f) {
                    if (!f.isDirectory()) {
                        if (!first) json += ","; first = false;
                        String nm = String(f.name()); if (!nm.startsWith("/")) nm = "/" + nm;
                        json += "{\"name\":\"" + jsonEscape(nm) + "\",\"size\":" + String((long)f.size()) + "}";
                    }
                    f = root.openNextFile();
                }
            }
        }
        json += "]}";
        server.send(200, "application/json", json);
    });

    // Speicher-Detailfenster: einzelne FFat-Datei lesen/schreiben (Inline-Editor)
    server.on("/api/fs/read", HTTP_GET, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        String path = server.hasArg("path") ? server.arg("path") : "";
        if (path.length() < 2 || path[0] != '/' || path.indexOf("..") >= 0 || !littleFsReady) { server.send(400, "text/plain", ""); return; }
        server.send(200, "text/plain", readFileFromFS(path, ""));
    });
    server.on("/api/fs/write", HTTP_POST, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        String path = server.hasArg("path") ? server.arg("path") : "";
        String content = server.hasArg("content") ? server.arg("content") : "";
        if (path.length() < 2 || path.length() > 40 || path[0] != '/' || path.indexOf("..") >= 0 || content.length() > 16000 || !littleFsReady) {
            server.send(400, "text/plain", "invalid"); return;
        }
        writeFileToFS(path, content);
        // Bekannte Dateien auch im RAM + NVS synchron halten, damit Hacker-Tools-Tab & Evil-Portal
        // sofort den neuen Stand nutzen statt erst nach einem Neustart.
        if (path == "/evil.html") { evilHTML = content; prefs.begin("clock", false); prefs.putString("evHtml", content); prefs.end(); }
        else if (path == "/badusb0.txt") { badUsbScripts[0] = content; prefs.begin("clock", false); prefs.putString("buS0", content); prefs.end(); }
        else if (path == "/badusb1.txt") { badUsbScripts[1] = content; prefs.begin("clock", false); prefs.putString("buS1", content); prefs.end(); }
        else if (path == "/badusb2.txt") { badUsbScripts[2] = content; prefs.begin("clock", false); prefs.putString("buS2", content); prefs.end(); }
        server.send(200, "text/plain", "OK");
    });

    // Systemverlauf (CPU-Takt/-Temperatur) fuers Detailfenster: laengere Zeitraeume als der Live-Poll
    server.on("/api/history/system", HTTP_GET, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        int n = server.hasArg("n") ? server.arg("n").toInt() : sysHistCount;
        if (n <= 0 || n > sysHistCount) n = sysHistCount;
        int start = sysHistCount - n;
        String json = "{\"interval_min\":15,\"freq\":[";
        for (int i = start; i < sysHistCount; i++) { if (i > start) json += ","; json += String((int)sysHistFreq[i]); }
        json += "],\"temp\":[";
        for (int i = start; i < sysHistCount; i++) { if (i > start) json += ","; json += String(sysHistTemp[i], 1); }
        json += "]}";
        server.send(200, "application/json", json);
    });

    // Schritt-Verlauf: Balkendiagramm je Stunde des laufenden Tages (Klick auf Schritte&Ziel-Balken)
    server.on("/api/history/steps", HTTP_GET, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        String json = "{\"hourly\":[";
        for (int i = 0; i < 24; i++) { if (i > 0) json += ","; json += String(stepHourly[i]); }
        json += "]}";
        server.send(200, "application/json", json);
    });

    // Eigenes Branding-Icon (25x25 PNG/JPG): Upload + Auslieferung, in FFat persistiert
    server.on("/api/brand/icon", HTTP_GET, []() {
        if (!littleFsReady || !FFat.exists("/brand_icon.bin")) { server.send(404, "text/plain", "not set"); return; }
        String ct = "image/png";
        prefs.begin("clock", true); ct = prefs.getString("brandIconCt", ct); prefs.end();
        fs::File f = FFat.open("/brand_icon.bin", "r");
        if (!f) { server.send(404, "text/plain", "not set"); return; }
        server.streamFile(f, ct);
        f.close();
    });
    server.on("/api/brand/icon", HTTP_POST, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        server.send(200, "text/plain", brandIconUploadOK ? "OK" : "FAIL");
    }, []() {
        HTTPUpload& upload = server.upload();
        static fs::File iconFile;
        if (upload.status == UPLOAD_FILE_START) {
            brandIconUploadOK = false; brandIconUploadBytes = 0;
            if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) return;
            String fn = upload.filename; fn.toLowerCase();
            String ct = (fn.endsWith(".jpg") || fn.endsWith(".jpeg")) ? "image/jpeg" : "image/png";
            prefs.begin("clock", false); prefs.putString("brandIconCt", ct); prefs.end();
            if (littleFsReady) iconFile = FFat.open("/brand_icon.bin", "w");
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            brandIconUploadBytes += upload.currentSize;
            if (brandIconUploadBytes > 65536) { if (iconFile) iconFile.close(); return; } // 64KB Obergrenze gegen Missbrauch
            if (iconFile) iconFile.write(upload.buf, upload.currentSize);
        } else if (upload.status == UPLOAD_FILE_END) {
            if (iconFile) { iconFile.close(); brandIconUploadOK = (brandIconUploadBytes > 0 && brandIconUploadBytes <= 65536); }
        }
    });

    // LoRa SX1262 API
    server.on("/api/lora/status", HTTP_GET, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        server.send(200, "application/json", LoraManager::getStatusJson());
    });
    server.on("/api/lora/send", HTTP_POST, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        String msg = server.hasArg("msg") ? server.arg("msg") : "";
        String sender = server.hasArg("sender") ? server.arg("sender") : "WebDash";
        if (msg.length() == 0) {
            server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"empty message\"}");
            return;
        }
        bool ok = LoraManager::sendMessage(msg, sender);
        server.send(200, "application/json", ok ? "{\"status\":\"ok\"}" : "{\"status\":\"error\",\"msg\":\"tx failed\"}");
    });
    server.on("/api/lora/beacon", HTTP_POST, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        bool enable = server.hasArg("enable") ? (server.arg("enable") == "1" || server.arg("enable") == "true") : false;
        int interval = server.hasArg("interval") ? server.arg("interval").toInt() : 5;
        if (interval < 1) interval = 5;
        LoraManager::setBeacon(enable, interval);
        server.send(200, "application/json", LoraManager::isBeaconActive() ? "{\"beacon\":true}" : "{\"beacon\":false}");
    });
    server.on("/api/lora/clear", HTTP_POST, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        LoraManager::clearMessages();
        server.send(200, "application/json", "{\"status\":\"ok\"}");
    });

    // Modulare App-Package API
    server.on("/api/apps/list", HTTP_GET, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        server.send(200, "application/json", AppManager::getPackagesJson());
    });
    server.on("/api/apps/toggle", HTTP_POST, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        if (!server.hasArg("id")) { server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"missing id\"}"); return; }
        String id = server.arg("id");
        bool enable = server.hasArg("enable") ? (server.arg("enable") == "1" || server.arg("enable") == "true") : true;
        AppManager::setAppEnabled(id, enable);
        server.send(200, "application/json", "{\"status\":\"ok\",\"id\":\"" + jsonEscape(id) + "\",\"enabled\":" + String(enable ? "true" : "false") + "}");
    });
    server.on("/api/apps/order", HTTP_POST, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        if (!server.hasArg("id") || !server.hasArg("order")) { server.send(400, "application/json", "{\"status\":\"error\"}"); return; }
        AppManager::setAppOrder(server.arg("id"), server.arg("order").toInt());
        server.send(200, "application/json", "{\"status\":\"ok\"}");
    });
    server.on("/api/apps/reset", HTTP_POST, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        AppManager::resetToDefaults();
        server.send(200, "application/json", "{\"status\":\"ok\"}");
    });
    server.on("/api/apps/install", HTTP_POST, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        String body = "";
        if (server.hasArg("plain")) body = server.arg("plain");
        else if (server.hasArg("json")) body = server.arg("json");
        if (body.length() < 5) {
            server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"Leerer Inhalt oder ungueltiges Paket\"}");
            return;
        }
        String outId, outError;
        if (AppManager::installPackageFromJson(body, outId, outError)) {
            server.send(200, "application/json", "{\"status\":\"ok\",\"id\":\"" + jsonEscape(outId) + "\",\"msg\":\"App erfolgreich installiert\"}");
        } else {
            server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"" + jsonEscape(outError) + "\"}");
        }
    });
    server.on("/api/apps/uninstall", HTTP_POST, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        if (!server.hasArg("id")) { server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"missing id\"}"); return; }
        String id = server.arg("id");
        if (AppManager::uninstallPackage(id)) {
            server.send(200, "application/json", "{\"status\":\"ok\",\"id\":\"" + jsonEscape(id) + "\",\"msg\":\"App deinstalliert\"}");
        } else {
            server.send(404, "application/json", "{\"status\":\"error\",\"msg\":\"App nicht gefunden\"}");
        }
    });
    server.on("/api/apps/export", HTTP_GET, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        if (!server.hasArg("id")) { server.send(400, "text/plain", "missing id"); return; }
        String id = server.arg("id");
        String appContent = AppManager::getPackageAppFile(id);
        if (appContent.length() == 0) {
            server.send(404, "text/plain", "Paket nicht gefunden");
            return;
        }
        server.sendHeader("Content-Disposition", "attachment; filename=\"" + id + ".app\"");
        server.send(200, "application/json", appContent);
    });

    server.on("/save", HTTP_POST, handleSave);
    server.on("/settime", HTTP_POST, handleSetTime);
    server.on("/clearlog", HTTP_POST, []() { evilLog = ""; server.sendHeader("Location", "/", true); server.send(302, "text/plain", ""); }); 
    server.on("/battery", HTTP_GET, []() { server.send(200, "text/html", getBatteryHtml()); });
    server.on("/battery/reset", HTTP_POST, []() {
        historyCount = 0;
        lastHistoryTime = 0;
        for (int i = 0; i < MAX_HISTORY; i++) batHistoryPct[i] = 0;
        prefs.begin("clock", false);
        prefs.putInt("hCount", 0);
        prefs.putInt("hFmt", 2);
        uint16_t emptyHist[MAX_HISTORY] = {0};
        prefs.putBytes("hData", emptyHist, MAX_HISTORY * sizeof(uint16_t));
        prefs.end();
        server.sendHeader("Location", "/battery", true);
        server.send(303, "text/plain", "");
    });
    
    server.on("/wifiscan", HTTP_GET, []() {
        int n = WiFi.scanNetworks(); String json = "[";
        for (int i = 0; i < n; ++i) { if(i>0) json += ","; String ssid = WiFi.SSID(i); ssid.replace("\"", "\\\""); json += "\"" + ssid + "\""; }
        json += "]"; server.send(200, "application/json", json); WiFi.scanDelete();
    });

    server.on("/routerscan", HTTP_GET, []() {
        int n = WiFi.scanNetworks(false, true); String json = "[";
        for (int i = 0; i < n; ++i) { if(i>0) json += ","; String ssid = WiFi.SSID(i); ssid.replace("\"", "\\\""); json += "{\"ssid\":\"" + ssid + "\",\"bssid\":\"" + WiFi.BSSIDstr(i) + "\",\"ch\":" + String(WiFi.channel(i)) + "}"; }
        json += "]"; server.send(200, "application/json", json); WiFi.scanDelete();
    });

    server.on("/deauth", HTTP_GET, []() {
        if(server.hasArg("mac")) {
            String bssidStr = server.arg("mac"); char* ptr;
            deauthMac[0] = strtol(bssidStr.substring(0, 2).c_str(), &ptr, 16); deauthMac[1] = strtol(bssidStr.substring(3, 5).c_str(), &ptr, 16); deauthMac[2] = strtol(bssidStr.substring(6, 8).c_str(), &ptr, 16);
            deauthMac[3] = strtol(bssidStr.substring(9, 11).c_str(), &ptr, 16); deauthMac[4] = strtol(bssidStr.substring(12, 14).c_str(), &ptr, 16); deauthMac[5] = strtol(bssidStr.substring(15, 17).c_str(), &ptr, 16);
            deauthState = 2; deauthActive = true;
            WiFi.mode(WIFI_AP); esp_wifi_set_promiscuous(true); lastSecond = -1;
        }
        server.send(200, "text/plain", "OK");
    });

    server.on("/stopbad", HTTP_POST, []() {
        killBadApps();
        server.send(200, "text/plain", "OK");
    });

    server.on("/sync", HTTP_POST, []() { 
        server.sendHeader("Connection", "close"); 
        server.send(200, "text/html", "<html><body style='background:#222;color:#fff;text-align:center;'><h2>Synchronisiere NTP...</h2><script>setTimeout(function(){window.location.href='/';}, 7000);</script></body></html>"); 
        WiFi.softAPdisconnect(true); 
        WiFi.mode(WIFI_STA); 
        WiFi.begin(userSSID.c_str(), userPWD.c_str()); 
        int retry = 0; 
        while (WiFi.status() != WL_CONNECTED && retry < 25) { delay(300); retry++; } 
        if (WiFi.status() == WL_CONNECTED) { 
            bool ok = syncNtpTime(8); 
            if (ok) showToastColor("NTP Sync OK 🕒", 3500, 0x07E0);
            else showToastColor("NTP Timeout", 3500, TFT_RED);
        } else {
            showToastColor("WLAN nicht erreichbar", 3500, TFT_RED);
        }
        startHotspotAP(); 
        lastSecond = -1; 
    });
    server.on("/reboot", HTTP_POST, []() { server.sendHeader("Connection", "close"); server.send(200, "text/plain", "Neustart..."); delay(1000); ESP.restart(); });
    server.on("/matrix", HTTP_POST, []() {
        matrixMode = !matrixMode;
        if (matrixMode) {
            // Display kann beim Ausloesen per Web bereits gedimmt/im AOD-/Sleep-Zustand sein.
            // Ohne diese drei Zeilen wuerde der Matrix-Effekt zwar rechnerisch laufen, aber auf
            // dem Panel unsichtbar bleiben, bis der naechste Wach-Trigger die Helligkeit hochsetzt.
            powerState = 1;
            lastTouchTime = millis();
            setBrightness(maxBright);
            tft.fillScreen(TFT_BLACK);
            for (int i = 0; i < 24; i++) matrixY[i] = 0;
        } else { lastSecond = -1; }
        server.sendHeader("Location", "/"); server.send(303);
    });
    server.on("/submit", HTTP_POST, handleEvilSubmit);
    server.on("/tos", HTTP_GET, []() { if (evilActive) handleEvilTOS(); else server.send(404, "text/plain", "Not Found"); });
    server.on("/imprint", HTTP_GET, []() { if (evilActive) handleEvilImprint(); else server.send(404, "text/plain", "Not Found"); });

    // Export settings as JSON
    server.on("/export", HTTP_GET, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        String j = "{";
        j += "\"ssid\":\"" + userSSID + "\",\"pwd\":\"" + userPWD + "\",\"ntpZ\":\"" + ntpZone + "\",";
        j += "\"apS\":\"" + apSSID + "\",\"apP\":\"" + apPWD + "\",";
        j += "\"tag\":" + String(gTag) + ",\"monat\":" + String(gMonat) + ",\"jahr\":" + String(gJahr) + ",\"std\":" + String(gStunde) + ",\"min\":" + String(gMinute) + ",\"alter\":" + String(zielAlter) + ",";
        j += "\"bootM\":" + String(bootMode) + ",\"aodM\":" + String(aodMode) + ",";
        j += "\"tDim\":" + String(time_dim_start) + ",\"tAod\":" + String(time_aod_start) + ",\"tSlp\":" + String(time_sleep_start) + ",";
        j += "\"maxB\":" + String(maxBright) + ",\"minB\":" + String(minBright) + ",";
        j += "\"cBG\":\"" + hexBG + "\",\"cFG\":\"" + hexFG + "\",\"cAcc1\":\"" + hexAcc1 + "\",\"cAcc2\":\"" + hexAcc2 + "\",\"cBat\":\"" + hexBat + "\",";
        j += "\"cHBG\":\"" + hexHeadBG + "\",\"cHFG\":\"" + hexHeadFG + "\",\"cABG\":\"" + hexAppBG + "\",\"cWrn\":\"" + hexWarn + "\",";
        j += "\"lang\":" + String(sysLang) + ",";
        j += "\"enP0\":" + String(enP0) + ",\"enP1\":" + String(enP1) + ",\"enP2\":" + String(enP2) + ",\"enP3\":" + String(enP3) + ",";
        j += "\"tAlarm\":" + String(timerAlarmMode) + ",\"hapW\":" + String(hapticWaveform) + ",";
        j += "\"pN\":\"" + parentName + "\",\"pD\":\"" + parentDate + "\",\"pT\":\"" + parentTime + "\",";
        j += "\"ppN\":\"" + papaName + "\",\"ppD\":\"" + papaDate + "\",\"ppT\":\"" + papaTime + "\",";
        j += "\"brandName\":\"" + jsonEscape(brandName) + "\",";
        j += "\"netS\":" + String(netSalary, 2) + ",\"pt\":" + String(partTimePct) + ",\"vac\":" + String(vacationDays) + ",\"sick\":" + String(sickDays) + ",\"wh\":" + String(workHoursPerDay, 1) + ",\"wd\":" + String(workDaysYear) + ",";
        j += "\"c1n\":\"" + child1Name + "\",\"c1d\":\"" + child1Date + "\",\"c1t\":\"" + child1Time + "\",";
        j += "\"c2n\":\"" + child2Name + "\",\"c2d\":\"" + child2Date + "\",\"c2t\":\"" + child2Time + "\",";
        j += "\"c3n\":\"" + child3Name + "\",\"c3d\":\"" + child3Date + "\",\"c3t\":\"" + child3Time + "\",";
        j += "\"evSsid\":\"" + evilSSID + "\",";
        j += "\"spamSsid\":\"" + spamSSIDs + "\",\"buLay\":" + String(badUsbLayout) + ",\"nmapIP\":\"" + nmapBaseIP + "\",\"nmapPorts\":\"" + nmapPorts + "\",";
        j += "\"tgS0\":\"" + targetSSIDs[0] + "\",\"tgP0\":\"" + targetPWDs[0] + "\",\"tgS1\":\"" + targetSSIDs[1] + "\",\"tgP1\":\"" + targetPWDs[1] + "\",\"tgS2\":\"" + targetSSIDs[2] + "\",\"tgP2\":\"" + targetPWDs[2] + "\",";
        j += "\"wfT\":" + String(wfShowTime) + ",\"wfD\":" + String(wfShowDate) + ",\"wfB\":" + String(wfShowBatt) + ",\"wfS\":" + String(wfShowSteps) + ",";
        j += "\"wfTY\":" + String(wfTimeY) + ",\"wfDY\":" + String(wfDateY) + ",\"wfBY\":" + String(wfBattY) + ",\"wfSY\":" + String(wfStepsY) + ",";
        j += "\"wfTX\":" + String(wfTimeX) + ",\"wfDX\":" + String(wfDateX) + ",\"wfBX\":" + String(wfBattX) + ",\"wfSX\":" + String(wfStepsX) + ",";
        j += "\"wfTSc\":" + String(wfTimeScale, 1) + ",\"wfDSc\":" + String(wfDateScale, 1) + ",\"wfBSc\":" + String(wfBattScale, 1) + ",\"wfSSc\":" + String(wfStepsScale, 1) + ",";
        j += "\"wfTFm\":" + String(wfTimeFmt) + ",";
        j += "\"wfWC\":" + String(wfShowWordClock) + ",\"wfBC\":" + String(wfShowBinClock) + ",\"wfAn\":" + String(wfShowAnalog) + ",";
        j += "\"wfWCY\":" + String(wfWordClockY) + ",\"wfWCX\":" + String(wfWordClockX) + ",\"wfBCY\":" + String(wfBinClockY) + ",\"wfBCX\":" + String(wfBinClockX) + ",";
        j += "\"wfAnY\":" + String(wfAnalogY) + ",\"wfAnX\":" + String(wfAnalogX) + ",";
        j += "\"wfWCSc\":" + String(wfWordClockScale, 1) + ",\"wfBCSc\":" + String(wfBinClockScale, 1) + ",\"wfAnSc\":" + String(wfAnalogScale, 1) + ",";
        j += "\"wfAnSt\":" + String(wfAnalogStyle) + ",";
        j += "\"hAodFG\":\"" + hexAodFG + "\",\"hAodAc\":\"" + hexAodAcc + "\"";
        j += "}";
        server.sendHeader("Content-Disposition", "attachment; filename=watchos_settings.json");
        server.send(200, "application/json", j);
    });

    // Import settings from JSON
    server.on("/import", HTTP_POST, []() {
        if (webAuthEnabled && webPass.length() > 0 && !server.authenticate(webUser.c_str(), webPass.c_str())) { server.requestAuthentication(); return; }
        String body = server.arg("plain");
        if (body.length() < 5) { server.send(400, "text/plain", "Leerer Body"); return; }
        // Minimal JSON parser - extract key:value pairs
        auto jStr = [&](const String& key) -> String { int i = body.indexOf("\"" + key + "\":\""); if(i<0) return ""; i += key.length()+4; int e = body.indexOf("\"", i); return (e>i) ? body.substring(i, e) : ""; };
        auto jInt = [&](const String& key, int def) -> int { int i = body.indexOf("\"" + key + "\":"); if(i<0) return def; i += key.length()+3; int e = i; while(e < (int)body.length() && (isdigit(body[e]) || body[e]=='-')) e++; return (e>i) ? body.substring(i, e).toInt() : def; };
        auto jFloat = [&](const String& key, float def) -> float { int i = body.indexOf("\"" + key + "\":"); if(i<0) return def; i += key.length()+3; int e = i; while(e < (int)body.length() && (isdigit(body[e]) || body[e]=='.' || body[e]=='-')) e++; return (e>i) ? body.substring(i, e).toFloat() : def; };
        auto jULong = [&](const String& key, unsigned long def) -> unsigned long { int i = body.indexOf("\"" + key + "\":"); if(i<0) return def; i += key.length()+3; int e = i; while(e < (int)body.length() && isdigit(body[e])) e++; return (e>i) ? strtoul(body.substring(i, e).c_str(), NULL, 10) : def; };
        // Apply values
        userSSID = jStr("ssid"); userPWD = jStr("pwd"); ntpZone = jStr("ntpZ");
        apSSID = jStr("apS"); apPWD = jStr("apP");
        gTag = jInt("tag", gTag); gMonat = jInt("monat", gMonat); gJahr = jInt("jahr", gJahr); gStunde = jInt("std", gStunde); gMinute = jInt("min", gMinute); zielAlter = jInt("alter", zielAlter);
        bootMode = jInt("bootM", bootMode); aodMode = jInt("aodM", aodMode);
        time_dim_start = jULong("tDim", time_dim_start); time_aod_start = jULong("tAod", time_aod_start); time_sleep_start = jULong("tSlp", time_sleep_start);
        maxBright = jInt("maxB", maxBright); minBright = jInt("minB", minBright);
        hexBG = jStr("cBG"); hexFG = jStr("cFG"); hexAcc1 = jStr("cAcc1"); hexAcc2 = jStr("cAcc2"); hexBat = jStr("cBat");
        hexHeadBG = jStr("cHBG"); hexHeadFG = jStr("cHFG"); hexAppBG = jStr("cABG"); hexWarn = jStr("cWrn");
        sysLang = jInt("lang", sysLang);
        enP0 = jInt("enP0", enP0); enP1 = jInt("enP1", enP1); enP2 = jInt("enP2", enP2); enP3 = jInt("enP3", enP3);
        timerAlarmMode = jInt("tAlarm", timerAlarmMode); hapticWaveform = jInt("hapW", hapticWaveform);
        parentName = jStr("pN"); parentDate = jStr("pD"); parentTime = jStr("pT");
        papaName = jStr("ppN"); papaDate = jStr("ppD"); papaTime = jStr("ppT");
        brandName = jStr("brandName");
        netSalary = jFloat("netS", netSalary); partTimePct = jInt("pt", partTimePct); vacationDays = jInt("vac", vacationDays); sickDays = jInt("sick", sickDays); workHoursPerDay = jFloat("wh", workHoursPerDay); workDaysYear = jInt("wd", workDaysYear);
        child1Name = jStr("c1n"); child1Date = jStr("c1d"); child1Time = jStr("c1t");
        child2Name = jStr("c2n"); child2Date = jStr("c2d"); child2Time = jStr("c2t");
        child3Name = jStr("c3n"); child3Date = jStr("c3d"); child3Time = jStr("c3t");
        evilSSID = jStr("evSsid");
        spamSSIDs = jStr("spamSsid"); badUsbLayout = jInt("buLay", badUsbLayout); nmapBaseIP = jStr("nmapIP"); nmapPorts = jStr("nmapPorts");
        targetSSIDs[0] = jStr("tgS0"); targetPWDs[0] = jStr("tgP0"); targetSSIDs[1] = jStr("tgS1"); targetPWDs[1] = jStr("tgP1"); targetSSIDs[2] = jStr("tgS2"); targetPWDs[2] = jStr("tgP2");
        wfShowTime = jInt("wfT", wfShowTime); wfShowDate = jInt("wfD", wfShowDate); wfShowBatt = jInt("wfB", wfShowBatt); wfShowSteps = jInt("wfS", wfShowSteps);
        wfTimeY = jInt("wfTY", wfTimeY); wfDateY = jInt("wfDY", wfDateY); wfBattY = jInt("wfBY", wfBattY); wfStepsY = jInt("wfSY", wfStepsY);
        wfTimeX = jInt("wfTX", wfTimeX); wfDateX = jInt("wfDX", wfDateX); wfBattX = jInt("wfBX", wfBattX); wfStepsX = jInt("wfSX", wfStepsX);
        wfTimeScale = jFloat("wfTSc", wfTimeScale); wfDateScale = jFloat("wfDSc", wfDateScale); wfBattScale = jFloat("wfBSc", wfBattScale); wfStepsScale = jFloat("wfSSc", wfStepsScale);
        wfTimeFmt = jInt("wfTFm", wfTimeFmt);
        wfShowWordClock = jInt("wfWC", wfShowWordClock); wfShowBinClock = jInt("wfBC", wfShowBinClock); wfShowAnalog = jInt("wfAn", wfShowAnalog);
        wfWordClockY = jInt("wfWCY", wfWordClockY); wfWordClockX = jInt("wfWCX", wfWordClockX);
        wfBinClockY = jInt("wfBCY", wfBinClockY); wfBinClockX = jInt("wfBCX", wfBinClockX);
        wfAnalogY = jInt("wfAnY", wfAnalogY); wfAnalogX = jInt("wfAnX", wfAnalogX);
        wfWordClockScale = jFloat("wfWCSc", wfWordClockScale); wfBinClockScale = jFloat("wfBCSc", wfBinClockScale); wfAnalogScale = jFloat("wfAnSc", wfAnalogScale);
        wfAnalogStyle = jInt("wfAnSt", wfAnalogStyle);
        hexAodFG = jStr("hAodFG"); hexAodAcc = jStr("hAodAc");
        // Save all to NVS
        prefs.begin("clock", false);
        prefs.putString("ssid", userSSID); prefs.putString("pwd", userPWD); prefs.putString("ntpZ", ntpZone);
        prefs.putString("apS", apSSID); prefs.putString("apP", apPWD);
        prefs.putInt("tag", gTag); prefs.putInt("monat", gMonat); prefs.putInt("jahr", gJahr); prefs.putInt("std", gStunde); prefs.putInt("min", gMinute); prefs.putInt("alter", zielAlter);
        prefs.putInt("bootM", bootMode); prefs.putInt("aodM", aodMode);
        prefs.putULong("tDim", time_dim_start); prefs.putULong("tAod", time_aod_start); prefs.putULong("tSlp", time_sleep_start);
        prefs.putInt("maxB", maxBright); prefs.putInt("minB", minBright);
        prefs.putString("cBG", hexBG); prefs.putString("cFG", hexFG); prefs.putString("cAcc1", hexAcc1); prefs.putString("cAcc2", hexAcc2); prefs.putString("cBat", hexBat);
        prefs.putString("cHBG", hexHeadBG); prefs.putString("cHFG", hexHeadFG); prefs.putString("cABG", hexAppBG); prefs.putString("cWrn", hexWarn);
        prefs.putInt("lang", sysLang);
        prefs.putString("ppN", papaName); prefs.putString("ppD", papaDate); prefs.putString("ppT", papaTime);
        prefs.putString("brandName", brandName);
        prefs.putBool("enP0", enP0); prefs.putBool("enP1", enP1); prefs.putBool("enP2", enP2); prefs.putBool("enP3", enP3);
        prefs.putInt("tAlarm", timerAlarmMode); prefs.putInt("hapW", hapticWaveform);
        prefs.putString("pN", parentName); prefs.putString("pD", parentDate); prefs.putString("pT", parentTime);
        prefs.putFloat("netS", netSalary); prefs.putInt("pt", partTimePct); prefs.putInt("vac", vacationDays); prefs.putInt("sick", sickDays); prefs.putFloat("wh", workHoursPerDay); prefs.putInt("wd", workDaysYear);
        prefs.putString("c1n", child1Name); prefs.putString("c1d", child1Date); prefs.putString("c1t", child1Time);
        prefs.putString("c2n", child2Name); prefs.putString("c2d", child2Date); prefs.putString("c2t", child2Time);
        prefs.putString("c3n", child3Name); prefs.putString("c3d", child3Date); prefs.putString("c3t", child3Time);
        prefs.putString("evSsid", evilSSID);
        prefs.putString("spamSsid", spamSSIDs); prefs.putInt("buLay", badUsbLayout); prefs.putString("nmapIP", nmapBaseIP); prefs.putString("nmapPorts", nmapPorts);
        prefs.putString("tgS0", targetSSIDs[0]); prefs.putString("tgP0", targetPWDs[0]); prefs.putString("tgS1", targetSSIDs[1]); prefs.putString("tgP1", targetPWDs[1]); prefs.putString("tgS2", targetSSIDs[2]); prefs.putString("tgP2", targetPWDs[2]);
        prefs.putBool("wfT", wfShowTime); prefs.putBool("wfD", wfShowDate); prefs.putBool("wfB", wfShowBatt); prefs.putBool("wfS", wfShowSteps);
        prefs.putInt("wfTY", wfTimeY); prefs.putInt("wfDY", wfDateY); prefs.putInt("wfBY", wfBattY); prefs.putInt("wfSY", wfStepsY);
        prefs.putInt("wfTX", wfTimeX); prefs.putInt("wfDX", wfDateX); prefs.putInt("wfBX", wfBattX); prefs.putInt("wfSX", wfStepsX);
        prefs.putFloat("wfTSc", wfTimeScale); prefs.putFloat("wfDSc", wfDateScale); prefs.putFloat("wfBSc", wfBattScale); prefs.putFloat("wfSSc", wfStepsScale);
        prefs.putInt("wfTFm", wfTimeFmt);
        prefs.putBool("wfWC", wfShowWordClock); prefs.putBool("wfBC", wfShowBinClock); prefs.putBool("wfAn", wfShowAnalog);
        prefs.putInt("wfWCY", wfWordClockY); prefs.putInt("wfWCX", wfWordClockX);
        prefs.putInt("wfBCY", wfBinClockY); prefs.putInt("wfBCX", wfBinClockX);
        prefs.putInt("wfAnY", wfAnalogY); prefs.putInt("wfAnX", wfAnalogX);
        prefs.putFloat("wfWCSc", wfWordClockScale); prefs.putFloat("wfBCSc", wfBinClockScale); prefs.putFloat("wfAnSc", wfAnalogScale);
        prefs.putInt("wfAnSt", wfAnalogStyle);
        prefs.putString("hAodFG", hexAodFG); prefs.putString("hAodAc", hexAodAcc);
        prefs.end();
        updateZielZeit(); applyTheme(); lastSecond = -1;
        server.send(200, "text/plain", "OK");
    });

    setupOTARoutes(); 
}

void startHotspotAP() {
    if (evilActive) return;
    if (!wlanActive || wlanClientMode) {
        wlanActive = true; 
        wlanClientMode = false;
        if (ESP.getCpuFreqMHz() != 240) setCpuFrequencyMhz(240); 
        WiFi.mode(WIFI_AP); 
        WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0)); 
        WiFi.softAP(apSSID.c_str(), apPWD.c_str()); 
        delay(50); 
        dnsServer.setErrorReplyCode(DNSReplyCode::NoError); 
        dnsServer.start(DNS_PORT, "*", WiFi.softAPIP()); 
        server.begin(); 
        MDNS.begin("watchos"); 
        wlanIP = "192.168.4.1"; 
        lastSecond = -1;
    }
}

void stopHotspotAP() {
    if (wlanActive && !wlanClientMode) {
        wlanActive = false; 
        dnsServer.stop(); 
        server.stop(); 
        WiFi.softAPdisconnect(true); 
        WiFi.mode(WIFI_OFF);
        wlanIP = ""; 
        lastSecond = -1;
    }
}

void toggleWLAN() {
    if (evilActive) toggleEvilPortal(); 
    if (!wlanActive) {
        startHotspotAP();
        showToast("Hotspot: " + apSSID, 4000);
    } 
    else if (wlanActive && !wlanClientMode) {
        wlanClientMode = true; dnsServer.stop(); server.stop(); WiFi.softAPdisconnect(true); showToast("Verbinde Client...", 2000);
        WiFi.mode(WIFI_STA); WiFi.begin(userSSID.c_str(), userPWD.c_str());
        int timeout = 0; while (WiFi.status() != WL_CONNECTED && timeout < 16) { delay(500); timeout++; }
        if (WiFi.status() == WL_CONNECTED) {
            server.begin(); MDNS.begin("watchos"); wlanIP = WiFi.localIP().toString(); showToast("IP: " + wlanIP, 4000);
        } else {
            startHotspotAP();
            showToast("Fallback: Hotspot", 4000);
        }
    } 
    else {
        wlanActive = false; wlanClientMode = false; dnsServer.stop(); server.stop(); WiFi.disconnect(true); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF);
        wlanIP = ""; showToast("WLAN Aus", 2000);
    }
}

// Schaltet den Hotspot automatisch ab, wenn er länger ungenutzt offen bleibt.
// Spart Akku, falls man das WLAN vergisst. Betrifft nur den reinen AP-Modus
// (Client-Modus und Evil Portal bleiben unberührt). Solange ein Gerät verbunden
// ist oder die Uhr am Strom hängt, läuft der Timer nicht.
#define WLAN_AP_IDLE_TIMEOUT_MS (10UL * 60UL * 1000UL) // 10 Minuten
void handleWlanIdleTimeout() {
    static unsigned long apIdleSince = 0;
    static unsigned long lastCheck = 0;

    if (!wlanActive || wlanClientMode || evilActive) { apIdleSince = 0; return; }
    if (isChargingCached(false)) { apIdleSince = 0; return; } // Am Strom niemals abschalten!

    unsigned long now = millis();
    if (now - lastCheck < 5000) return; // alle 5s prüfen reicht
    lastCheck = now;

    if (WiFi.softAPgetStationNum() > 0) { apIdleSince = now; return; } // Client da -> Timer zurück
    if (apIdleSince == 0) { apIdleSince = now; return; }               // Leerlauf beginnt jetzt

    if (now - apIdleSince >= WLAN_AP_IDLE_TIMEOUT_MS) {
        stopHotspotAP();
        showToastColor("Hotspot aus (inaktiv)", 4000, 0x4208);
        lastSecond = -1;
    }
}
