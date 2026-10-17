// --- GoodApps.cpp ---
#include "GoodApps.h"
#include "Globals.h"
#include "Display.h"
#include "Logic.h"
#include "TotpAuth.h"
#include "NotificationManager.h"
#include "HardwareConfig.h"
#include "AppManager.h"
#include <Wire.h>
#include <math.h>
#include <time.h>

// =====================================================================
// =====================================================================
// Productivity Apps Menu (Page 22) — Modulares Package Menü
// =====================================================================
void renderGoodMenu(TFT_eSprite *spr) {
    auto apps = AppManager::getAppsByCategory(CAT_PRODUCTIVITY, true);
    int total = (int)apps.size();
    int perPage = 8;
    int maxPage = (total > 0) ? ((total - 1) / perPage) : 0;
    if (AppManager::prodMenuPage > maxPage) AppManager::prodMenuPage = maxPage;
    if (AppManager::prodMenuPage < 0) AppManager::prodMenuPage = 0;

    int curPage = AppManager::prodMenuPage;
    int startIdx = curPage * perPage;
    int endIdx = min(startIdx + perPage, total);

    // Header mit dynamischer Seitenzahl falls > 1 Seite
    spr->setTextDatum(TC_DATUM);
    spr->setTextColor(0x07E0);
    if (maxPage > 0) {
        char hBuf[32];
        snprintf(hBuf, sizeof(hBuf), "Productivity (%d/%d)", curPage + 1, maxPage + 1);
        spr->drawString(hBuf, 120, 2, 2);
    } else {
        spr->drawString("Productivity", 120, 2, 2);
    }
    spr->setTextDatum(MC_DATUM);

    int bW = 100, bH = 38, gapY = 4, startY = 22;
    for (int i = startIdx; i < endIdx; i++) {
        int slot = i - startIdx;
        int col = slot % 2;
        int row = slot / 2;
        int bx = (col == 0) ? 15 : 125;
        int by = startY + row * (bH + gapY);

        bool isRunning = (apps[i]->statusFn != nullptr) ? apps[i]->statusFn() : false;

        spr->fillRoundRect(bx, by, bW, bH, 8, cAppBG);
        uint16_t borderCol = isRunning ? apps[i]->color : cBatBG;
        spr->drawRoundRect(bx, by, bW, bH, 8, borderCol);

        // Icon Badge
        if (apps[i]->iconText.length() > 0) {
            spr->fillRoundRect(bx + 4, by + 9, 20, 20, 4, tft.color565(15, 30, 20));
            spr->setTextColor(apps[i]->color);
            spr->setTextDatum(MC_DATUM);
            spr->drawString(apps[i]->iconText, bx + 14, by + 19, 1);
        }

        spr->setTextColor(apps[i]->color);
        spr->setTextDatum(ML_DATUM);
        spr->drawString(apps[i]->name, bx + 28, by + bH / 2, 2);

        if (isRunning) {
            spr->fillCircle(bx + bW - 7, by + 8, 3, TFT_GREEN);
            spr->fillCircle(bx + bW - 7, by + 8, 1, 0xFFFF);
        }
    }

    // Paginierungs-Indikator unten
    if (maxPage > 0) {
        spr->setTextDatum(MC_DATUM);
        spr->setTextColor(0x8BD0);
        spr->drawString("<", 20, 200, 2);
        spr->drawString(">", 220, 200, 2);
        for (int p = 0; p <= maxPage; p++) {
            int dotX = 120 + (p - (float)maxPage / 2.0f) * 14;
            if (p == curPage) spr->fillCircle(dotX, 200, 3, 0x07E0);
            else spr->drawCircle(dotX, 200, 3, 0x52AA);
        }
    }
}

void handleGoodMenuTouch(int x, int touchY) {
    auto apps = AppManager::getAppsByCategory(CAT_PRODUCTIVITY, true);
    int total = (int)apps.size();
    int perPage = 8;
    int maxPage = (total > 0) ? ((total - 1) / perPage) : 0;
    int curPage = AppManager::prodMenuPage;

    // Bottom Navigation Bar Tap (Page < >)
    if (maxPage > 0 && touchY >= 190) {
        if (x < 80 && curPage > 0) {
            AppManager::prodMenuPage--;
            triggerVibration();
            lastSecond = -1;
            return;
        } else if (x > 160 && curPage < maxPage) {
            AppManager::prodMenuPage++;
            triggerVibration();
            lastSecond = -1;
            return;
        }
    }

    int bH = 38, gapY = 4, startY = 22;
    int row = (touchY - startY) / (bH + gapY);
    int col = (x < 120) ? 0 : 1;
    int slot = row * 2 + col;

    if (row < 0 || row > 3 || touchY < startY) return;

    int idx = curPage * perPage + slot;
    if (idx >= total) return;

    AppManager::launchApp(apps[idx]->pageId);
}

