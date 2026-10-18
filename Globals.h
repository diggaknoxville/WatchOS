// --- Globals.h ---
#pragma once
#include <Arduino.h>
#include <vector>
#include <XPowersLib.h>        
#include <TouchDrvFT6X36.hpp>  
#include <SensorDRV2605.hpp>   
#include <Preferences.h>
#include <time.h>
#include <USB.h>
#include <USBHIDMouse.h>
#include <USBHIDKeyboard.h> 

#define MAX_HISTORY 200
#define WATCHOS_VERSION "1.3.1"

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  #define INIT_BACKLIGHT_PWM(pin) do { ledcAttach(pin, 5000, 8); } while(0)
  #define WRITE_BACKLIGHT_PWM(pin, val) do { ledcWrite(pin, val); } while(0)
#else
  #define INIT_BACKLIGHT_PWM(pin) do { ledcSetup(0, 5000, 8); ledcAttachPin(pin, 0); } while(0)
  #define WRITE_BACKLIGHT_PWM(pin, val) do { ledcWrite(0, val); } while(0)
#endif

extern XPowersAXP2101 pmu;
extern TouchDrvFT6X36 touch;
extern SensorDRV2605 drv;
extern Preferences prefs;
extern USBHIDMouse Mouse;
extern USBHIDKeyboard Keyboard; 

extern String userSSID, userPWD, apSSID, apPWD, ntpServer, ntpZone;
extern bool wlanActive, btActive, bleInitDone;
extern bool wlanClientMode;
extern String wlanIP;

extern int gTag, gMonat, gJahr, gStunde, gMinute, zielAlter;
extern float netSalary, workHoursPerDay;
extern int partTimePct, vacationDays, sickDays, workDaysYear;
extern String child1Name, child1Date, child1Time;
extern String child2Name, child2Date, child2Time;
extern String child3Name, child3Date, child3Time;
extern String parentName, parentDate, parentTime; // Mama
extern String papaName, papaDate, papaTime;
extern String brandName;

#define SYS_HIST_MAX 288
extern float sysHistFreq[SYS_HIST_MAX], sysHistTemp[SYS_HIST_MAX];
extern int sysHistCount;
extern unsigned long lastSysHistTime;

extern String hexBG, hexFG, hexAcc1, hexAcc2, hexBat, hexHeadBG, hexHeadFG, hexAppBG, hexWarn;
extern uint16_t cBG, cFG, cAcc1, cAcc2, cBatBG, cHeadBG, cHeadFG, cAppBG, cWarn;
extern int sysLang; // NEU: 0=DE, 1=EN, 2=FR

extern unsigned long time_dim_start, time_aod_start, time_sleep_start;
extern int bootMode, aodMode, maxBright, minBright;
extern bool aodActive, vibActive;
extern int powerState; 
extern bool enP0, enP1, enP2, enP3; 
extern int timerAlarmMode; 

extern struct tm zielZeit;
extern int currentPage, mainPage, lastSecond;
extern const char* wochentage[];

// Semantische Seiten-IDs (vermeidet fehleranfällige Magic Numbers)
namespace Pages {
    constexpr int LifeGoal      = 0;
    constexpr int Opportunity   = 1;
    constexpr int Children      = 2;
    constexpr int Parents       = 3;
    constexpr int QuickSettings = 5;
    constexpr int AppLauncher   = 6;
    constexpr int AppDrawer     = 6;
    constexpr int Stopwatch     = 7;
    constexpr int Timer         = 8;
    constexpr int Decibel       = 9;
    constexpr int BatteryStats  = 10;
    constexpr int Flashlight    = 11;
    constexpr int EvilPortal    = 12;
    constexpr int Calculator    = 13;
    constexpr int MouseJiggler  = 14;
    constexpr int BadUSB        = 15;
    constexpr int WifiSpam      = 16;
    constexpr int BleSpam       = 17;
    constexpr int Nmap          = 18;
    constexpr int Deauth        = 19;
    constexpr int ProbeMonitor  = 20;
    constexpr int BleScanner    = 21;
    constexpr int GoodAppsMenu  = 22;
    constexpr int BadAppsMenu   = 23;
    constexpr int Dice          = 24;
    constexpr int Level         = 25;
    constexpr int TotpAuth      = 26;
    constexpr int NotificationCenter = 27;
    constexpr int LoraMessenger = 28;
}

extern int currentTotpIdx;
extern int notifScrollOffset;
extern int loraTabMode;

