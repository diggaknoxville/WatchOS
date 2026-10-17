// --- WatchOS.ino ---
#ifdef ARDUINO_USB_CDC_ON_BOOT
#undef ARDUINO_USB_CDC_ON_BOOT
#endif
#define ARDUINO_USB_CDC_ON_BOOT 1

#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <FFat.h>
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
#include "GoodApps.h" 
#include "BadApps.h" 
#include "TotpAuth.h"
#include "NotificationManager.h"
#include "SystemEvents.h" 
#include "LoraManager.h" 
#include "AppManager.h" 

// WICHTIG: Arduino Loop Task Stack von 8KB auf 16KB erhöhen!
// Ohne das gibt es Stack Overflow bei slideToPage -> renderPage
SET_LOOP_TASK_STACK_SIZE(16384);

XPowersAXP2101 pmu; TouchDrvFT6X36 touch; SensorDRV2605 drv; TFT_eSPI tft = TFT_eSPI(); TFT_eSprite cdSprite = TFT_eSprite(&tft); 
DNSServer dnsServer; WebServer server(80); Preferences prefs; USBHIDMouse Mouse;

String userSSID = "Skynet", userPWD = "1nt3rn3t!"; 
String apSSID = "WatchOS", apPWD = "12345678";
int gTag = 1, gMonat = 1, gJahr = 1990, gStunde = 12, gMinute = 0, zielAlter = 90; 
float netSalary = 2000.00, workHoursPerDay = 8.0; 
int partTimePct = 100, vacationDays = 30, sickDays = 0, workDaysYear = 250;

String child1Name = "Kind 1", child1Date = "2020-01-01", child1Time = "12:00"; 
String child2Name = "", child2Date = "", child2Time = "12:00"; 
String child3Name = "", child3Date = "", child3Time = "12:00";
String parentName = "Mama", parentDate = "1960-01-01", parentTime = "12:00";
String papaName = "Papa", papaDate = "1960-01-01", papaTime = "12:00";
String brandName = "WatchOS";
float sysHistFreq[SYS_HIST_MAX] = {0}, sysHistTemp[SYS_HIST_MAX] = {0};
int sysHistCount = 0;
unsigned long lastSysHistTime = 0;

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
long swLaps[MAX_LAPS] = {0}; int swLapCount = 0;
String toastMsg = ""; unsigned long toastEndTime = 0; uint16_t toastColor = 0x2945;
uint16_t batHistoryPct[MAX_HISTORY]; int historyCount = 0; unsigned long lastHistoryTime = 0; const unsigned long historyInterval = 600000;
float battRateNormal = 5.0, battRateHacking = 12.0, battRateCharging = 50.0;
uint16_t battSamplesNormal = 0, battSamplesHacking = 0, battSamplesCharging = 0;
int stepGoal = 8000;
unsigned long lastTouchTime = 0, lastTapTime = 0; int16_t startX = -1, startY = -1, lastX = -1, lastY = -1;
bool isSwiping = false, wasCharging = false, bleInitDone = false; int powerState = 1; 
bool hotspotPromptActive = false; unsigned long hotspotPromptStart = 0; int watchfaceMode = 0; 
String phoneMac = ""; String phoneName = ""; bool phoneConnected = false; 
int loraTabMode = 0; 

bool evilActive = false;
String evilLog = "";

// Probe Request Monitor
bool probeMonActive = false;
int probeCount = 0;
String probeMacs[MAX_PROBES];
String probeSSIDs[MAX_PROBES];
int probeRSSI[MAX_PROBES];
int probeScrollOffset = 0;

// BLE Scanner
bool bleScanActive = false;
int bleScanCount = 0;
String bleScanNames[MAX_BLE_RESULTS];
String bleScanMacs[MAX_BLE_RESULTS];
int bleScanRssi[MAX_BLE_RESULTS];
int bleScanScrollOffset = 0;

// Multi-Port Nmap
String nmapPorts = "80,443,22,8080";
int nmapCurrentPort = 0;

// Web Auth
String webUser = "admin";
String webPass = "";
bool webAuthEnabled = false;

// Haptic Config
int hapticWaveform = 1;

// FFat (FAT-Dateisystem auf ffat-Partition)
bool littleFsReady = false;

int currentTotpIdx = 0;
int notifScrollOffset = 0;