// =====================================================================
// Rechner App (Page 13) — aus RechnerApp.cpp übernommen
// =====================================================================
String calcInput = "0"; double calcPrev = 0; char calcOp = ' '; bool calcNewNumber = true;

void showRechner(TFT_eSprite *spr) {
    spr->fillRect(0, 0, 240, 50, cBatBG); 
    spr->setTextDatum(MR_DATUM); 
    spr->setTextColor(cFG); 
    spr->drawString(calcInput, 185, 25, 4); 
    
    int bw = 60, bh = 38;
    const char* keys[4][4] = { {"7","8","9","/"}, {"4","5","6","*"}, {"1","2","3","-"}, {"C","0","=","+"} };
    spr->setTextDatum(MC_DATUM);
    for(int r=0; r<4; r++) { 
        for(int c=0; c<4; c++) {
            int kx = c*bw; int ky = 50 + r*bh;
            spr->drawRect(kx, ky, bw, bh, TFT_DARKGREY); 
            spr->setTextColor((c==3||(r==3&&c==0)) ? cAcc1 : cFG); 
            spr->drawString(keys[r][c], kx+bw/2, ky+bh/2, 2);
        }
    }
}

void handleRechnerTouch(int x, int y) {
    if(y < 50) return;
    int c = x / 60; int r = (y - 50) / 38;
    if(c>=4 || r>=4 || c<0 || r<0) return;
    triggerVibration();
    const char* keys[4][4] = { {"7","8","9","/"}, {"4","5","6","*"}, {"1","2","3","-"}, {"C","0","=","+"} };
    char k = keys[r][c][0];
    
    if(k >= '0' && k <= '9') {
        if(calcNewNumber) { calcInput = String(k); calcNewNumber = false; } 
        else { if(calcInput.length() < 10) calcInput += k; }
    } 
    else if (k == 'C') { calcInput = "0"; calcPrev = 0; calcOp = ' '; calcNewNumber = true; }
    else if (k == '=') {
        if(calcOp != ' ') {
            double cur = calcInput.toDouble();
            if(calcOp=='+') calcPrev += cur; 
            else if(calcOp=='-') calcPrev -= cur; 
            else if(calcOp=='*') calcPrev *= cur; 
            else if(calcOp=='/' && cur!=0) calcPrev /= cur;
            if(calcPrev == (long)calcPrev && abs(calcPrev) < 1e9) {
                calcInput = String((long)calcPrev);
            } else {
                char resBuf[16]; snprintf(resBuf, sizeof(resBuf), "%.6g", calcPrev);
                calcInput = String(resBuf);
            }
            calcOp = ' '; calcNewNumber = true;
        }
    } else {
        calcPrev = calcInput.toDouble(); calcOp = k; calcNewNumber = true;
    }
}

// =====================================================================
// Jiggler App (Page 14) — aus JigglerApp.cpp übernommen
// =====================================================================
bool jigglerActive = false;

void showJiggler(TFT_eSprite *spr) {
    spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Mouse-Jiggler", 120, 10, 2);
    spr->setTextDatum(MC_DATUM); spr->setTextColor(cBatBG); spr->drawString("Haelt den PC wach", 120, 40, 2);
    
    uint16_t bCol = jigglerActive ? TFT_GREEN : TFT_RED;
    spr->fillRoundRect(55, 70, 130, 60, 10, bCol);
    spr->setTextColor(jigglerActive ? TFT_BLACK : TFT_WHITE);
    spr->drawString(jigglerActive ? "AKTIV" : "INAKTIV", 120, 100, 4);
    
    if(jigglerActive) { 
        spr->setTextColor(TFT_RED); 
        spr->drawString("Stoppen startet Uhr neu!", 120, 180, 2); 
    }
}

