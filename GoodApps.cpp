// --- GoodApps.cpp ---
#include "GoodApps.h"
#include "Globals.h"
#include "Display.h"
#include "Logic.h"
#include <Wire.h>
#include <math.h>

// =====================================================================
// Good Apps Menu (Page 22) — Liste aller "guten" Apps
// =====================================================================
void renderGoodMenu(TFT_eSprite *spr) {
    spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Good Apps", 120, 2, 2);
    spr->setTextDatum(MC_DATUM);

    struct AppEntry { const char* label; uint16_t color; };
    AppEntry apps[] = {
        {"Stoppuhr",  cAcc1},       // -> Page 7
        {"Timer",     cAcc2},       // -> Page 8
        {"Dezibel",   TFT_GREEN},   // -> Page 9
        {"Rechner",   cFG},         // -> Page 13
        {"Jiggler",   TFT_CYAN},    // -> Page 14
        {"Wuerfel",   TFT_YELLOW},  // -> Page 24
        {"Waage",     TFT_ORANGE},  // -> Page 25
    };
    int count = 7;

    int bW = 100, bH = 38, gapX = 10, gapY = 4;
    int startY = 22;
    for (int i = 0; i < count; i++) {
        int col = i % 2;
        int row = i / 2;
        int bx = (col == 0) ? 15 : 125;
        int by = startY + row * (bH + gapY);
        spr->fillRoundRect(bx, by, bW, bH, 8, cAppBG);
        spr->setTextColor(apps[i].color);
        spr->drawString(apps[i].label, bx + bW / 2, by + bH / 2, 2);
    }
    // Status-Dots für laufende Apps
    if (swRunning)       spr->fillCircle(15 + bW - 6, startY + 6, 4, TFT_GREEN);
    if (timerRunning)    spr->fillCircle(125 + bW - 6, startY + 6, 4, TFT_GREEN);
    if (jigglerActive)   spr->fillCircle(125 + bW - 6, startY + 2*(bH+gapY) + 6, 4, TFT_GREEN);
}

void handleGoodMenuTouch(int x, int touchY) {
    int bH = 38, gapY = 4, startY = 22;
    int row = (touchY - startY) / (bH + gapY);
    int col = (x < 120) ? 0 : 1;
    int idx = row * 2 + col;

    if (row < 0 || row > 3 || touchY < startY) return;
    if (idx > 6) return;

    int pages[] = {7, 8, 9, 13, 14, 24, 25};
    int targetPage = pages[idx];

    triggerVibration();
    if (targetPage == 9) startMic();
    slideToPage(targetPage, 0, -1);
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

static void bmaReadAccel(float &ax, float &ay, float &az) {
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

// =====================================================================
// Bewegungssensor: Schrittzähler (Software-Pedometer) + Handgelenk-Wake
// =====================================================================
// Nutzt den bereits verbauten BMA423 (siehe bmaReadAccel oben). Das ist ein
// Software-Pedometer per Peak-Erkennung — nicht so exakt wie der interne
// HW-Zähler des BMA423, aber komplett eigenständig und ohne Config-Blob.
// Wird bei jedem Loop aus checkBackgroundTasks() aufgerufen und intern auf
// ~12 Hz begrenzt. Fällt der Sensor aus, liefert er (0,0,0) -> zählt einfach
// nichts (keine Fehlzählungen).
long stepCount = 0;                  // Schritte seit Tagesbeginn
static int stepDay = -1;             // tm_yday des aktuellen Zähltags
static float stepMagAvg = 1.0f;      // gleitender Mittelwert der Beschleunigung (g)
static bool stepArmed = true;        // bereit für die nächste Aufwärtsflanke
static unsigned long lastStepMs = 0;
static unsigned long lastMotionSample = 0;
static unsigned long lastStepPersistMs = 0;
static long lastPersistedSteps = -1;
static float wakeLastAz = 1.0f;

void loadMotionPrefs() {
    prefs.begin("clock", true);
    stepCount = prefs.getInt("steps", 0);
    stepDay   = prefs.getInt("stepDay", -1);
    prefs.end();
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
        if (lt->tm_year + 1900 > 2020) { stepDay = lt->tm_yday; stepCount = 0; }
        else if (stepDay == -1) { stepDay = lt->tm_yday; }
    }

    // --- Schritt-Erkennung: Peak über dynamischem Schwellwert ---
    stepMagAvg = stepMagAvg * 0.9f + mag * 0.1f; // Schwerkraft / DC nachführen
    float dyn = mag - stepMagAvg;
    const float STEP_HI = 0.13f; // g über dem Mittel = Schritt-Impuls
    const float STEP_LO = 0.04f;
    if (stepArmed && dyn > STEP_HI && (now - lastStepMs) > 280) {
        stepCount++; lastStepMs = now; stepArmed = false;
        if (currentPage == 10) lastSecond = -1; // Statistikseite live halten
    } else if (!stepArmed && dyn < STEP_LO) {
        stepArmed = true; // Flanke abgeschlossen -> bereit für nächsten Schritt
    }

    // --- Handgelenk-Wake: NUR aus dem AOD (State 3), bewusst konservativ ---
    // Klare Aufwärtsdrehung des Displays (az springt Richtung +1g). Aus dem
    // echten Tiefschlaf (State 4) wird bewusst NICHT geweckt, um Fehlauslöser
    // teuer zu vermeiden. Schwellwerte ggf. am Gerät nachjustieren.
    if (powerState == 3 && wakeLastAz < 0.35f && az > 0.75f) {
        lastTouchTime = now; // weckt in den aktiven Zustand
        lastSecond = -1;
    }
    wakeLastAz = az;

    // --- Schritte gelegentlich sichern (alle 3 min, nur bei Änderung) ---
    if (stepCount != lastPersistedSteps && (now - lastStepPersistMs) > 180000UL) {
        lastStepPersistMs = now;
        lastPersistedSteps = stepCount;
        prefs.begin("clock", false);
        prefs.putInt("steps", (int)stepCount);
        prefs.putInt("stepDay", stepDay);
        prefs.end();
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
