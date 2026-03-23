// --- WatchOS.ino ---
#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <WebServer.h>         
#include <DNSServer.h>         
#include <Preferences.h>       
#include <Update.h>            
#include "time.h"
#include <math.h> 
#include <USB.h>
#include <USBHIDMouse.h>
#include <ESPmDNS.h>

#include "Globals.h"
#include "HardwareConfig.h"
#include "Display.h"
#include "WebManager.h"
#include "Logic.h"
#include "UpdateOTA.h" 
#include "RechnerApp.h" 
#include "JigglerApp.h" 
#include "EvilPortalApp.h" 

XPowersAXP2101 pmu; TouchDrvFT6X36 touch; SensorDRV2605 drv; TFT_eSPI tft = TFT_eSPI(); TFT_eSprite cdSprite = TFT_eSprite(&tft); 
DNSServer dnsServer; WebServer server(80); Preferences prefs; USBHIDMouse Mouse;

String userSSID = "WLAN_NAME", userPWD = "wlan_password"; 
String apSSID = "WatchOS", apPWD = "password";
int gTag = 1, gMonat = 1, gJahr = 1990, gStunde = 12, gMinute = 0, zielAlter = 90; 
float netSalary = 2000.00, workHoursPerDay = 8.0; 
int partTimePct = 100, vacationDays = 30, sickDays = 0, workDaysYear = 250;

String child1Name = "Kind 1", child1Date = "2020-01-01", child1Time = "12:00"; 
String child2Name = "", child2Date = "", child2Time = "12:00"; 
String child3Name = "", child3Date = "", child3Time = "12:00";
String parentName = "Elternteil", parentDate = "1960-01-01", parentTime = "12:00"; 

String hexBG = "#000000", hexFG = "#ffffff", hexAcc1 = "#ffea00", hexAcc2 = "#00ff00", hexBat = "#333333"; 
uint16_t cBG, cFG, cAcc1, cAcc2, cBatBG;

unsigned long time_dim_start = 10000, time_aod_start = 20000, time_sleep_start = 40000;  
int bootMode = 0, aodMode = 0; int maxBright = 255, minBright = 10;
bool aodActive = false, wlanActive = false, btActive = false, vibActive = true; 
String ntpServer = "pool.ntp.org", ntpZone = "CET-1CEST,M3.5.0,M10.5.0/3"; 

struct tm zielZeit; int currentPage = 1, mainPage = 1, lastSecond = -1; 
const char* wochentage[] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
bool matrixMode = false; int matrixY[24] = {0}; bool micActive = false; double debug_rms = 0.0; 
unsigned long swStartTime = 0, swElapsed = 0, timerLastTick = 0; bool swRunning = false, timerRunning = false; long timerRemaining = 0; int t1 = 3, t2 = 5, t3 = 10; 
String toastMsg = ""; unsigned long toastEndTime = 0;
uint8_t batHistoryPct[MAX_HISTORY]; int historyCount = 0; unsigned long lastHistoryTime = 0; const unsigned long historyInterval = 600000; 
unsigned long lastTouchTime = 0, lastTapTime = 0; int16_t startX = -1, startY = -1, lastX = -1, lastY = -1;
bool isSwiping = false, wasCharging = false, bleInitDone = false; int powerState = 1; 

bool evilActive = false;
String evilLog = "";

void setup() {
    Serial.begin(115200); setCpuFrequencyMhz(240); 
    loadPreferences();
    initHardware();
    
    setupServerRoutes(); // NEU: Routen-Weiche exakt EINMAL laden!
    
    showBootSequence();
    applyTheme(); updateZielZeit();
    lastTouchTime = millis(); tft.fillScreen(cBG);
}

void loop() {
    unsigned long currentMillis = millis();
    if (wlanActive || evilActive) { dnsServer.processNextRequest(); server.handleClient(); }
    checkBackgroundTasks(currentMillis);
    handlePowerManagement(currentMillis);
    if (powerState != 4) { handleTouchInput(); }
    if (!matrixMode) {
        if (powerState == 1 || powerState == 2) { renderUI(currentMillis); } else if (powerState == 3) { renderAOD(); }
        delay(20); 
    } else {
        delay(2); 
    }
}