void handleJigglerTouch(int x, int y) {
    if(y > 70 && y < 130 && x > 55 && x < 185) {
        triggerVibration(); 
        if(!jigglerActive) {
            jigglerActive = true;
            if (!usbHidActive) {
                usbHidActive = true;
                prefs.begin("clock", false); prefs.putBool("usbHid", true); prefs.end();
                showToast("Jiggler: Neustart fuer USB HID", 2000);
                delay(800);
                ESP.restart();
            }
            showToast("Jiggler gestartet", 2500); 
        } else {
            ESP.restart(); 
        }
    }
}

// =====================================================================
// Würfel / Zufallsgenerator App (Page 24)
// =====================================================================
static unsigned long diceAnimStart = 0;
static int diceAnimFrame = 0;

const char* diceTypeNames[] = {"d4", "d6", "d8", "d10", "d12", "d20", "d100", "Muenze"};
const int diceMaxValues[]   = {  4,    6,    8,   10,    12,    20,   100,      2};

void drawDiceFace(TFT_eSprite *spr, int val, int cx, int cy, int size) {
    // Würfel-Körper
    spr->fillRoundRect(cx - size/2, cy - size/2, size, size, 8, TFT_WHITE);
    spr->drawRoundRect(cx - size/2, cy - size/2, size, size, 8, cBatBG);
    
    if (diceType == 7) { // Münze
        spr->fillCircle(cx, cy, size/2 - 4, TFT_YELLOW);
        spr->drawCircle(cx, cy, size/2 - 4, tft.color565(180, 150, 0));
        spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_BLACK);
        spr->drawString(val == 1 ? "Kopf" : "Zahl", cx, cy, 4);
        return;
    }
    
    // Standard-Würfel: Augen für d6, Zahl für andere
    if (diceType == 1 && val <= 6) {
        // Klassische Würfel-Augen
        int r = size / 10; int off = size / 4;
        uint16_t dotCol = TFT_BLACK;
        if (val == 1 || val == 3 || val == 5) spr->fillCircle(cx, cy, r, dotCol);           // Mitte
        if (val >= 2) { spr->fillCircle(cx - off, cy - off, r, dotCol); spr->fillCircle(cx + off, cy + off, r, dotCol); } // Diagonal
        if (val >= 4) { spr->fillCircle(cx + off, cy - off, r, dotCol); spr->fillCircle(cx - off, cy + off, r, dotCol); } // Andere Diagonal
        if (val == 6) { spr->fillCircle(cx - off, cy, r, dotCol); spr->fillCircle(cx + off, cy, r, dotCol); }            // Seiten
    } else {
        // Zahl für d4, d8, d10, d12, d20
        spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_BLACK);
        spr->drawString(String(val), cx, cy, 6);
    }
}

void showDice(TFT_eSprite *spr) {
    spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Wuerfel", 120, 0, 2);
    
    // Typ-Auswahl Pfeile + Name
    spr->fillRoundRect(15, 18, 30, 25, 5, cBatBG); spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_WHITE); spr->drawString("<", 30, 30, 2);
    spr->fillRoundRect(195, 18, 30, 25, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString(">", 210, 30, 2);
    spr->setTextColor(TFT_YELLOW); spr->drawString(diceTypeNames[diceType], 120, 30, 4);
    
    // Würfel anzeigen
    if (diceRolling) {
        // Animation: Zufallswerte schnell durchblättern
        int animVal = random(1, diceMaxValues[diceType] + 1);
        drawDiceFace(spr, animVal, 120, 110, 100);
    } else if (diceResult > 0) {
        drawDiceFace(spr, diceResult, 120, 110, 100);
    } else {
        spr->setTextColor(cBatBG); spr->drawString("Tippen zum", 120, 95, 2);
        spr->drawString("Wuerfeln!", 120, 115, 2);
    }
    
    // Würfeln-Button
    spr->fillRoundRect(55, 170, 130, 30, 8, diceRolling ? TFT_ORANGE : TFT_GREEN);
    spr->setTextColor(TFT_BLACK); spr->drawString("Wuerfeln!", 120, 185, 2);
}

