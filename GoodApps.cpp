// --- GoodApps.cpp ---
#include "GoodApps.h"
#include "Globals.h"
#include "Display.h"
#include "Logic.h"

// =====================================================================
// Good Apps Menu (Page 22) — Liste aller "guten" Apps
// =====================================================================
void renderGoodMenu(TFT_eSprite *spr) {
    spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Good Apps", 120, 2, 2);
    spr->setTextDatum(MC_DATUM);

    // 7 Apps in 2 Spalten: links (x=60), rechts (x=180)
    struct AppEntry { const char* label; uint16_t color; };
    AppEntry apps[] = {
        {"Stoppuhr",  cAcc1},       // -> Page 7
        {"Timer",     cAcc2},       // -> Page 8
        {"Dezibel",   TFT_GREEN},   // -> Page 9
        {"Rechner",   cFG},         // -> Page 13
        {"Jiggler",   TFT_CYAN},    // -> Page 14
    };
    int count = 5;

    int bW = 100, bH = 42, gapX = 10, gapY = 6;
    int startY = 24;
    for (int i = 0; i < count; i++) {
        int col = i % 2;  // 0=links, 1=rechts
        int row = i / 2;
        int bx = (col == 0) ? 15 : 125;
        int by = startY + row * (bH + gapY);
        spr->fillRoundRect(bx, by, bW, bH, 8, cAppBG);
        spr->setTextColor(apps[i].color);
        spr->drawString(apps[i].label, bx + bW / 2, by + bH / 2, 2);
    }
    // Status-Dots für laufende Apps
    if (swRunning)       spr->fillCircle(15 + bW - 6, startY + 6, 4, TFT_GREEN);  // Stoppuhr
    if (timerRunning)    spr->fillCircle(125 + bW - 6, startY + 6, 4, TFT_GREEN);  // Timer
    if (jigglerActive)   spr->fillCircle(125 + bW - 6, startY + 2*(bH+gapY) + 6, 4, TFT_GREEN); // Jiggler
}

void handleGoodMenuTouch(int x, int touchY) {
    // Grid: 2 Spalten, 4 Reihen
    // Cols: links < 120, rechts >= 120
    // Rows: App0/1 row0, App2/3 row1, App4/5 row2, App6 row3
    int bH = 42, gapY = 6, startY = 24;
    int row = (touchY - startY) / (bH + gapY);
    int col = (x < 120) ? 0 : 1;
    int idx = row * 2 + col;

    if (row < 0 || row > 2 || touchY < startY) return;
    if (idx > 4) return; // nur 5 Apps

    // Mapping: idx -> page
    int pages[] = {7, 8, 9, 13, 14};
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
            USB.begin(); 
            Mouse.begin();
            showToast("Jiggler gestartet (USB locked)", 2500); 
        } else {
            ESP.restart(); 
        }
    }
}
