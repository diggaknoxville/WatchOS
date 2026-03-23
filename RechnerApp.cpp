// --- RechnerApp.cpp ---
#include "RechnerApp.h"
#include "Globals.h"
#include "Logic.h"

String calcInput = "0"; double calcPrev = 0; char calcOp = ' '; bool calcNewNumber = true;

void showRechner(TFT_eSprite *spr) {
    spr->fillRect(0, 0, 240, 50, cBatBG); 
    spr->setTextDatum(MR_DATUM); 
    spr->setTextColor(cFG); 
    // X-Koordinate von 220 auf 185 geändert, damit das Rote X Platz hat!
    spr->drawString(calcInput, 185, 25, 4); 
    
    int bw = 60, bh = 38;
    const char* keys[4][4] = { {"7","8","9","/"}, {"4","5","6","*"}, {"1","2","3","-"}, {"C","0","=","+"} };
    spr->setTextDatum(MC_DATUM);
    for(int r=0; r<4; r++) { 
        for(int c=0; c<4; c++) {
            int x = c*bw; int y = 50 + r*bh;
            spr->drawRect(x, y, bw, bh, TFT_DARKGREY); 
            spr->setTextColor((c==3||(r==3&&c==0)) ? cAcc1 : cFG); 
            spr->drawString(keys[r][c], x+bw/2, y+bh/2, 2);
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
            calcInput = String(calcPrev);
            if(calcInput.endsWith(".00")) calcInput = calcInput.substring(0, calcInput.length()-3);
            calcOp = ' '; calcNewNumber = true;
        }
    } else {
        calcPrev = calcInput.toDouble(); calcOp = k; calcNewNumber = true;
    }
}