void handleDiceTouch(int x, int y) {
    // Typ-Wechsel Pfeile
    if (y >= 18 && y <= 43) {
        if (x > 10 && x < 60) { diceType--; if(diceType < 0) diceType = 7; triggerVibration(); lastSecond = -1; return; }
        if (x > 180 && x < 230) { diceType++; if(diceType > 7) diceType = 0; triggerVibration(); lastSecond = -1; return; }
    }
    // Würfeln (ganzer mittlerer Bereich oder Button)
    if (y >= 50 && !diceRolling) {
        diceRolling = true;
        diceAnimStart = millis();
        diceAnimFrame = 0;
        triggerVibration();
        lastSecond = -1;
    }
}

void updateDiceAnimation() {
    if (!diceRolling) return;
    unsigned long elapsed = millis() - diceAnimStart;
    if (elapsed > 600) {
        // Animation fertig — Ergebnis würfeln
        diceRolling = false;
        diceResult = random(1, diceMaxValues[diceType] + 1);
        triggerVibration();
        lastSecond = -1;
    } else {
        // Schnelles Umblättern für Animation
        if (millis() - diceAnimStart > diceAnimFrame * 60) {
            diceAnimFrame++;
            lastSecond = -1;
        }
    }
}

// =====================================================================
// Wasserwaage / Neigungsmesser (Page 25)
// =====================================================================
// BMA423 über I2C (Addr 0x19 auf Wire = PMU Bus)
#define BMA423_ADDR 0x19
#define BMA4_ACCEL_X_LSB_ADDR 0x12

static bool bmaOK = false;

static void bmaInit() {
    // Accelerometer aktivieren: Power Control Register
    Wire.beginTransmission(BMA423_ADDR);
    Wire.write(0x7D); Wire.write(0x04); // ACC enable
    Wire.endTransmission();
    delay(5);
    // Accelerometer Range: ±2g (0x00), ODR 100Hz (0x08)  
    Wire.beginTransmission(BMA423_ADDR);
    Wire.write(0x40); Wire.write(0x28); // acc_conf: odr=100Hz, bwp=norm
    Wire.endTransmission();
    Wire.beginTransmission(BMA423_ADDR);
    Wire.write(0x41); Wire.write(0x00); // acc_range: ±2g
    Wire.endTransmission();
    bmaOK = true;
}

void bmaReadAccel(float &ax, float &ay, float &az) {
    if (!bmaOK) bmaInit();
    uint8_t buf[6];
    Wire.beginTransmission(BMA423_ADDR);
    Wire.write(BMA4_ACCEL_X_LSB_ADDR);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)BMA423_ADDR, (uint8_t)6);
    for (int i = 0; i < 6; i++) buf[i] = Wire.available() ? Wire.read() : 0;
    
    int16_t rawX = (int16_t)((buf[1] << 8) | buf[0]) >> 4; // 12-bit
    int16_t rawY = (int16_t)((buf[3] << 8) | buf[2]) >> 4;
    int16_t rawZ = (int16_t)((buf[5] << 8) | buf[4]) >> 4;
    
    // ±2g Range: 1 LSB = 0.98 mg  -> /1024 für g
    ax = rawX / 1024.0;
    ay = rawY / 1024.0;
    az = rawZ / 1024.0;
}

void bmaInvalidate() {
    bmaOK = false;
}

// =====================================================================
// Bewegungssensor: Schrittzähler (Software-Pedometer) + Handgelenk-Wake
// =====================================================================
// Nutzt den bereits verbauten BMA423 (siehe bmaReadAccel oben).
// Schritte und Historie werden im batteriegepufferten RTC-RAM gesichert
// (RTC_DATA_ATTR). Dadurch überstehen sie Resets und Sleep-Phasen, ohne
// den Flash-Speicher durch ständiges NVS-Schreiben zu verschleißen.
RTC_DATA_ATTR long stepCount = 0;                  // Schritte seit Tagesbeginn
RTC_DATA_ATTR int stepHourly[24] = {0};            // Schritte je Stunde des laufenden Tages (fuers Web-Dashboard)
RTC_DATA_ATTR int stepDay = -1;                     // tm_yday des aktuellen Zähltags
RTC_DATA_ATTR bool stepRtcInitialized = false;

