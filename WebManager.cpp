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
#include <FFat.h>

const byte DNS_PORT = 53;

// Globale Variablen für den Client-Modus
bool wlanClientMode = false;
String wlanIP = "";

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

void handleApiStatus() {
    time_t jetzt; time(&jetzt); struct tm *aktuell = localtime(&jetzt);
    float dailyNet = (netSalary * 12.0) / 365.0; float fraction = (millis() % 1000) / 1000.0; 
    long secToday = aktuell->tm_hour * 3600 + aktuell->tm_min * 60 + aktuell->tm_sec; 
    float earnedToday = dailyNet * ((secToday + fraction) / 86400.0);

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
    
    int battPct = getBatteryCached(false);
    int etaMin = estimateBatteryEtaMinutes(isCharging, battPct);
    const char* etaMode = "unknown";
    if (etaMin >= 0) etaMode = isCharging ? "charge" : "discharge";

    char json[1024];
    snprintf(json, sizeof(json), "{\"akku\":%d,\"volt\":\"%.2f\",\"cpu\":\"%.1f\",\"ram_free\":%d,\"ram_tot\":%d,\"geld\":\"%.4f\",\"wifi\":%s,\"charging\":%s,\"bt\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"cpufreq\":%d,\"uptime\":%lu,\"rssi\":%d,\"rssi_pct\":%d,\"eta_min\":%d,\"eta_mode\":\"%s\"}", 
             battPct, getBatteryVoltageCached(false), cachedCpuTemp, 
             (int)(ESP.getFreeHeap() / 1024), (int)(ESP.getHeapSize() / 1024), earnedToday, 
             (WiFi.status() == WL_CONNECTED || wlanActive || evilActive || wifiSpamActive || deauthActive || nmapActive) ? "true" : "false",
             isCharging ? "true" : "false", btActive ? "true" : "false", currentSSID.c_str(), currentIP.c_str(),
             ESP.getCpuFreqMHz(), millis() / 1000, rssi, rssi_pct, etaMin, etaMode);
    server.send(200, "application/json", json);
}

