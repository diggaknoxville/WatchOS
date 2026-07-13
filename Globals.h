// --- Globals.h ---
#pragma once
#include <Arduino.h>
#include <XPowersLib.h>        
#include <TouchDrvFT6X36.hpp>  
#include <SensorDRV2605.hpp>   
#include <Preferences.h>
#include <time.h>
#include <USB.h>
#include <USBHIDMouse.h>
#include <USBHIDKeyboard.h> 

#define MAX_HISTORY 200
#define WATCHOS_VERSION "1.3.0"

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
extern String parentName, parentDate, parentTime;

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

extern unsigned long lastTouchTime, lastTapTime;
extern int16_t startX, startY, lastX, lastY;
extern bool isSwiping;

extern bool jigglerActive;
extern bool usbHidActive;
extern String usbModeSource;
extern bool usbModeDiagPending;
extern bool evilActive;
extern String evilLog;
extern String evilSSID; 
extern String evilHTML; 

extern bool badUsbActive;
extern bool wifiSpamActive;
extern bool bleSpamActive; 

extern String badUsbNames[3];
extern String badUsbScripts[3];
extern int currentBadUsbScript;

extern String spamSSIDs;
extern int badUsbLayout; 

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

// FFat (FAT-Dateisystem)
extern bool littleFsReady;

// Würfel App (Page 24)
extern int diceType;       // 0=d4, 1=d6, 2=d8, 3=d10, 4=d12, 5=d20, 6=d100, 7=Münze
extern int diceResult;
extern bool diceRolling;

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