static float stepMagAvg = 1.0f;      // gleitender Mittelwert der Beschleunigung (g)
static bool stepArmed = true;        // bereit für die nächste Aufwärtsflanke
static unsigned long lastStepMs = 0;
static unsigned long lastMotionSample = 0;
static unsigned long lastStepPersistMs = 0;
static long lastPersistedSteps = -1;
static float wakeLastAz = 1.0f;

void saveMotionPrefs() {
    lastPersistedSteps = stepCount;
    prefs.begin("clock", false);
    prefs.putInt("steps", (int)stepCount);
    prefs.putInt("stepDay", stepDay);
    prefs.end();
}

void loadMotionPrefs() {
    // Nur bei Kaltstart (RTC-RAM uninitialisiert) aus dem NVS-Flash nachladen
    if (!stepRtcInitialized || stepDay == -1) {
        prefs.begin("clock", true);
        stepCount = prefs.getInt("steps", 0);
        stepDay   = prefs.getInt("stepDay", -1);
        prefs.end();
        stepRtcInitialized = true;
    }
    lastPersistedSteps = stepCount;
}

void updateMotion(unsigned long now) {
    // ~12 Hz reichen für Schritt-/Gestenerkennung und schonen den Akku.
    if (now - lastMotionSample < 80) return;
    lastMotionSample = now;

    float ax, ay, az;
    bmaReadAccel(ax, ay, az);
    float mag = sqrtf(ax * ax + ay * ay + az * az);
    if (mag < 0.05f) return; // Sensor liefert nichts -> ignorieren

    // Tageswechsel: Zähler zurücksetzen (nur bei plausibler Uhrzeit).
    time_t t; time(&t); struct tm *lt = localtime(&t);
    if (lt->tm_yday != stepDay) {
        if (lt->tm_year + 1900 > 2020) {
            saveMotionPrefs(); // Vorigen Tag sichern
            stepDay = lt->tm_yday;
            stepCount = 0;
            for (int i = 0; i < 24; i++) stepHourly[i] = 0;
            saveMotionPrefs();
        }
        else if (stepDay == -1) { stepDay = lt->tm_yday; }
    }

    // --- Schritt-Erkennung: Peak über dynamischem Schwellwert ---
    stepMagAvg = stepMagAvg * 0.9f + mag * 0.1f; // Schwerkraft / DC nachführen
    float dyn = mag - stepMagAvg;
    const float STEP_HI = 0.13f; // g über dem Mittel = Schritt-Impuls
    const float STEP_LO = 0.04f;
    if (stepArmed && dyn > STEP_HI && (now - lastStepMs) > 280) {
        stepCount++; stepHourly[lt->tm_hour]++; lastStepMs = now; stepArmed = false;
        if (currentPage == 10) lastSecond = -1; // Statistikseite live halten
    } else if (!stepArmed && dyn < STEP_LO) {
        stepArmed = true; // Flanke abgeschlossen -> bereit für nächsten Schritt
    }

    // --- Handgelenk-Wake: aus dem AOD (State 3) und Dim (State 2) ---
    // Klare Aufwärtsdrehung des Displays (az springt Richtung +1g).
    if ((powerState == 3 || powerState == 2) && wakeLastAz < 0.35f && az > 0.75f) {
        lastTouchTime = now; // weckt in den aktiven Zustand
        powerState = 1;
        setBrightness(maxBright);
        triggerVibration();
        lastSecond = -1;
    }
    wakeLastAz = az;

    // --- Schritte schonend im Flash sichern (nur alle 60 min statt alle 3 min) ---
    // RTC-RAM hält den Stand sicher im laufenden Betrieb und im Deep-Sleep.
    if (stepCount != lastPersistedSteps && (now - lastStepPersistMs) > 3600000UL) {
        lastStepPersistMs = now;
        saveMotionPrefs();
    }
}


