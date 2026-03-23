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
#include "EvilPortalApp.h"

const byte DNS_PORT = 53;

// Globale Variablen für den Client-Modus
bool wlanClientMode = false;
String wlanIP = "";

const char* tr(int lang, const char* de, const char* en, const char* fr) {
    if(lang == 1) return en;
    if(lang == 2) return fr;
    return de;
}

void handleApiStatus() {
    time_t jetzt; time(&jetzt); struct tm *aktuell = localtime(&jetzt);
    float dailyNet = (netSalary * 12.0) / 365.0; float fraction = (millis() % 1000) / 1000.0; 
    long secToday = aktuell->tm_hour * 3600 + aktuell->tm_min * 60 + aktuell->tm_sec; 
    float earnedToday = dailyNet * ((secToday + fraction) / 86400.0);
    
    bool isCharging = pmu.isVbusIn() || pmu.isCharging();
    String currentSSID = ""; String currentIP = "";
    if (wlanActive) { if (wlanClientMode) { currentSSID = userSSID; currentIP = wlanIP; } else { currentSSID = apSSID; currentIP = "192.168.4.1"; } } 
    else if (nmapActive) { currentSSID = targetSSIDs[currentTargetNmap]; currentIP = WiFi.localIP().toString(); } 
    else if (evilActive) { currentSSID = evilSSID; currentIP = "192.168.4.1"; } 
    else if (deauthActive || wifiSpamActive) { currentSSID = "Hacking..."; currentIP = "RAW TX"; }
    
    // RSSI zu Prozent Umrechnung (-100dBm = 0%, -50dBm = 100%)
    int rssi = WiFi.RSSI();
    int rssi_pct = 0;
    if (rssi <= -100) rssi_pct = 0;
    else if (rssi >= -50) rssi_pct = 100;
    else rssi_pct = 2 * (rssi + 100);
    
    char json[700];
    snprintf(json, sizeof(json), "{\"akku\":%d,\"volt\":\"%.2f\",\"cpu\":\"%.1f\",\"ram_free\":%d,\"ram_tot\":%d,\"geld\":\"%.4f\",\"wifi\":%s,\"charging\":%s,\"bt\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"cpufreq\":%d,\"uptime\":%lu,\"rssi\":%d,\"rssi_pct\":%d}", 
             getBattery(), pmu.getBattVoltage()/1000.0, temperatureRead(), 
             (int)(ESP.getFreeHeap() / 1024), (int)(ESP.getHeapSize() / 1024), earnedToday, 
             (WiFi.status() == WL_CONNECTED || wlanActive || evilActive || wifiSpamActive || deauthActive || nmapActive) ? "true" : "false",
             isCharging ? "true" : "false", btActive ? "true" : "false", currentSSID.c_str(), currentIP.c_str(),
             ESP.getCpuFreqMHz(), millis() / 1000, rssi, rssi_pct);
    server.send(200, "application/json", json);
}