void setup() {
    Serial.begin(115200); delay(200);
    Serial.println("\n\n=== WatchOS " WATCHOS_VERSION " Start ===");
    Serial.printf("Heap: %d  PSRAM: %d  MaxAlloc: %d\n", ESP.getFreeHeap(), ESP.getFreePsram(), ESP.getMaxAllocHeap());
    setCpuFrequencyMhz(240); 
    SystemEvents::init();
    NotificationManager::init();
    TotpAuth::init(); 
    
    // NVS Gesundheitscheck: Prüfe ob Namespace funktioniert
    {
        Preferences testPrefs;
        if (testPrefs.begin("clock", false)) {
            size_t freeE = testPrefs.freeEntries();
            Serial.printf("[NVS] Namespace 'clock' OK, freeEntries=%d\n", freeE);
            if (freeE < 20) {
                Serial.println("[NVS] WARNUNG: NVS fast voll! Weniger als 20 freie Eintraege!");
            }
            // Schreib-/Lese-Test
            testPrefs.putInt("_test", 42);
            int v = testPrefs.getInt("_test", 0);
            testPrefs.remove("_test");
            if (v != 42) {
                Serial.println("[NVS] KRITISCH: NVS Schreib/Lese-Test FEHLGESCHLAGEN!");
                Serial.println("[NVS] Versuche NVS-Namespace zu loeschen und neu zu starten...");
                testPrefs.clear(); // Alle Keys im Namespace löschen
            } else {
                Serial.println("[NVS] Schreib/Lese-Test OK");
            }
            testPrefs.end();
        } else {
            Serial.println("[NVS] FEHLER: Namespace 'clock' nicht oeffenbar!");
        }
    }
    
    // FFat: Erst ohne Format versuchen, bei Fehler formatieren
    littleFsReady = FFat.begin(false);
    if (!littleFsReady) {
        Serial.println("FFat: Partition nicht lesbar, versuche Format...");
        delay(10); // WDT feed via yield
        littleFsReady = FFat.begin(true); // formatOnFail
        Serial.println(littleFsReady ? "FFat: OK nach Format" : "FFat: FAIL - FS deaktiviert");
    } else {
        Serial.printf("FFat: OK (total=%d, used=%d)\n", FFat.totalBytes(), FFat.usedBytes());
    }
    loadPreferences();
    if (ntpZone.length() == 0) ntpZone = "CET-1CEST,M3.5.0,M10.5.0/3";
    setenv("TZ", ntpZone.c_str(), 1);
    tzset();
    loadMotionPrefs(); // gespeicherten Schrittzähler-Stand laden
    Serial.printf("Nach loadPreferences: heap=%d cBG=%s cHBG=%s bootM=%d\n", ESP.getFreeHeap(), hexBG.c_str(), hexHeadBG.c_str(), bootMode);
    Serial.printf("[WIFI-DBG] apSSID='%s' apPWD='%s' userSSID='%s'\n", apSSID.c_str(), apPWD.c_str(), userSSID.c_str());
    initHardware();
    
    setupServerRoutes(); // NEU: Routen-Weiche exakt EINMAL laden!
    
    showBootSequence();
    Serial.printf("Nach Boot: heap=%d psram=%d cdSprite=%s\n", ESP.getFreeHeap(), ESP.getFreePsram(), cdSprite.getPointer() ? "OK" : "NULL!");
    applyTheme(); updateZielZeit();
    if (usbModeDiagPending) {
        String usbMode = usbHidActive ? "USB HID" : "USB COM";
        showToast("USB: " + usbMode + " (" + usbModeSource + ")", 3500);
        usbModeDiagPending = false;
    }
    lastTouchTime = millis(); tft.fillScreen(cBG);
    startHotspotAP();
    LoraManager::init();
    AppManager::init();
    Serial.println("=== Setup fertig ===");
}

void loop() {
    unsigned long currentMillis = millis();
    
    // Stack-Monitor (alle 10 Sekunden)
    static unsigned long lastStackCheck = 0;
    if (currentMillis - lastStackCheck > 10000) {
        lastStackCheck = currentMillis;
        Serial.printf("[STACK] frei=%d von %d bytes  Heap=%d PSRAM=%d\n", 
            uxTaskGetStackHighWaterMark(NULL), 16384, ESP.getFreeHeap(), ESP.getFreePsram());
    }
    
    if (wlanActive || evilActive) { dnsServer.processNextRequest(); server.handleClient(); }
    handleWlanIdleTimeout(); // ungenutzten Hotspot automatisch abschalten (Akku)
    checkBackgroundTasks(currentMillis);
    handlePowerManagement(currentMillis);
    LoraManager::loop();
    if (powerState != 4) { handleTouchInput(); }
    if (!matrixMode) {
        if (powerState == 1 || powerState == 2) { renderUI(currentMillis); } else if (powerState == 3) { renderAOD(); }
        AppPackage* curP = AppManager::getAppByPage(currentPage);
        String pid = curP ? (curP->driver.length() > 0 ? curP->driver : curP->id) : "";
        bool isFastPage = (currentPage == Pages::Opportunity || pid == "stopwatch" || pid == "mic" || pid == "level" || (curP && AppManager::hasJsScript(curP)));
        int loopDelayMs = (powerState == 3) ? 500 : (powerState == 2) ? 80 : (isFastPage ? 20 : 50);
        delay(loopDelayMs);
    } else {
        delay(2); 
    }
    yield(); // WDT Feed sicherstellen
}