extern bool matrixMode;
extern int matrixY[24]; 
extern bool micActive;
extern double debug_rms; 

extern unsigned long swStartTime, swElapsed, timerLastTick;
extern bool swRunning, timerRunning;
extern long timerRemaining;
extern int t1, t2, t3; 

// Stoppuhr Rundenzeiten
#define MAX_LAPS 10
extern long swLaps[MAX_LAPS];
extern int swLapCount;

extern String toastMsg;
extern unsigned long toastEndTime;
extern uint16_t toastColor; // Toast-Hintergrundfarbe

extern uint16_t batHistoryPct[MAX_HISTORY];
extern int historyCount;
extern unsigned long lastHistoryTime;
extern const unsigned long historyInterval;
extern bool wasCharging;
extern bool hotspotPromptActive;
extern unsigned long hotspotPromptStart;
extern int watchfaceMode;

// --- Watchface- & Karussell-System ---
enum CarouselFaceId {
    FACE_ULTRA_DASH     = 0,   // Ultra Rings Dashboard
    FACE_CHRONO         = 1,   // Swiss Classic Chronograph
    FACE_CASIO          = 2,   // Retro Casio Illuminator
    FACE_LIFETIME       = 3,   // 3D Lebenszeit Countdown
    FACE_BAUHAUS        = 4,   // Bauhaus Minimalist Analog
    FACE_CYBERPUNK      = 5,   // Cyberpunk Matrix Terminal
    FACE_WORDCLOCK      = 6,   // Typografische Wortuhr
    FACE_FINANCE        = 7,   // Finanz & Opportunitaet
    FACE_CHILDREN       = 8,   // Kinder-Zeitleiste
    FACE_PARENTS        = 9,   // Generationen & Eltern
    FACE_PILOT_FLIEGER  = 10,  // Flieger B-Uhr Aviator
    FACE_DIVER_PRO      = 11,  // Deep Sea Submariner Diver
    FACE_NEON_HORIZON   = 12,  // Synthwave 80s Sunset Grid
    FACE_RADAR_TACTICAL = 13,  // Tactical Military Sonar Radar
    FACE_ASTRO_ORBIT    = 14,  // Celestial Planetary Orbit & Lunar Phase
    FACE_MINIMAL_TYPO   = 15   // Swiss Bold Modern Typographic
};
const int NUM_CAROUSEL_FACES = 16;

extern std::vector<int> carouselOrder;
extern int carouselIndex;
extern String carouselOrderStr;

bool isCarouselPage(int page);
void rebuildCarouselOrder(const String &orderStr);

extern String phoneMac;
extern String phoneName;
extern bool phoneConnected;

// Adaptive Akku-Restdauer: gelernte %/h-Raten je Modus (EMA), persistiert in NVS.
// battRateCharging ist eine Ladegeschwindigkeit (Gewinn), die anderen beiden Entladeraten (Verlust).
extern float battRateNormal, battRateHacking, battRateCharging;
extern uint16_t battSamplesNormal, battSamplesHacking, battSamplesCharging;
extern int stepGoal;

// Dekodiert Modus/Prozent aus einem batHistoryPct-Rohwert (0-100=Normal, 101-200=Hacking, 201-300=Laden).
static inline int battRawMode(uint16_t raw) { return raw > 200 ? 2 : (raw > 100 ? 1 : 0); }
static inline int battRawPct(uint16_t raw) {
    if (raw == 0) return 0;
    int pct = (raw > 200) ? ((int)raw - 200) : ((raw > 100) ? ((int)raw - 100) : (int)raw);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}
static inline int battLearnConfidencePct(uint16_t samples) {
    int c = 20 + (int)samples * 3;
    return c > 97 ? 97 : c;
}

extern unsigned long lastTouchTime, lastTapTime;
extern int16_t startX, startY, lastX, lastY;
extern bool isSwiping;

extern bool jigglerActive;
extern bool usbHidActive;
extern String usbModeSource;
extern bool usbModeDiagPending;

// Security & Captive Portal Telemetrie
extern bool portalActive;
extern String portalLog;
extern String portalSSID; 
extern String portalHTML; 
extern const char* defaultEvilHTML;
#define defaultPortalHTML defaultEvilHTML
#define evilActive portalActive
#define evilLog portalLog
#define evilSSID portalSSID
#define evilHTML portalHTML