String getBatteryHtml() {
    time_t jetzt; time(&jetzt); struct tm *aktuell = localtime(&jetzt); int curH = aktuell->tm_hour; int curM = aktuell->tm_min; String s; s.reserve(6400);

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
      body{background:#1a1a1a;color:#fff;font-family:sans-serif;text-align:center;padding:20px;margin:0;}
      .btn{background:#555;color:white;padding:12px 20px;border:none;border-radius:8px;text-decoration:none;display:inline-block;margin-top:20px;min-height:44px;line-height:20px;}
      #chart-container { position: relative; max-width: 900px; margin: 0 auto; background: #2d2d2d; border-radius: 12px; padding: 18px; box-shadow: 0 4px 15px rgba(0,0,0,0.3); }
      canvas { width: 100%; height: auto; cursor: crosshair; }
      #tooltip { position: absolute; background: rgba(0,0,0,0.9); padding: 10px; border-radius: 6px; color: #fff; font-size: 13px; pointer-events: none; display: none; text-align: left; z-index: 10; border: 1px solid #555; white-space: nowrap; transform: translate(-50%, -120%); }
      .legend { display: flex; justify-content: center; flex-wrap: wrap; gap: 12px; margin-top: 16px; font-size: 12px; }
      .leg-item { display: flex; align-items: center; gap: 6px; color: #ccc; }
      .dot { width: 12px; height: 12px; border-radius: 50%; }
      .filters { display:flex; gap:10px; flex-wrap:wrap; justify-content:center; margin: 0 0 14px 0; }
      .fg { background:#1f1f1f; border:1px solid #505050; border-radius:8px; padding:8px 10px; }
      .grp { display:flex; align-items:center; gap:8px; flex-wrap:wrap; }
      .tag { font-size:12px; color:#bbb; }
      .fbtn { background:#3a3a3a; color:#fff; border:1px solid #666; border-radius:6px; padding:8px 10px; min-height:38px; cursor:pointer; }
      .fbtn.active { background:#4CAF50; color:#000; border-color:#4CAF50; font-weight:bold; }
      select { background:#111; color:#fff; border:1px solid #666; border-radius:6px; padding:8px; min-height:38px; }
    </style>
    </head><body>)rawliteral");

    s += "<h2>" + String(tTitle) + "</h2>";
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
    s += F("<div class='legend'>");
    s += "<div class='leg-item'><div class='dot' style='background:#4CAF50;'></div>" + String(tr(sysLang, "Linie: Normal", "Line: Normal", "Ligne: Normal")) + "</div>";
    s += "<div class='leg-item'><div class='dot' style='background:#ff0000;'></div>" + String(tr(sysLang, "Linie: Hacking", "Line: Hacking", "Ligne: Hacking")) + "</div>";
    s += "<div class='leg-item'><div class='dot' style='background:#00aaff;'></div>" + String(tr(sysLang, "Linie: Laden", "Line: Charging", "Ligne: Charge")) + "</div>";
    s += "<div class='leg-item'><div class='dot' style='background:#2ecc71;'></div>" + String(tr(sysLang, "Flaeche: 75-100%", "Area: 75-100%", "Surface: 75-100%")) + "</div>";
    s += "<div class='leg-item'><div class='dot' style='background:#f1c40f;'></div>" + String(tr(sysLang, "Flaeche: 50-74%", "Area: 50-74%", "Surface: 50-74%")) + "</div>";
    s += "<div class='leg-item'><div class='dot' style='background:#ff9800;'></div>" + String(tr(sysLang, "Flaeche: 25-49%", "Area: 25-49%", "Surface: 25-49%")) + "</div>";
    s += "<div class='leg-item'><div class='dot' style='background:#e53935;'></div>" + String(tr(sysLang, "Flaeche: 0-24%", "Area: 0-24%", "Surface: 0-24%")) + "</div>";
    s += F("</div></div>");
    s += "<div style='display:flex; gap:10px; justify-content:center; flex-wrap:wrap;'>";
    s += "<a href='/' class='btn'>&#8592; " + String(tBack) + "</a>";
    s += "<form method='POST' action='/battery/reset' onsubmit='return confirm(\"" + String(tResetConfirm) + "\")' style='margin:0;'>";
    s += "<button type='submit' class='btn' style='background:#a33;'>" + String(tResetHist) + "</button></form></div>";
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
    time_t uTime = stringToTime(String(bdayStr), String(btimeStr)); time_t pTime = stringToTime(parentDate, parentTime);
    
    String parentStr = tr(sysLang, "Daten fehlen", "No Data", "Pas de données");
    if(uTime != 0 && pTime != 0) {
        long myAgeSec = jetzt - uTime; time_t targetDate = pTime + myAgeSec; struct tm *tgt = localtime(&targetDate); 
        char dBuf[30]; snprintf(dBuf, sizeof(dBuf), "%02d.%02d.%04d", tgt->tm_mday, tgt->tm_mon+1, tgt->tm_year+1900); 
        long myAgeThenSec = targetDate - uTime; int yearsThen = myAgeThenSec / 31536000;
        if (myAgeThenSec < 0) parentStr = String(dBuf) + " ; " + tr(sysLang, "Noch nicht geboren", "Not born yet", "Pas encore né"); 
        else parentStr = String(dBuf) + " ; " + String(yearsThen) + " " + tr(sysLang, "Jahre", "Years", "Ans");
    }
    
    String kindStr = "";
    if (child1Name != "") { long d1 = (jetzt-stringToTime(child1Date, child1Time))/86400; kindStr += child1Name + " (" + String(d1) + "T) "; }
    if (child2Name != "") { long d2 = (jetzt-stringToTime(child2Date, child2Time))/86400; kindStr += child2Name + " (" + String(d2) + "T) "; }
    if (child3Name != "") { long d3 = (jetzt-stringToTime(child3Date, child3Time))/86400; kindStr += child3Name + " (" + String(d3) + "T) "; }

    s += F(R"rawliteral(<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>
<style>
body{font-family:sans-serif;background:#1a1a1a;color:#ffffff;padding:10px;margin:0; transition: background 0.3s, color 0.3s;} 
.tabs{display:flex;background:#2d2d2d;border-radius:8px;margin-bottom:15px;overflow:hidden;flex-wrap:wrap; transition: background 0.3s;} 
.tab{flex:1;padding:12px;text-align:center;cursor:pointer;color:#aaa;border-bottom:3px solid transparent;font-size:14px;min-width:70px; transition: color 0.3s, background 0.3s;} 
.tab.active{color:#fff;border-bottom:3px solid #4CAF50;background:#383838;} 
.panel{display:none;background:#2d2d2d;padding:20px;border-radius:12px; transition: background 0.3s;} 
.panel.active{display:block;} 
input,select,textarea{padding:10px;margin:8px 0;border-radius:6px;border:1px solid #555;width:100%;box-sizing:border-box;background:#fff;color:#000; transition: background 0.3s, color 0.3s, border 0.3s;} 
input[type='color']{padding:0;height:40px;cursor:pointer;} 
.btn{background:#4CAF50;color:white;font-weight:bold;border:none;cursor:pointer;padding:12px;border-radius:6px;width:100%;margin-bottom:10px;min-height:44px;} 
.btn-blue{background:#2196F3;} .btn-red{background:#f44336;} .btn-gray{background:#555;} 
.toast{visibility:hidden;min-width:250px;background-color:#4CAF50;color:#fff;text-align:center;border-radius:5px;padding:16px;position:fixed;z-index:1;left:50%;bottom:30px;transform:translateX(-50%);} 
.toast.show{visibility:visible;animation:fadein 0.5s, fadeout 0.5s 2.5s;} 
@keyframes fadein{from{bottom:0;opacity:0;}to{bottom:30px;opacity:1;}} @keyframes fadeout{from{bottom:30px;opacity:1;}to{bottom:0;opacity:0;}} 
h3{margin-top:0;} .cb-lbl{display:flex; align-items:center; gap:10px; margin-bottom:10px; font-size:16px;} 
input[type='range']{width:100%; cursor:pointer;} 
.box{background:#333; padding:12px; border-radius:8px; font-size:14px; line-height:1.6; transition: background 0.3s;} 
.box-title{color:#4CAF50; margin:0 0 8px 0; border-bottom:1px solid #555; padding-bottom:5px; font-weight:bold;}

/* LIGHT MODE CSS */
body.light-mode { background: #e8e8e8; color: #111; }
body.light-mode .tabs, body.light-mode .panel, body.light-mode .box { background: #ffffff; border: 1px solid #ccc; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }
body.light-mode .tab { color: #666; }
body.light-mode .tab.active { background: #f4f4f4; color: #000; }
body.light-mode input, body.light-mode select, body.light-mode textarea { background: #f9f9f9; color: #000; border: 1px solid #aaa; }
body.light-mode .box-title { color: #2e7d32; border-bottom: 1px solid #eee; }
body.light-mode #w_ssid, body.light-mode #w_ip { color: #555 !important; }
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
body.high-contrast #wifi_icon, body.high-contrast #bt_icon { stroke:#fff !important; }
/* RESPONSIVE: hamburger for small screens */
.hamburger{display:none;background:#2d2d2d;color:#fff;border:none;font-size:28px;cursor:pointer;padding:10px 16px;width:100%;text-align:left;border-radius:8px;margin-bottom:4px;}
body.light-mode .hamburger{background:#fff;color:#111;border:1px solid #ccc;}
@media(max-width:520px){.hamburger{display:block;} .tabs{display:none;flex-direction:column;} .tabs.open{display:flex;} .tab{text-align:left;padding:14px 18px;border-bottom:none;border-left:3px solid transparent;} .tab.active{border-left:3px solid #4CAF50;border-bottom:none;}}
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
    stopBadAppsOk: { de:'Bad Apps gestoppt.', en:'Bad apps stopped.', fr:'Apps bad arretees.' },
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
    aodNetError: { de:'AOD Netzwerkfehler: ', en:'AOD network error: ', fr:'Erreur reseau AOD : ' }
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

function updateLive() {
    if (liveInFlight) return;
    liveInFlight = true;
    fetch('/api/status', { cache: 'no-store' }).then(r=>r.json()).then(d=>{
        requestAnimationFrame(function(){
            setHtmlIfChanged('d_ak', d.akku + '%' + (d.charging ? (' <span style="color:#00aaff;">(' + t('chargingShort') + ')</span>') : ''));
            setHtmlIfChanged('d_vo', d.volt + ' V');
            setHtmlIfChanged('d_cp', d.cpu + ' &deg;C');
            setHtmlIfChanged('d_ge', d.geld + ' &euro;');
            setHtmlIfChanged('d_cf', d.cpufreq + ' MHz');

            if(document.getElementById('d_ra')) {
                let rPct = Math.round((d.ram_free / d.ram_tot) * 100);
                setHtmlIfChanged('d_ra', d.ram_free + ' KB / ' + d.ram_tot + ' KB (' + rPct + '%)');
            }
            if(document.getElementById('d_rs')) {
                setHtmlIfChanged('d_rs', (d.rssi < 0) ? d.rssi + ' dBm (' + d.rssi_pct + '%)' : '--');
            }
            if(document.getElementById('d_up')) {
                let u = d.uptime; let h = Math.floor(u/3600); let m = Math.floor((u%3600)/60);
                setHtmlIfChanged('d_up', h+'h '+m+'m '+(u%60)+'s');
            }
            setHtmlIfChanged('d_eta', fmtEta(d.eta_min, d.eta_mode));

            if(document.getElementById('h_batt_txt')) {
                setTextIfChanged('h_batt_txt', d.akku + '%' + (d.charging ? ' \u26A1' : ''));
                var f = document.getElementById('h_batt_fill');
                f.style.width = d.akku + '%';
                if(d.charging) f.style.background = '#00aaff'; else if(d.akku > 50) f.style.background = '#4CAF50'; else if(d.akku > 20) f.style.background = '#ff9800'; else f.style.background = '#f44336';
            }

            setTextIfChanged('w_ssid', d.ssid);
            setTextIfChanged('w_ip', d.ip);

            if(document.getElementById('wifi_icon')) document.getElementById('wifi_icon').setAttribute('stroke', d.wifi ? (document.body.classList.contains('light-mode') ? '#000' : '#fff') : '#555');
            if(document.getElementById('bt_icon')) document.getElementById('bt_icon').setAttribute('stroke', d.bt ? '#00aaff' : '#555');
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
    updateLive();
};
function syncTitle(id, val) { document.getElementById(id).innerText = val; }
function updateVal(id, val) { document.getElementById(id).innerText = val + '%'; }
</script></head><body>)rawliteral");

    // HEADER mit Dark/Light Switcher (Sonne/Mond)
    s += F("<div style='display:flex; align-items:center; justify-content:space-between; margin-bottom:15px; padding-top:5px;'>");
    s += F("<h2 style='margin:0;'>"); s += tr(sysLang, "WatchOS Zentrale", "WatchOS Hub", "Centre WatchOS"); s += F("</h2>");
    s += F("<div style='display:flex; align-items:center; gap:15px;'>");
    
    s += F("<div style='display:flex; align-items:center; gap:5px;'><svg id='bt_icon' width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='#555' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><polyline points='6.5 6.5 17.5 17.5 12 23 12 1 17.5 6.5 6.5 17.5'></polyline></svg></div>");
    s += F("<div style='display:flex; align-items:center; gap:5px; text-align:right;'><div style='display:flex; flex-direction:column; font-size:10px; color:#aaa; line-height:1.2;'><span id='w_ssid'></span><span id='w_ip'></span></div><svg id='wifi_icon' width='24' height='24' viewBox='0 0 24 24' fill='none' stroke='#555' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M5 12.55a11 11 0 0 1 14.08 0'></path><path d='M1.42 9a16 16 0 0 1 21.16 0'></path><path d='M8.53 16.11a6 6 0 0 1 6.95 0'></path><line x1='12' y1='20' x2='12.01' y2='20'></line></svg></div>");
    s += F("<a href='/battery' style='display:flex; align-items:center; gap:5px; font-size:13px; font-weight:bold; color:inherit; text-decoration:none;'><span id='h_batt_txt'>--%</span><div style='width:22px; height:11px; border:2px solid #aaa; border-radius:3px; position:relative;'><div id='h_batt_fill' style='background:#4CAF50; width:50%; height:100%; transition: width 0.3s;'></div><div style='position:absolute; right:-4px; top:2px; width:2px; height:5px; background:#aaa; border-radius:0 2px 2px 0;'></div></div></a>");
    
    // Sun / Moon Toggle
    s += F("<div style='cursor:pointer;' onclick='toggleTheme()'>");
    s += F("<svg id='moon_icon' width='22' height='22' viewBox='0 0 24 24' fill='none' stroke='#aaa' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z'></path></svg>");
    s += F("<svg id='sun_icon' width='22' height='22' viewBox='0 0 24 24' fill='none' stroke='#ffa500' stroke-width='2' stroke-linecap='round' stroke-linejoin='round' style='display:none;'><circle cx='12' cy='12' r='5'></circle><line x1='12' y1='1' x2='12' y2='3'></line><line x1='12' y1='21' x2='12' y2='23'></line><line x1='4.22' y1='4.22' x2='5.64' y2='5.64'></line><line x1='18.36' y1='18.36' x2='19.78' y2='19.78'></line><line x1='1' y1='12' x2='3' y2='12'></line><line x1='21' y1='12' x2='23' y2='12'></line><line x1='4.22' y1='19.78' x2='5.64' y2='18.36'></line><line x1='18.36' y1='5.64' x2='19.78' y2='4.22'></line></svg>");
    s += F("</div>");
    s += F("<div style='cursor:pointer; color:#aaa; font-size:14px; font-weight:bold; border:1px solid #666; border-radius:6px; padding:4px 8px;' onclick='toggleContrast()' title='"); s += tr(sysLang, "Hoher Kontrast", "High Contrast", "Haut contraste"); s += F("'>HC</div>");

    s += F("<form action='/reboot' method='POST' style='margin:0;' onsubmit='return confirm(\""); s += tr(sysLang, "Uhr wirklich neustarten?", "Really reboot watch?", "Redemarrer la montre ?"); s += F("\");'><button type='submit' style='background:transparent; border:none; color:#f44336; cursor:pointer; padding:0;' title='"); s += tr(sysLang, "Neustart", "Reboot", "Redemarrer"); s += F("'><svg width='28' height='28' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M18.36 6.64a9 9 0 1 1-12.73 0'></path><line x1='12' y1='2' x2='12' y2='12'></line></svg></button></form></div></div>");

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
    s += F("</div><div class='tab' onclick='showTab(\"p6\", this);document.querySelector(\".tabs\").classList.remove(\"open\")'>");
    s += tr(sysLang, "Update", "Update", "Mise a jour");
    s += F("</div></div>");

    // 1. DASHBOARD
    s += F("<div id='p1' class='panel active'><h3>Dashboard</h3><div style='display:grid; grid-template-columns: 1fr 1fr; gap:10px;'>");
    s += F("<div class='box'><div class='box-title'>"); s += tr(sysLang, "Hardware", "Hardware", "Materiel"); s += F("</div>");
    s += tr(sysLang, "Akku", "Battery", "Batterie"); s += F(": <b id='d_ak' style='color:#fff;'>--%</b><br>");
    s += tr(sysLang, "Volt", "Voltage", "Tension"); s += F(": <b id='d_vo' style='color:#fff;'>-- V</b><br>");
    s += tr(sysLang, "CPU Takt", "CPU Clock", "Frequence CPU"); s += F(": <b id='d_cf' style='color:#fff;'>-- MHz</b><br>");
    s += tr(sysLang, "CPU Temp", "CPU Temp", "Temp CPU"); s += F(": <b id='d_cp' style='color:#fff;'>-- &deg;C</b><br>");
    s += F("RAM: <b id='d_ra' style='color:#fff;'>-- KB</b><br>");
    s += tr(sysLang, "Akku ETA", "Battery ETA", "ETA batterie"); s += F(": <b id='d_eta' style='color:#fff;'>--</b><br>");
    s += F("Uptime: <b id='d_up' style='color:#fff;'>--</b><br>");
    s += tr(sysLang, "WLAN Signal", "WiFi Signal", "Signal WiFi"); s += F(": <b id='d_rs' style='color:#fff;'>--</b></div>");
    s += F("<div class='box'><div class='box-title'>"); s += tr(sysLang, "Leben", "Life", "Vie"); s += F("</div>");
    s += tr(sysLang, "Restzeit", "Remaining time", "Temps restant"); s += F(": <b style='color:#fff;'>"); s += String(rJ); s += F(" J, "); s += String(rM); s += F(" M, "); s += String(rT); s += F(" T</b><br>");
    s += tr(sysLang, "O-Kosten", "Opportunity cost", "Cout opportunite"); s += F(": <b id='d_ge' style='color:#fff;'>0.0000 &euro;</b><br>");
    s += tr(sysLang, "Eltern", "Parents", "Parents"); s += F(": <b style='color:#fff;'>"); s += parentStr; s += F("</b><br>");
    s += tr(sysLang, "Kinder", "Children", "Enfants"); s += F(": <b style='color:#fff;'>"); s += kindStr; s += F("</b><br><br><form action='/matrix' method='POST' style='margin:0;'><input type='submit' class='btn' style='background:#000;color:#0f0;border:1px solid #0f0;margin:0;padding:8px;' value='[ ENTER MATRIX ]'></form></div>");
    s += F("</div></div>");
    
    // 2. ANZEIGE (9 Farben im Grid)
    s += F("<div id='p2' class='panel'>");
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
    s += F("<b>Helligkeit Aktiv: </b><span id='slM_v' style='color:#4CAF50;'>"); s += valMax; s += F("%</span><br><input name='maxb_pct' type='range' min='5' max='100' value='"); s += valMax; s += F("' oninput='updateVal(\"slM_v\", this.value)'>");
    s += F("<b>Helligkeit AOD: </b><span id='slA_v' style='color:#4CAF50;'>"); s += valMin; s += F("%</span><br><input name='minb_pct' type='range' min='0' max='100' value='"); s += valMin; s += F("' oninput='updateVal(\"slA_v\", this.value)'><br><br>");
    
    s += F("<div style='display:grid; grid-template-columns:1fr 1fr 1fr; gap:5px; text-align:center;'><div><b>Dimmen (s)</b><br><input name='tdim' type='number' min='5' max='120' value='"); s += (time_dim_start/1000); s += F("'></div>");
    s += F("<div><b>AOD (s)</b><br><input name='taod' type='number' min='10' max='300' value='"); s += (time_aod_start/1000); s += F("'></div>");
    s += F("<div><b>Sleep (s)</b><br><input name='tslp' type='number' min='20' max='600' value='"); s += (time_sleep_start/1000); s += F("'></div></div><br>");
    
    s += F("<b>Timer Alarm Typ:</b><br><select name='tAlarm'><option value='0'"); if(timerAlarmMode==0) s += F(" selected"); s += F(">Nur Haptisch</option><option value='1'"); if(timerAlarmMode==1) s += F(" selected"); s += F(">Nur Akustisch</option><option value='2'"); if(timerAlarmMode==2) s += F(" selected"); s += F(">Beides</option></select><br><br>");
    
    s += F("<b>Haptic Waveform (1-123): </b><span id='hap_v' style='color:#4CAF50;'>"); s += hapticWaveform; s += F("</span><br><input name='hapW' type='range' min='1' max='123' value='"); s += hapticWaveform; s += F("' oninput='updateVal(\"hap_v\", this.value)'><hr>");
    
    s += F("<h3>"); s += tr(sysLang, "Web Dashboard Schutz", "Web Dashboard Protection", "Protection Dashboard Web"); s += F("</h3>");
    s += F("<label class='cb-lbl'><input type='checkbox' name='webAuth' value='1' "); if(webAuthEnabled) s += F("checked"); s += F("> "); s += tr(sysLang, "Passwort-Schutz aktivieren", "Enable password protection", "Activer protection par mot de passe"); s += F("</label>");
    s += F("<b>"); s += tr(sysLang, "Benutzername", "Username", "Nom d'utilisateur"); s += F(":</b><input name='webUsr' type='text' value='"); s += htmlEscape(webUser); s += F("'>");
    s += F("<b>"); s += tr(sysLang, "Passwort", "Password", "Mot de passe"); s += F(":</b><input name='webPwd' type='password' value='"); s += htmlEscape(webPass); s += F("'><hr>");
    
    s += F("<input type='submit' class='btn' value='"); s += tr(sysLang, "System speichern", "Save system", "Enregistrer systeme"); s += F("'></form><hr>");

    s += F("<form id='fTime' onsubmit='return saveTime()'><h3>"); s += tr(sysLang, "Uhrzeit & Zeitzone", "Time & Timezone", "Heure & Fuseau horaire"); s += F("</h3>");
    s += F("<div style='display:grid; grid-template-columns:1fr 1fr; gap:10px; margin-bottom:10px;'><div><b>"); s += tr(sysLang, "Datum", "Date", "Date"); s += F(":</b><br><input type='date' name='md' value='"); s += sysD; s += F("'></div><div><b>"); s += tr(sysLang, "Uhrzeit", "Time", "Heure"); s += F(":</b><br><input type='time' name='mt' value='"); s += sysT; s += F("'></div></div>");
    s += F("<div style='display:grid; grid-template-columns:1fr 1fr; gap:10px; align-items:center;'><div><b>"); s += tr(sysLang, "Zeitzone", "Timezone", "Fuseau horaire"); s += F(":</b><br><select name='ntpz'><option value='CET-1CEST,M3.5.0,M10.5.0/3'"); if(ntpZone == "CET-1CEST,M3.5.0,M10.5.0/3") s += F(" selected"); s += F(">Berlin</option><option value='GMT0BST,M3.5.0/1,M10.5.0'"); if(ntpZone == "GMT0BST,M3.5.0/1,M10.5.0") s += F(" selected"); s += F(">London</option></select></div><div><br><label class='cb-lbl'><input type='checkbox' name='dst' value='1' "); if(aktuell->tm_isdst > 0) s += F("checked"); s += F("> "); s += tr(sysLang, "Sommerzeit", "Daylight saving", "Heure d'ete"); s += F("</label></div></div>");
    s += F("<div style='display:flex; gap:10px; margin-top:10px;'><input type='submit' class='btn btn-blue' value='"); s += tr(sysLang, "Manuell setzen", "Set manually", "Regler manuellement"); s += F("' style='margin:0;'><button type='button' class='btn btn-gray' style='margin:0;' onclick='fetch(\"/sync\", {method:\"POST\"}).then(()=>setTimeout(()=>location.reload(), 4000))'>NTP Sync</button></div></form>");
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
    s += F("<b>Elternteil Name:</b><input name='pN' type='text' value='"); s += parentName; s += F("'>");
    s += F("<b>Geburt & Zeit:</b><br><input name='pD' type='date' value='"); s += parentDate; s += F("' style='width:60%;display:inline-block;'><input name='pT' type='time' value='"); s += parentTime; s += F("' style='width:35%;display:inline-block;float:right;'><hr>");
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
    s += tr(sysLang, "Not-Aus: Bad Apps stoppen", "Emergency Stop: Stop Bad Apps", "Arret d'urgence: Stopper Apps Bad");
    s += F("</button></div>");
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
    
    s += F("<details style='background:#222;padding:10px;border-radius:6px;margin-bottom:8px;'><summary style='cursor:pointer;color:#4CAF50;font-weight:bold;'>"); s += tr(sysLang, "Skript 1", "Script 1", "Script 1"); s += F(": <span id='sN0'>"); s += htmlEscape(badUsbNames[0]); s += F("</span></summary>");
    s += F("Name: <input name='buN0' type='text' value='"); s += htmlEscape(badUsbNames[0]); s += F("' oninput='syncTitle(\"sN0\", this.value)'><br>");
    s += F("Code: <textarea name='buS0' rows='4' style='font-family:monospace; font-size:11px;'>"); s += htmlEscape(badUsbScripts[0]); s += F("</textarea></details>");

    s += F("<details style='background:#222;padding:10px;border-radius:6px;margin-bottom:8px;'><summary style='cursor:pointer;color:#4CAF50;font-weight:bold;'>"); s += tr(sysLang, "Skript 2", "Script 2", "Script 2"); s += F(": <span id='sN1'>"); s += htmlEscape(badUsbNames[1]); s += F("</span></summary>");
    s += F("Name: <input name='buN1' type='text' value='"); s += htmlEscape(badUsbNames[1]); s += F("' oninput='syncTitle(\"sN1\", this.value)'><br>");
    s += F("Code: <textarea name='buS1' rows='4' style='font-family:monospace; font-size:11px;'>"); s += htmlEscape(badUsbScripts[1]); s += F("</textarea></details>");

    s += F("<details style='background:#222;padding:10px;border-radius:6px;margin-bottom:8px;'><summary style='cursor:pointer;color:#4CAF50;font-weight:bold;'>"); s += tr(sysLang, "Skript 3", "Script 3", "Script 3"); s += F(": <span id='sN2'>"); s += htmlEscape(badUsbNames[2]); s += F("</span></summary>");
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
    s += F("<p>"); s += tr(sysLang, "WatchOS Version", "WatchOS Version", "Version WatchOS"); s += F(": <b id='ota_cur_ver' style='color:#4CAF50;'>"); s += WATCHOS_VERSION; s += F("</b><br>Build: <b style='color:#4CAF50;'>");
    s += String(__DATE__) + " - " + String(__TIME__);
    s += F("</b></p><hr><p>"); s += tr(sysLang, "Waehle die kompilierte .bin Datei aus.", "Select the compiled .bin file.", "Selectionnez le fichier .bin compile."); s += F("</p>");
    s += F("<input type='file' id='otafile' accept='.bin' onchange='updateOtaMeta()' style='background:#333;color:#fff;padding:10px;margin:8px 0;border-radius:6px;width:100%;box-sizing:border-box;'><br>");
    s += F("<div id='ota_meta' style='font-size:12px; color:#bbb; margin:4px 0;'></div>");
    s += F("<div id='ota_sha' style='font-family:monospace; font-size:11px; color:#9fd; margin:4px 0 10px 0; word-break:break-all;'></div>");
    s += F("<button type='button' class='btn btn-red' onclick='uploadOTA()'>"); s += tr(sysLang, "Firmware flashen", "Flash firmware", "Flasher le firmware"); s += F("</button>");
    s += F("<div id='otaprog-container' style='display:none;background:#222;border-radius:4px;margin-top:10px;height:20px;width:100%;overflow:hidden;'><div id='otaprog' style='background:#4CAF50;height:100%;width:0%;text-align:center;font-size:12px;line-height:20px;'>0%</div></div></div>");
    
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
        prefs.putFloat("netS", netSalary); prefs.putInt("pt", partTimePct); prefs.putInt("vac", vacationDays); prefs.putInt("sick", sickDays); prefs.putFloat("wh", workHoursPerDay); prefs.putInt("wd", workDaysYear);
        prefs.putString("c1n", child1Name); prefs.putString("c1d", child1Date); prefs.putString("c1t", child1Time); prefs.putString("c2n", child2Name); prefs.putString("c2d", child2Date); prefs.putString("c2t", child2Time); prefs.putString("c3n", child3Name); prefs.putString("c3d", child3Date); prefs.putString("c3t", child3Time);
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

void setupServerRoutes() {
    server.on("/", []() { if (evilActive) handleEvilRoot(); else handleRoot(); });
    server.onNotFound([]() { if (evilActive) server.sendHeader("Location", "http://hotspot.bund/", true); else server.sendHeader("Location", "/", true); server.send(302, "text/plain", ""); });
    server.on("/api/status", HTTP_GET, handleApiStatus);
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

    server.on("/sync", HTTP_POST, []() { server.sendHeader("Connection", "close"); server.send(200, "text/html", "<html><body style='background:#222;color:#fff;text-align:center;'><h2>Synchronisiere...</h2><script>setTimeout(function(){window.location.href='/';}, 6000);</script></body></html>"); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); WiFi.begin(userSSID.c_str(), userPWD.c_str()); int retry = 0; while (WiFi.status() != WL_CONNECTED && retry < 20) { delay(400); retry++; } if (WiFi.status() == WL_CONNECTED) { configTzTime(ntpZone.c_str(), ntpServer.c_str()); struct tm timeinfo; int ntpWait = 0; while (!getLocalTime(&timeinfo, 1000) && ntpWait < 2) { ntpWait++; } } WiFi.disconnect(true); WiFi.mode(WIFI_AP); WiFi.softAP(apSSID.c_str(), apPWD.c_str()); lastSecond = -1; });
    server.on("/reboot", HTTP_POST, []() { server.sendHeader("Connection", "close"); server.send(200, "text/plain", "Neustart..."); delay(1000); ESP.restart(); });
    server.on("/matrix", HTTP_POST, []() { matrixMode = !matrixMode; if(matrixMode) { tft.fillScreen(TFT_BLACK); for(int i=0; i<24; i++) matrixY[i] = 0; } else { lastSecond = -1; } server.sendHeader("Location", "/"); server.send(303); });
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

void toggleWLAN() {
    if (evilActive) toggleEvilPortal(); 
    if (!wlanActive) {
        wlanActive = true; wlanClientMode = false;
        if (ESP.getCpuFreqMHz() != 240) setCpuFrequencyMhz(240); 
        WiFi.mode(WIFI_AP); WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0)); WiFi.softAP(apSSID.c_str(), apPWD.c_str()); 
        delay(100); dnsServer.setErrorReplyCode(DNSReplyCode::NoError); dnsServer.start(DNS_PORT, "*", WiFi.softAPIP()); server.begin(); MDNS.begin("watchos"); 
        wlanIP = "192.168.4.1"; showToast("Hotspot: " + apSSID, 4000);
    } 
    else if (wlanActive && !wlanClientMode) {
        wlanClientMode = true; dnsServer.stop(); server.stop(); WiFi.softAPdisconnect(true); showToast("Verbinde Client...", 2000);
        WiFi.mode(WIFI_STA); WiFi.begin(userSSID.c_str(), userPWD.c_str());
        int timeout = 0; while (WiFi.status() != WL_CONNECTED && timeout < 16) { delay(500); timeout++; }
        if (WiFi.status() == WL_CONNECTED) {
            server.begin(); MDNS.begin("watchos"); wlanIP = WiFi.localIP().toString(); showToast("IP: " + wlanIP, 4000);
        } else {
            wlanClientMode = false; WiFi.disconnect(true); WiFi.mode(WIFI_AP); WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0)); WiFi.softAP(apSSID.c_str(), apPWD.c_str()); 
            delay(100); dnsServer.setErrorReplyCode(DNSReplyCode::NoError); dnsServer.start(DNS_PORT, "*", WiFi.softAPIP()); server.begin(); MDNS.begin("watchos"); 
            wlanIP = "192.168.4.1"; showToast("Fallback: Hotspot", 4000);
        }
    } 
    else {
        wlanActive = false; wlanClientMode = false; dnsServer.stop(); server.stop(); WiFi.disconnect(true); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF); 
        wlanIP = ""; showToast("WLAN Aus", 2000);
    }
}