String getBatteryHtml() {
    time_t jetzt; time(&jetzt); struct tm *aktuell = localtime(&jetzt); int curH = aktuell->tm_hour; int curM = aktuell->tm_min; String s; s.reserve(4000);
    
    s += F(R"rawliteral(<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>
    <style>
      body{background:#1a1a1a;color:#fff;font-family:sans-serif;text-align:center;padding:20px;margin:0;}
      .btn{background:#555;color:white;padding:12px 20px;border:none;border-radius:6px;text-decoration:none;display:inline-block;margin-top:20px;}
      #chart-container { position: relative; max-width: 800px; margin: 0 auto; background: #2d2d2d; border-radius: 10px; padding: 20px; box-shadow: 0 4px 15px rgba(0,0,0,0.3); }
      canvas { width: 100%; height: auto; cursor: crosshair; }
      #tooltip { position: absolute; background: rgba(0,0,0,0.85); padding: 12px; border-radius: 6px; color: #fff; font-size: 13px; pointer-events: none; display: none; text-align: left; z-index: 10; border: 1px solid #555; white-space: nowrap; transform: translate(-50%, -120%); }
      .legend { display: flex; justify-content: center; flex-wrap: wrap; gap: 15px; margin-top: 20px; font-size: 13px; }
      .leg-item { display: flex; align-items: center; gap: 6px; color: #ccc; }
      .dot { width: 12px; height: 12px; border-radius: 50%; }
    </style>
    </head><body>
    <h2>Akku Verlauf & Power Status</h2>
    <div id='chart-container'>
      <div id='tooltip'></div>
      <canvas id='chart' width='800' height='350'></canvas>
      <div class='legend'>
         <div class='leg-item'><div class='dot' style='background:#4CAF50;'></div>Normal (AOD / Sleep)</div>
         <div class='leg-item'><div class='dot' style='background:#ff0000;'></div>Stealth (Hacking aktiv)</div>
         <div class='leg-item'><div class='dot' style='background:#00aaff;'></div>Am Strom geladen</div>
      </div>
    </div>
    <a href='/' class='btn'>&#8592; Zurueck zum Dashboard</a>
    <script>)rawliteral");

    s += F("var rawData = [");
    for(int i=0; i<historyCount; i++) { s += batHistoryPct[i]; if(i < historyCount - 1) s += F(","); }
    s += F("];\nvar intervalMins = "); s += (historyInterval / 60000); s += F(";\nvar curH = "); s += curH; s += F(";\nvar curM = "); s += curM; s += F(";\n");

    s += F(R"rawliteral(
    var canvas = document.getElementById('chart'); var ctx = canvas.getContext('2d'); var tooltip = document.getElementById('tooltip');
    var w = canvas.width; var h = canvas.height; var padX = 50; var padY = 30; var chartW = w - padX * 2; var chartH = h - padY * 2;
    var curDate = new Date(); curDate.setHours(curH, curM, 0, 0);

    var data = [];
    for(var i=0; i<rawData.length; i++) {
        var v = rawData[i]; var pct = v % 100;
        if(v >= 200 && pct === 0 && v !== 200) pct = 100; 
        var state = (v >= 200) ? 2 : (v >= 100 ? 1 : 0);
        var t = new Date(curDate.getTime() - (rawData.length - 1 - i) * intervalMins * 60000);
        data.push({ x: padX + (i / Math.max(1, rawData.length - 1)) * chartW, y: padY + chartH - (pct / 100) * chartH, pct: pct, state: state, time: t });
    }

    function drawBase() {
        ctx.clearRect(0,0,w,h); ctx.fillStyle = '#aaa'; ctx.strokeStyle = '#444'; ctx.lineWidth = 1; ctx.font = '12px sans-serif'; ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
        ctx.beginPath();
        for(var i=0; i<=4; i++){
            var y = padY + chartH - (i/4)*chartH;
            ctx.moveTo(padX, y); ctx.lineTo(w-padX, y);
            ctx.fillText(i*25+'%', padX - 10, y);
        }
        ctx.stroke();

        ctx.textAlign = 'center'; ctx.textBaseline = 'top';
        if(data.length > 0) {
            var numLabels = 6;
            for(var i=0; i<=numLabels; i++) {
                var idx = Math.floor(i * (data.length-1) / numLabels);
                if(data[idx]) {
                    var tx = data[idx].x;
                    var tm = ('0'+data[idx].time.getHours()).slice(-2) + ':' + ('0'+data[idx].time.getMinutes()).slice(-2);
                    ctx.fillText(tm, tx, h - padY + 10);
                }
            }
        }

        if(data.length > 1) {
            ctx.lineWidth = 3;
            for(var i=1; i<data.length; i++) {
                ctx.beginPath(); ctx.moveTo(data[i-1].x, data[i-1].y); ctx.lineTo(data[i].x, data[i].y);
                if(data[i].state === 2) ctx.strokeStyle = '#00aaff'; else if(data[i].state === 1) ctx.strokeStyle = '#ff0000'; else ctx.strokeStyle = '#4CAF50';
                ctx.stroke();
            }
        }
    }
    drawBase();

    canvas.addEventListener('mousemove', function(e) {
        var rect = canvas.getBoundingClientRect(); var scaleX = canvas.width / rect.width; var scaleY = canvas.height / rect.height;
        var mouseX = (e.clientX - rect.left) * scaleX;
        if(data.length === 0 || mouseX < padX || mouseX > w - padX) { tooltip.style.display = 'none'; drawBase(); return; }

        var closest = data[0]; var minDist = Math.abs(mouseX - closest.x);
        for(var i=1; i<data.length; i++) { var d = Math.abs(mouseX - data[i].x); if(d < minDist) { minDist = d; closest = data[i]; } }

        drawBase();
        ctx.beginPath(); ctx.strokeStyle = '#fff'; ctx.lineWidth = 1; ctx.setLineDash([4, 4]); ctx.moveTo(closest.x, padY); ctx.lineTo(closest.x, padY + chartH); ctx.stroke(); ctx.setLineDash([]);
        ctx.beginPath(); ctx.arc(closest.x, closest.y, 6, 0, 2*Math.PI); ctx.fillStyle = closest.state === 2 ? '#00aaff' : (closest.state === 1 ? '#ff0000' : '#4CAF50'); ctx.fill(); ctx.lineWidth=2; ctx.strokeStyle='#fff'; ctx.stroke();

        var stateStr = closest.state === 2 ? '⚡ Laden (USB)' : (closest.state === 1 ? '🔥 Hacking aktiv' : '💤 Normal');
        var timeStr = ('0'+closest.time.getHours()).slice(-2) + ':' + ('0'+closest.time.getMinutes()).slice(-2);
        tooltip.innerHTML = '<b style="color:#4CAF50;">' + timeStr + ' Uhr</b><br><br><b>Akku:</b> ' + closest.pct + '%<br><b>Status:</b> ' + stateStr;
        tooltip.style.display = 'block';
        
        var tipX = (closest.x / scaleX);
        if(tipX + 120 > rect.width) tipX -= 50; else if(tipX - 120 < 0) tipX += 50;
        tooltip.style.left = tipX + 'px'; tooltip.style.top = (closest.y / scaleY) + 'px';
    });

    canvas.addEventListener('mouseleave', function() { tooltip.style.display = 'none'; drawBase(); });
    </script></body></html>)rawliteral");
    return s;
}

String htmlEscape(String str) { str.replace("&", "&amp;"); str.replace("<", "&lt;"); str.replace(">", "&gt;"); str.replace("\"", "&quot;"); return str; }

String getHtmlContent() {
    String s; s.reserve(25000); 
    
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
.btn{background:#4CAF50;color:white;font-weight:bold;border:none;cursor:pointer;padding:12px;border-radius:6px;width:100%;margin-bottom:10px;} 
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
</style>
<script>
function showTab(id, el){ document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active')); document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active')); document.getElementById(id).classList.add('active'); el.classList.add('active'); } 
function toggleTheme() { 
    document.body.classList.toggle('light-mode'); 
    var isLight = document.body.classList.contains('light-mode'); 
    localStorage.setItem('lightMode', isLight);
    document.getElementById('moon_icon').style.display = isLight ? 'none' : 'block';
    document.getElementById('sun_icon').style.display = isLight ? 'block' : 'none';
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
function saveForm(formId){ var data = new URLSearchParams(new FormData(document.getElementById(formId))); fetch('/save', {method:'POST', body:data}).then(()=>{ var t = document.getElementById('toast'); t.className = 'toast show'; setTimeout(function(){ t.className = t.className.replace('show', ''); }, 3000); }); return false; }
function saveTime(){ var data = new URLSearchParams(new FormData(document.getElementById('fTime'))); fetch('/settime', {method:'POST', body:data}).then(()=>{ var t = document.getElementById('toast'); t.className = 'toast show'; setTimeout(function(){ t.className = t.className.replace('show', ''); }, 3000); }); return false; }
function previewHTML() { var w = window.open('', '_blank'); w.document.write(document.getElementsByName('evHtml')[0].value); w.document.close(); }
function uploadOTA() { var file = document.getElementById('otafile').files[0]; if(!file) { alert('Bitte Firmware-Datei waehlen!'); return; } var fd = new FormData(); fd.append('update', file); var xhr = new XMLHttpRequest(); document.getElementById('otaprog-container').style.display = 'block'; xhr.upload.addEventListener('progress', function(e) { if(e.lengthComputable) { var pct = Math.round((e.loaded / e.total) * 100); document.getElementById('otaprog').style.width = pct + '%'; document.getElementById('otaprog').innerHTML = pct + '%'; } }, false); xhr.addEventListener('load', function() { if(xhr.status == 200) { document.getElementById('otaprog').innerHTML = 'Erfolgreich! Neustart...'; setTimeout(function(){ location.reload(); }, 6000); } else { document.getElementById('otaprog').innerHTML = 'Fehler!'; document.getElementById('otaprog').style.background = 'red'; } }, false); xhr.open('POST', '/update?size=' + file.size, true); xhr.send(fd); }

function scanWifi() {
    var btn = document.getElementById('scanBtn'); btn.value = 'Scanne... (kann dauern)';
    fetch('/wifiscan').then(r=>r.json()).then(d=>{
        btn.value = 'WLANs scannen';
        var dl = document.getElementById('scannedWifis'); dl.innerHTML = '';
        var unique = []; d.forEach(ssid => { if(unique.indexOf(ssid) === -1 && ssid.length > 0) { unique.push(ssid); var opt = document.createElement('option'); opt.value = ssid; dl.appendChild(opt); } });
        alert(unique.length + ' Netzwerke gefunden!');
    }).catch(e=>{ btn.value = 'WLANs scannen'; alert('Fehler beim Scannen'); });
}

function scanRouters() {
    var btn = document.getElementById('scanRBtn'); btn.value = 'Scanne Router...';
    fetch('/routerscan').then(r=>r.json()).then(d=>{
        btn.value = 'Routern scannen';
        var t = document.getElementById('routerTable'); t.innerHTML = '';
        d.forEach(n => {
            var div = document.createElement('div');
            div.style = 'background:transparent; padding:10px; margin-bottom:5px; border:1px solid #555; border-radius:5px; display:flex; justify-content:space-between; align-items:center;';
            div.innerHTML = '<div><b>'+n.ssid+'</b><br><span style="font-size:11px;color:#aaa;">'+n.bssid+' (Ch: '+n.ch+')</span></div><button type="button" class="btn btn-red" style="margin:0; width:auto; padding:8px 15px;" onclick="deauthMac(\''+n.bssid+'\')">Deauth</button>';
            t.appendChild(div);
        });
    });
}
function deauthMac(mac) { fetch('/deauth?mac='+mac); alert('Deauth Angriff auf ' + mac + ' gestartet!'); }

function updateLive() { 
    fetch('/api/status').then(r=>r.json()).then(d=>{ 
        if(document.getElementById('d_ak')) document.getElementById('d_ak').innerHTML = d.akku + '%' + (d.charging ? ' <span style="color:#00aaff;">(L&auml;dt)</span>' : ''); 
        if(document.getElementById('d_vo')) document.getElementById('d_vo').innerHTML = d.volt + ' V'; 
        if(document.getElementById('d_cp')) document.getElementById('d_cp').innerHTML = d.cpu + ' &deg;C'; 
        if(document.getElementById('d_ge')) document.getElementById('d_ge').innerHTML = d.geld + ' &euro;'; 
        if(document.getElementById('d_cf')) document.getElementById('d_cf').innerHTML = d.cpufreq + ' MHz';
        
        // RAM (RELATIV)
        if(document.getElementById('d_ra')) { 
            let rPct = Math.round((d.ram_free / d.ram_tot) * 100);
            document.getElementById('d_ra').innerHTML = d.ram_free + ' KB / ' + d.ram_tot + ' KB (' + rPct + '%)'; 
        }
        // WLAN SIGNAL (RELATIV)
        if(document.getElementById('d_rs')) { 
            document.getElementById('d_rs').innerHTML = (d.rssi < 0) ? d.rssi + ' dBm (' + d.rssi_pct + '%)' : '--'; 
        }
        
        if(document.getElementById('d_up')) { let u = d.uptime; let h = Math.floor(u/3600); let m = Math.floor((u%3600)/60); document.getElementById('d_up').innerHTML = h+'h '+m+'m '+(u%60)+'s'; }
        
        if(document.getElementById('h_batt_txt')) {
            document.getElementById('h_batt_txt').innerText = d.akku + '%' + (d.charging ? ' \u26A1' : '');
            var f = document.getElementById('h_batt_fill'); f.style.width = d.akku + '%';
            if(d.charging) f.style.background = '#00aaff'; else if(d.akku > 50) f.style.background = '#4CAF50'; else if(d.akku > 20) f.style.background = '#ff9800'; else f.style.background = '#f44336';
        }
        
        if(document.getElementById('w_ssid')) document.getElementById('w_ssid').innerText = d.ssid;
        if(document.getElementById('w_ip')) document.getElementById('w_ip').innerText = d.ip;
        
        if(document.getElementById('wifi_icon')) document.getElementById('wifi_icon').setAttribute('stroke', d.wifi ? (document.body.classList.contains('light-mode') ? '#000' : '#fff') : '#555');
        if(document.getElementById('bt_icon')) document.getElementById('bt_icon').setAttribute('stroke', d.bt ? '#00aaff' : '#555');
        
        setTimeout(updateLive, 500); 
    }).catch(e=>{ setTimeout(updateLive, 2000); }); 
}
window.onload = function() {
    if(localStorage.getItem('lightMode') === 'true') { toggleTheme(); }
    updateLive();
};
function syncTitle(id, val) { document.getElementById(id).innerText = val; }
function updateVal(id, val) { document.getElementById(id).innerText = val + '%'; }
</script></head><body>)rawliteral");

    // HEADER mit Dark/Light Switcher (Sonne/Mond)
    s += F("<div style='display:flex; align-items:center; justify-content:space-between; margin-bottom:15px; padding-top:5px;'>");
    s += F("<h2 style='margin:0;'>WatchOS Zentrale</h2>");
    s += F("<div style='display:flex; align-items:center; gap:15px;'>");
    
    s += F("<div style='display:flex; align-items:center; gap:5px;'><svg id='bt_icon' width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='#555' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><polyline points='6.5 6.5 17.5 17.5 12 23 12 1 17.5 6.5 6.5 17.5'></polyline></svg></div>");
    s += F("<div style='display:flex; align-items:center; gap:5px; text-align:right;'><div style='display:flex; flex-direction:column; font-size:10px; color:#aaa; line-height:1.2;'><span id='w_ssid'></span><span id='w_ip'></span></div><svg id='wifi_icon' width='24' height='24' viewBox='0 0 24 24' fill='none' stroke='#555' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M5 12.55a11 11 0 0 1 14.08 0'></path><path d='M1.42 9a16 16 0 0 1 21.16 0'></path><path d='M8.53 16.11a6 6 0 0 1 6.95 0'></path><line x1='12' y1='20' x2='12.01' y2='20'></line></svg></div>");
    s += F("<div style='display:flex; align-items:center; gap:5px; font-size:13px; font-weight:bold;'><span id='h_batt_txt'>--%</span><div style='width:22px; height:11px; border:2px solid #aaa; border-radius:3px; position:relative;'><div id='h_batt_fill' style='background:#4CAF50; width:50%; height:100%; transition: width 0.3s;'></div><div style='position:absolute; right:-4px; top:2px; width:2px; height:5px; background:#aaa; border-radius:0 2px 2px 0;'></div></div></div>");
    
    // Sun / Moon Toggle
    s += F("<div style='cursor:pointer;' onclick='toggleTheme()'>");
    s += F("<svg id='moon_icon' width='22' height='22' viewBox='0 0 24 24' fill='none' stroke='#aaa' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z'></path></svg>");
    s += F("<svg id='sun_icon' width='22' height='22' viewBox='0 0 24 24' fill='none' stroke='#ffa500' stroke-width='2' stroke-linecap='round' stroke-linejoin='round' style='display:none;'><circle cx='12' cy='12' r='5'></circle><line x1='12' y1='1' x2='12' y2='3'></line><line x1='12' y1='21' x2='12' y2='23'></line><line x1='4.22' y1='4.22' x2='5.64' y2='5.64'></line><line x1='18.36' y1='18.36' x2='19.78' y2='19.78'></line><line x1='1' y1='12' x2='3' y2='12'></line><line x1='21' y1='12' x2='23' y2='12'></line><line x1='4.22' y1='19.78' x2='5.64' y2='18.36'></line><line x1='18.36' y1='5.64' x2='19.78' y2='4.22'></line></svg>");
    s += F("</div>");

    s += F("<form action='/reboot' method='POST' style='margin:0;' onsubmit='return confirm(\"Uhr wirklich neustarten?\");'><button type='submit' style='background:transparent; border:none; color:#f44336; cursor:pointer; padding:0;' title='Neustart'><svg width='28' height='28' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M18.36 6.64a9 9 0 1 1-12.73 0'></path><line x1='12' y1='2' x2='12' y2='12'></line></svg></button></form></div></div>");

    // TABS (Mit Übersetzung)
    s += F("<div class='tabs'><div class='tab active' onclick='showTab(\"p1\", this)'>Home</div><div class='tab' onclick='showTab(\"p2\", this)'>");
    s += tr(sysLang, "Anzeige", "Display", "Affichage");
    s += F("</div><div class='tab' onclick='showTab(\"p3\", this)'>");
    s += tr(sysLang, "System", "System", "Système");
    s += F("</div><div class='tab' onclick='showTab(\"p5\", this)'>");
    s += tr(sysLang, "Persoenlich", "Personal", "Personnel");
    s += F("</div><div class='tab' onclick='showTab(\"p8\", this)'>Hacker Tools</div><div class='tab' onclick='showTab(\"p6\", this)'>Update</div></div>");

    // 1. DASHBOARD
    s += F("<div id='p1' class='panel active'><h3>Dashboard</h3><div style='display:grid; grid-template-columns: 1fr 1fr; gap:10px;'>");
    s += F("<div class='box'><div class='box-title'>Hardware</div>Akku: <b id='d_ak' style='color:#fff;'>--%</b><br>Volt: <b id='d_vo' style='color:#fff;'>-- V</b><br>CPU Takt: <b id='d_cf' style='color:#fff;'>-- MHz</b><br>CPU Temp: <b id='d_cp' style='color:#fff;'>-- &deg;C</b><br>RAM: <b id='d_ra' style='color:#fff;'>-- KB</b><br>Uptime: <b id='d_up' style='color:#fff;'>--</b><br>WLAN Signal: <b id='d_rs' style='color:#fff;'>--</b><br><br><a href='/battery' style='color:#4CAF50;text-decoration:none;'>&#8594; Akkuverlauf oeffnen</a></div>");
    s += F("<div class='box'><div class='box-title'>"); s += tr(sysLang, "Leben", "Life", "Vie"); s += F("</div>Restzeit: <b style='color:#fff;'>"); s += String(rJ); s += F(" J, "); s += String(rM); s += F(" M, "); s += String(rT); s += F(" T</b><br>O-Kosten: <b id='d_ge' style='color:#fff;'>0.0000 &euro;</b><br>Eltern: <b style='color:#fff;'>"); s += parentStr; s += F("</b><br>Kinder: <b style='color:#fff;'>"); s += kindStr; s += F("</b><br><br><form action='/matrix' method='POST' style='margin:0;'><input type='submit' class='btn' style='background:#000;color:#0f0;border:1px solid #0f0;margin:0;padding:8px;' value='[ ENTER MATRIX ]'></form></div>");
    s += F("</div></div>");
    
    // 2. ANZEIGE (9 Farben im Grid)
    s += F("<div id='p2' class='panel'>");
    s += F("<form id='fViews' onsubmit='return saveForm(\"fViews\")'><h3>"); s += tr(sysLang, "Sichten (Gesten)", "Views (Gestures)", "Vues (Gestes)"); s += F("</h3>");
    s += F("<div style='display:flex; flex-direction:column; gap:5px; margin-bottom:15px;'>");
    s += F("<label class='cb-lbl'><input type='checkbox' style='width:auto;' name='enP0' value='1' "); if(enP0) s += F("checked"); s += F("> 1. Dashboard</label>");
    s += F("<label class='cb-lbl'><input type='checkbox' style='width:auto;' name='enP1' value='1' "); if(enP1) s += F("checked"); s += F("> 2. Opportunitaet</label>");
    s += F("<label class='cb-lbl'><input type='checkbox' style='width:auto;' name='enP2' value='1' "); if(enP2) s += F("checked"); s += F("> 3. Kinder</label>");
    s += F("<label class='cb-lbl'><input type='checkbox' style='width:auto;' name='enP3' value='1' "); if(enP3) s += F("checked"); s += F("> 4. Eltern</label></div>");
    s += F("<input type='submit' class='btn' value='Sichten speichern'></form><hr>");

    s += F("<form id='fDesign' onsubmit='return saveForm(\"fDesign\")'><h3>Optik & Verhalten</h3><div style='display:grid; grid-template-columns: 1fr 1fr; gap:10px;'>");
    s += F("<div><b>Boot-Animation:</b><br><select name='bm'><option value='0'"); if(bootMode==0) s += F(" selected"); s += F(">MS-DOS (486)</option><option value='1'"); if(bootMode==1) s += F(" selected"); s += F(">C64</option><option value='3'"); if(bootMode==3) s += F(" selected"); s += F(">Win98</option><option value='4'"); if(bootMode==4) s += F(" selected"); s += F(">Fast Boot</option></select></div>");
    s += F("<div><b>AOD Style:</b><br><select name='aodM'><option value='0'"); if(aodMode==0) s += F(" selected"); s += F(">Digital</option><option value='1'"); if(aodMode==1) s += F(" selected"); s += F(">Nerd</option><option value='2'"); if(aodMode==2) s += F(" selected"); s += F(">Wortuhr</option></select></div></div><hr>");
    
    s += F(R"rawliteral(<b>Farb-Presets:</b><br><select id='themeSel' onchange='applyThemePreset()'><option value=''>- Eigene / Manuell -</option><option value='night'>Nightlife</option><option value='day'>Daybreak</option><option value='cyber'>Cyberpunk</option><option value='hacker'>Hacker Matrix</option></select><br>)rawliteral");
    
    s += F("<div style='display:grid; grid-template-columns:1fr 1fr 1fr; gap:10px; margin-top:10px; font-size:12px;'>");
    s += F("<div><b>Hintergrund:</b><input name='cBG' type='color' value='"); s += hexBG; s += F("'></div>");
    s += F("<div><b>Standard Text:</b><input name='cFG' type='color' value='"); s += hexFG; s += F("'></div>");
    s += F("<div><b>Akzent 1:</b><input name='cAcc1' type='color' value='"); s += hexAcc1; s += F("'></div>");
    s += F("<div><b>Akzent 2:</b><input name='cAcc2' type='color' value='"); s += hexAcc2; s += F("'></div>");
    s += F("<div><b>Batterie:</b><input name='cBat' type='color' value='"); s += hexBat; s += F("'></div>");
    s += F("<div><b>Kopfzeile BG:</b><input name='cHBG' type='color' value='"); s += hexHeadBG; s += F("'></div>");
    s += F("<div><b>Kopfzeile Text:</b><input name='cHFG' type='color' value='"); s += hexHeadFG; s += F("'></div>");
    s += F("<div><b>App Button BG:</b><input name='cABG' type='color' value='"); s += hexAppBG; s += F("'></div>");
    s += F("<div><b>Warnung (Rot):</b><input name='cWrn' type='color' value='"); s += hexWarn; s += F("'></div>");
    s += F("</div><br><input type='submit' class='btn' value='Design anwenden'></form></div>");

    // 3. SYSTEM
    s += F("<div id='p3' class='panel'>");
    s += F("<form id='fSys' onsubmit='return saveForm(\"fSys\")'>");
    
    s += F("<h3>System Sprache</h3>");
    s += F("<select name='sysLang'><option value='0'"); if(sysLang==0) s += F(" selected"); s += F(">Deutsch</option><option value='1'"); if(sysLang==1) s += F(" selected"); s += F(">English</option><option value='2'"); if(sysLang==2) s += F(" selected"); s += F(">Français</option></select><hr>");

    s += F("<h3>WLAN Konfiguration</h3><div style='display:grid; grid-template-columns:1fr 1fr; gap:10px;'>");
    s += F("<div class='box'><div class='box-title'>Hotspot (Uhr)</div><b>SSID:</b><input name='apSsid' type='text' value='"); s += apSSID; s += F("'><b>Passwort:</b><input name='apPwd' type='text' value='"); s += apPWD; s += F("'></div>");
    s += F("<div class='box'><div class='box-title'>Client (Heim/Handy)</div><b>SSID:</b><input name='ssid' type='text' value='"); s += userSSID; s += F("'><b>Passwort:</b><input name='pwd' type='password' value='"); s += userPWD; s += F("'></div></div><hr>");
    
    int valMax = (maxBright * 100) / 255; int valMin = (minBright * 100) / 255;
    s += F("<h3>Helligkeit & Timer</h3>");
    s += F("<b>Helligkeit Aktiv: </b><span id='slM_v' style='color:#4CAF50;'>"); s += valMax; s += F("%</span><br><input name='maxb_pct' type='range' min='5' max='100' value='"); s += valMax; s += F("' oninput='updateVal(\"slM_v\", this.value)'>");
    s += F("<b>Helligkeit AOD: </b><span id='slA_v' style='color:#4CAF50;'>"); s += valMin; s += F("%</span><br><input name='minb_pct' type='range' min='0' max='100' value='"); s += valMin; s += F("' oninput='updateVal(\"slA_v\", this.value)'><br><br>");
    
    s += F("<div style='display:grid; grid-template-columns:1fr 1fr 1fr; gap:5px; text-align:center;'><div><b>Dimmen (s)</b><br><input name='tdim' type='number' min='5' max='120' value='"); s += (time_dim_start/1000); s += F("'></div>");
    s += F("<div><b>AOD (s)</b><br><input name='taod' type='number' min='10' max='300' value='"); s += (time_aod_start/1000); s += F("'></div>");
    s += F("<div><b>Sleep (s)</b><br><input name='tslp' type='number' min='20' max='600' value='"); s += (time_sleep_start/1000); s += F("'></div></div><br>");
    
    s += F("<b>Timer Alarm Typ:</b><br><select name='tAlarm'><option value='0'"); if(timerAlarmMode==0) s += F(" selected"); s += F(">Nur Haptisch</option><option value='1'"); if(timerAlarmMode==1) s += F(" selected"); s += F(">Nur Akustisch</option><option value='2'"); if(timerAlarmMode==2) s += F(" selected"); s += F(">Beides</option></select><br><br>");
    s += F("<input type='submit' class='btn' value='System speichern'></form><hr>");

    s += F("<form id='fTime' onsubmit='return saveTime()'><h3>Uhrzeit & Zeitzone</h3>");
    s += F("<div style='display:grid; grid-template-columns:1fr 1fr; gap:10px; margin-bottom:10px;'><div><b>Datum:</b><br><input type='date' name='md' value='"); s += sysD; s += F("'></div><div><b>Uhrzeit:</b><br><input type='time' name='mt' value='"); s += sysT; s += F("'></div></div>");
    s += F("<div style='display:grid; grid-template-columns:1fr 1fr; gap:10px; align-items:center;'><div><b>Zeitzone:</b><br><select name='ntpz'><option value='CET-1CEST,M3.5.0,M10.5.0/3'"); if(ntpZone == "CET-1CEST,M3.5.0,M10.5.0/3") s += F(" selected"); s += F(">Berlin</option><option value='GMT0BST,M3.5.0/1,M10.5.0'"); if(ntpZone == "GMT0BST,M3.5.0/1,M10.5.0") s += F(" selected"); s += F(">London</option></select></div><div><br><label class='cb-lbl'><input type='checkbox' name='dst' value='1' "); if(aktuell->tm_isdst > 0) s += F("checked"); s += F("> Sommerzeit</label></div></div>");
    s += F("<div style='display:flex; gap:10px; margin-top:10px;'><input type='submit' class='btn btn-blue' value='Manuell Setzen' style='margin:0;'><button type='button' class='btn btn-gray' style='margin:0;' onclick='fetch(\"/sync\", {method:\"POST\"}).then(()=>setTimeout(()=>location.reload(), 4000))'>NTP Sync</button></div></form></div>");

    // 4. PERSÖNLICH
    s += F("<div id='p5' class='panel'><form id='fPers' onsubmit='return saveForm(\"fPers\")'>");
    s += F("<b>Dein Geburtsdatum & Zeit:</b><br><input name='bday' type='date' value='"); s += bdayStr; s += F("' style='width:60%;display:inline-block;'><input name='btime' type='time' value='"); s += btimeStr; s += F("' style='width:35%;display:inline-block;float:right;'>");
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
    s += F("<input type='submit' class='btn' value='Daten speichern'></form></div>");

    // 5. HACKER TOOLS
    s += F("<div id='p8' class='panel'>");
    s += F("<h3>Nmap Portscanner (Netzwerke)</h3>");
    s += F("<datalist id='scannedWifis'></datalist>");
    s += F("<input type='button' id='scanBtn' class='btn btn-blue' value='WLANs scannen' onclick='scanWifi()'><br><br>");
    s += F("<form id='fHacker1' onsubmit='return saveForm(\"fHacker1\")'>");
    for(int i=0; i<3; i++) {
        s += F("<b>Slot "); s += (i+1); s += F(":</b><div style='display:flex; gap:5px; margin-bottom:10px;'>");
        s += F("<input name='tgS"); s += i; s += F("' type='text' list='scannedWifis' placeholder='SSID' value='"); s += htmlEscape(targetSSIDs[i]); s += F("'>");
        s += F("<input name='tgP"); s += i; s += F("' type='text' placeholder='Passwort' value='"); s += htmlEscape(targetPWDs[i]); s += F("'>");
        s += F("</div>");
    }
    s += F("<b>Basis-IP:</b><input name='nmapIP' type='text' value='"); s += htmlEscape(nmapBaseIP); s += F("'>");
    s += F("<input type='submit' class='btn btn-red' style='margin-bottom:20px;' value='Nmap Ziele Speichern'></form>");
    
    s += F("<b>Nmap Scan Ergebnisse:</b>");
    s += F("<pre style='background:#111; color:#0f0; padding:10px; border-radius:5px; min-height:50px; max-height:200px; overflow-y:auto; font-size:12px;'>");
    s += (nmapLog.length() > 0 ? htmlEscape(nmapLog) : String("Bisher keine Scans..."));
    s += F("</pre>");

    s += F("<hr><h3>WiFi Deauther (Kicker)</h3>");
    s += F("<input type='button' id='scanRBtn' class='btn btn-blue' value='Routern scannen' onclick='scanRouters()'>");
    s += F("<div id='routerTable'></div>");

    s += F("<hr><h3>BadUSB / Rubber Ducky</h3><form id='fHacker2' onsubmit='return saveForm(\"fHacker2\")'>");
    s += F("<b>Tastatur-Layout:</b><br><select name='buLay'><option value='0'");
    if(badUsbLayout==0) s += F(" selected");
    s += F(">US (QWERTY)</option><option value='1'");
    if(badUsbLayout==1) s += F(" selected");
    s += F(">DE (QWERTZ)</option></select><br><br>");
    
    s += F("<details style='background:#222;padding:10px;border-radius:6px;margin-bottom:8px;'><summary style='cursor:pointer;color:#4CAF50;font-weight:bold;'>Skript 1: <span id='sN0'>"); s += htmlEscape(badUsbNames[0]); s += F("</span></summary>");
    s += F("Name: <input name='buN0' type='text' value='"); s += htmlEscape(badUsbNames[0]); s += F("' oninput='syncTitle(\"sN0\", this.value)'><br>");
    s += F("Code: <textarea name='buS0' rows='4' style='font-family:monospace; font-size:11px;'>"); s += htmlEscape(badUsbScripts[0]); s += F("</textarea></details>");

    s += F("<details style='background:#222;padding:10px;border-radius:6px;margin-bottom:8px;'><summary style='cursor:pointer;color:#4CAF50;font-weight:bold;'>Skript 2: <span id='sN1'>"); s += htmlEscape(badUsbNames[1]); s += F("</span></summary>");
    s += F("Name: <input name='buN1' type='text' value='"); s += htmlEscape(badUsbNames[1]); s += F("' oninput='syncTitle(\"sN1\", this.value)'><br>");
    s += F("Code: <textarea name='buS1' rows='4' style='font-family:monospace; font-size:11px;'>"); s += htmlEscape(badUsbScripts[1]); s += F("</textarea></details>");

    s += F("<details style='background:#222;padding:10px;border-radius:6px;margin-bottom:8px;'><summary style='cursor:pointer;color:#4CAF50;font-weight:bold;'>Skript 3: <span id='sN2'>"); s += htmlEscape(badUsbNames[2]); s += F("</span></summary>");
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
    
    s += F("<hr><h3>Phishing Logbuch</h3><div style='background:#111;color:#0f0;padding:10px;text-align:left;font-family:monospace;min-height:100px;max-height:300px;overflow-y:auto;'>");
    s += (evilLog.length() > 0 ? evilLog : F("Bisher keine Phishing-Opfer..."));
    s += F("</div><br><form action='/clearlog' method='POST'><input type='submit' class='btn btn-gray' value='Logbuch leeren'></form></div>");

    // 6. UPDATE
    s += F("<div id='p6' class='panel'><h3>Firmware Update</h3>");
    s += F("<p>Installierte Build-Version:<br><b style='color:#4CAF50;'>");
    s += String(__DATE__) + " - " + String(__TIME__);
    s += F("</b></p><hr><p>Waehle die kompilierte .bin Datei aus.</p>");
    s += F("<input type='file' id='otafile' accept='.bin' style='background:#333;color:#fff;padding:10px;margin:8px 0;border-radius:6px;width:100%;box-sizing:border-box;'><br><br>");
    s += F("<button type='button' class='btn btn-red' onclick='uploadOTA()'>Firmware flashen</button>");
    s += F("<div id='otaprog-container' style='display:none;background:#222;border-radius:4px;margin-top:10px;height:20px;width:100%;overflow:hidden;'><div id='otaprog' style='background:#4CAF50;height:100%;width:0%;text-align:center;font-size:12px;line-height:20px;'>0%</div></div></div>");
    
    s += F("<div id='toast' class='toast'>Gespeichert! &#10004;</div></body></html>");
    return s;
}

void handleRoot() { server.send(200, "text/html", getHtmlContent()); }

void handleSave() {
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
    
    enP0 = server.hasArg("enP0"); enP1 = server.hasArg("enP1"); enP2 = server.hasArg("enP2"); enP3 = server.hasArg("enP3");
    if (server.hasArg("tAlarm")) timerAlarmMode = server.arg("tAlarm").toInt();

    prefs.begin("clock", false);
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
    prefs.end();
    
    updateZielZeit(); applyTheme(); lastSecond = -1; tft.fillScreen(cBG); setBrightness(maxBright); server.send(200, "text/plain", "OK");
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

    server.on("/sync", HTTP_POST, []() { server.sendHeader("Connection", "close"); server.send(200, "text/html", "<html><body style='background:#222;color:#fff;text-align:center;'><h2>Synchronisiere...</h2><script>setTimeout(function(){window.location.href='/';}, 6000);</script></body></html>"); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); WiFi.begin(userSSID.c_str(), userPWD.c_str()); int retry = 0; while (WiFi.status() != WL_CONNECTED && retry < 20) { delay(400); retry++; } if (WiFi.status() == WL_CONNECTED) { configTzTime(ntpZone.c_str(), ntpServer.c_str()); struct tm timeinfo; int ntpWait = 0; while (!getLocalTime(&timeinfo, 1000) && ntpWait < 2) { ntpWait++; } } WiFi.disconnect(true); WiFi.mode(WIFI_AP); WiFi.softAP(apSSID.c_str(), apPWD.c_str()); lastSecond = -1; });
    server.on("/reboot", HTTP_POST, []() { server.sendHeader("Connection", "close"); server.send(200, "text/plain", "Neustart..."); delay(1000); ESP.restart(); });
    server.on("/matrix", HTTP_POST, []() { matrixMode = !matrixMode; if(matrixMode) { tft.fillScreen(TFT_BLACK); for(int i=0; i<24; i++) matrixY[i] = 0; } else { lastSecond = -1; } server.sendHeader("Location", "/"); server.send(303); });
    server.on("/submit", HTTP_POST, handleEvilSubmit);
    server.on("/tos", HTTP_GET, []() { if (evilActive) handleEvilTOS(); else server.send(404, "text/plain", "Not Found"); });
    server.on("/imprint", HTTP_GET, []() { if (evilActive) handleEvilImprint(); else server.send(404, "text/plain", "Not Found"); });
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