extern bool duckyActive;
extern String duckyNames[3];
extern String duckyScripts[3];
extern int currentDuckyScript;
extern int duckyLayout;
#define badUsbActive duckyActive
#define badUsbNames duckyNames
#define badUsbScripts duckyScripts
#define currentBadUsbScript currentDuckyScript
#define badUsbLayout duckyLayout

extern bool wifiSpamActive;
extern bool bleSpamActive; 
extern String spamSSIDs;

extern String targetSSIDs[3];
extern String targetPWDs[3];
extern int currentTargetNmap;

extern bool nmapActive;
extern int nmapState;
extern int nmapIP;
extern String nmapLog;
extern String nmapBaseIP;

extern bool deauthActive;
extern int deauthState;
extern int currentDeauthTarget;
extern uint8_t deauthMac[6];
extern bool isScanningAir;
extern int numScannedNetworks;
extern String scannedSSIDs[15];
extern String scannedMACs[15];

// Probe Request Monitor (Page 20)
#define MAX_PROBES 20
extern bool probeMonActive;
extern int probeCount;
extern String probeMacs[MAX_PROBES];
extern String probeSSIDs[MAX_PROBES];
extern int probeRSSI[MAX_PROBES];
extern int probeScrollOffset;

// BLE Scanner (Page 21)
#define MAX_BLE_RESULTS 20
extern bool bleScanActive;
extern int bleScanCount;
extern String bleScanNames[MAX_BLE_RESULTS];
extern String bleScanMacs[MAX_BLE_RESULTS];
extern int bleScanRssi[MAX_BLE_RESULTS];
extern int bleScanScrollOffset;

// Multi-Port Nmap
extern String nmapPorts;
extern int nmapCurrentPort;

// Web Auth
extern String webUser;
extern String webPass;
extern bool webAuthEnabled;

// Haptic Config
extern int hapticWaveform;

// Eltern- / Kindersicht Toggle (Page 3)
extern int parentViewMode;

// FFat (FAT-Dateisystem)
extern bool littleFsReady;

// Würfel App (Page 24)
extern int diceType;       // 0=d4, 1=d6, 2=d8, 3=d10, 4=d12, 5=d20, 6=d100, 7=Münze
extern int diceResult;
extern bool diceRolling;

// Bewegungssensor (BMA423): Schrittzähler + Handgelenk-Wake
extern long stepCount;     // Schritte seit Tagesbeginn
extern int stepHourly[24]; // Schritte je Stunde des laufenden Tages (fuers Web-Dashboard)

// Wasserwaage App (Page 25)
// (nutzt BMA423 Accelerometer - keine Extra-Globals nötig)

// AOD Manager (ehemals Watchface Designer)
extern bool wfShowTime, wfShowDate, wfShowBatt, wfShowSteps;
extern int wfTimeY, wfDateY, wfBattY, wfStepsY;
extern int wfTimeX, wfDateX, wfBattX, wfStepsX;
extern int wfTimeSize, wfDateSize;
extern int wfTimeFmt; // 0=digital, 1=7seg
extern float wfTimeScale, wfDateScale, wfBattScale, wfStepsScale; // stufenlos 0.5-3.0
// AOD-Elemente
extern bool wfShowWordClock, wfShowBinClock, wfShowAnalog;
extern int wfWordClockY, wfWordClockX;
extern int wfBinClockY, wfBinClockX;
extern int wfAnalogY, wfAnalogX;
extern float wfWordClockScale, wfBinClockScale, wfAnalogScale;
extern int wfAnalogStyle; // 0=normal, 1=binär Ziffern, 2=hex Ziffern
// Neue AOD-Elemente
extern bool wfShowLine, wfShowSecArc, wfShowWeekday;
extern int wfLineY, wfLineX;
extern int wfSecArcY, wfSecArcX;
extern int wfWeekdayY, wfWeekdayX;
extern float wfLineScale, wfSecArcScale, wfWeekdayScale;
// AOD-eigene Farben (unabhängig vom Watchface-Farbschema)
extern String hexAodFG, hexAodAcc;
extern uint16_t cAodFG, cAodAcc;

// Escaped einen String JSON-sicher.
inline String jsonEscape(const String &in) {
    String out; out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
                else out += c;
        }
    }
    return out;
}

// Escaped einen String HTML-sicher (inkl. einfacher und doppelter Anführungszeichen).
inline String htmlEscape(String str) {
    str.replace("&", "&amp;");
    str.replace("<", "&lt;");
    str.replace(">", "&gt;");
    str.replace("\"", "&quot;");
    str.replace("'", "&#39;");
    return str;
}