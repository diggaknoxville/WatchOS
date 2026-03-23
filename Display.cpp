// --- Display.cpp ---
#include "Display.h"
#include "Globals.h" 
#include "RechnerApp.h" 
#include "JigglerApp.h" 
#include "EvilPortalApp.h"
#include "HardwareConfig.h" 
#include <WiFi.h>

#define HEAD_H 34
extern int getDecibels();

uint16_t hex2rgb565(String hex) {
    if(hex.length() < 7) return 0; long rgb = strtol(hex.substring(1).c_str(), NULL, 16);
    return tft.color565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

void applyTheme() { 
    cBG = hex2rgb565(hexBG); cFG = hex2rgb565(hexFG); cAcc1 = hex2rgb565(hexAcc1); cAcc2 = hex2rgb565(hexAcc2); cBatBG = hex2rgb565(hexBat); 
    cHeadBG = hex2rgb565(hexHeadBG); cHeadFG = hex2rgb565(hexHeadFG); cAppBG = hex2rgb565(hexAppBG); cWarn = hex2rgb565(hexWarn);
}
void setBrightness(int b) { ledcWrite(45, b); } 

time_t stringToTime(String dateStr, String timeStr) {
    if(dateStr.length() != 10) return 0; struct tm t; memset(&t, 0, sizeof(struct tm));
    t.tm_year = dateStr.substring(0, 4).toInt() - 1900; t.tm_mon = dateStr.substring(5, 7).toInt() - 1; t.tm_mday = dateStr.substring(8, 10).toInt(); 
    if(timeStr.length() == 5) { t.tm_hour = timeStr.substring(0,2).toInt(); t.tm_min = timeStr.substring(3,5).toInt(); }
    return mktime(&t);
}

void draw7Segment(TFT_eSprite &spr, int x, int y, int num, int h, int w, uint16_t color) {
    const uint8_t segs[] = {0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F}; uint8_t bits = segs[num % 10]; int d = h / 10; if (d < 2) d = 2;
    if (bits & 0x01) spr.fillRect(x + d, y, w - 2*d, d, color); if (bits & 0x40) spr.fillRect(x + d, y + h/2 - d/2, w - 2*d, d, color); if (bits & 0x08) spr.fillRect(x + d, y + h - d, w - 2*d, d, color); if (bits & 0x02) spr.fillRect(x + w - d, y + d, d, h/2 - 1.5*d, color); if (bits & 0x04) spr.fillRect(x + w - d, y + h/2 + d/2, d, h/2 - 1.5*d, color); if (bits & 0x10) spr.fillRect(x, y + h/2 + d/2, d, h/2 - 1.5*d, color); if (bits & 0x20) spr.fillRect(x, y + d, d, h/2 - 1.5*d, color);          
}

int draw7SegString(TFT_eSprite &spr, String text, int x, int y, int h, int w, uint16_t color, int spacing) {
    int curX = x; int d = h / 10; if (d < 2) d = 2; 
    for (int i = 0; i < text.length(); i++) {
        char c = text[i];
        if (c == '.') { spr.fillRect(curX, y + h - d, d, d, color); curX += d + spacing; } else if (c == ' ') { curX += (w/2) + spacing; } else if (c == '|') { spr.fillRect(curX + w/2 - d/2, y, d, h, color); curX += w/2 + spacing; } else {
            uint8_t bits = 0; if (c >= '0' && c <= '9') { const uint8_t segs[] = {0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F}; bits = segs[c - '0']; } else if (c == 'E' || c == 'e') { bits = 0x79; } else if (c == 'h' || c == 'H') { bits = 0x74; } else if (c == '-') { bits = 0x40; } else if (c == ':') { spr.fillRect(curX + w/2 - d/2, y + h/3, d, d, color); spr.fillRect(curX + w/2 - d/2, y + 2*h/3, d, d, color); curX += w/2 + spacing; continue; }
            if (bits & 0x01) spr.fillRect(curX + d, y, w - 2*d, d, color); 
            if (bits & 0x40) spr.fillRect(curX + d, y + h/2 - d/2, w - 2*d, d, color); 
            if (bits & 0x08) spr.fillRect(curX + d, y + h - d, w - 2*d, d, color); 
            if (bits & 0x02) spr.fillRect(curX + w - d, y + d, d, h/2 - 1.5*d, color); 
            if (bits & 0x04) spr.fillRect(curX + w - d, y + h/2 + d/2, d, h/2 - 1.5*d, color); 
            if (bits & 0x10) spr.fillRect(curX, y + h/2 + d/2, d, h/2 - 1.5*d, color); 
            if (bits & 0x20) spr.fillRect(curX, y + d, d, h/2 - 1.5*d, color); // HIER WAR DER BUG! (x statt curX)
            curX += w + spacing;
        }
    } return curX - x; 
}

int get7SegWidth(String text, int h, int w, int spacing) {
    int curX = 0; int d = h / 10; if (d < 2) d = 2; 
    for (int i = 0; i < text.length(); i++) { char c = text[i]; if (c == '.') curX += d + spacing; else if (c == ' ' || c == '|' || c == ':') curX += (w/2) + spacing; else curX += w + spacing; } return curX - spacing; 
}

void drawXButton(TFT_eSprite &spr) {
    spr.fillRoundRect(185, 5, 50, 50, 8, TFT_RED); spr.drawLine(195, 15, 225, 45, TFT_WHITE); spr.drawLine(196, 15, 226, 45, TFT_WHITE); spr.drawLine(225, 15, 195, 45, TFT_WHITE); spr.drawLine(226, 15, 196, 45, TFT_WHITE);
}

void drawBattery(int x, int y, int pct, bool charging) {
    tft.drawRect(x, y, 22, 11, cFG); tft.fillRect(x+22, y+3, 2, 5, cFG); tft.fillRect(x+1, y+1, 20, 9, cBG); uint16_t color = charging ? TFT_BLUE : ((pct >= 80) ? TFT_GREEN : ((pct >= 60) ? tft.color565(173, 255, 47) : ((pct >= 40) ? TFT_YELLOW : ((pct >= 20) ? TFT_ORANGE : TFT_RED)))); int w = (20 * pct) / 100; if (w > 0) tft.fillRect(x+1, y+1, w, 9, color);
}

void drawHeader(struct tm *aktuell, int akku, bool charging) {
    tft.fillRect(0, 0, 240, HEAD_H, cBG); tft.setTextDatum(TL_DATUM); int commonY = 8; drawBattery(6, commonY + 3, akku, charging);
    tft.setTextColor(cFG, cBG); tft.drawString(String(akku) + "%", 34, commonY, 2); tft.setTextDatum(TC_DATUM); tft.setTextColor(cAcc1, cBG);
    char timeBuf[10]; snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", aktuell->tm_hour, aktuell->tm_min); tft.drawString(timeBuf, 120, commonY, 2);
    
    // BT Icon Watch Header
    uint16_t btCol = btActive ? TFT_BLUE : tft.color565(80, 80, 80);
    tft.setTextDatum(TR_DATUM); tft.setTextColor(btCol, cBG); tft.drawString("BT", 160, commonY, 2);
    
    // WLAN Fächer Watch Header
    bool wifiConn = (WiFi.status() == WL_CONNECTED) || wlanActive || evilActive || wifiSpamActive || deauthActive || nmapActive;
    uint16_t wifiCol = wifiConn ? TFT_WHITE : tft.color565(80, 80, 80);
    if (wlanActive && wlanClientMode) wifiCol = TFT_BLUE; 
    
    tft.setTextColor(wifiCol, cBG); tft.drawString("WIFI", 195, commonY, 2);
    
    tft.setTextColor(cAcc2, cBG); char dateBuf[20]; snprintf(dateBuf, sizeof(dateBuf), "%s %02d.%02d.", wochentage[aktuell->tm_wday], aktuell->tm_mday, aktuell->tm_mon + 1); tft.drawString(dateBuf, 234, commonY, 2); 
}

void renderPage(int page, TFT_eSprite *spr, struct tm *aktuell) {
    time_t jetzt; time(&jetzt);
    
    if (page == 0) {
        long diff = mktime(&zielZeit) - jetzt; if (diff < 0) diff = 0;
        int vals[] = {(int)(diff / 31536000), (int)((diff % 31536000) / 2592000), (int)((diff % 2592000) / 86400), (int)((diff % 86400) / 3600), (int)((diff % 3600) / 60), (int)(diff % 60)};
        const char* labs[] = {"Jahre", "Monate", "Tage", "Stunden", "Minuten", "Sekunden"}; int rowH = spr->height() / 6; int sH = 26; int sW = 16; 
        for(int i = 0; i < 6; i++) { int y = i * rowH + (rowH - sH) / 2; char buf[5]; snprintf(buf, sizeof(buf), "%02d", vals[i]); draw7SegString(*spr, String(buf), 15, y, sH, sW, cFG, 4); spr->setTextDatum(ML_DATUM); spr->setTextColor(cBatBG); spr->drawString(labs[i], 70, y + sH/2 + 2, 4); }
        int barYStart = (rowH - sH) / 2; int fullBarH = (5 * rowH + barYStart + sH) - barYStart; float aktAlter = (float)(aktuell->tm_year + 1900 - gJahr) + (float)aktuell->tm_yday/365.0; float prozent = aktAlter / (float)zielAlter; int fillH = (int)(fullBarH * (prozent > 1.0 ? 1.0 : prozent)); spr->drawRect(215, barYStart, 15, fullBarH, cBatBG); spr->fillRect(217, (barYStart + fullBarH) - fillH, 11, fillH, (prozent > 0.8) ? TFT_RED : (prozent > 0.5 ? TFT_ORANGE : TFT_GREEN));
    } 
    else if (page == 1) {
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cAcc1); spr->drawString("Opportunitaet", 120, 20, 4);
        float dailyNet = (netSalary * 12.0) / 365.0; float fraction = (millis() % 1000) / 1000.0; long secToday = aktuell->tm_hour * 3600 + aktuell->tm_min * 60 + aktuell->tm_sec; float earnedToday = dailyNet * ((secToday + fraction) / 86400.0);
        char moneyBuf[20]; snprintf(moneyBuf, sizeof(moneyBuf), "%.4f E", earnedToday); int mH = 38; int mW = 22; int mSpace = 6; int totalW = get7SegWidth(String(moneyBuf), mH, mW, mSpace); draw7SegString(*spr, String(moneyBuf), 120 - (totalW / 2), 60, mH, mW, cFG, mSpace);
        float workDaysPerYear = workDaysYear - vacationDays - sickDays; float workHoursPerYear = workDaysPerYear * workHoursPerDay * (partTimePct / 100.0); float trueHourlyWage = 0; if (workHoursPerYear > 0) trueHourlyWage = (netSalary * 12.0) / workHoursPerYear;
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cBatBG); spr->drawString("Echter Stundenlohn", 120, 135, 2); char hrBuf[20]; snprintf(hrBuf, sizeof(hrBuf), "%.2f E|h", trueHourlyWage); 
        int hH = 22; int hW = 14; int hSpace = 4; int hTotalW = get7SegWidth(String(hrBuf), hH, hW, hSpace); draw7SegString(*spr, String(hrBuf), 120 - (hTotalW / 2), 160, hH, hW, cAcc2, hSpace);
    }
    else if (page == 2) {
        int activeKids = 0; String kNames[3]; String kDates[3]; String kTimes[3];
        if(child1Name.length() > 0 && child1Date.length() == 10) { kNames[activeKids] = child1Name; kDates[activeKids] = child1Date; kTimes[activeKids] = child1Time; activeKids++; }
        if(child2Name.length() > 0 && child2Date.length() == 10) { kNames[activeKids] = child2Name; kDates[activeKids] = child2Date; kTimes[activeKids] = child2Time; activeKids++; }
        if(child3Name.length() > 0 && child3Date.length() == 10) { kNames[activeKids] = child3Name; kDates[activeKids] = child3Name; kTimes[activeKids] = child3Time; activeKids++; }
        if (activeKids > 0) {
            int yName = 18, yJahre = 60, yMonate = 100, yWochen = 140, yTage = 180; spr->setTextDatum(MC_DATUM); spr->setTextColor(cBatBG);
            if (activeKids <= 2) { spr->drawString("Jahre", 120, yJahre, 2); spr->drawString("Monate", 120, yMonate, 2); spr->drawString("Wochen", 120, yWochen, 2); spr->drawString("Tage", 120, yTage, 2); }
            for(int i=0; i<activeKids; i++) {
                int xCenter = 120; if (activeKids == 2) xCenter = (i == 0) ? 45 : 195; else if (activeKids == 3) xCenter = 40 + (i * 80);
                spr->setTextDatum(MC_DATUM); spr->setTextColor(cFG); spr->drawString(kNames[i], xCenter, yName, 4); 
                time_t bTime = stringToTime(kDates[i], kTimes[i]); long diff = jetzt - bTime; if (diff < 0) diff = 0;
                long dDays = diff / 86400; long dWeeks = dDays / 7;
                int bYear = kDates[i].substring(0, 4).toInt(); int bMon = kDates[i].substring(5, 7).toInt(); int bDay = kDates[i].substring(8, 10).toInt();
                int dYears = (aktuell->tm_year + 1900) - bYear; if ((aktuell->tm_mon + 1) < bMon || ((aktuell->tm_mon + 1) == bMon && aktuell->tm_mday < bDay)) dYears--; if (dYears < 0) dYears = 0;
                int dMonths = ((aktuell->tm_year + 1900) - bYear) * 12 + ((aktuell->tm_mon + 1) - bMon); if (aktuell->tm_mday < bDay) dMonths--; if (dMonths < 0) dMonths = 0;
                spr->setTextColor(cAcc2); spr->drawString(String(dYears), xCenter, yJahre, 4); spr->drawString(String(dMonths), xCenter, yMonate, 4); spr->drawString(String(dWeeks), xCenter, yWochen, 4); spr->drawString(String(dDays), xCenter, yTage, 4);
            }
        } else { spr->setTextDatum(MC_DATUM); spr->setTextColor(cBatBG); spr->drawString("Keine Kinder", 120, 103, 2); spr->drawString("eingetragen", 120, 123, 2); }
    }
    else if (page == 3) {
        char uB[12]; char uT[6]; snprintf(uB, sizeof(uB), "%04d-%02d-%02d", gJahr, gMonat, gTag); snprintf(uT, sizeof(uT), "%02d:%02d", gStunde, gMinute);
        time_t uTime = stringToTime(String(uB), String(uT)); time_t pTime = stringToTime(parentDate, parentTime);
        if(uTime != 0 && pTime != 0) {
            long myAgeSec = jetzt - uTime; time_t targetDate = pTime + myAgeSec; struct tm *tgt = localtime(&targetDate);
            const char* monate[] = {"Januar", "Februar", "Maerz", "April", "Mai", "Juni", "Juli", "August", "September", "Oktober", "November", "Dezember"};
            char dBuf[30]; snprintf(dBuf, sizeof(dBuf), "%02d. %s %04d", tgt->tm_mday, monate[tgt->tm_mon], tgt->tm_year+1900);
            long myAgeThenSec = targetDate - uTime; int yearsThen = myAgeThenSec / 31536000;
            spr->setTextDatum(MC_DATUM); spr->setTextColor(cBatBG); spr->drawString("Heute war " + parentName, 120, 30, 2); spr->drawString("in deinem Alter der", 120, 50, 2);
            spr->setTextColor(cFG); spr->drawString(dBuf, 120, 85, 4); spr->setTextColor(cBatBG); spr->drawString("An diesem Tag warst Du", 120, 135, 2);
            if (myAgeThenSec < 0) { spr->setTextColor(cAcc2); spr->drawString("Noch nicht", 120, 160, 4); spr->drawString("geboren.", 120, 185, 4); } else { spr->setTextColor(cAcc2); spr->drawString(String(yearsThen) + " Jahre alt.", 120, 175, 4); }
        } else { spr->setTextDatum(MC_DATUM); spr->setTextColor(cBatBG); spr->drawString("Daten fehlen", 120, 100, 2); }
    }
    // FIX: IP Überlappung behoben!
    else if (page == 5) {
        spr->setTextDatum(TC_DATUM); 
        if (wlanActive && wlanIP != "") {
            spr->setTextColor(TFT_WHITE, cBG);
            spr->drawString("IP: " + wlanIP, 120, 5, 2);
        } else {
            spr->setTextColor(cFG, cBG); 
            spr->drawString("Quick Settings", 120, 5, 2);
        }

        spr->setTextDatum(MC_DATUM); 
        
        uint16_t wCol = cBatBG; 
        String wTxt = "WLAN";
        if (wlanActive) {
            if (wlanClientMode) { wCol = TFT_BLUE; wTxt = "CLIENT"; }
            else { wCol = TFT_RED; wTxt = "HOTSPOT"; }
        }
        
        spr->fillRoundRect(15, 30, 100, 50, 10, wCol); spr->setTextColor(wlanActive ? TFT_WHITE : cFG); spr->drawString(wTxt, 65, 55, 2); 
        
        spr->fillRoundRect(125, 30, 100, 50, 10, btActive ? TFT_BLUE : cBatBG); spr->setTextColor(btActive ? TFT_WHITE : cFG); spr->drawString("BT", 175, 55, 2);
        spr->fillRoundRect(15, 90, 100, 50, 10, vibActive ? TFT_ORANGE : cBatBG); spr->setTextColor(vibActive ? TFT_WHITE : cFG); spr->drawString("Vibra", 65, 115, 2);
        spr->fillRoundRect(125, 90, 100, 50, 10, aodActive ? TFT_GREEN : cBatBG); spr->setTextColor(aodActive ? TFT_BLACK : cFG); spr->drawString("AOD", 175, 115, 2);
        spr->fillRoundRect(15, 150, 100, 50, 10, cBatBG); spr->setTextColor(cFG); spr->drawString("Akku", 65, 175, 2);
        spr->fillRoundRect(125, 150, 100, 50, 10, cBatBG); spr->setTextColor(cFG); spr->drawString("Licht", 175, 175, 2);
    }
    else if (page == 6) { 
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Apps", 120, 2, 2); spr->setTextDatum(MC_DATUM); 
        spr->fillRoundRect(15, 25, 45, 50, 8, cBatBG); spr->setTextColor(cFG); spr->drawString("Stop", 37, 50, 2);
        spr->fillRoundRect(70, 25, 45, 50, 8, cBatBG); spr->drawString("Time", 92, 50, 2);
        spr->fillRoundRect(125, 25, 45, 50, 8, cBatBG); spr->drawString("dB", 147, 50, 2);
        spr->fillRoundRect(180, 25, 45, 50, 8, cBatBG); spr->drawString("Calc", 202, 50, 2);
        
        spr->fillRoundRect(15, 85, 45, 50, 8, cBatBG); spr->drawString("Mous", 37, 110, 2); 
        spr->fillRoundRect(70, 85, 45, 50, 8, cBatBG); spr->drawString("Phis", 92, 110, 2);
        spr->fillRoundRect(125, 85, 45, 50, 8, cBatBG); spr->setTextColor(TFT_RED); spr->drawString("Dcky", 147, 110, 2);
        spr->fillRoundRect(180, 85, 45, 50, 8, cBatBG); spr->setTextColor(TFT_RED); spr->drawString("Spam", 202, 110, 2); 

        spr->fillRoundRect(15, 145, 45, 50, 8, cBatBG); spr->setTextColor(TFT_MAGENTA); spr->drawString("BLE", 37, 170, 2); 
        spr->fillRoundRect(70, 145, 45, 50, 8, cBatBG); spr->setTextColor(TFT_GREEN); spr->drawString("Nmap", 92, 170, 2); 
        spr->fillRoundRect(125, 145, 45, 50, 8, cBatBG); spr->setTextColor(TFT_RED); spr->drawString("Deau", 147, 170, 2); 
    }
    else if (page == 7) { 
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Stoppuhr", 120, 0, 2); long ms = swRunning ? (swElapsed + (millis() - swStartTime)) : swElapsed;
        char swBuf[20]; snprintf(swBuf, sizeof(swBuf), "%02d:%02d.%02d", (int)(ms/60000), (int)((ms/1000)%60), (int)((ms%1000)/10));
        int bW = get7SegWidth(String(swBuf), 28, 16, 5); draw7SegString(*spr, String(swBuf), 120 - (bW/2), 70, 28, 16, cAcc1, 5);
        spr->setTextDatum(MC_DATUM); spr->fillRoundRect(20, 150, 90, 40, 6, swRunning ? TFT_ORANGE : TFT_GREEN); spr->setTextColor(TFT_BLACK); spr->drawString(swRunning ? "Stop" : "Start", 65, 170, 2);
        spr->fillRoundRect(130, 150, 90, 40, 6, cBatBG); spr->setTextColor(cFG); spr->drawString("Reset", 175, 170, 2); drawXButton(*spr);
    }
    else if (page == 8) { 
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Timer", 120, 0, 2); spr->setTextDatum(MC_DATUM); 
        if (!timerRunning && timerRemaining == 0) { spr->setTextColor(cAcc2); spr->drawString("Waehlen:", 120, 50, 4); spr->fillRoundRect(20, 80, 60, 50, 6, cBatBG); spr->setTextColor(cFG); spr->drawString(String(t1)+"m", 50, 105, 2); spr->fillRoundRect(90, 80, 60, 50, 6, cBatBG); spr->drawString(String(t2)+"m", 120, 105, 2); spr->fillRoundRect(160, 80, 60, 50, 6, cBatBG); spr->drawString(String(t3)+"m", 190, 105, 2); } else { char tBuf[16]; snprintf(tBuf, sizeof(tBuf), "%02d:%02d", (int)(timerRemaining/60), (int)(timerRemaining%60)); int bW = get7SegWidth(String(tBuf), 40, 24, 6); draw7SegString(*spr, String(tBuf), 120 - (bW/2), 70, 40, 24, TFT_RED, 6); spr->fillRoundRect(75, 150, 90, 40, 6, TFT_RED); spr->setTextColor(TFT_WHITE); spr->drawString("Abbruch", 120, 170, 2); } drawXButton(*spr);
    }
    else if (page == 9) { 
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Dezibel-Meter", 120, 0, 2); int db = getDecibels(); spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_WHITE); spr->drawString(String(db) + " dB", 120, 50, 6); spr->setTextColor(cBatBG); spr->drawString("RMS: " + String(debug_rms, 1), 120, 85, 2); int barH = db; if(barH > 120) barH = 120; uint16_t bCol = (db >= 90) ? TFT_RED : ((db >= 65) ? TFT_YELLOW : TFT_GREEN); spr->fillRect(60, 195 - barH, 120, barH, bCol); spr->drawRect(59, 74, 122, 122, TFT_WHITE); drawXButton(*spr);
    }
    else if (page == 10) {
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Akku Details", 120, 0, 2); spr->drawString(String(getBattery()) + "%    " + String(pmu.getBattVoltage() / 1000.0, 2) + " V", 120, 20, 4); String estStr = "Laufzeit: Berechne..."; bool isCharging = pmu.isVbusIn() || pmu.isCharging();
        if (isCharging) estStr = "Wird geladen"; else if (historyCount >= 2) { int drop = (batHistoryPct[0]%100) - (batHistoryPct[historyCount-1]%100); if (drop > 0) { int elapsedMins = (historyCount - 1) * (historyInterval / 60000); float dropPerMin = (float)drop / elapsedMins; float remainingMins = getBattery() / dropPerMin; estStr = "Noch ca. " + String((int)(remainingMins / 60)) + "h " + String((int)remainingMins % 60) + "m"; } else estStr = "Noch ca. >24h"; } spr->setTextColor(cAcc1); spr->drawString(estStr, 120, 45, 2);
        int gX = 20, gY = 70, gW = 200, gH = 70; spr->drawRect(gX, gY, gW, gH, cBatBG);
        if (historyCount > 1) { 
            float stepX = (float)gW / (MAX_HISTORY - 1); 
            for (int i = 0; i < historyCount - 1; i++) { 
                uint8_t raw1 = batHistoryPct[i]; uint8_t raw2 = batHistoryPct[i+1];
                int p1 = raw1 % 100; int p2 = raw2 % 100;
                int px1 = gX + (i * stepX); int px2 = gX + ((i + 1) * stepX); 
                int py1 = gY + gH - (p1 * gH / 100); int py2 = gY + gH - (p2 * gH / 100); 
                uint16_t col = TFT_GREEN; 
                if (raw1 >= 200) col = TFT_BLUE; else if (raw1 >= 100) col = TFT_RED; else if(p1 < 20) col = TFT_RED; else if(p1 < 40) col = TFT_ORANGE; else if(p1 < 60) col = TFT_YELLOW; else if(p1 < 80) col = tft.color565(173, 255, 47); 
                spr->fillTriangle(px1, gY+gH-1, px2, gY+gH-1, px1, py1, col); spr->fillTriangle(px1, py1, px2, py2, px2, gY+gH-1, col); spr->drawLine(px1, py1, px2, py2, TFT_WHITE); 
            } 
        } 
        drawXButton(*spr);
    }
    else if (page == 11) { spr->fillSprite(TFT_WHITE); drawXButton(*spr); }
    else if (page == 12) { showEvilPortal(spr); drawXButton(*spr); } 
    else if (page == 13) { showRechner(spr); drawXButton(*spr); }
    else if (page == 14) { showJiggler(spr); drawXButton(*spr); }
    else if (page == 15) { 
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Rubber Ducky", 120, 10, 2);
        spr->fillRoundRect(15, 35, 30, 30, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString("<", 30, 42, 2);
        spr->fillRoundRect(195, 35, 30, 30, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString(">", 210, 42, 2);
        spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_YELLOW); spr->drawString(badUsbNames[currentBadUsbScript], 120, 50, 2);
        spr->fillRoundRect(40, 85, 160, 60, 10, badUsbActive ? TFT_RED : cBatBG);
        spr->setTextColor(TFT_WHITE); spr->drawString(badUsbActive ? "SCRIPT LÄUFT" : "START SCRIPT", 120, 115, 2);
        drawXButton(*spr);
    }
    else if (page == 16) { 
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("WiFi Spammer", 120, 10, 2);
        spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_YELLOW); spr->drawString("Gefakte Netzwerke:", 120, 40, 2);
        spr->fillRoundRect(40, 80, 160, 60, 10, wifiSpamActive ? TFT_RED : cBatBG);
        spr->setTextColor(TFT_WHITE); spr->drawString(wifiSpamActive ? "SPAMMING... (Stop)" : "START SPAM", 120, 110, 2);
        drawXButton(*spr);
    }
    else if (page == 17) { 
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("BLE Spammer", 120, 10, 2);
        spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_YELLOW); spr->drawString("Apple Popups", 120, 40, 2);
        spr->fillRoundRect(40, 80, 160, 60, 10, bleSpamActive ? TFT_RED : cBatBG);
        spr->setTextColor(TFT_WHITE); spr->drawString(bleSpamActive ? "SPAMMING... (Stop)" : "START SPAM", 120, 110, 2);
        drawXButton(*spr);
    }
    else if (page == 18) {
        spr->setTextDatum(TC_DATUM); spr->setTextColor(TFT_GREEN); spr->drawString("Nmap Port 80", 120, 5, 2);
        spr->fillRoundRect(15, 35, 30, 30, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString("<", 30, 42, 2);
        spr->fillRoundRect(195, 35, 30, 30, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString(">", 210, 42, 2);
        
        spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_YELLOW); 
        spr->drawString(targetSSIDs[currentTargetNmap].length() > 0 ? targetSSIDs[currentTargetNmap] : "Leer", 120, 50, 2);

        if (nmapState == 0 || nmapState == 3 || nmapState == 4) {
            spr->fillRoundRect(40, 85, 160, 40, 10, nmapActive ? TFT_RED : cBatBG);
            spr->setTextColor(TFT_WHITE); spr->drawString(nmapActive ? "STOPPING..." : "CONNECT & SCAN", 120, 105, 2);
        } else {
            spr->fillRoundRect(40, 85, 160, 40, 10, TFT_RED);
            spr->setTextColor(TFT_WHITE); spr->drawString("SCANNING... (Stop)", 120, 105, 2);
            spr->setTextColor(TFT_YELLOW); spr->drawString("IP: " + String(nmapIP), 120, 140, 2);
        }
        
        spr->setTextColor(cBatBG);
        if (nmapState == 1) spr->drawString("Verbinde...", 120, 170, 2);
        else if (nmapState == 3) spr->drawString("Abgeschlossen!", 120, 170, 2);
        else if (nmapState == 4) spr->drawString("Fehler!", 120, 170, 2);
        else if (nmapLog != "") spr->drawString("Ergebnis im Webportal", 120, 170, 2);
        
        drawXButton(*spr);
    }
    else if (page == 19) {
        spr->setTextDatum(TC_DATUM); spr->setTextColor(TFT_RED); spr->drawString("WiFi Deauther", 120, 5, 2);
        spr->fillRoundRect(15, 35, 30, 30, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString("<", 30, 42, 2);
        spr->fillRoundRect(195, 35, 30, 30, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString(">", 210, 42, 2);
        
        spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_YELLOW); 
        if (numScannedNetworks > 0) spr->drawString(scannedSSIDs[currentDeauthTarget], 120, 50, 2);
        else spr->drawString("Nichts gescannt", 120, 50, 2);

        spr->fillRoundRect(40, 75, 160, 40, 10, deauthActive ? TFT_RED : cBatBG);
        spr->setTextColor(TFT_WHITE); spr->drawString(deauthActive ? "ATTACKING..." : "START DEAUTH", 120, 95, 2);

        spr->fillRoundRect(60, 130, 120, 30, 6, isScanningAir ? TFT_ORANGE : cBatBG);
        spr->setTextColor(TFT_WHITE); spr->drawString(isScanningAir ? "Scanne..." : "Scan Umgebung", 120, 145, 2);

        spr->setTextColor(cBatBG);
        if (deauthState == 2) spr->drawString(scannedMACs[currentDeauthTarget], 120, 185, 2);

        drawXButton(*spr);
    }
    
    if (millis() < toastEndTime) { int tW = spr->textWidth(toastMsg, 2) + 40; int tX = (240 - tW) / 2; spr->fillRoundRect(tX, 165, tW, 30, 15, TFT_DARKGREY); spr->setTextColor(TFT_WHITE, TFT_DARKGREY); spr->drawCentreString(toastMsg, 120, 173, 2); }
}

void slideToPage(int newPage, int dx, int dy) {
    TFT_eSprite nextSpr = TFT_eSprite(&tft); void* ptr = nextSpr.createSprite(240, 240 - HEAD_H); time_t jetzt; time(&jetzt); struct tm *aktuell = localtime(&jetzt);
    if (ptr == nullptr) { currentPage = newPage; lastSecond = -1; return; } renderPage(newPage, &nextSpr, aktuell); int steps = 6; 
    for(int i=1; i<=steps; i++) { int p = (240 * i) / steps; if(dx == -1) { cdSprite.pushSprite(-p, HEAD_H); nextSpr.pushSprite(240-p, HEAD_H); } else if (dx == 1) { cdSprite.pushSprite(p, HEAD_H); nextSpr.pushSprite(p-240, HEAD_H); } else if (dy == -1) { cdSprite.pushSprite(0, HEAD_H - p); nextSpr.pushSprite(0, HEAD_H + 206 - p); } else if (dy == 1) { cdSprite.pushSprite(0, HEAD_H + p); nextSpr.pushSprite(0, HEAD_H - 206 + p); } }
    currentPage = newPage; lastSecond = -1; nextSpr.deleteSprite();
}

void renderUI(unsigned long currentMillis) {
    time_t jetzt; time(&jetzt); struct tm *aktuell = localtime(&jetzt); bool isCharging = pmu.isVbusIn() || pmu.isCharging(); static unsigned long lastFrame = 0;
    bool fastRefresh = (currentPage == 1 || currentPage == 7 || currentPage == 9); bool forceRender = (aktuell->tm_sec != lastSecond) || (fastRefresh && (currentMillis - lastFrame > 50)) || (currentMillis < toastEndTime) || (currentPage >= 13 && currentPage <= 19);
    if (forceRender) { lastSecond = aktuell->tm_sec; lastFrame = currentMillis; if(currentPage != 11) drawHeader(aktuell, getBattery(), isCharging); cdSprite.fillSprite(currentPage == 11 ? TFT_WHITE : cBG); renderPage(currentPage, &cdSprite, aktuell); if(currentPage != 11) cdSprite.pushSprite(0, HEAD_H); else cdSprite.pushSprite(0, 0); }
}

void renderAOD() {
    time_t jetzt; time(&jetzt); struct tm *aktuell = localtime(&jetzt); bool isCharging = pmu.isVbusIn() || pmu.isCharging(); static int lastAodMin = -1; static bool lastAodChargeState = false; static int lastAodBatPct = -1;
    bool forceAodUpdate = (aktuell->tm_min != lastAodMin) || (isCharging != lastAodChargeState) || (isCharging && getBattery() != lastAodBatPct) || (lastSecond == -1);
    if (forceAodUpdate) {
        lastAodMin = aktuell->tm_min; lastAodChargeState = isCharging; lastAodBatPct = getBattery(); lastSecond = 0; tft.fillScreen(TFT_BLACK); int aodX = 120 + (random(0, 10) - 5); int aodY = 110 + (random(0, 10) - 5);
        
        if (aodMode == 0) { tft.setTextDatum(MC_DATUM); tft.setTextColor(cFG, TFT_BLACK); char aodBuf[10]; snprintf(aodBuf, sizeof(aodBuf), "%02d:%02d", aktuell->tm_hour, aktuell->tm_min); tft.drawString(aodBuf, aodX, aodY - 35, 7); tft.setTextColor(cBatBG, TFT_BLACK); char dateBuf[20]; snprintf(dateBuf, sizeof(dateBuf), "%s, %02d.%02d.", wochentage[aktuell->tm_wday], aktuell->tm_mday, aktuell->tm_mon + 1); tft.drawString(dateBuf, aodX, aodY + 20, 4); } 
        else if (aodMode == 1) { tft.setTextDatum(MC_DATUM); tft.setTextColor(cAcc2, TFT_BLACK); String hStr = "", mStr = ""; int numH = aktuell->tm_hour, numM = aktuell->tm_min; for(int i=0; i<6; i++) { hStr = String(numH % 2) + hStr; numH /= 2; } for(int i=0; i<6; i++) { mStr = String(numM % 2) + mStr; numM /= 2; } tft.drawString(hStr, aodX, aodY - 20, 4); tft.drawString(mStr, aodX, aodY + 15, 4); } 
        else if (aodMode == 2) { tft.setTextDatum(TL_DATUM); tft.setTextColor(cBatBG, TFT_BLACK); const char* grid[10] = {"ESKISTAFUNF", "ZEHNZWANZIG", "DREIVIERTEL", "TGNACHVORJM", "HALBXZWOLFP", "ZWEINSIEBEN", "KDREIRHFUNF", "ELFNEUNVIER", "WACHTZEHNRS", "BSECHSFUHRX"}; bool active[10][11] = {false}; int h = aktuell->tm_hour; int m = aktuell->tm_min; auto setW = [&](int r, int c, int l) { for(int i=0; i<l; i++) active[r][c+i] = true; }; setW(0,0,2); setW(0,3,3); int m5 = m / 5; switch(m5) { case 0: setW(9,7,3); break; case 1: setW(0,7,4); setW(3,2,4); break; case 2: setW(1,0,4); setW(3,2,4); break; case 3: setW(2,4,7); setW(3,2,4); break; case 4: setW(1,4,7); setW(3,2,4); break; case 5: setW(0,7,4); setW(3,6,3); setW(4,0,4); break; case 6: setW(4,0,4); break; case 7: setW(0,7,4); setW(3,2,4); setW(4,0,4); break; case 8: setW(1,4,7); setW(3,6,3); break; case 9: setW(2,4,7); setW(3,6,3); break; case 10: setW(1,0,4); setW(3,6,3); break; case 11: setW(0,7,4); setW(3,6,3); break; } int sH = h; if (m >= 25) sH++; sH = sH % 12; if (sH == 0) sH = 12; switch(sH) { case 1: if (m5==0) setW(5,2,3); else setW(5,2,4); break; case 2: setW(5,0,4); break; case 3: setW(6,1,4); break; case 4: setW(7,7,4); break; case 5: setW(6,7,4); break; case 6: setW(9,1,5); break; case 7: setW(5,5,6); break; case 8: setW(8,1,4); break; case 9: setW(7,3,4); break; case 10: setW(8,5,4); break; case 11: setW(7,0,3); break; case 12: setW(4,5,5); break; } int stX = aodX - 95; int stY = aodY - 100; int sX = 18; int sY = 18; for(int r=0; r<10; r++) { for(int c=0; c<11; c++) { if(active[r][c]) { tft.setTextColor(cAcc1, TFT_BLACK); tft.drawChar(grid[r][c], stX + c*sX, stY + r*sY, 2); } else { tft.setTextColor(cBatBG, TFT_BLACK); tft.drawChar(grid[r][c], stX + c*sX, stY + r*sY, 2); } } } } 
        
        tft.setTextDatum(MC_DATUM); if (isCharging) { tft.setTextColor(TFT_YELLOW, TFT_BLACK); tft.drawString("⚡ " + String(lastAodBatPct) + "%", aodX, aodY + 90, 2); } else { tft.setTextColor(cBatBG, TFT_BLACK); tft.drawString(String(lastAodBatPct) + "%", aodX, aodY + 90, 2); }
    }
}