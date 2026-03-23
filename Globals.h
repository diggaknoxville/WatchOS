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

extern String toastMsg;
extern unsigned long toastEndTime;

extern uint8_t batHistoryPct[MAX_HISTORY];
extern int historyCount;
extern unsigned long lastHistoryTime;
extern const unsigned long historyInterval; 
extern bool wasCharging;

extern unsigned long lastTouchTime, lastTapTime;
extern int16_t startX, startY, lastX, lastY;
extern bool isSwiping;

extern bool jigglerActive;
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