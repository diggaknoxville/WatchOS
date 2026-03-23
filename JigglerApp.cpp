// --- JigglerApp.cpp ---
#include "JigglerApp.h"
#include "Globals.h"
#include "Logic.h"

bool jigglerActive = false;

void showJiggler(TFT_eSprite *spr) {
    spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Mouse-Jiggler", 120, 10, 2);
    spr->setTextDatum(MC_DATUM); spr->setTextColor(cBatBG); spr->drawString("Haelt den PC wach", 120, 40, 2);
    
    uint16_t bCol = jigglerActive ? TFT_GREEN : TFT_RED;
    spr->fillRoundRect(40, 70, 160, 80, 10, bCol);
    spr->setTextColor(jigglerActive ? TFT_BLACK : TFT_WHITE);
    spr->drawString(jigglerActive ? "AKTIV" : "INAKTIV", 120, 110, 4);
    
    if(jigglerActive) { 
        spr->setTextColor(TFT_RED); 
        spr->drawString("Stoppen startet Uhr neu!", 120, 180, 2); 
    }
}

void handleJigglerTouch(int x, int y) {
    if(y > 70 && y < 150 && x > 40 && x < 200) {
        triggerVibration(); 
        if(!jigglerActive) {
            jigglerActive = true;
            USB.begin(); 
            Mouse.begin();
            showToast("Jiggler gestartet (USB locked)", 2500); 
        } else {
            // Einziger Weg, den USB COM Port zuverlässig an Windows zurückzugeben!
            ESP.restart(); 
        }
    }
}