void showLevel(TFT_eSprite *spr) {
    spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Wasserwaage", 120, 0, 2);
    
    float ax, ay, az;
    bmaReadAccel(ax, ay, az);
    
    // Neigungswinkel berechnen
    float pitchRad = atan2(ax, sqrt(ay * ay + az * az));
    float rollRad  = atan2(ay, sqrt(ax * ax + az * az));
    float pitch = pitchRad * 180.0 / 3.14159;
    float roll  = rollRad  * 180.0 / 3.14159;
    
    // Kreisförmige Wasserwaage (Bullseye)
    int cx = 120, cy = 100, outerR = 70;
    spr->drawCircle(cx, cy, outerR, cBatBG);
    spr->drawCircle(cx, cy, outerR/2, cBatBG);
    spr->drawCircle(cx, cy, 3, cBatBG);
    // Fadenkreuz
    spr->drawLine(cx - outerR, cy, cx + outerR, cy, tft.color565(50, 50, 50));
    spr->drawLine(cx, cy - outerR, cx, cy + outerR, tft.color565(50, 50, 50));
    
    // Bubble-Position (invertiert: wenn Uhr nach rechts kippt, geht Blase nach rechts)
    float scale = outerR / 30.0; // ±30° = Randanschlag
    int bx = cx + (int)(roll * scale);
    int by = cy + (int)(pitch * scale);
    // Clamp innerhalb des Kreises
    float dist = sqrt((bx-cx)*(bx-cx) + (by-cy)*(by-cy));
    if (dist > outerR - 8) {
        bx = cx + (int)((bx - cx) * (outerR - 8) / dist);
        by = cy + (int)((by - cy) * (outerR - 8) / dist);
    }
    
    // Bubble (grün wenn fast mittig, gelb wenn leicht, rot wenn stark geneigt)
    float totalAngle = sqrt(pitch*pitch + roll*roll);
    uint16_t bubbleCol = (totalAngle < 2.0) ? TFT_GREEN : (totalAngle < 10.0) ? TFT_YELLOW : TFT_RED;
    spr->fillCircle(bx, by, 8, bubbleCol);
    spr->drawCircle(bx, by, 8, TFT_WHITE);
    
    // Winkelanzeige
    spr->setTextDatum(MC_DATUM);
    char buf[20];
    snprintf(buf, sizeof(buf), "X: %.1f", roll);
    spr->setTextColor(cAcc1); spr->drawString(buf, 60, 185, 2);
    snprintf(buf, sizeof(buf), "Y: %.1f", pitch);
    spr->setTextColor(cAcc2); spr->drawString(buf, 180, 185, 2);
    
    // Level-Indikator Text
    if (totalAngle < 2.0) {
        spr->setTextColor(TFT_GREEN); spr->drawString("EBEN", 120, 195, 2);
    }
}

// =====================================================================
// 2FA Authenticator (Page 26)
// =====================================================================
void showTotpAuth(TFT_eSprite *spr) {
    spr->setTextDatum(TC_DATUM);
    spr->setTextColor(0x07FF);
    spr->drawString("2FA Authenticator", 120, 2, 2);

    int count = TotpAuth::getAccountCount();
    if (count == 0) {
        spr->setTextDatum(MC_DATUM);
        spr->setTextColor(cBatBG);
        spr->drawString("Keine Konten", 120, 90, 4);
        spr->drawString("Im Web-Dashboard", 120, 125, 2);
        spr->drawString("anlegen & verwalten", 120, 145, 2);
        return;
    }

    if (currentTotpIdx >= count) currentTotpIdx = 0;
    if (currentTotpIdx < 0) currentTotpIdx = count - 1;

    TotpAccount acc;
    TotpAuth::getAccount(currentTotpIdx, acc);

    time_t jetzt;
    time(&jetzt);
    uint32_t code = TotpAuth::generateCode(currentTotpIdx, jetzt);
    int remSec = TotpAuth::getRemainingSeconds(jetzt);

    // Account-Titel mit Index-Indikator
    spr->setTextDatum(TC_DATUM);
    spr->setTextColor(TFT_WHITE);
    char accTitle[40];
    snprintf(accTitle, sizeof(accTitle), "< %s (%d/%d) >", acc.name, currentTotpIdx + 1, count);
    spr->drawString(accTitle, 120, 26, 2);

    // 3D Glassmorphic Code Card
    drawGlassCard(spr, 15, 52, 210, 85, 12, 0x07FF, 0x1463, 0x0A22);

    // 6-stelliger Code mit Leerzeichen (z. B. "123 456")
    char codeBuf[16];
    snprintf(codeBuf, sizeof(codeBuf), "%03u %03u", (unsigned int)(code / 1000), (unsigned int)(code % 1000));
    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(0x07FF);
    spr->drawString(codeBuf, 120, 94, 6);

    // Animierter 30s Countdown-Fortschrittsbalken
    int barX = 25, barY = 148, barW = 190, barH = 10;
    spr->fillRoundRect(barX, barY, barW, barH, 4, 0x18C3);
    int fillW = (remSec * barW) / 30;
    uint16_t barCol = (remSec > 12) ? TFT_GREEN : (remSec > 5) ? TFT_YELLOW : TFT_RED;
    if (fillW > 0) {
        spr->fillRoundRect(barX, barY, fillW, barH, 4, barCol);
    }
    spr->drawRoundRect(barX, barY, barW, barH, 4, 0x39E7);

    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(barCol);
    spr->drawString(String(remSec) + "s verbleibend", 120, 170, 2);

    // Hinweis
    spr->setTextColor(0x8BD0);
    spr->drawString("Tap = Naechstes Konto", 120, 192, 1);
}

void handleTotpTouch(int x, int y) {
    int count = TotpAuth::getAccountCount();
    if (count <= 1) return;
    triggerVibration();
    currentTotpIdx = (currentTotpIdx + 1) % count;
    lastSecond = -1;
}

// =====================================================================
// Notification Center (Page 27)
// =====================================================================
void showNotificationCenter(TFT_eSprite *spr) {
    spr->setTextDatum(TC_DATUM);
    spr->setTextColor(TFT_CYAN);
    int total = NotificationManager::getCount();
    spr->drawString("Mitteilungen (" + String(total) + ")", 120, 2, 2);

    if (total == 0) {
        spr->setTextDatum(MC_DATUM);
        spr->setTextColor(cBatBG);
        spr->drawString("Keine neuen", 120, 90, 4);
        spr->drawString("Mitteilungen", 120, 125, 4);
        spr->setTextColor(0x8BD0);
        spr->drawString("API: POST /api/notify", 120, 160, 2);
        return;
    }

    // Zeige bis zu 2 Benachrichtigungen gleichzeitig
    int startIdx = notifScrollOffset;
    if (startIdx >= total) startIdx = total - 1;
    if (startIdx < 0) startIdx = 0;

    int y = 24;
    for (int i = 0; i < 2 && (startIdx + i) < total; i++) {
        WatchNotification n;
        if (NotificationManager::get(startIdx + i, n)) {
            uint16_t border = n.read ? 0x2945 : TFT_CYAN;
            drawGlassCard(spr, 10, y, 220, 68, 8, border, 0x1423, 0x0C12);

            // Title & Timestamp
            spr->setTextDatum(TL_DATUM);
            spr->setTextColor(n.read ? 0xCE79 : TFT_WHITE);
            spr->drawString(n.title, 18, y + 6, 2);

            struct tm *tmInfo = localtime(&n.timestamp);
            char tBuf[10];
            snprintf(tBuf, sizeof(tBuf), "%02d:%02d", tmInfo->tm_hour, tmInfo->tm_min);
            spr->setTextDatum(TR_DATUM);
            spr->setTextColor(0x8BD0);
            spr->drawString(tBuf, 220, y + 6, 1);

            // Message Body
            spr->setTextDatum(TL_DATUM);
            spr->setTextColor(0x8BD0);
            String m = String(n.message);
            if (m.length() > 28) {
                spr->drawString(m.substring(0, 28), 18, y + 26, 1);
                spr->drawString(m.substring(28, 56), 18, y + 38, 1);
            } else {
                spr->drawString(m, 18, y + 26, 2);
            }
            y += 74;
        }
    }

    // Button: "Alle löschen"
    spr->setTextDatum(MC_DATUM);
    spr->fillRoundRect(40, 175, 160, 30, 8, 0x6186);
    spr->drawRoundRect(40, 175, 160, 30, 8, TFT_RED);
    spr->setTextColor(TFT_WHITE);
    spr->drawString("Alle loeschen", 120, 190, 2);
}

void handleNotificationTouch(int x, int y) {
    int total = NotificationManager::getCount();
    if (total == 0) return;

    if (y >= 170 && y <= 215) {
        triggerVibration();
        NotificationManager::clear();
        showToastColor("Mitteilungen geloescht", 2000, 0x4208);
        lastSecond = -1;
        return;
    }

    // Scrollen durch Antippen der Karten
    if (total > 2) {
        triggerVibration();
        notifScrollOffset = (notifScrollOffset + 2) % total;
        lastSecond = -1;
    }
}
