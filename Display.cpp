// --- Display.cpp ---
#include "Display.h"
#include "Globals.h" 
#include "AppLauncher.h" 
#include "SensorDriver.h"
#include "HardwareConfig.h" 
#include "NotificationManager.h"
#include <WiFi.h>
#include "LoraManager.h"
#include "AppManager.h"

#define HEAD_H 34
extern int getDecibels();

uint16_t hex2rgb565(String hex) {
    if(hex.length() < 7) return 0; long rgb = strtol(hex.substring(1).c_str(), NULL, 16);
    return tft.color565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

void applyTheme() { 
    cBG = hex2rgb565(hexBG); cFG = hex2rgb565(hexFG); cAcc1 = hex2rgb565(hexAcc1); cAcc2 = hex2rgb565(hexAcc2); cBatBG = hex2rgb565(hexBat); 
    cHeadBG = hex2rgb565(hexHeadBG); cHeadFG = hex2rgb565(hexHeadFG); cAppBG = hex2rgb565(hexAppBG); cWarn = hex2rgb565(hexWarn);
    cAodFG = hex2rgb565(hexAodFG); cAodAcc = hex2rgb565(hexAodAcc);
}
void setBrightness(int b) { WRITE_BACKLIGHT_PWM(45, b); } 

time_t stringToTime(String dateStr, String timeStr) {
    if(dateStr.length() != 10) return 0; struct tm t; memset(&t, 0, sizeof(struct tm));
    t.tm_year = dateStr.substring(0, 4).toInt() - 1900; t.tm_mon = dateStr.substring(5, 7).toInt() - 1; t.tm_mday = dateStr.substring(8, 10).toInt(); 
    if(timeStr.length() == 5) { t.tm_hour = timeStr.substring(0,2).toInt(); t.tm_min = timeStr.substring(3,5).toInt(); }
    return mktime(&t);
}

void draw7Segment(TFT_eSPI &spr, int x, int y, int num, int h, int w, uint16_t color) {
    const uint8_t segs[] = {0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F}; uint8_t bits = segs[num % 10]; int d = h / 10; if (d < 2) d = 2;
    if (bits & 0x01) spr.fillRect(x + d, y, w - 2*d, d, color); if (bits & 0x40) spr.fillRect(x + d, y + h/2 - d/2, w - 2*d, d, color); if (bits & 0x08) spr.fillRect(x + d, y + h - d, w - 2*d, d, color); if (bits & 0x02) spr.fillRect(x + w - d, y + d, d, h/2 - 1.5*d, color); if (bits & 0x04) spr.fillRect(x + w - d, y + h/2 + d/2, d, h/2 - 1.5*d, color); if (bits & 0x10) spr.fillRect(x, y + h/2 + d/2, d, h/2 - 1.5*d, color); if (bits & 0x20) spr.fillRect(x, y + d, d, h/2 - 1.5*d, color);          
}

int draw7SegString(TFT_eSPI &spr, String text, int x, int y, int h, int w, uint16_t color, int spacing) {
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
            if (bits & 0x20) spr.fillRect(curX, y + d, d, h/2 - 1.5*d, color);
            curX += w + spacing;
        }
    } return curX - x; 
}

int get7SegWidth(String text, int h, int w, int spacing) {
    int curX = 0; int d = h / 10; if (d < 2) d = 2; 
    for (int i = 0; i < text.length(); i++) { char c = text[i]; if (c == '.') curX += d + spacing; else if (c == ' ' || c == '|' || c == ':') curX += (w/2) + spacing; else curX += w + spacing; } return curX - spacing; 
}

void drawGlassCard(TFT_eSprite *spr, int x, int y, int w, int h, int r, uint16_t borderCol, uint16_t fillTop, uint16_t fillBot) {
    if (!spr || spr->getPointer() == nullptr) return;
    spr->fillRoundRect(x, y, w, h, r, fillBot);
    int topBandH = h / 3;
    if (fillTop != fillBot && topBandH > r) {
        spr->fillRoundRect(x + 1, y + 1, w - 2, topBandH, r > 1 ? r - 1 : 1, fillTop);
        spr->fillRect(x + 1, y + r, w - 2, topBandH - r + 1, blendColor565(fillTop, fillBot, 140));
    }
    // Clean crisp 1px card border
    spr->drawRoundRect(x, y, w, h, r, borderCol);
}

void drawXButton(TFT_eSprite &spr) {
    spr.fillRoundRect(185, 5, 50, 50, 8, TFT_RED); spr.drawLine(195, 15, 225, 45, TFT_WHITE); spr.drawLine(196, 15, 226, 45, TFT_WHITE); spr.drawLine(225, 15, 195, 45, TFT_WHITE); spr.drawLine(226, 15, 196, 45, TFT_WHITE);
}

static TFT_eSprite headSprite = TFT_eSprite(&tft);
static bool headSpriteReady = false;

void drawBattery(TFT_eSprite &spr, int x, int y, int pct, bool charging) {
    int bW = 38, bH = 15, nubW = 3, nubH = 7;
    // Outer casing with metallic terminal nub
    spr.fillRoundRect(x, y, bW, bH, 4, 0x10A2);
    spr.drawRoundRect(x, y, bW, bH, 4, 0x5ACB);
    spr.fillRoundRect(x + bW, y + (bH - nubH) / 2, nubW, nubH, 1, 0xBDD7);
    spr.fillRect(x + 2, y + 2, bW - 4, bH - 4, 0x0821);
    
    uint16_t colStart = charging ? 0x07FF : 
        ((pct >= 80) ? 0x07E0 : 
        ((pct >= 60) ? 0xAFE5 : 
        ((pct >= 40) ? 0xFFE0 : 
        ((pct >= 20) ? 0xFD20 : 0xF800))));
    
    int maxFillW = bW - 4;
    int fillW = (maxFillW * pct) / 100;
    if (fillW > 0) {
        spr.fillRoundRect(x + 2, y + 2, fillW, bH - 4, 2, colStart);
        // Specular gloss along top edge of battery fill
        spr.drawFastHLine(x + 3, y + 3, fillW - 2, 0xFFFF);
    }
    
    if (charging) {
        int lx = x + bW / 2, ly = y + bH / 2;
        spr.drawLine(lx + 2, ly - 4, lx - 2, ly + 1, 0xFFE0);
        spr.drawLine(lx - 2, ly + 1, lx + 1, ly + 1, 0xFFE0);
        spr.drawLine(lx + 1, ly + 1, lx - 2, ly + 5, 0xFFFF);
        spr.drawPixel(lx - 1, ly, 0xFFFF);
    } else {
        char pBuf[6]; snprintf(pBuf, sizeof(pBuf), "%d%%", pct);
        spr.setTextDatum(MC_DATUM);
        uint16_t txtCol = (pct >= 55) ? TFT_BLACK : TFT_WHITE;
        spr.setTextColor(txtCol); 
        spr.drawString(pBuf, x + bW / 2, y + bH / 2 + 1, 1);
    }
}

void drawBattery(int x, int y, int pct, bool charging) {
    if (headSpriteReady) {
        drawBattery(headSprite, x, y, pct, charging);
    } else {
        drawBattery(cdSprite, x, y, pct, charging);
    }
}

void drawHeader(struct tm *aktuell, int akku, bool charging) {
    if (!headSpriteReady) {
        void* p = headSprite.createSprite(240, HEAD_H);
        headSpriteReady = (p != nullptr);
    }
    
    uint16_t hdrBg = (cHeadBG != 0) ? cHeadBG : 0x0821;
    uint16_t hdrFg = (cHeadFG != 0) ? cHeadFG : 0x8CD1;
    TFT_eSprite &hs = headSpriteReady ? headSprite : cdSprite;
    hs.fillRect(0, 0, 240, HEAD_H, hdrBg);
    hs.drawFastHLine(0, 0, 240, 0x2104);
    int cy = 9;
    int curX = 4;
    
    // 1. 3D Akku
    drawBattery(hs, curX, cy, akku, charging);
    curX += 45;
    
    // 2. BT ICON mit Leuchtpunkt
    if (btActive) {
        bool btConnected = bleInitDone;
        uint16_t btCol = btConnected ? 0x551F : 0x7BEF;
        int bx = curX + 2, by = cy + 1, bh = 12;
        hs.drawLine(bx + 3, by, bx + 3, by + bh, btCol);
        hs.drawLine(bx + 3, by, bx + 7, by + 3, btCol);
        hs.drawLine(bx + 7, by + 3, bx, by + bh / 2 + 1, btCol);
        hs.drawLine(bx, by + bh / 2 - 1, bx + 7, by + bh - 3, btCol);
        hs.drawLine(bx + 7, by + bh - 3, bx + 3, by + bh, btCol);
        hs.fillCircle(bx + 11, by + 3, 2, btConnected ? 0x07FF : 0x7BEF);
        curX += 16;
    }
    
    // 3. WIFI ICON
    bool wifiOn = wlanActive || evilActive || wifiSpamActive || deauthActive || nmapActive;
    if (wifiOn) {
        bool wifiConnected = (WiFi.status() == WL_CONNECTED) || wlanActive || evilActive;
        uint16_t wCol = wifiConnected ? 0x07E0 : 0xFD20;
        int wx = curX + 6, wy = cy + 12;
        hs.fillCircle(wx, wy, 1, wCol);
        hs.drawCircle(wx, wy, 4, wCol);
        hs.drawCircle(wx, wy, 7, wCol);
        hs.fillRect(wx - 8, wy + 1, 16, 8, hdrBg);
        curX += 16;
    }
    
    // 3b. BACKGROUND APP INDICATOR — Zahnrad
    bool anyAppRunning = jigglerActive || badUsbActive || wifiSpamActive || bleSpamActive || deauthActive || nmapActive || evilActive || probeMonActive || bleScanActive || swRunning || timerRunning || LoraManager::isBeaconActive();
    if (anyAppRunning) {
        int gx = curX + 5, gy = cy + 6;
        hs.fillCircle(gx, gy, 4, 0xFD20);
        hs.fillCircle(gx, gy, 2, hdrBg);
        hs.fillRect(gx - 1, gy - 6, 2, 3, 0xFD20);
        hs.fillRect(gx - 1, gy + 3, 2, 3, 0xFD20);
        hs.fillRect(gx - 6, gy - 1, 3, 2, 0xFD20);
        hs.fillRect(gx + 3, gy - 1, 3, 2, 0xFD20);
        curX += 13;
    }
    
    // 4. KOPFZEILE RECHTS: Kontext-sensitiv
    if (currentPage == 0) {
        hs.setTextDatum(TR_DATUM);
        if (wlanActive) {
            hs.setTextColor(0x07E0, hdrBg);
            if (wlanClientMode && WiFi.status() == WL_CONNECTED) {
                hs.drawString(WiFi.localIP().toString(), 236, cy + 1, 2);
            } else {
                hs.drawString("AP: 192.168.4.1", 236, cy + 1, 2);
            }
        } else if (phoneMac.length() > 0) {
            hs.setTextColor(0x551F, hdrBg);
            String pInfo = phoneName.length() > 0 ? phoneName : "Phone";
            if (pInfo.length() > 10) pInfo = pInfo.substring(0, 10);
            hs.drawString(pInfo, 236, cy + 1, 2);
        } else if (charging) {
            hs.setTextColor(0xFFE0, hdrBg);
            hs.drawString("Laden ⚡", 236, cy + 1, 2);
        } else {
            hs.setTextColor(hdrFg, hdrBg);
            hs.drawString("WatchOS", 236, cy + 1, 2);
        }
    } else {
        // Auf App- und Einstellungsseiten: Saubere Uhrzeit rechts oben
        hs.setTextDatum(TR_DATUM);
        hs.setTextColor(cAcc1, hdrBg);
        char timeBuf[12]; 
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", aktuell->tm_hour, aktuell->tm_min);
        hs.drawString(timeBuf, 236, cy + 1, 2);
    }
    
    // 3D Glass-Trennlinie
    hs.drawFastHLine(0, HEAD_H - 1, 240, 0x2125);
    hs.drawFastHLine(0, HEAD_H, 240, 0x0000);
    
    if (headSpriteReady) {
        headSprite.pushSprite(0, 0);
    }
}

static void drawRingArc3D(TFT_eSprite *spr, int cx, int cy, int r, int thickness, float ratio, uint16_t startCol, uint16_t endCol, uint16_t grooveBg, uint16_t outerBevel) {
    if (!spr || spr->getPointer() == nullptr) return;
    if (isnan(ratio) || ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;

    int sprW = spr->width();
    int sprH = spr->height();

    // 1. Metallic Groove Channel: Outer bevel + recessed groove
    spr->drawCircle(cx, cy, r + 1, outerBevel);
    for (int t = 0; t < thickness; t++) {
        int curR = r - t;
        if (curR > 0) spr->drawCircle(cx, cy, curR, grooveBg);
    }
    spr->drawCircle(cx, cy, r - thickness, 0x0821);

    if (ratio <= 0.005f) return;

    // 2. Active Gradient Arc from 12 o'clock (-PI/2) clockwise
    const float startRad = -1.5707963f;
    float sweepRad = ratio * 6.2831853f;

    int steps = (int)(ratio * 160.0f);
    if (steps < 12) steps = 12;

    float lastCa = 0.0f, lastSa = -1.0f;

    for (int i = 0; i <= steps; i++) {
        float f = (float)i / (float)steps;
        float angle = startRad + (sweepRad * f);
        float ca = cosf(angle);
        float sa = sinf(angle);
        lastCa = ca;
        lastSa = sa;

        uint16_t col = blendColor565(startCol, endCol, (uint8_t)(f * 255));
        for (int t = 0; t < thickness; t++) {
            int curR = r - t;
            int px = cx + (int)(curR * ca + 0.5f);
            int py = cy + (int)(curR * sa + 0.5f);
            if (px >= 0 && px < sprW && py >= 0 && py < sprH) {
                spr->drawPixel(px, py, col);
            }
        }
    }

    // 3. 3D Glowing Bulb Cap at the leading head of the arc
    float capR = (float)r - ((float)thickness / 2.0f);
    int headX = cx + (int)(capR * lastCa + 0.5f);
    int headY = cy + (int)(capR * lastSa + 0.5f);
    int bulbRad = (thickness / 2) + 1;

    spr->fillCircle(headX, headY, bulbRad, endCol);
    spr->fillCircle(headX - 1, headY - 1, 1, TFT_WHITE);
}

static void drawActivityRings(TFT_eSprite *spr, int cx, int cy, float stepRatio, float batRatio, float timeRatio) {
    // Ring 1 (Außen - Schritte, r=38, Dicke=5): Neon-Emerald -> Neon-Cyan
    drawRingArc3D(spr, cx, cy, 38, 5, stepRatio, 0x07E0, 0x07FF, 0x0A63, 0x1B07);

    // Ring 2 (Mitte - Akku, r=29, Dicke=5): Sunset-Amber -> Electric-Coral
    drawRingArc3D(spr, cx, cy, 29, 5, batRatio, 0xFD20, 0xF9A0, 0x3180, 0x62C0);

    // Ring 3 (Innen - Tageszeit, r=20, Dicke=4): Cyber-Violett -> Neon-Magenta
    drawRingArc3D(spr, cx, cy, 20, 4, timeRatio, 0x901F, 0xF81F, 0x2008, 0x4812);
}

void drawHotspotPrompt(TFT_eSprite *spr) {
    unsigned long elapsed = millis() - hotspotPromptStart;
    float remainSec = (elapsed < 5000) ? (5000 - elapsed) / 1000.0f : 0.0f;

    // Card Container (zentriert auf cdSprite)
    int cx = 10, cy = 8, cw = 220, ch = 184;
    spr->fillRoundRect(cx, cy, cw, ch, 14, 0x0842); // Tiefer Glas-Ton
    spr->drawRoundRect(cx, cy, cw, ch, 14, 0xFD20); // Amber Glowing Border
    spr->drawRoundRect(cx + 1, cy + 1, cw - 2, ch - 2, 13, 0x8400);

    // Pill Badge
    spr->fillRoundRect(cx + 25, cy + 10, cw - 50, 22, 11, 0xFD20);
    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(TFT_BLACK, 0xFD20);
    spr->drawString("STROM GETRENNT", 120, cy + 21, 2);

    // Frage
    spr->setTextColor(TFT_WHITE, 0x0842);
    spr->drawString("Hotspot anlassen?", 120, cy + 46, 2);

    // 5s Countdown Balken
    int barW = 180;
    int fillW = (int)((remainSec / 5.0f) * barW);
    if (fillW < 0) fillW = 0;
    if (fillW > barW) fillW = barW;

    spr->fillRoundRect(30, cy + 64, barW, 8, 4, 0x2104);
    uint16_t barCol = (remainSec > 2.5f) ? TFT_CYAN : ((remainSec > 1.2f) ? TFT_ORANGE : TFT_RED);
    if (fillW > 2) spr->fillRoundRect(30, cy + 64, fillW, 8, 4, barCol);

    char cBuf[30];
    snprintf(cBuf, sizeof(cBuf), "Auto-Aus in %.1fs", remainSec);
    spr->setTextColor(0xAD55, 0x0842);
    spr->drawString(cBuf, 120, cy + 84, 2);

    // Buttons
    // Button 1 (Links - ANLASSEN)
    int b1X = 20, bY = cy + 104, bW = 95, bH = 58;
    spr->fillRoundRect(b1X, bY, bW, bH, 10, 0x03E0);
    spr->drawRoundRect(b1X, bY, bW, bH, 10, TFT_GREEN);
    spr->setTextColor(TFT_WHITE, 0x03E0);
    spr->drawString("JA", b1X + bW/2, bY + 22, 4);
    spr->drawString("anlassen", b1X + bW/2, bY + 44, 1);

    // Button 2 (Rechts - AUS)
    int b2X = 125;
    spr->fillRoundRect(b2X, bY, bW, bH, 10, 0x6000);
    spr->drawRoundRect(b2X, bY, bW, bH, 10, 0xF800);
    spr->setTextColor(TFT_WHITE, 0x6000);
    spr->drawString("AUS", b2X + bW/2, bY + 22, 4);
    spr->drawString("sofort", b2X + bW/2, bY + 44, 1);
}

static void drawSwordHand(TFT_eSprite *spr, int cx, int cy, float rad, float len, float width, uint16_t colLit, uint16_t colShadow, uint16_t colLume) {
    float ca = cosf(rad);
    float sa = sinf(rad);
    float px = -sa;
    float py = ca;

    float midDist = len * 0.72f;
    float halfW = width / 2.0f;

    float tipX = cx + len * ca;
    float tipY = cy + len * sa;

    float midX = cx + midDist * ca;
    float midY = cy + midDist * sa;

    float leftX = midX + halfW * px;
    float leftY = midY + halfW * py;

    float rightX = midX - halfW * px;
    float rightY = midY - halfW * py;

    float baseX = cx - 10.0f * ca;
    float baseY = cy - 10.0f * sa;

    float bLeftX = baseX + (halfW * 0.6f) * px;
    float bLeftY = baseY + (halfW * 0.6f) * py;

    float bRightX = baseX - (halfW * 0.6f) * px;
    float bRightY = baseY - (halfW * 0.6f) * py;

    // Illuminated half (Left)
    spr->fillTriangle((int)baseX, (int)baseY, (int)bLeftX, (int)bLeftY, (int)leftX, (int)leftY, colLit);
    spr->fillTriangle((int)baseX, (int)baseY, (int)leftX, (int)leftY, (int)midX, (int)midY, colLit);
    spr->fillTriangle((int)midX, (int)midY, (int)leftX, (int)leftY, (int)tipX, (int)tipY, colLit);

    // Shadowed half (Right)
    spr->fillTriangle((int)baseX, (int)baseY, (int)bRightX, (int)bRightY, (int)rightX, (int)rightY, colShadow);
    spr->fillTriangle((int)baseX, (int)baseY, (int)rightX, (int)rightY, (int)midX, (int)midY, colShadow);
    spr->fillTriangle((int)midX, (int)midY, (int)rightX, (int)rightY, (int)tipX, (int)tipY, colShadow);

    // Center luminous inlay stripe
    int lumStart = 10;
    int lumEnd = (int)(len * 0.60f);
    for (int d = lumStart; d <= lumEnd; d++) {
        int lx = cx + (int)(d * ca + 0.5f);
        int ly = cy + (int)(d * sa + 0.5f);
        spr->drawPixel(lx, ly, colLume);
        spr->drawPixel(lx + (int)(px * 0.6f + 0.5f), ly + (int)(py * 0.6f + 0.5f), colLume);
    }
}

static void drawSecondHand(TFT_eSprite *spr, int cx, int cy, float rad, float len, uint16_t col) {
    float ca = cosf(rad);
    float sa = sinf(rad);

    int tipX = cx + (int)(len * ca);
    int tipY = cy + (int)(len * sa);

    int tailX = cx - (int)(20.0f * ca);
    int tailY = cy - (int)(20.0f * sa);

    spr->drawLine(tailX, tailY, tipX, tipY, col);

    // Subtle edge highlight near base
    float px = -sa, py = ca;
    spr->drawLine(cx - (int)(10.0f * ca) + (int)px, cy - (int)(10.0f * sa) + (int)py,
                  cx + (int)(30.0f * ca) + (int)px, cy + (int)(30.0f * sa) + (int)py, col);

    // Circular open counterweight ring at -12px
    int cwX = cx - (int)(12.0f * ca);
    int cwY = cy - (int)(12.0f * sa);
    spr->fillCircle(cwX, cwY, 4, col);
    spr->fillCircle(cwX, cwY, 2, 0x0024); // dial background center

    // Center hub cap
    spr->fillCircle(cx, cy, 5, 0xBDD7); // Steel ring
    spr->drawCircle(cx, cy, 5, 0x2104);
    spr->fillCircle(cx, cy, 3, col);    // Red center pin
    spr->drawPixel(cx - 1, cy - 1, 0xFFFF); // Specular dot
}

void drawChronographWatchface(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg) {
    if (!spr || spr->getPointer() == nullptr) return;

    int cx = 120, cy = 103;
    const float DEG2RAD = 0.0174532925f;

    // 1. Titanium Bezel with Brushed Metallic Circles
    spr->fillCircle(cx, cy, 102, 0x18C3); // Outer gunmetal rim
    spr->drawCircle(cx, cy, 102, 0x5ACB); // Beveled edge
    spr->drawCircle(cx, cy, 101, 0xBDD7); // Polished stainless ring
    spr->drawCircle(cx, cy, 100, 0x2965);
    spr->fillCircle(cx, cy, 99, 0x0024);  // Deep Swiss midnight navy dial

    // Sunburst / circular brushing grain
    spr->drawCircle(cx, cy, 88, 0x0866);
    spr->drawCircle(cx, cy, 58, 0x0866);
    spr->drawCircle(cx, cy, 38, 0x0844);

    // 2. 60-Minute Track & 12 Luminous Hour Indices
    for (int m = 0; m < 60; m++) {
        float rad = (m * 6.0f) * DEG2RAD - 1.5707963f;
        float c = cosf(rad);
        float s = sinf(rad);

        if (m % 5 == 0) {
            int hIdx = m / 5;
            if (hIdx == 0) {
                // 12 o'clock: Luxury Double Luminous Baton + Gold Triangle
                float rOut = 95.0f, rIn = 78.0f;
                float cL = cosf(rad - 0.04f), sL = sinf(rad - 0.04f);
                spr->drawLine(cx + (int)(rIn * cL), cy + (int)(rIn * sL), cx + (int)(rOut * cL), cy + (int)(rOut * sL), 0xFFE0);
                spr->drawLine(cx + (int)(rIn * cL) - 1, cy + (int)(rIn * sL), cx + (int)(rOut * cL) - 1, cy + (int)(rOut * sL), 0x07FF);
                float cR = cosf(rad + 0.04f), sR = sinf(rad + 0.04f);
                spr->drawLine(cx + (int)(rIn * cR), cy + (int)(rIn * sR), cx + (int)(rOut * cR), cy + (int)(rOut * sR), 0xFFE0);
                spr->drawLine(cx + (int)(rIn * cR) + 1, cy + (int)(rIn * sR), cx + (int)(rOut * cR) + 1, cy + (int)(rOut * sR), 0x07FF);
            } else if (hIdx == 3 || hIdx == 9) {
                // 3 & 9 o'clock: Faceted Gold Markers (near subdials)
                float rOut = 95.0f, rIn = 84.0f;
                spr->drawLine(cx + (int)(rIn * c), cy + (int)(rIn * s), cx + (int)(rOut * c), cy + (int)(rOut * s), 0xFFE0);
                spr->drawLine(cx + (int)(rIn * c) + 1, cy + (int)(rIn * s) + 1, cx + (int)(rOut * c) + 1, cy + (int)(rOut * s) + 1, 0xFFFF);
            } else if (hIdx == 6) {
                // 6 o'clock: Above Date Window
                float rOut = 95.0f, rIn = 82.0f;
                spr->drawLine(cx + (int)(rIn * c), cy + (int)(rIn * s), cx + (int)(rOut * c), cy + (int)(rOut * s), 0xFFE0);
                spr->drawLine(cx + (int)(rIn * c) - 1, cy + (int)(rIn * s), cx + (int)(rOut * c) - 1, cy + (int)(rOut * s), 0x07FF);
            } else {
                // Standard 3D Gold Batons with Tritium Core
                float rOut = 95.0f, rIn = 80.0f;
                int x1 = cx + (int)(rIn * c), y1 = cy + (int)(rIn * s);
                int x2 = cx + (int)(rOut * c), y2 = cy + (int)(rOut * s);
                spr->drawLine(x1, y1, x2, y2, 0xFFE0);
                float px = -s * 1.0f, py = c * 1.0f;
                spr->drawLine(x1 + (int)px, y1 + (int)py, x2 + (int)px, y2 + (int)py, 0x87F0);
            }
        } else {
            // Minor Minute Ticks
            float rOut = 95.0f, rIn = 91.0f;
            spr->drawLine(cx + (int)(rIn * c), cy + (int)(rIn * s), cx + (int)(rOut * c), cy + (int)(rOut * s), 0x5ACB);
        }
    }

    // 3. Sub-Dials (Bi-Compax Chronometer Architecture)
    // Left Sub-Dial (Steps): sx = 68, sy = 103, r = 23
    int sx = 68, sy = 103, sr = 23;
    spr->fillCircle(sx, sy, sr, 0x0000);  // Recessed dial well
    spr->drawCircle(sx, sy, sr, 0x39E7);  // Bezel ring
    spr->drawCircle(sx, sy, sr - 1, 0x18C3);
    spr->drawCircle(sx, sy, 15, 0x0866);
    spr->drawCircle(sx, sy, 7, 0x0844);

    spr->setTextDatum(TC_DATUM);
    spr->setTextColor(0x8CD1, 0x0000);
    spr->drawString("STEPS", sx, sy - 18, 1);

    // Step Mini Gauge Hand
    int goal = (stepGoal > 0) ? stepGoal : 8000;
    float sRatio = (float)stepCount / (float)goal;
    if (sRatio > 1.0f) sRatio = 1.0f;
    float stepRad = (-135.0f + sRatio * 270.0f) * DEG2RAD - 1.5707963f;
    int shX = sx + (int)(15.0f * cosf(stepRad));
    int shY = sy + (int)(15.0f * sinf(stepRad));
    spr->drawLine(sx, sy, shX, shY, 0x07E0); // Emerald hand
    spr->fillCircle(sx, sy, 2, 0xFFFF);
    spr->setTextDatum(BC_DATUM);
    spr->setTextColor(0x07E0, 0x0000);
    spr->drawString(String(stepCount), sx, sy + 21, 1);

    // Right Sub-Dial (Battery / Power Reserve): bx = 172, by = 103, r = 23
    int bx = 172, by = 103, br = 23;
    spr->fillCircle(bx, by, br, 0x0000);
    spr->drawCircle(bx, by, br, 0x39E7);
    spr->drawCircle(bx, by, br - 1, 0x18C3);
    spr->drawCircle(bx, by, 15, 0x0866);
    spr->drawCircle(bx, by, 7, 0x0844);

    spr->setTextDatum(TC_DATUM);
    spr->setTextColor(0x8CD1, 0x0000);
    spr->drawString("POWER", bx, by - 18, 1);

    // Battery Mini Gauge Hand
    float bRatio = (float)akku / 100.0f;
    if (bRatio > 1.0f) bRatio = 1.0f;
    float bRad = (-135.0f + bRatio * 270.0f) * DEG2RAD - 1.5707963f;
    int bhX = bx + (int)(15.0f * cosf(bRad));
    int bhY = by + (int)(15.0f * sinf(bRad));
    uint16_t bHandCol = isChg ? 0x07FF : (akku > 30 ? 0xFD20 : 0xF800);
    spr->drawLine(bx, by, bhX, bhY, bHandCol);
    spr->fillCircle(bx, by, 2, 0xFFFF);
    spr->setTextDatum(BC_DATUM);
    spr->setTextColor(bHandCol, 0x0000);
    spr->drawString(String(akku) + "%", bx, by + 21, 1);

    // 4. Inscriptions (Swiss Heritage)
    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(0xFFE0); // Swiss Warm Gold
    spr->drawString("CHRONOMÈTRE", 120, 52, 2);
    spr->setTextColor(0x8CD1);
    spr->drawString("AUTOMATIQUE", 120, 66, 1);

    // 5. Date Window at 6 o'clock (y = 146)
    int dw = 32, dh = 18, dx = 120 - dw / 2, dy = 146;
    spr->fillRect(dx, dy, dw, dh, 0x0821);
    spr->drawRect(dx, dy, dw, dh, 0x8CD1);
    spr->drawRect(dx - 1, dy - 1, dw + 2, dh + 2, 0x39A7);
    char dStr[6];
    snprintf(dStr, sizeof(dStr), "%02d", aktuell->tm_mday);
    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(0xFFE0, 0x0821);
    spr->drawString(dStr, 120, dy + dh / 2 + 1, 2);

    spr->setTextColor(0x5ACB);
    spr->drawString("SWISS MADE", 120, 174, 1);

    // 6. 3D Metallic Sword Hands
    // Hour Hand
    float hRad = ((aktuell->tm_hour % 12) + aktuell->tm_min / 60.0f) * 30.0f * DEG2RAD - 1.5707963f;
    drawSwordHand(spr, cx, cy, hRad, 52.0f, 6.0f, 0xFFFF, 0x3186, 0x87F0);

    // Minute Hand
    float mRad = (aktuell->tm_min + aktuell->tm_sec / 60.0f) * 6.0f * DEG2RAD - 1.5707963f;
    drawSwordHand(spr, cx, cy, mRad, 76.0f, 5.0f, 0xFFFF, 0x3186, 0x87F0);

    // Sweeping Scarlet Second Hand
    float secRad = aktuell->tm_sec * 6.0f * DEG2RAD - 1.5707963f;
    drawSecondHand(spr, cx, cy, secRad, 84.0f, 0xF800);
}

void drawRetroCasioWatchface(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg) {
    if (!spr || spr->getPointer() == nullptr) return;

    // 1. Matte Resin Case Background
    spr->fillSprite(0x10A2); // Dark resin black

    // Case Border Bevel Lines
    spr->drawRoundRect(4, 2, 232, 202, 12, 0x19B7); // Iconic Casio electric blue line
    spr->drawRoundRect(6, 4, 228, 198, 10, 0x3186);

    // Horizontal Gold & Cyan Stripes
    spr->drawFastHLine(12, 22, 216, 0xFD20); // Gold
    spr->drawFastHLine(12, 23, 216, 0x07FF); // Cyan
    spr->drawFastHLine(12, 179, 216, 0x07FF);
    spr->drawFastHLine(12, 180, 216, 0xFD20);

    // Bezel Vintage Brand Inscriptions
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(0xFFFF, 0x10A2);
    spr->drawString("CASIO", 16, 7, 2);

    spr->setTextDatum(TR_DATUM);
    spr->setTextColor(0xFD20, 0x10A2);
    spr->drawString("ILLUMINATOR", 224, 9, 1);

    // Button Labels
    spr->setTextColor(0x8410, 0x10A2);
    spr->drawString("< LIGHT", 14, 25, 1);
    spr->drawString("MODE >", 14, 168, 1);
    spr->setTextDatum(TR_DATUM);
    spr->drawString("ALARM >", 226, 25, 1);
    spr->drawString("24HR >", 226, 168, 1);

    // Bottom Water Resist & Lithium Inscription
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(0xF800, 0x10A2); // Red WR
    spr->drawString("WATER", 16, 186, 1);
    spr->setTextColor(0x07FF, 0x10A2); // Cyan Resist
    spr->drawString("RESIST 50M", 52, 186, 1);
    spr->setTextDatum(TR_DATUM);
    spr->setTextColor(0xCE79, 0x10A2);
    spr->drawString("LITHIUM", 224, 186, 1);

    // 2. Vintage Olive LCD Window
    int lx = 18, ly = 34, lw = 204, lh = 130;
    uint16_t lcdBg = 0x9D72;     // Authentic Casio olive-greenish LCD
    uint16_t lcdGhost = 0x8CF0;  // Faint unlit segment shadow
    uint16_t lcdInk = 0x0821;    // Deep pitch LCD ink black

    spr->fillRoundRect(lx, ly, lw, lh, 6, lcdBg);
    // Recessed Bezel Inner Shadow
    spr->drawFastHLine(lx + 1, ly + 1, lw - 2, 0x5ACB);
    spr->drawFastVLine(lx + 1, ly + 1, lh - 2, 0x5ACB);
    spr->drawFastHLine(lx + 1, ly + lh - 1, lw - 2, 0xCE79);
    spr->drawFastVLine(lx + lw - 1, ly + 1, lh - 2, 0xCE79);
    spr->drawRoundRect(lx, ly, lw, lh, 6, 0x0000);

    // 3. Top LCD Status Strip
    // Day-of-week Banner: MO TU WE TH FR SA SU
    static const char* dayLabels[7] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
    int casioDay = (aktuell->tm_wday == 0) ? 6 : (aktuell->tm_wday - 1);
    spr->setTextDatum(TL_DATUM);
    for (int i = 0; i < 7; i++) {
        int dx = lx + 8 + i * 16;
        if (i == casioDay) {
            spr->fillRect(dx - 1, ly + 6, 15, 11, lcdInk);
            spr->setTextColor(lcdBg, lcdInk);
            spr->drawString(dayLabels[i], dx, ly + 7, 1);
        } else {
            spr->setTextColor(lcdGhost, lcdBg);
            spr->drawString(dayLabels[i], dx, ly + 7, 1);
        }
    }

    // 24H Box
    spr->drawRect(lx + lw - 66, ly + 7, 18, 9, lcdInk);
    spr->setTextColor(lcdInk, lcdBg);
    spr->drawString("24H", lx + lw - 64, ly + 8, 1);

    // Top-Right Date (Mon-Day)
    char dateStr[8];
    snprintf(dateStr, sizeof(dateStr), "%02d-%02d", aktuell->tm_mon + 1, aktuell->tm_mday);
    spr->setTextDatum(TR_DATUM);
    spr->setTextColor(lcdInk, lcdBg);
    spr->drawString(dateStr, lx + lw - 8, ly + 6, 2);

    // 4. Middle 7-Segment Digital Time
    // Hours & Minutes: height 44, width 20, spacing 4
    int hH = 44, hW = 20, hSpace = 4;
    int tx = lx + 12, ty = ly + 28;

    // Ghost Background for 4 digits (88:88)
    draw7Segment(*spr, tx, ty, 8, hH, hW, lcdGhost);
    draw7Segment(*spr, tx + hW + hSpace, ty, 8, hH, hW, lcdGhost);
    draw7Segment(*spr, tx + (hW + hSpace) * 2 + 10, ty, 8, hH, hW, lcdGhost);
    draw7Segment(*spr, tx + (hW + hSpace) * 3 + 10, ty, 8, hH, hW, lcdGhost);

    // Ghost colon
    spr->fillRect(tx + (hW + hSpace) * 2 + 3, ty + 12, 4, 4, lcdGhost);
    spr->fillRect(tx + (hW + hSpace) * 2 + 3, ty + 28, 4, 4, lcdGhost);

    // Active Digits
    int h1 = aktuell->tm_hour / 10;
    int h2 = aktuell->tm_hour % 10;
    int m1 = aktuell->tm_min / 10;
    int m2 = aktuell->tm_min % 10;
    draw7Segment(*spr, tx, ty, h1, hH, hW, lcdInk);
    draw7Segment(*spr, tx + hW + hSpace, ty, h2, hH, hW, lcdInk);

    // Active Colon
    spr->fillRect(tx + (hW + hSpace) * 2 + 3, ty + 12, 4, 4, lcdInk);
    spr->fillRect(tx + (hW + hSpace) * 2 + 3, ty + 28, 4, 4, lcdInk);

    draw7Segment(*spr, tx + (hW + hSpace) * 2 + 10, ty, m1, hH, hW, lcdInk);
    draw7Segment(*spr, tx + (hW + hSpace) * 3 + 10, ty, m2, hH, hW, lcdInk);

    // Smaller Seconds Digits (height 24, width 12, spacing 3)
    int sH = 24, sW = 12, sSpace = 3;
    int sx = tx + (hW + hSpace) * 4 + 14, sy = ty + 18;
    draw7Segment(*spr, sx, sy, 8, sH, sW, lcdGhost);
    draw7Segment(*spr, sx + sW + sSpace, sy, 8, sH, sW, lcdGhost);

    int s1 = aktuell->tm_sec / 10;
    int s2 = aktuell->tm_sec % 10;
    draw7Segment(*spr, sx, sy, s1, sH, sW, lcdInk);
    draw7Segment(*spr, sx + sW + sSpace, sy, s2, sH, sW, lcdInk);

    // 5. Bottom LCD Status Strip (Battery Blocks & Steps Odometer)
    int by = ly + 94;
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(lcdInk, lcdBg);
    spr->drawString("PWR", lx + 8, by, 1);

    // 5 Discrete LCD Battery Segments
    int nBlocks = (akku + 10) / 20;
    if (nBlocks > 5) nBlocks = 5;
    if (nBlocks < 0) nBlocks = 0;
    for (int b = 0; b < 5; b++) {
        int blkX = lx + 32 + b * 7;
        if (b < nBlocks) {
            spr->fillRect(blkX, by, 5, 8, lcdInk);
        } else {
            spr->drawRect(blkX, by, 5, 8, lcdGhost);
        }
    }
    if (isChg) {
        int cxBolt = lx + 70;
        spr->drawLine(cxBolt + 2, by, cxBolt, by + 4, lcdInk);
        spr->drawLine(cxBolt, by + 4, cxBolt + 3, by + 4, lcdInk);
        spr->drawLine(cxBolt + 3, by + 4, cxBolt + 1, by + 8, lcdInk);
    } else {
        spr->drawString(String(akku) + "%", lx + 70, by, 1);
    }

    // Step Odometer on the Right
    spr->setTextDatum(TR_DATUM);
    spr->setTextColor(lcdInk, lcdBg);
    char stpBuf[16];
    snprintf(stpBuf, sizeof(stpBuf), "STP %05d", stepCount);
    spr->drawString(stpBuf, lx + lw - 8, by, 2);
}

void drawUltraDashboard(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg) {
    if (!spr || spr->getPointer() == nullptr) return;
    time_t jetzt; time(&jetzt);
    int wday = (aktuell->tm_wday >= 0 && aktuell->tm_wday <= 6) ? aktuell->tm_wday : 0;
    int mon  = (aktuell->tm_mon >= 0 && aktuell->tm_mon <= 11) ? aktuell->tm_mon : 0;
    int mday = (aktuell->tm_mday >= 1 && aktuell->tm_mday <= 31) ? aktuell->tm_mday : 1;

    long diff = 0;
    if (zielZeit.tm_year > 0) {
        time_t zt = mktime(&zielZeit);
        if (zt != (time_t)-1 && zt > jetzt) {
            diff = zt - jetzt;
        }
    }
    int yearsLeft = (int)(diff / 31536000L);
    int daysLeft  = (int)((diff % 31536000L) / 86400L);

    // === 1. TOP PILL: Datum & Wochentag ===
    static const char* monKurz[] = {"Jan", "Feb", "Mrz", "Apr", "Mai", "Jun", "Jul", "Aug", "Sep", "Okt", "Nov", "Dez"};
    char dPill[32];
    snprintf(dPill, sizeof(dPill), "%s, %02d. %s", wochentage[wday], mday, monKurz[mon]);
    
    drawGlassCard(spr, 28, 4, 184, 22, 11, COLOR_BORDER_HI, COLOR_SURFACE, COLOR_BG_DARK);
    spr->fillCircle(40, 15, 3, COLOR_CYAN_TECH);
    spr->fillCircle(40, 15, 1, COLOR_TEXT_MAIN);
    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(COLOR_ACCENT_TIME);
    spr->drawString(dPill, 126, 15, 2);

    // === 2. HERO TIME: Apple Watch Ultra Digital Clock ===
    char tBuf[8];
    snprintf(tBuf, sizeof(tBuf), "%02d:%02d", aktuell->tm_hour, aktuell->tm_min);
    spr->setTextDatum(MR_DATUM);
    spr->setTextColor(COLOR_TEXT_MAIN, cBG);
    spr->drawString(tBuf, 148, 48, 7);

    char sBuf[6];
    snprintf(sBuf, sizeof(sBuf), ":%02d", aktuell->tm_sec);
    spr->setTextDatum(ML_DATUM);
    spr->setTextColor(COLOR_CYAN_TECH, cBG);
    spr->drawString(sBuf, 152, 50, 4);

    // === 3. CHROMATIC ACTIVITY RINGS ===
    int goal = (stepGoal > 0) ? stepGoal : 8000;
    float stepRatio = (goal > 0) ? ((float)stepCount / (float)goal) : 0.0f;
    float batRatio  = (float)akku / 100.0f;
    float timeRatio = (float)(aktuell->tm_hour * 60 + aktuell->tm_min) / 1440.0f;
    drawActivityRings(spr, 48, 114, stepRatio, batRatio, timeRatio);

    // === 4. COMPLICATION CARDS (Rechts neben den Ringen) ===
    int cX = 98, cW = 132;
    
    // Schritte Card
    drawGlassCard(spr, cX, 74, cW, 36, 6, COLOR_BORDER, COLOR_SURFACE, COLOR_BG_DARK);
    spr->fillCircle(cX + 12, 88, 3, COLOR_PROD_GREEN);
    spr->fillCircle(cX + 12, 88, 1, COLOR_TEXT_MAIN);
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(COLOR_TEXT_MAIN);
    spr->drawString(String(stepCount) + " Steps", cX + 22, 78, 2);
    int sBarW = cW - 28;
    int sFillW = (int)(stepRatio * sBarW);
    if (sFillW > sBarW) sFillW = sBarW;
    spr->fillRoundRect(cX + 22, 98, sBarW, 4, 2, COLOR_SURFACE_HI);
    if (sFillW > 1) spr->fillRoundRect(cX + 22, 98, sFillW, 4, 2, COLOR_PROD_GREEN);

    // Akku & Power Card
    drawGlassCard(spr, cX, 116, cW, 36, 6, isChg ? COLOR_AMBER_WARN : COLOR_BORDER, COLOR_SURFACE, COLOR_BG_DARK);
    spr->fillCircle(cX + 12, 130, 3, isChg ? COLOR_AMBER_WARN : COLOR_PROD_GREEN);
    spr->fillCircle(cX + 12, 130, 1, COLOR_TEXT_MAIN);
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(COLOR_TEXT_MAIN);
    spr->drawString(String(akku) + "% Akku", cX + 22, 120, 2);
    spr->setTextColor(COLOR_TEXT_MUTED);
    String voltPwr = String(getBatteryVoltageCached(false), 2) + "V  " + (isChg ? "Laden ⚡" : "Akku");
    spr->drawString(voltPwr, cX + 22, 136, 1);
    if (isChg) {
        int lX = cX + cW - 14, lY = 124;
        spr->drawLine(lX + 3, lY, lX, lY + 5, COLOR_AMBER_WARN);
        spr->drawLine(lX, lY + 5, lX + 4, lY + 5, COLOR_AMBER_WARN);
        spr->drawLine(lX + 4, lY + 5, lX + 1, lY + 10, COLOR_TEXT_MAIN);
    }

    // === 5. BOTTOM PILL: Lebensziel-Vorschau ===
    drawGlassCard(spr, 12, 160, 216, 26, 8, COLOR_BORDER, COLOR_SURFACE, COLOR_BG_DARK);
    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(COLOR_CYAN_TECH);
    char goalPill[44];
    snprintf(goalPill, sizeof(goalPill), "Lebensziel: %dJ %d Tage  >", yearsLeft, daysLeft);
    spr->drawString(goalPill, 120, 173, 2);
}

void drawLifetimeWatchface(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg) {
    if (!spr || spr->getPointer() == nullptr) return;
    time_t jetzt; time(&jetzt);
    long diff = 0;
    if (zielZeit.tm_year > 0) {
        time_t zt = mktime(&zielZeit);
        if (zt != (time_t)-1 && zt > jetzt) {
            diff = zt - jetzt;
        }
    }
    int vals[] = {(int)(diff / 31536000L), (int)((diff % 31536000L) / 2592000L), (int)((diff % 2592000L) / 86400L), (int)((diff % 86400L) / 3600L), (int)((diff % 3600L) / 60L), (int)(diff % 60L)};
    const char* labs[] = {"Jahre", "Monate", "Tage", "Stunden", "Minuten", "Sekunden"};
    int rowH = 28; 
    int sH = 20; int sW = 12; 
    for(int i = 0; i < 6; i++) { 
        int y = 4 + i * (rowH + 3);
        drawGlassCard(spr, 8, y, 196, rowH, 5, (i < 3) ? COLOR_BORDER_HI : COLOR_BORDER, COLOR_SURFACE, COLOR_BG_DARK);
        char buf[5]; snprintf(buf, sizeof(buf), "%02d", vals[i]); 
        draw7SegString(*spr, String(buf), 16, y + 4, sH, sW, (i < 3) ? COLOR_TEXT_MAIN : COLOR_ACCENT_TIME, 3); 
        spr->setTextDatum(ML_DATUM); 
        spr->setTextColor((i < 3) ? COLOR_CYAN_TECH : COLOR_TEXT_MUTED); 
        spr->drawString(labs[i], 66, y + rowH/2, 2); 
    }
    
    // Lebensziel Progress Bar Rechts mit Säule
    float aktAlter = (float)(aktuell->tm_year + 1900 - gJahr) + (float)aktuell->tm_yday/365.0f; 
    float prozent = (zielAlter > 0) ? (aktAlter / (float)zielAlter) : 0.0f; 
    if (prozent > 1.0f) prozent = 1.0f;
    if (prozent < 0.0f) prozent = 0.0f;
    int fullBarH = 183; 
    int fillH = (int)(fullBarH * prozent); 
    drawGlassCard(spr, 212, 4, 20, fullBarH, 6, COLOR_BORDER, COLOR_SURFACE, COLOR_BG_DARK);
    uint16_t pCol = (prozent > 0.8f) ? COLOR_SEC_RED : (prozent > 0.5f ? COLOR_AMBER_WARN : COLOR_PROD_GREEN);
    if (fillH > 4) {
        spr->fillRoundRect(214, (4 + fullBarH) - fillH, 16, fillH, 5, pCol);
        spr->drawFastHLine(215, (4 + fullBarH) - fillH + 1, 14, COLOR_TEXT_MAIN);
    }
}

void drawBauhausWatchface(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg) {
    if (!spr || spr->getPointer() == nullptr) return;
    const float DEG2RAD = 0.0174532925f;
    int cx = 120, cy = 103;

    // 1. Bauhaus Canvas: Warm Minimalist Charcoal Dial
    spr->fillSprite(0x10A2); // Charcoal Bauhaus canvas
    spr->drawCircle(cx, cy, 102, 0x2965); // Outer minimalist frame
    spr->drawCircle(cx, cy, 101, 0x18C3);

    // 2. Minimalist Hour Batons & Circular Accent Pips
    for (int m = 0; m < 60; m++) {
        float rad = (m * 6.0f) * DEG2RAD - 1.5707963f;
        float c = cosf(rad);
        float s = sinf(rad);

        if (m % 5 == 0) {
            int h = m / 5;
            if (h == 0 || h == 3 || h == 6 || h == 9) {
                // Cardinal Hours: Solid 2px baton + Cardinal Pip
                int rIn = 82, rOut = 95;
                spr->drawLine(cx + (int)(rIn * c), cy + (int)(rIn * s), cx + (int)(rOut * c), cy + (int)(rOut * s), TFT_WHITE);
                spr->drawLine(cx + (int)(rIn * c) + 1, cy + (int)(rIn * s), cx + (int)(rOut * c) + 1, cy + (int)(rOut * s), TFT_WHITE);
                // Circular Bauhaus Pip
                int pipR = 76;
                spr->fillCircle(cx + (int)(pipR * c), cy + (int)(pipR * s), 2, 0xF800); // Bauhaus Red pip
            } else {
                // Regular Hour Batons: Crisp 1px
                int rIn = 86, rOut = 95;
                spr->drawLine(cx + (int)(rIn * c), cy + (int)(rIn * s), cx + (int)(rOut * c), cy + (int)(rOut * s), 0xCE79);
            }
        } else {
            // Fine 1px minute dots
            int r = 94;
            spr->drawPixel(cx + (int)(r * c), cy + (int)(r * s), 0x4208);
        }
    }

    // 3. Typographic Cardinal Markers (Max Bill style 12, 3, 6, 9)
    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(0xFFFF, 0x10A2);
    spr->drawString("12", cx, cy - 64, 2);
    spr->drawString("3", cx + 64, cy, 2);
    spr->drawString("6", cx, cy + 64, 2);
    spr->drawString("9", cx - 64, cy, 2);

    // 4. Bauhaus Inscription
    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(0x8410, 0x10A2);
    spr->drawString("BAUHAUS", cx, cy - 32, 1);
    spr->drawString("AUTOMATIK", cx, cy - 22, 1);
    spr->drawString("GERMANY", cx, cy + 42, 1);

    // 5. Date Aperture (at 3 o'clock)
    char dBuf[6];
    snprintf(dBuf, sizeof(dBuf), "%02d", aktuell->tm_mday);
    drawGlassCard(spr, cx + 24, cy - 8, 22, 16, 3, 0x39E7, 0x1CE7, 0x10A2);
    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(0xF800, 0x10A2); // Red date digit
    spr->drawString(dBuf, cx + 35, cy + 1, 1);

    // 6. Hands
    // Hour Hand (Matte Black with White Core, Length 50)
    float hRad = ((aktuell->tm_hour % 12) + aktuell->tm_min / 60.0f) * 30.0f * DEG2RAD - 1.5707963f;
    float hLen = 50.0f;
    spr->drawLine(cx, cy, cx + (int)(hLen * cosf(hRad)), cy + (int)(hLen * sinf(hRad)), TFT_WHITE);
    spr->drawLine(cx - 1, cy, cx + (int)(hLen * cosf(hRad)) - 1, cy + (int)(hLen * sinf(hRad)), TFT_WHITE);
    spr->drawLine(cx + 1, cy, cx + (int)(hLen * cosf(hRad)) + 1, cy + (int)(hLen * sinf(hRad)), 0xCE79);

    // Minute Hand (Slender white baton, Length 76)
    float mRad = (aktuell->tm_min + aktuell->tm_sec / 60.0f) * 6.0f * DEG2RAD - 1.5707963f;
    float mLen = 76.0f;
    spr->drawLine(cx, cy, cx + (int)(mLen * cosf(mRad)), cy + (int)(mLen * sinf(mRad)), TFT_WHITE);
    spr->drawLine(cx + (int)(-sinf(mRad)), cy + (int)(cosf(mRad)),
                  cx + (int)(mLen * cosf(mRad)) + (int)(-sinf(mRad)), cy + (int)(mLen * sinf(mRad)) + (int)(cosf(mRad)), 0xCE79);

    // Sweeping Bauhaus Red Second Hand with circular eye
    float sRad = aktuell->tm_sec * 6.0f * DEG2RAD - 1.5707963f;
    float sLen = 86.0f;
    int sx = cx + (int)(sLen * cosf(sRad));
    int sy = cy + (int)(sLen * sinf(sRad));
    int tailX = cx - (int)(18.0f * cosf(sRad));
    int tailY = cy - (int)(18.0f * sinf(sRad));
    spr->drawLine(tailX, tailY, sx, sy, 0xF800);
    int eyeX = cx - (int)(12.0f * cosf(sRad));
    int eyeY = cy - (int)(12.0f * sinf(sRad));
    spr->fillCircle(eyeX, eyeY, 3, 0xF800);
    spr->fillCircle(eyeX, eyeY, 1, 0x10A2);

    // Center Pin Cap
    spr->fillCircle(cx, cy, 4, 0xCE79);
    spr->fillCircle(cx, cy, 2, 0xF800);
}

void drawCyberpunkWatchface(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg) {
    if (!spr || spr->getPointer() == nullptr) return;

    // 1. Matrix/Cyber Background with Grid Lines
    spr->fillSprite(0x0000);
    for (int y = 20; y < 200; y += 30) {
        spr->drawFastHLine(0, y, 240, 0x0821);
    }
    for (int x = 20; x < 240; x += 40) {
        spr->drawFastVLine(x, 0, 206, 0x0821);
    }

    // 2. Cyber HUD Corner Brackets
    uint16_t hudCol = 0x07FF; // Cyan
    spr->drawFastHLine(10, 8, 16, hudCol); spr->drawFastVLine(10, 8, 16, hudCol);
    spr->drawFastHLine(214, 8, 16, hudCol); spr->drawFastVLine(229, 8, 16, hudCol);
    spr->drawFastHLine(10, 192, 16, hudCol); spr->drawFastVLine(10, 176, 16, hudCol);
    spr->drawFastHLine(214, 192, 16, hudCol); spr->drawFastVLine(229, 176, 16, hudCol);

    // 3. Top Cyberpunk Terminal Header
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(0x07E0, 0x0000); // Neon Green
    spr->drawString("// SYS.TERMINAL // v2.4", 20, 10, 1);

    spr->setTextDatum(TR_DATUM);
    spr->setTextColor(isChg ? 0xFFE0 : 0x07FF, 0x0000);
    spr->drawString(isChg ? "PWR:CHG" : "SEC:MAX", 220, 10, 1);

    // 4. Hero Digital Time [ 14:32:05 ]
    char tBuf[10];
    snprintf(tBuf, sizeof(tBuf), "%02d:%02d", aktuell->tm_hour, aktuell->tm_min);
    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(0x07FF, 0x0000);
    spr->drawString("[", 32, 54, 7);
    spr->drawString("]", 208, 54, 7);
    spr->setTextColor(0xFFFF, 0x0000);
    spr->drawString(tBuf, 114, 54, 7);

    char sBuf[6];
    snprintf(sBuf, sizeof(sBuf), ":%02d", aktuell->tm_sec);
    spr->setTextDatum(ML_DATUM);
    spr->setTextColor(0xFD20, 0x0000); // Neon Amber
    spr->drawString(sBuf, 168, 54, 4);

    // 5. Date Strip
    static const char* cpWday[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    char dLine[36];
    snprintf(dLine, sizeof(dLine), "DATE: %04d.%02d.%02d // %s", aktuell->tm_year + 1900, aktuell->tm_mon + 1, aktuell->tm_mday, cpWday[aktuell->tm_wday % 7]);
    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(0x8410, 0x0000);
    spr->drawString(dLine, 120, 92, 1);

    // 6. Telemetry Status Modules (4 Grid Rows)
    int yStart = 106;
    int rowH = 19;

    // CPU Telemetry
    drawGlassCard(spr, 16, yStart, 208, rowH - 2, 3, 0x18C3, 0x0821, 0x0000);
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(0x07E0, 0x0000);
    char cpuBuf[32];
    snprintf(cpuBuf, sizeof(cpuBuf), "CPU: %dMHz | %.1fC", ESP.getCpuFreqMHz(), debug_rms > 0 ? (float)getDecibels() : 41.2f);
    spr->drawString(cpuBuf, 22, yStart + 2, 1);
    int cpuBarW = 60;
    int cpuFill = (ESP.getCpuFreqMHz() * cpuBarW) / 240;
    spr->drawRect(156, yStart + 3, cpuBarW, 8, 0x2104);
    spr->fillRect(158, yStart + 5, cpuFill > 4 ? cpuFill - 4 : 0, 4, 0x07E0);

    // RAM Telemetry
    drawGlassCard(spr, 16, yStart + rowH, 208, rowH - 2, 3, 0x18C3, 0x0821, 0x0000);
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(0x07FF, 0x0000);
    char ramBuf[32];
    snprintf(ramBuf, sizeof(ramBuf), "RAM: %dK / %dK", (int)(ESP.getFreeHeap() / 1024), (int)(ESP.getHeapSize() / 1024));
    spr->drawString(ramBuf, 22, yStart + rowH + 2, 1);
    int ramBarW = 60;
    int ramFill = (int)(((float)ESP.getFreeHeap() / (float)ESP.getHeapSize()) * ramBarW);
    spr->drawRect(156, yStart + rowH + 3, ramBarW, 8, 0x2104);
    spr->fillRect(158, yStart + rowH + 5, ramFill > 4 ? ramFill - 4 : 0, 4, 0x07FF);

    // Battery / Power Telemetry
    drawGlassCard(spr, 16, yStart + rowH * 2, 208, rowH - 2, 3, 0x18C3, 0x0821, 0x0000);
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(isChg ? 0xFFE0 : (akku > 30 ? 0x07E0 : 0xF800), 0x0000);
    char pwrBuf[32];
    snprintf(pwrBuf, sizeof(pwrBuf), "PWR: %.2fV [%d%%] %s", getBatteryVoltageCached(false), akku, isChg ? "+CHG" : "");
    spr->drawString(pwrBuf, 22, yStart + rowH * 2 + 2, 1);
    int pwrBarW = 60;
    int pwrFill = (akku * pwrBarW) / 100;
    spr->drawRect(156, yStart + rowH * 2 + 3, pwrBarW, 8, 0x2104);
    spr->fillRect(158, yStart + rowH * 2 + 5, pwrFill > 4 ? pwrFill - 4 : 0, 4, isChg ? 0xFFE0 : 0x07E0);

    // Steps Telemetry
    drawGlassCard(spr, 16, yStart + rowH * 3, 208, rowH - 2, 3, 0x18C3, 0x0821, 0x0000);
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(0xFD20, 0x0000);
    char stpBuf[32];
    int goal = (stepGoal > 0) ? stepGoal : 8000;
    snprintf(stpBuf, sizeof(stpBuf), "STP: %d / %d", stepCount, goal);
    spr->drawString(stpBuf, 22, yStart + rowH * 3 + 2, 1);
    int stpBarW = 60;
    int stpFill = (stepCount * stpBarW) / goal;
    if (stpFill > stpBarW) stpFill = stpBarW;
    spr->drawRect(156, yStart + rowH * 3 + 3, stpBarW, 8, 0x2104);
    spr->fillRect(158, yStart + rowH * 3 + 5, stpFill > 4 ? stpFill - 4 : 0, 4, 0xFD20);
}

void drawWordClockWatchface(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg) {
    if (!spr || spr->getPointer() == nullptr) return;

    // 1. Deep Matte Dark Background
    spr->fillSprite(0x0821);

    // 2. German Qlocktwo Word Logic
    int h = aktuell->tm_hour;
    int m = aktuell->tm_min;

    // In German: from minute 25 onwards ("fünf vor halb"), the next hour is mentioned
    int displayHour = h;
    if (m >= 25) displayHour = (displayHour + 1) % 24;
    displayHour = displayHour % 12;
    if (displayHour == 0) displayHour = 12;

    int minMod = m % 5; // 0, 1, 2, 3, 4 extra minute corner dots
    int mBlock = m / 5; // 0..11

    bool wES = true;
    bool wIST = true;
    bool wFUENF_M = (mBlock == 1 || mBlock == 5 || mBlock == 7 || mBlock == 11);
    bool wZEHN_M = (mBlock == 2 || mBlock == 10);
    bool wZWANZIG = (mBlock == 4 || mBlock == 8);
    bool wVIERTEL = (mBlock == 3 || mBlock == 9);
    bool wVOR = (mBlock == 5 || mBlock == 8 || mBlock == 9 || mBlock == 10 || mBlock == 11);
    bool wNACH = (mBlock == 1 || mBlock == 2 || mBlock == 3 || mBlock == 4 || mBlock == 7);
    bool wHALB = (mBlock >= 5 && mBlock <= 7);

    bool wEINS = (displayHour == 1);
    bool wZWEI = (displayHour == 2);
    bool wDREI = (displayHour == 3);
    bool wVIER = (displayHour == 4);
    bool wFUENF_H = (displayHour == 5);
    bool wSECHS = (displayHour == 6);
    bool wSIEBEN = (displayHour == 7);
    bool wACHT = (displayHour == 8);
    bool wNEUN = (displayHour == 9);
    bool wZEHN_H = (displayHour == 10);
    bool wELF = (displayHour == 11);
    bool wZWOELF = (displayHour == 12);
    bool wUHR = (mBlock == 0);

    uint16_t cActive = 0xFFFF; // Bright Crisp White
    uint16_t cDim = 0x2104;    // Subtle Dim Charcoal

    auto drawWord = [&](const char* text, int x, int y, bool active) {
        spr->setTextDatum(ML_DATUM);
        spr->setTextColor(active ? cActive : cDim, 0x0821);
        spr->drawString(text, x, y, 2);
    };

    // Matrix Layout (7 rows, height 206px):
    drawWord("ES", 14, 16, wES);
    drawWord("IST", 52, 16, wIST);
    drawWord("FÜNF", 154, 16, wFUENF_M);

    drawWord("ZEHN", 14, 42, wZEHN_M);
    drawWord("ZWANZIG", 114, 42, wZWANZIG);

    drawWord("VIERTEL", 14, 68, wVIERTEL);
    drawWord("VOR", 126, 68, wVOR);
    drawWord("NACH", 172, 68, wNACH);

    drawWord("HALB", 14, 94, wHALB);
    drawWord("ZWÖLF", 88, 94, wZWOELF);
    drawWord("ZWEI", 168, 94, wZWEI);

    drawWord((wUHR && wEINS) ? "EIN" : "EINS", 14, 120, wEINS);
    drawWord("DREI", 86, 120, wDREI);
    drawWord("VIER", 158, 120, wVIER);

    drawWord("SECHS", 14, 146, wSECHS);
    drawWord("ACHT", 92, 146, wACHT);
    drawWord("SIEBEN", 154, 146, wSIEBEN);

    drawWord("NEUN", 14, 172, wNEUN);
    drawWord("ZEHN", 76, 172, wZEHN_H);
    drawWord("ELF", 136, 172, wELF);
    drawWord("UHR", 182, 172, wUHR);

    // 3. 4 Corner Indicator Dots for Single Minutes
    int dotR = 3;
    uint16_t cDotActive = 0x07FF; // Cyan Glow
    uint16_t cDotDim = 0x18C3;
    spr->fillCircle(8, 8, dotR, minMod >= 1 ? cDotActive : cDotDim);     // Top-Left: +1
    spr->fillCircle(232, 8, dotR, minMod >= 2 ? cDotActive : cDotDim);   // Top-Right: +2
    spr->fillCircle(232, 198, dotR, minMod >= 3 ? cDotActive : cDotDim); // Bottom-Right: +3
    spr->fillCircle(8, 198, dotR, minMod >= 4 ? cDotActive : cDotDim);   // Bottom-Left: +4
}

void drawFinanceWatchface(TFT_eSprite *spr, struct tm *aktuell) {
    if (!spr || spr->getPointer() == nullptr) return;
    spr->setTextDatum(TC_DATUM);
    spr->setTextColor(COLOR_TEXT_MAIN);
    spr->drawString("Opportunitaet", 120, 8, 4);
    
    float dailyNet = (netSalary * 12.0) / 365.0;
    float fraction = (millis() % 1000) / 1000.0;
    long secToday = aktuell->tm_hour * 3600 + aktuell->tm_min * 60 + aktuell->tm_sec;
    float earnedToday = dailyNet * ((secToday + fraction) / 86400.0);
    
    // Karte 1: Heutiger Verdienst
    drawGlassCard(spr, 12, 38, 216, 68, 10, COLOR_BORDER, COLOR_SURFACE, COLOR_BG_DARK);
    
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(COLOR_TEXT_MUTED);
    spr->drawString("Heute verdient", 22, 46, 2);
    
    char moneyBuf[16];
    snprintf(moneyBuf, sizeof(moneyBuf), "%.4f", earnedToday);
    int mH = 26; int mW = 15; int mSpace = 4;
    int totalW = get7SegWidth(String(moneyBuf), mH, mW, mSpace);
    draw7SegString(*spr, String(moneyBuf), 22, 68, mH, mW, COLOR_PROD_GREEN, mSpace);
    
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(COLOR_PROD_GREEN);
    spr->drawString("EUR", 22 + totalW + 6, 74, 2);

    // Karte 2: Echter Stundenlohn
    float workDaysPerYear = workDaysYear - vacationDays - sickDays;
    float workHoursPerYear = workDaysPerYear * workHoursPerDay * (partTimePct / 100.0);
    float trueHourlyWage = 0;
    if (workHoursPerYear > 0) trueHourlyWage = (netSalary * 12.0) / workHoursPerYear;
    
    drawGlassCard(spr, 12, 114, 216, 68, 10, COLOR_BORDER, COLOR_SURFACE, COLOR_BG_DARK);
    
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(COLOR_TEXT_MUTED);
    spr->drawString("Echter Stundenlohn", 22, 122, 2);
    
    char hrBuf[16];
    snprintf(hrBuf, sizeof(hrBuf), "%.2f", trueHourlyWage); 
    int hH = 24; int hW = 14; int hSpace = 3;
    int hTotalW = get7SegWidth(String(hrBuf), hH, hW, hSpace);
    draw7SegString(*spr, String(hrBuf), 22, 144, hH, hW, COLOR_CYAN_TECH, hSpace);
    
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(COLOR_CYAN_TECH);
    spr->drawString("EUR/h", 22 + hTotalW + 6, 150, 2);
}

void drawChildrenWatchface(TFT_eSprite *spr, struct tm *aktuell) {
    if (!spr || spr->getPointer() == nullptr) return;
    time_t jetzt; time(&jetzt);
    int activeKids = 0; String kNames[3]; String kDates[3]; String kTimes[3];
    if(child1Name.length() > 0 && child1Date.length() == 10) { kNames[activeKids] = child1Name; kDates[activeKids] = child1Date; kTimes[activeKids] = child1Time; activeKids++; }
    if(child2Name.length() > 0 && child2Date.length() == 10) { kNames[activeKids] = child2Name; kDates[activeKids] = child2Date; kTimes[activeKids] = child2Time; activeKids++; }
    if(child3Name.length() > 0 && child3Date.length() == 10) { kNames[activeKids] = child3Name; kDates[activeKids] = child3Date; kTimes[activeKids] = child3Time; activeKids++; }
    if (activeKids > 0) {
        int yName = 18, yJahre = 60, yMonate = 100, yWochen = 140, yTage = 180;
        spr->setTextDatum(MC_DATUM); spr->setTextColor(COLOR_TEXT_MUTED);
        if (activeKids <= 2) {
            spr->drawString("Jahre", 120, yJahre, 2);
            spr->drawString("Monate", 120, yMonate, 2);
            spr->drawString("Wochen", 120, yWochen, 2);
            spr->drawString("Tage", 120, yTage, 2);
        }
        for(int i=0; i<activeKids; i++) {
            int xCenter = 120;
            if (activeKids == 2) xCenter = (i == 0) ? 45 : 195;
            else if (activeKids == 3) xCenter = 40 + (i * 80);
            
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(COLOR_ACCENT_TIME);
            spr->drawString(kNames[i], xCenter, yName, 4); 
            
            time_t bTime = stringToTime(kDates[i], kTimes[i]);
            long diff = jetzt - bTime; if (diff < 0) diff = 0;
            long dDays = diff / 86400; long dWeeks = dDays / 7;
            int bYear = kDates[i].substring(0, 4).toInt();
            int bMon = kDates[i].substring(5, 7).toInt();
            int bDay = kDates[i].substring(8, 10).toInt();
            int dYears = (aktuell->tm_year + 1900) - bYear;
            if ((aktuell->tm_mon + 1) < bMon || ((aktuell->tm_mon + 1) == bMon && aktuell->tm_mday < bDay)) dYears--;
            if (dYears < 0) dYears = 0;
            int dMonths = ((aktuell->tm_year + 1900) - bYear) * 12 + ((aktuell->tm_mon + 1) - bMon);
            if (aktuell->tm_mday < bDay) dMonths--;
            if (dMonths < 0) dMonths = 0;
            
            spr->setTextColor(COLOR_TEXT_MAIN);
            spr->drawString(String(dYears), xCenter, yJahre, 4);
            spr->drawString(String(dMonths), xCenter, yMonate, 4);
            spr->drawString(String(dWeeks), xCenter, yWochen, 4);
            spr->drawString(String(dDays), xCenter, yTage, 4);
        }
    } else {
        drawGlassCard(spr, 20, 45, 200, 116, 12, COLOR_BORDER, COLOR_SURFACE, COLOR_BG_DARK);
        spr->fillCircle(120, 72, 14, COLOR_SURFACE_HI);
        spr->drawCircle(120, 72, 14, COLOR_BORDER_HI);
        spr->setTextDatum(MC_DATUM);
        spr->setTextColor(COLOR_ACCENT_TIME);
        spr->drawString("+", 120, 72, 2);
        spr->setTextColor(COLOR_TEXT_MAIN);
        spr->drawString("Keine Kinder", 120, 104, 2);
        spr->setTextColor(COLOR_TEXT_MUTED);
        spr->drawString("Im WebDashboard anlegen", 120, 126, 1);
    }
}

void drawParentsWatchface(TFT_eSprite *spr, struct tm *aktuell) {
    if (!spr || spr->getPointer() == nullptr) return;
    time_t jetzt; time(&jetzt);
    char uB[12]; char uT[6]; snprintf(uB, sizeof(uB), "%04d-%02d-%02d", gJahr, gMonat, gTag); snprintf(uT, sizeof(uT), "%02d:%02d", gStunde, gMinute);
    time_t uTime = stringToTime(String(uB), String(uT));
    time_t p1Time = (parentName.length() > 0) ? stringToTime(parentDate, parentTime) : 0;
    time_t p2Time = (papaName.length() > 0) ? stringToTime(papaDate, papaTime) : 0;
    const char* monateKurz[] = {"Jan", "Feb", "Mrz", "Apr", "Mai", "Jun", "Jul", "Aug", "Sep", "Okt", "Nov", "Dez"};

    // Header Switcher Pill (oben zentriert): Zeigt aktuellen Modus an
    drawGlassCard(spr, 24, 2, 192, 20, 10, COLOR_BORDER, COLOR_SURFACE, COLOR_BG_DARK);
    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(parentViewMode == 0 ? COLOR_ACCENT_TIME : COLOR_CYAN_TECH);
    spr->drawString(parentViewMode == 0 ? "ELTERN-SICHT \u21C4 (Tippen)" : "KINDER-SICHT \u21C4 (Tippen)", 120, 12, 1);

    if (parentViewMode == 0) {
        // === MODUS 0: Wie alt waren meine Eltern, als sie so alt waren wie ich heute ===
        if (uTime != 0 && p1Time != 0 && p2Time != 0) {
            int64_t myAgeSec = (int64_t)jetzt - (int64_t)uTime;
            struct tm uTm; localtime_r(&uTime, &uTm);

            auto drawParentCard = [&](int yCard, const String &pName, time_t pTime, uint16_t borderCol) {
                time_t targetDate = (time_t)((int64_t)pTime + myAgeSec);
                struct tm tgt; localtime_r(&targetDate, &tgt);
                int yearsThen = (tgt.tm_year + 1900) - (uTm.tm_year + 1900);
                if (tgt.tm_mon < uTm.tm_mon || (tgt.tm_mon == uTm.tm_mon && tgt.tm_mday < uTm.tm_mday)) yearsThen--;
                char dBuf[28];
                snprintf(dBuf, sizeof(dBuf), "%02d. %s %04d", tgt.tm_mday, monateKurz[tgt.tm_mon], tgt.tm_year + 1900);

                drawGlassCard(spr, 10, yCard, 220, 78, 10, borderCol, COLOR_SURFACE, COLOR_BG_DARK);
                spr->setTextDatum(TL_DATUM);
                spr->setTextColor(COLOR_TEXT_MUTED);
                spr->drawString("Heute war fuer " + pName, 20, yCard + 7, 2);
                spr->setTextColor(COLOR_TEXT_MAIN);
                spr->drawString(dBuf, 20, yCard + 26, 4);
                spr->setTextColor(borderCol);
                if (yearsThen < 0) spr->drawString("Du: noch nicht geb.", 20, yCard + 56, 2);
                else spr->drawString("Du warst " + String(yearsThen) + " Jahre", 20, yCard + 56, 2);
            };

            drawParentCard(26, parentName, p1Time, COLOR_ACCENT_TIME);
            drawParentCard(108, papaName, p2Time, COLOR_CYAN_TECH);
        } else if (uTime != 0 && (p1Time != 0 || p2Time != 0)) {
            time_t pTime = (p1Time != 0) ? p1Time : p2Time;
            String pName = (p1Time != 0) ? parentName : papaName;
            int64_t myAgeSec = (int64_t)jetzt - (int64_t)uTime;
            time_t targetDate = (time_t)((int64_t)pTime + myAgeSec);
            struct tm tgt; localtime_r(&targetDate, &tgt);
            char dBuf[30]; snprintf(dBuf, sizeof(dBuf), "%02d. %s %04d", tgt.tm_mday, monateKurz[tgt.tm_mon], tgt.tm_year+1900);
            struct tm uTm; localtime_r(&uTime, &uTm);
            int yearsThen = (tgt.tm_year + 1900) - (uTm.tm_year + 1900);
            if (tgt.tm_mon < uTm.tm_mon || (tgt.tm_mon == uTm.tm_mon && tgt.tm_mday < uTm.tm_mday)) yearsThen--;
            
            drawGlassCard(spr, 12, 26, 216, 160, 12, COLOR_BORDER, COLOR_SURFACE, COLOR_BG_DARK);
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(COLOR_TEXT_MUTED);
            spr->drawString("Heute war fuer " + pName, 120, 48, 2);
            spr->drawString("in deinem Alter der", 120, 68, 2);
            spr->setTextColor(COLOR_TEXT_MAIN);
            spr->drawString(dBuf, 120, 102, 4);
            spr->setTextColor(COLOR_TEXT_MUTED);
            spr->drawString("An diesem Tag warst Du", 120, 140, 2);
            spr->setTextColor(COLOR_ACCENT_TIME);
            if (yearsThen < 0) spr->drawString("Noch nicht geboren", 120, 165, 2);
            else spr->drawString(String(yearsThen) + " Jahre", 120, 165, 4);
        } else {
            drawGlassCard(spr, 20, 40, 200, 120, 12, COLOR_BORDER, COLOR_SURFACE, COLOR_BG_DARK);
            spr->fillCircle(120, 70, 14, COLOR_SURFACE_HI);
            spr->drawCircle(120, 70, 14, COLOR_BORDER_HI);
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(COLOR_ACCENT_TIME);
            spr->drawString("+", 120, 70, 2);
            spr->setTextColor(COLOR_TEXT_MAIN);
            spr->drawString("Keine Elterndaten", 120, 102, 2);
            spr->setTextColor(COLOR_TEXT_MUTED);
            spr->drawString("Im WebDashboard anlegen", 120, 124, 1);
        }
    } else {
        // === MODUS 1: Wie alt bin ich, wenn meine Kinder so alt sind wie ich heute ===
        int activeKids = 0; String kNames[3]; String kDates[3]; String kTimes[3];
        if(child1Name.length() > 0 && child1Date.length() == 10) { kNames[activeKids] = child1Name; kDates[activeKids] = child1Date; kTimes[activeKids] = child1Time; activeKids++; }
        if(child2Name.length() > 0 && child2Date.length() == 10) { kNames[activeKids] = child2Name; kDates[activeKids] = child2Date; kTimes[activeKids] = child2Time; activeKids++; }
        if(child3Name.length() > 0 && child3Date.length() == 10) { kNames[activeKids] = child3Name; kDates[activeKids] = child3Date; kTimes[activeKids] = child3Time; activeKids++; }

        if (uTime != 0 && activeKids > 0) {
            int64_t myAgeSec = (int64_t)jetzt - (int64_t)uTime;
            struct tm uTm; localtime_r(&uTime, &uTm);
            int myCurYears = (aktuell->tm_year + 1900) - (uTm.tm_year + 1900);
            if (aktuell->tm_mon < uTm.tm_mon || (aktuell->tm_mon == uTm.tm_mon && aktuell->tm_mday < uTm.tm_mday)) myCurYears--;

            if (activeKids == 1) {
                time_t kTime = stringToTime(kDates[0], kTimes[0]);
                time_t targetDate = (time_t)((int64_t)kTime + myAgeSec);
                struct tm tgt; localtime_r(&targetDate, &tgt);
                int myYearsThen = (tgt.tm_year + 1900) - (uTm.tm_year + 1900);
                if (tgt.tm_mon < uTm.tm_mon || (tgt.tm_mon == uTm.tm_mon && tgt.tm_mday < uTm.tm_mday)) myYearsThen--;
                char dBuf[30]; snprintf(dBuf, sizeof(dBuf), "%02d. %s %04d", tgt.tm_mday, monateKurz[tgt.tm_mon], tgt.tm_year + 1900);

                drawGlassCard(spr, 12, 26, 216, 160, 12, COLOR_PROD_GREEN, COLOR_SURFACE, COLOR_BG_DARK);
                spr->setTextDatum(MC_DATUM);
                spr->setTextColor(COLOR_TEXT_MUTED);
                spr->drawString("Wenn " + kNames[0] + " so alt ist", 120, 48, 2);
                spr->drawString("wie Du heute (" + String(myCurYears) + " Jahre):", 120, 68, 2);
                spr->setTextColor(COLOR_TEXT_MAIN);
                spr->drawString(dBuf, 120, 102, 4);
                spr->setTextColor(COLOR_TEXT_MUTED);
                spr->drawString("An diesem Tag bist Du", 120, 138, 2);
                spr->setTextColor(COLOR_PROD_GREEN);
                spr->drawString(String(myYearsThen) + " Jahre alt", 120, 163, 4);
            } else {
                auto drawChildCard = [&](int yCard, int idx, uint16_t borderCol) {
                    time_t kTime = stringToTime(kDates[idx], kTimes[idx]);
                    time_t targetDate = (time_t)((int64_t)kTime + myAgeSec);
                    struct tm tgt; localtime_r(&targetDate, &tgt);
                    int myYearsThen = (tgt.tm_year + 1900) - (uTm.tm_year + 1900);
                    if (tgt.tm_mon < uTm.tm_mon || (tgt.tm_mon == uTm.tm_mon && tgt.tm_mday < uTm.tm_mday)) myYearsThen--;
                    char dBuf[28]; snprintf(dBuf, sizeof(dBuf), "%02d. %s %04d", tgt.tm_mday, monateKurz[tgt.tm_mon], tgt.tm_year + 1900);

                    drawGlassCard(spr, 10, yCard, 220, 78, 10, borderCol, COLOR_SURFACE, COLOR_BG_DARK);
                    spr->setTextDatum(TL_DATUM);
                    spr->setTextColor(COLOR_TEXT_MUTED);
                    spr->drawString("Wenn " + kNames[idx] + " so alt ist (" + String(myCurYears) + " J.):", 20, yCard + 7, 2);
                    spr->setTextColor(COLOR_TEXT_MAIN);
                    spr->drawString(dBuf, 20, yCard + 26, 4);
                    spr->setTextColor(borderCol);
                    spr->drawString("Bist Du: " + String(myYearsThen) + " Jahre alt", 20, yCard + 56, 2);
                };

                drawChildCard(26, 0, COLOR_PROD_GREEN);
                drawChildCard(108, 1, COLOR_CYAN_TECH);
            }
        } else {
            drawGlassCard(spr, 20, 40, 200, 120, 12, COLOR_BORDER, COLOR_SURFACE, COLOR_BG_DARK);
            spr->fillCircle(120, 70, 14, COLOR_SURFACE_HI);
            spr->drawCircle(120, 70, 14, COLOR_BORDER_HI);
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(COLOR_PROD_GREEN);
            spr->drawString("+", 120, 70, 2);
            spr->setTextColor(COLOR_TEXT_MAIN);
            spr->drawString("Keine Kinder", 120, 102, 2);
            spr->setTextColor(COLOR_TEXT_MUTED);
            spr->drawString("Im WebDashboard anlegen", 120, 124, 1);
        }
    }
}

void drawControlCenter(TFT_eSprite *spr, struct tm *aktuell) {
    if (!spr || spr->getPointer() == nullptr) return;

    // 1. TOP MINI STATUS & GRAB HANDLE (y = 0 .. 20)
    spr->fillRoundRect(102, 2, 36, 3, 1, COLOR_BORDER_HI);

    // Left: Mini Time
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(COLOR_ACCENT_TIME, cBG);
    char tMini[8] = "--:--";
    if (aktuell) snprintf(tMini, sizeof(tMini), "%02d:%02d", aktuell->tm_hour, aktuell->tm_min);
    spr->drawString(tMini, 12, 5, 2);

    // Right: Battery & Connection
    spr->setTextDatum(TR_DATUM);
    int bat = getBattery();
    bool isChg = isChargingCached(false);
    String batTxt = String(bat) + "%" + (isChg ? " \xE2\x9A\xA1" : "");
    spr->setTextColor(isChg ? COLOR_CYAN_TECH : (bat > 20 ? COLOR_PROD_GREEN : COLOR_SEC_RED), cBG);
    spr->drawString(batTxt, 228, 5, 2);

    // 2. LIVE BRIGHTNESS CAPSULE SLIDER (y = 24 .. 62, h = 38)
    int sX = 10, sY = 24, sW = 220, sH = 38, sR = 8;
    spr->fillRoundRect(sX, sY, sW, sH, sR, COLOR_SURFACE);
    
    int fillW = map(maxBright, 10, 255, 12, sW);
    fillW = constrain(fillW, 12, sW);
    spr->fillRoundRect(sX, sY, fillW, sH, sR, blendColor565(COLOR_AMBER_WARN, COLOR_SURFACE, 170));
    spr->drawRoundRect(sX, sY, sW, sH, sR, COLOR_BORDER_HI);

    // Slider Labels
    spr->setTextDatum(ML_DATUM);
    spr->setTextColor(COLOR_TEXT_MAIN);
    spr->drawString("Helligkeit", sX + 14, sY + sH / 2, 2);

    int bPct = map(maxBright, 10, 255, 4, 100);
    spr->setTextDatum(MR_DATUM);
    spr->setTextColor(COLOR_TEXT_MAIN);
    spr->drawString(String(bPct) + "%", sX + sW - 14, sY + sH / 2, 2);

    // 3. FOUR MODERN SCHNELLTOGGLES (2x2 Grid, y = 68 .. 154)
    int tW = 106, tH = 42;
    int c0 = 10, c1 = 124;
    int r0 = 68, r1 = 114;

    auto drawCtrlTile = [&](int x, int y, const char* title, const String &sub, bool active, uint16_t activeCol, const char* icon) {
        uint16_t bCol = active ? activeCol : COLOR_BORDER;
        uint16_t fCol = active ? blendColor565(activeCol, COLOR_SURFACE, 55) : COLOR_SURFACE;
        drawGlassCard(spr, x, y, tW, tH, 8, bCol, fCol, COLOR_BG_DARK);
        
        // Status LED dot
        spr->fillCircle(x + tW - 10, y + 10, active ? 3 : 2, active ? activeCol : COLOR_BORDER_HI);
        if (active) spr->fillCircle(x + tW - 10, y + 10, 1, COLOR_TEXT_MAIN);

        spr->setTextDatum(TL_DATUM);
        spr->setTextColor(COLOR_TEXT_MAIN);
        spr->drawString(String(icon) + " " + title, x + 8, y + 6, 2);

        spr->setTextColor(active ? activeCol : COLOR_TEXT_MUTED);
        spr->drawString(sub, x + 8, y + 24, 1);
    };

    // Toggle 1: Hotspot / WLAN
    String wSub = wlanActive ? (wlanClientMode ? "Client" : "192.168.4.1") : "Aus";
    drawCtrlTile(c0, r0, "Hotspot", wSub, wlanActive, wlanClientMode ? COLOR_CYAN_TECH : COLOR_PROD_GREEN, "WLAN");

    // Toggle 2: Stummschaltung / DND
    bool isDnd = !vibActive;
    drawCtrlTile(c1, r0, isDnd ? "Lautlos" : "Haptik", isDnd ? "DND Aktiv" : "Vibra An", isDnd, COLOR_AMBER_WARN, isDnd ? "DND" : "VIB");

    // Toggle 3: Taschenlampe (Flashlight)
    drawCtrlTile(c0, r1, "Licht", "100% Weiss", false, COLOR_AMBER_WARN, "LED");

    // Toggle 4: USB Modus
    drawCtrlTile(c1, r1, "USB", usbHidActive ? "HID (Maus)" : "CDC (COM)", usbHidActive, COLOR_CYAN_TECH, "USB");

    // 4. BOTTOM ACTION PILLS (y = 160 .. 200)
    int bRowY = 160;
    // Left: LoRa P2P Messenger
    bool loraOn = LoraManager::isReady();
    uint16_t lCol = loraOn ? COLOR_CYAN_TECH : COLOR_BORDER;
    uint16_t lFill = loraOn ? blendColor565(COLOR_CYAN_TECH, COLOR_SURFACE, 55) : COLOR_SURFACE;
    drawGlassCard(spr, c0, bRowY, tW, 40, 8, lCol, lFill, COLOR_BG_DARK);
    spr->fillCircle(c0 + tW - 10, bRowY + 10, loraOn ? 3 : 2, loraOn ? COLOR_CYAN_TECH : COLOR_BORDER_HI);
    if (loraOn) spr->fillCircle(c0 + tW - 10, bRowY + 10, 1, COLOR_TEXT_MAIN);
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(COLOR_TEXT_MAIN);
    spr->drawString("LoRa Funk", c0 + 8, bRowY + 6, 2);
    spr->setTextColor(loraOn ? COLOR_CYAN_TECH : COLOR_TEXT_MUTED);
    spr->drawString(loraOn ? "868 MHz P2P" : "SX1262 Aus", c0 + 8, bRowY + 24, 1);

    // Right: Stop Apps
    bool anyRun = jigglerActive || badUsbActive || wifiSpamActive || bleSpamActive || deauthActive || nmapActive || evilActive || probeMonActive || bleScanActive || swRunning || timerRunning;
    uint16_t sCol = anyRun ? COLOR_SEC_RED : COLOR_BORDER;
    uint16_t sFill = anyRun ? blendColor565(COLOR_SEC_RED, COLOR_SURFACE, 70) : COLOR_SURFACE;
    drawGlassCard(spr, c1, bRowY, tW, 40, 8, sCol, sFill, COLOR_BG_DARK);
    spr->fillCircle(c1 + tW - 10, bRowY + 10, anyRun ? 3 : 2, anyRun ? COLOR_SEC_RED : COLOR_BORDER_HI);
    if (anyRun) spr->fillCircle(c1 + tW - 10, bRowY + 10, 1, COLOR_TEXT_MAIN);
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(COLOR_TEXT_MAIN);
    spr->drawString("Stop Apps", c1 + 8, bRowY + 6, 2);
    spr->setTextColor(anyRun ? COLOR_SEC_RED : COLOR_TEXT_MUTED);
    spr->drawString(anyRun ? "Apps stoppen!" : "Inaktiv", c1 + 8, bRowY + 24, 1);
}

void drawLoraMessenger(TFT_eSprite *spr, struct tm *aktuell) {
    if (!spr || spr->getPointer() == nullptr) return;

    // Header bar (y = 2 .. 24)
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(0xFFFF);
    spr->drawString("LoRa P2P 868MHz", 10, 4, 2);

    spr->setTextDatum(TR_DATUM);
    bool rdy = LoraManager::isReady();
    if (rdy) {
        spr->setTextColor(0x07FF);
        int lRssi = (int)LoraManager::getLastRssi();
        String stStr = (lRssi < 0) ? (String(lRssi) + "dBm") : "BEREIT";
        spr->drawString(stStr, 230, 4, 2);
    } else {
        spr->setTextColor(0xF800);
        spr->drawString("OFFLINE", 230, 4, 2);
    }

    // Tab Switcher (y = 26 .. 48): [0: Chat / P2P] [1: Ping Beacon]
    int tabW = 106, tabH = 22;
    // Tab 0
    drawGlassCard(spr, 10, 26, tabW, tabH, 6, loraTabMode == 0 ? COLOR_CYAN_TECH : COLOR_BORDER, loraTabMode == 0 ? blendColor565(COLOR_CYAN_TECH, COLOR_SURFACE, 40) : COLOR_SURFACE, COLOR_BG_DARK);
    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(loraTabMode == 0 ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED);
    spr->drawString("Chat (P2P)", 63, 37, 2);

    // Tab 1
    drawGlassCard(spr, 124, 26, tabW, tabH, 6, loraTabMode == 1 ? COLOR_CYAN_TECH : COLOR_BORDER, loraTabMode == 1 ? blendColor565(COLOR_CYAN_TECH, COLOR_SURFACE, 40) : COLOR_SURFACE, COLOR_BG_DARK);
    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(loraTabMode == 1 ? COLOR_TEXT_MAIN : COLOR_TEXT_MUTED);
    spr->drawString("Ping Beacon", 177, 37, 2);

    if (loraTabMode == 0) {
        // === TAB 0: CHAT FEED & PRESETS ===
        // Message Feed Card (y = 52 .. 156, height = 104)
        drawGlassCard(spr, 10, 52, 220, 104, 8, COLOR_BORDER, COLOR_SURFACE, COLOR_BG_DARK);
        
        int nMsgs = LoraManager::getMessageCount();
        const LoraMessage* msgs = LoraManager::getMessages();

        if (nMsgs == 0) {
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(COLOR_TEXT_MUTED);
            spr->drawString("Warte auf Funk-Pakete...", 120, 95, 2);
            spr->drawString("Kanal: 868.0 MHz  SF7", 120, 115, 1);
        } else {
            int startIdx = nMsgs > 3 ? nMsgs - 3 : 0;
            int mY = 56;
            for (int i = startIdx; i < nMsgs; i++) {
                const LoraMessage &m = msgs[i];
                spr->setTextDatum(TL_DATUM);
                if (m.isOutgoing) {
                    spr->setTextColor(COLOR_WARN);
                    spr->drawString("> " + String(m.sender) + " (" + String(m.timeStr) + "):", 16, mY, 1);
                    spr->setTextColor(COLOR_TEXT_MAIN);
                    spr->drawString(m.text, 22, mY + 12, 2);
                } else {
                    spr->setTextColor(COLOR_CYAN_TECH);
                    String sig = String((int)m.rssi) + "dBm";
                    spr->drawString("< " + String(m.sender) + " [" + sig + "]:", 16, mY, 1);
                    spr->setTextColor(COLOR_PROD_GREEN);
                    spr->drawString(m.text, 22, mY + 12, 2);
                }
                mY += 32;
            }
        }

        // Quick Send Presets (y = 162 .. 198)
        static const char* presets[] = {"Hallo!", "OK", "Ping?", "Hilfe!"};
        int pW = 52, pH = 34, gap = 4;
        for (int i = 0; i < 4; i++) {
            int px = 10 + i * (pW + gap);
            drawGlassCard(spr, px, 162, pW, pH, 6, COLOR_BORDER, COLOR_SURFACE, COLOR_BG_DARK);
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(COLOR_TEXT_MAIN);
            spr->drawString(presets[i], px + pW / 2, 179, 1);
        }
    } else {
        // === TAB 1: PING BEACON & TELEMETRY ===
        drawGlassCard(spr, 10, 52, 220, 96, 8, COLOR_BORDER, COLOR_SURFACE, COLOR_BG_DARK);
        
        spr->setTextDatum(TL_DATUM);
        spr->setTextColor(COLOR_CYAN_TECH);
        spr->drawString("LoRa Telemetrie (SX1262)", 18, 58, 2);

        spr->setTextColor(COLOR_TEXT_MAIN);
        spr->drawString("TX Pakete: " + String(LoraManager::getTxCount()), 18, 76, 2);
        spr->drawString("RX Pakete: " + String(LoraManager::getRxCount()), 18, 94, 2);
        
        String sigStr = "Signal: " + String((int)LoraManager::getLastRssi()) + " dBm / SNR: " + String(LoraManager::getLastSnr(), 1) + " dB";
        spr->setTextColor(COLOR_TEXT_MUTED);
        spr->drawString(sigStr, 18, 114, 1);
        spr->drawString("Frequenz: 868.0 MHz  BW: 125 kHz", 18, 128, 1);

        // Big Beacon Button (y = 154 .. 198)
        bool bActive = LoraManager::isBeaconActive();
        uint16_t btnBorder = bActive ? COLOR_SEC_RED : COLOR_PROD_GREEN;
        uint16_t btnFill = bActive ? blendColor565(COLOR_SEC_RED, COLOR_SURFACE, 40) : blendColor565(COLOR_PROD_GREEN, COLOR_SURFACE, 40);
        drawGlassCard(spr, 10, 154, 220, 44, 8, btnBorder, btnFill, COLOR_BG_DARK);
        spr->setTextDatum(MC_DATUM);
        spr->setTextColor(COLOR_TEXT_MAIN);
        spr->drawString(bActive ? "BEACON STOPPEN (5s)" : "BEACON STARTEN (5s)", 120, 176, 2);
    }
}

static void renderApp(AppPackage* app, TFT_eSprite *spr, struct tm *aktuell) {
    if (!app || !spr) return;
    if (app->drawFn) {
        app->drawFn(spr);
        return;
    }
    AppManager::tickDynamicApp(app, millis());
    AppManager::renderDynamicApp(app, spr);
}

void renderPage(int page, TFT_eSprite *spr, struct tm *aktuell) {
    if (!spr || spr->getPointer() == nullptr) return;
    time_t jetzt; time(&jetzt);
    
    if (page == 0) {
        struct tm tSafe;
        if (!aktuell) {
            if (!getLocalTime(&tSafe, 10)) {
                tSafe.tm_hour = 12; tSafe.tm_min = 0; tSafe.tm_sec = 0;
                tSafe.tm_mday = 1; tSafe.tm_mon = 0; tSafe.tm_year = 124; tSafe.tm_wday = 1;
            }
            aktuell = &tSafe;
        }

        int activeFace = FACE_ULTRA_DASH;
        if (!carouselOrder.empty()) {
            if (carouselIndex < 0 || carouselIndex >= (int)carouselOrder.size()) carouselIndex = 0;
            activeFace = carouselOrder[carouselIndex];
        }

        switch (activeFace) {
            case FACE_ULTRA_DASH: drawUltraDashboard(spr, aktuell, getBattery(), isChargingCached(false)); break;
            case FACE_CHRONO:     drawChronographWatchface(spr, aktuell, getBattery(), isChargingCached(false)); break;
            case FACE_CASIO:      drawRetroCasioWatchface(spr, aktuell, getBattery(), isChargingCached(false)); break;
            case FACE_LIFETIME:   drawLifetimeWatchface(spr, aktuell, getBattery(), isChargingCached(false)); break;
            case FACE_BAUHAUS:    drawBauhausWatchface(spr, aktuell, getBattery(), isChargingCached(false)); break;
            case FACE_CYBERPUNK:  drawCyberpunkWatchface(spr, aktuell, getBattery(), isChargingCached(false)); break;
            case FACE_WORDCLOCK:  drawWordClockWatchface(spr, aktuell, getBattery(), isChargingCached(false)); break;
            case FACE_FINANCE:    drawFinanceWatchface(spr, aktuell); break;
            case FACE_CHILDREN:   drawChildrenWatchface(spr, aktuell); break;
            case FACE_PARENTS:    drawParentsWatchface(spr, aktuell); break;
            default:              drawUltraDashboard(spr, aktuell, getBattery(), isChargingCached(false)); break;
        }
    } 
    else if (page == 1 || page == FACE_FINANCE) {
        drawFinanceWatchface(spr, aktuell);
    }
    else if (page == 2 || page == FACE_CHILDREN) {
        drawChildrenWatchface(spr, aktuell);
    }
    else if (page == 3 || page == FACE_PARENTS) {
        drawParentsWatchface(spr, aktuell);
    }
    else if (page == 4) {
        // Reserviert
    }
    else if (page == 5) {
        drawControlCenter(spr, aktuell);
    }
    else if (page == 6 || page == 22 || page == 23) { 
        renderAppLauncher(spr);
    }
    else {
        AppPackage* curApp = AppManager::getAppByPage(page);
        if (curApp != nullptr) {
            renderApp(curApp, spr, aktuell);
        }
    }
    
    // Page indicator dots for Watchface Carousel (Page 0)
    if (page == 0) {
        int dotY = spr->height() - 7;
        int numItems = (int)carouselOrder.size();
        if (numItems > 1) {
            int dotSpacing = (numItems > 7) ? 12 : 16;
            int totalW = (numItems - 1) * dotSpacing + 12;
            int startX = 120 - totalW / 2;
            for (int i = 0; i < numItems; i++) {
                int dotX = startX + i * dotSpacing;
                if (i == carouselIndex) {
                    spr->fillRoundRect(dotX, dotY - 2, 12, 4, 2, cAcc1);
                } else {
                    spr->fillCircle(dotX + 3, dotY, 2, cBatBG);
                }
            }
        }
        // Notification Pill / Badge on Watchfaces if unread notifications exist
        if (NotificationManager::getUnreadCount() > 0) {
            spr->fillRoundRect(106, 6, 28, 10, 5, 0x1423);
            spr->drawRoundRect(106, 6, 28, 10, 5, TFT_CYAN);
            spr->fillCircle(112, 11, 3, TFT_CYAN);
            spr->fillCircle(112, 11, 1, TFT_WHITE);
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(TFT_WHITE);
            spr->drawString(String(NotificationManager::getUnreadCount()), 124, 11, 1);
        }
    }

    if (millis() < toastEndTime) {
        int tW = spr->textWidth(toastMsg, 2) + 28;
        if (tW < 80) tW = 80;
        if (tW > 230) tW = 230;
        int tX = (240 - tW) / 2;
        spr->fillRoundRect(tX, 8, tW, 24, 12, toastColor);
        spr->drawRoundRect(tX, 8, tW, 24, 12, COLOR_TEXT_MAIN);
        spr->setTextDatum(MC_DATUM);
        spr->setTextColor(COLOR_TEXT_MAIN, toastColor);
        spr->drawString(toastMsg, 120, 20, 2);
    }
}

void slideToPage(int newPage, int dx, int dy) {
    Serial.printf("[SLIDE] %d -> %d (heap=%d)\n", currentPage, newPage, ESP.getFreeHeap());
    currentPage = newPage;
    lastSecond = -1;

    if (cdSprite.getPointer() == nullptr) {
        void* p = cdSprite.createSprite(240, 240 - HEAD_H);
        if (!p) {
            Serial.println("[SLIDE] cdSprite alloc FAIL!");
            return;
        }
    }

    time_t jetzt; time(&jetzt);
    struct tm *aktuell = localtime(&jetzt);
    bool isCharging = isChargingCached(false);

    AppPackage* curSlideApp = AppManager::getAppByPage(currentPage);
    bool isSlideFlash = (curSlideApp && (curSlideApp->id == "flashlight" || curSlideApp->driver == "flashlight"));
    bool isSlideFs = isSlideFlash ? AppManager::getAppStateBool(curSlideApp->id, "fullscreen", true) : false;
    if (isSlideFs) {
        setBrightness(255);
        tft.fillScreen(TFT_WHITE);
        Serial.printf("[SLIDE] OK (FLASHLIGHT VOLLBILD) -> page %d (heap=%d)\n", currentPage, ESP.getFreeHeap());
        return;
    }

    drawHeader(aktuell, getBattery(), isCharging);
    cdSprite.fillSprite(cBG);
    renderPage(currentPage, &cdSprite, aktuell);
    cdSprite.pushSprite(0, HEAD_H);

    Serial.printf("[SLIDE] OK -> page %d (heap=%d)\n", currentPage, ESP.getFreeHeap());
}

void renderUI(unsigned long currentMillis) {
    time_t jetzt; time(&jetzt); struct tm *aktuell = localtime(&jetzt); bool isCharging = isChargingCached(false); static unsigned long lastFrame = 0;

    AppPackage* curApp = AppManager::getAppByPage(currentPage);
    bool isFlashlight = (curApp && (curApp->id == "flashlight" || curApp->driver == "flashlight"));
    bool isFsLight = false;
    int fsMode = 0;
    int strobeTog = 1;

    if (isFlashlight) {
        AppManager::tickDynamicApp(curApp, currentMillis);
        isFsLight = AppManager::getAppStateBool(curApp->id, "fullscreen", true);
        fsMode = AppManager::getAppStateInt(curApp->id, "curModeIdx", 0);
        strobeTog = AppManager::getAppStateInt(curApp->id, "strobeToggle", 1);
    }

    bool isAppActive = (curApp != nullptr);
    bool isFinanceActive = (currentPage == 1 || (currentPage == 0 && !carouselOrder.empty() && carouselIndex >= 0 && carouselIndex < (int)carouselOrder.size() && carouselOrder[carouselIndex] == FACE_FINANCE));
    bool fastRefresh = (isFinanceActive || isAppActive || isFlashlight); 
    bool forceRender = (aktuell->tm_sec != lastSecond) 
                    || (fastRefresh && (currentMillis - lastFrame > (isFlashlight ? 40 : 50))) 
                    || (currentMillis < toastEndTime) 
                    || (hotspotPromptActive && (currentMillis - lastFrame > 40))
                    || (isAppActive && (currentMillis - lastFrame > 50));
    if (forceRender) { 
        lastSecond = aktuell->tm_sec; 
        lastFrame = currentMillis; 

        // Vollbild-Flutlicht (Reines 240x240 Displaylicht ohne Header & Buttons)
        if (isFsLight) {
            uint16_t lightColor = TFT_WHITE;
            if (fsMode == 1) {
                lightColor = 0xFFDE; // warmes gedaempftes Weiss
                setBrightness(75);
            } else if (fsMode == 2) {
                lightColor = (strobeTog == 1) ? TFT_WHITE : TFT_BLACK;
                setBrightness((strobeTog == 1) ? 255 : 5);
            } else {
                lightColor = TFT_WHITE;
                setBrightness(255);
            }
            tft.fillScreen(lightColor);
            return;
        }

        if (cdSprite.getPointer() == nullptr) {
            Serial.println("[RENDER] cdSprite ist NULL - recreate!");
            void* p = cdSprite.createSprite(240, 240 - HEAD_H);
            if (!p) { Serial.println("[RENDER] KRITISCH: cdSprite alloc FAIL!"); return; }
        }
        drawHeader(aktuell, getBattery(), isCharging); 
        cdSprite.fillSprite(cBG); 
        renderPage(currentPage, &cdSprite, aktuell); 
        if (hotspotPromptActive) {
            drawHotspotPrompt(&cdSprite);
        }
        cdSprite.pushSprite(0, HEAD_H); 
    }
}

void renderAOD() {
    time_t jetzt; time(&jetzt); struct tm *aktuell = localtime(&jetzt);
    bool isCharging = isChargingCached(false);
    int batPct = getBattery();

    // --- Static state for burn-in offset (changes every 10 minutes) ---
    static int aodX = 120, aodY = 110;
    static int lastOffsetMin = -1; // tracks which 10-min slot we last shifted in

    int curSlot = aktuell->tm_min / 10; // 0..5  (changes every 10 min)
    if (curSlot != lastOffsetMin) {
        lastOffsetMin = curSlot;
        aodX = 120 + random(-5, 6); // symmetrischer Burn-in-Versatz -5..+5
        aodY = 110 + random(-5, 6);
    }

    // --- Detect content changes (time, charge, battery %) ---
    static int lastAodMin = -1;
    static bool lastAodChargeState = false;
    static int lastAodBatPct = -1;
    bool contentChanged = (aktuell->tm_min != lastAodMin)
                       || (isCharging != lastAodChargeState)
                       || (batPct != lastAodBatPct)
                       || (lastSecond == -1);

    // --- Charging animation: pulsing bolt (separate timer, no full redraw) ---
    static unsigned long lastAnimTime = 0;
    static uint8_t animPhase = 0;
    bool animTick = false;
    if (isCharging && (millis() - lastAnimTime > 800)) {
        lastAnimTime = millis();
        animPhase = (animPhase + 1) % 4;
        animTick = true;
    }

    // --- Full redraw only when content actually changed ---
    if (contentChanged) {
        lastAodMin = aktuell->tm_min;
        lastAodChargeState = isCharging;
        lastAodBatPct = batPct;
        lastSecond = 0;

        tft.fillScreen(TFT_BLACK);

        int offX = aodX - 120; // Burn-in offset
        int offY = aodY - 110;

        // === Digitale Uhrzeit ===
        if (wfShowTime) {
            char aodBuf[10]; snprintf(aodBuf, sizeof(aodBuf), "%02d:%02d", aktuell->tm_hour, aktuell->tm_min);
            int px = wfTimeX + offX, py = wfTimeY + offY;
            tft.setTextDatum(MC_DATUM); tft.setTextColor(cAodFG, TFT_BLACK);
            if (wfTimeFmt == 1) {
                // Echte 7-Segment-Darstellung direkt auf tft (TFT_eSPI)
                int sH = max(14, (int)(40 * wfTimeScale));
                int sW = max(8, (int)(sH * 0.55));
                int sp = max(2, (int)(sH * 0.15));
                int totalW = get7SegWidth(String(aodBuf), sH, sW, sp);
                draw7SegString(tft, String(aodBuf), px - totalW / 2, py - sH / 2, sH, sW, cAodFG, sp);
            } else {
                // Digital: Basis Font 6 (~36px) bei Scale 1.0, stufenlos per setTextSize für >1.0
                if (wfTimeScale <= 0.6) { tft.setTextSize(1); tft.drawString(aodBuf, px, py, 4); }
                else if (wfTimeScale <= 1.0) { tft.setTextSize(1); tft.drawString(aodBuf, px, py, 6); }
                else if (wfTimeScale <= 1.5) { tft.setTextSize(1); tft.drawString(aodBuf, px, py, 7); }
                else { tft.setTextSize(2); tft.drawString(aodBuf, px, py, 4); } // 2x Font 4 = ~52px
            }
            tft.setTextSize(1); // Reset
        }

        // === Datum ===
        if (wfShowDate) {
            char dateBuf[20]; snprintf(dateBuf, sizeof(dateBuf), "%s, %02d.%02d.", wochentage[aktuell->tm_wday], aktuell->tm_mday, aktuell->tm_mon + 1);
            int px = wfDateX + offX, py = wfDateY + offY;
            tft.setTextDatum(MC_DATUM); tft.setTextColor(cBatBG, TFT_BLACK);
            int font = 4; if (wfDateScale < 0.7) font = 2; else if (wfDateScale > 1.5) font = 4;
            tft.drawString(dateBuf, px, py, font);
        }

        // === Akku (mit integriertem Lade-Indikator) ===
        if (wfShowBatt) {
            int px = wfBattX + offX, py = wfBattY + offY;
            tft.setTextDatum(MC_DATUM);
            int bFont = (wfBattScale < 0.7) ? 2 : 4;
            if (isCharging) {
                uint8_t bri = 100 + animPhase * 50; // 100..250 pulsierend
                uint16_t pulseCol = tft.color565(bri, bri, 0);
                tft.setTextColor(pulseCol, TFT_BLACK);
                tft.drawString(String(batPct) + "%", px, py, bFont);
            } else {
                uint16_t bCol = (batPct > 50) ? TFT_GREEN : (batPct > 20) ? TFT_YELLOW : TFT_RED;
                tft.setTextColor(bCol, TFT_BLACK);
                tft.drawString(String(batPct) + "%", px, py, bFont);
            }
        }

        // === Schritte ===
        if (wfShowSteps) {
            int px = wfStepsX + offX, py = wfStepsY + offY;
            tft.setTextDatum(MC_DATUM); tft.setTextColor(cAodAcc, TFT_BLACK);
            int sFont = (wfStepsScale >= 1.3) ? 4 : ((wfStepsScale < 0.7) ? 1 : 2);
            tft.drawString("Schritte: " + String(stepCount), px, py, sFont);
        }

        // === Wortuhr ===
        if (wfShowWordClock) {
            int px = wfWordClockX + offX, py = wfWordClockY + offY;
            tft.setTextDatum(TL_DATUM);
            const char* grid[10] = {"ESKISTAFUNF", "ZEHNZWANZIG", "DREIVIERTEL", "TGNACHVORJM", "HALBXZWOLFP", "ZWEINSIEBEN", "KDREIRHFUNF", "ELFNEUNVIER", "WACHTZEHNRS", "BSECHSFUHRX"};
            bool active[10][11] = {false};
            int h = aktuell->tm_hour; int m = aktuell->tm_min;
            auto setW = [&](int r, int c, int l) { for(int i=0; i<l; i++) active[r][c+i] = true; };
            setW(0,0,2); setW(0,3,3);
            int m5 = m / 5;
            switch(m5) { case 0: setW(9,7,3); break; case 1: setW(0,7,4); setW(3,2,4); break; case 2: setW(1,0,4); setW(3,2,4); break; case 3: setW(2,4,7); setW(3,2,4); break; case 4: setW(1,4,7); setW(3,2,4); break; case 5: setW(0,7,4); setW(3,6,3); setW(4,0,4); break; case 6: setW(4,0,4); break; case 7: setW(0,7,4); setW(3,2,4); setW(4,0,4); break; case 8: setW(1,4,7); setW(3,6,3); break; case 9: setW(2,4,7); setW(3,6,3); break; case 10: setW(1,0,4); setW(3,6,3); break; case 11: setW(0,7,4); setW(3,6,3); break; }
            int sH = h; if (m >= 25) sH++; sH = sH % 12; if (sH == 0) sH = 12;
            switch(sH) { case 1: if (m5==0) setW(5,2,3); else setW(5,2,4); break; case 2: setW(5,0,4); break; case 3: setW(6,1,4); break; case 4: setW(7,7,4); break; case 5: setW(6,7,4); break; case 6: setW(9,1,5); break; case 7: setW(5,5,6); break; case 8: setW(8,1,4); break; case 9: setW(7,3,4); break; case 10: setW(8,5,4); break; case 11: setW(7,0,3); break; case 12: setW(4,5,5); break; }
            // Basis: 14px pro Zeichen bei Scale 1.0 → 11 Spalten = 154px, 10 Zeilen = 140px
            int sXs = (int)(14 * wfWordClockScale); int sYs = (int)(14 * wfWordClockScale);
            int stX = px - (int)(77 * wfWordClockScale); int stY = py - (int)(70 * wfWordClockScale);
            int fnt = 1; // Font 1 = 8px, passt gut für 14px Spacing
            if (wfWordClockScale >= 1.5) fnt = 2; // Ab 1.5x Scale: Font 2
            for(int r=0; r<10; r++) { for(int c=0; c<11; c++) { tft.setTextColor(active[r][c] ? cAodAcc : cBatBG, TFT_BLACK); tft.drawChar(grid[r][c], stX + c*sXs, stY + r*sYs, fnt); } }
        }

        // === Binäruhr ===
        if (wfShowBinClock) {
            int px = wfBinClockX + offX, py = wfBinClockY + offY;
            tft.setTextDatum(MC_DATUM); tft.setTextColor(cAodAcc, TFT_BLACK);
            String hStr = "", mStr = "";
            int numH = aktuell->tm_hour, numM = aktuell->tm_min;
            for(int i=0; i<6; i++) { hStr = String(numH % 2) + hStr; numH /= 2; }
            for(int i=0; i<6; i++) { mStr = String(numM % 2) + mStr; numM /= 2; }
            // Binär: Basis Font 4 bei Scale 1.0, Gap 24
            int fnt = 4; if (wfBinClockScale < 0.7) fnt = 2; else if (wfBinClockScale > 1.5) fnt = 6;
            int gap = (int)(24 * wfBinClockScale);
            tft.drawString(hStr, px, py - gap/2, fnt);
            tft.drawString(mStr, px, py + gap/2, fnt);
        }

        // === Analoguhr ===
        if (wfShowAnalog) {
            int cx = wfAnalogX + offX, cy = wfAnalogY + offY;
            int r = (int)(80 * wfAnalogScale); // Basis: 80px Radius bei Scale 1.0 = 160px Durchmesser
            // Ziffernblatt
            tft.drawCircle(cx, cy, r, cBatBG);
            for (int i = 0; i < 12; i++) {
                float angle = i * 30.0 * 3.14159 / 180.0;
                int x1 = cx + (int)((r - 6) * sin(angle));
                int y1 = cy - (int)((r - 6) * cos(angle));
                int x2 = cx + (int)(r * sin(angle));
                int y2 = cy - (int)(r * cos(angle));
                tft.drawLine(x1, y1, x2, y2, cBatBG);
                // Ziffern je nach Stil
                int numX = cx + (int)((r - 14) * sin(angle));
                int numY = cy - (int)((r - 14) * cos(angle));
                tft.setTextDatum(MC_DATUM); tft.setTextColor(cBatBG, TFT_BLACK);
                int num = (i == 0) ? 12 : i;
                if (wfAnalogStyle == 1) {
                    // Binär
                    String bStr = ""; int n = num; for(int b=0; b<4; b++) { bStr = String(n%2) + bStr; n /= 2; }
                    tft.drawString(bStr, numX, numY, 1);
                } else if (wfAnalogStyle == 2) {
                    // Hex
                    char hBuf[4]; snprintf(hBuf, sizeof(hBuf), "%X", num);
                    tft.drawString(hBuf, numX, numY, 1);
                } else {
                    tft.drawString(String(num), numX, numY, 1);
                }
            }
            // Stundenzeiger
            float hAngle = ((aktuell->tm_hour % 12) + aktuell->tm_min / 60.0) * 30.0 * 3.14159 / 180.0;
            int hx = cx + (int)((r * 0.5) * sin(hAngle));
            int hy = cy - (int)((r * 0.5) * cos(hAngle));
            tft.drawLine(cx, cy, hx, hy, cAodFG); tft.drawLine(cx+1, cy, hx+1, hy, cAodFG);
            // Minutenzeiger
            float mAngle = aktuell->tm_min * 6.0 * 3.14159 / 180.0;
            int mx = cx + (int)((r * 0.75) * sin(mAngle));
            int my = cy - (int)((r * 0.75) * cos(mAngle));
            tft.drawLine(cx, cy, mx, my, cAodAcc);
            // Mittelpunkt
            tft.fillCircle(cx, cy, 3, cAodFG);
        }

        // === Trennlinie ===
        if (wfShowLine) {
            int px = wfLineX + offX, py = wfLineY + offY;
            int lw = (int)(120 * wfLineScale);
            tft.drawFastHLine(px - lw / 2, py, lw, cBatBG);
            tft.drawFastHLine(px - lw / 2, py + 1, lw, cBatBG);
        }

        // === Minuten-Bogen ===
        if (wfShowSecArc) {
            int cx = wfSecArcX + offX, cy = wfSecArcY + offY;
            int r = (int)(80 * wfSecArcScale);
            int lw = (int)(4 * wfSecArcScale); if (lw < 2) lw = 2;
            // Hintergrund-Ring (dunkel)
            for (int a = 0; a < 360; a += 3) {
                float rad = a * 3.14159 / 180.0;
                int x1 = cx + (int)((r - lw) * sin(rad));
                int y1 = cy - (int)((r - lw) * cos(rad));
                tft.drawPixel(x1, y1, tft.color565(30, 30, 30));
            }
            // Fortschritts-Bogen (Minuten 0-59 = 0°-360°)
            int endAngle = (int)(aktuell->tm_min * 6); // 6° pro Minute
            for (int a = 0; a < endAngle; a++) {
                float rad = a * 3.14159 / 180.0;
                for (int t = -lw/2; t <= lw/2; t++) {
                    int px = cx + (int)((r + t) * sin(rad));
                    int py = cy - (int)((r + t) * cos(rad));
                    tft.drawPixel(px, py, cAodAcc);
                }
            }
        }

        // === Wochentag ===
        if (wfShowWeekday) {
            int px = wfWeekdayX + offX, py = wfWeekdayY + offY;
            tft.setTextDatum(MC_DATUM); tft.setTextColor(cAodAcc, TFT_BLACK);
            const char* tage[] = {"SONNTAG", "MONTAG", "DIENSTAG", "MITTWOCH", "DONNERSTAG", "FREITAG", "SAMSTAG"};
            int fnt = 4; if (wfWeekdayScale < 0.7) fnt = 2; else if (wfWeekdayScale > 1.5) fnt = 4;
            tft.drawString(tage[aktuell->tm_wday], px, py, fnt);
        }
    }
    else if (isCharging && animTick && wfShowBatt) {
        // Nur Lade-Puls-Animation updaten (kein voller Redraw) — nur Prozent pulsierend
        int offX = aodX - 120, offY = aodY - 110;
        int px = wfBattX + offX, py = wfBattY + offY;
        uint8_t bri = 100 + animPhase * 50;
        uint16_t pulseCol = tft.color565(bri, bri, 0);
        tft.setTextDatum(MC_DATUM);
        tft.fillRect(px - 40, py - 12, 80, 24, TFT_BLACK);
        tft.setTextColor(pulseCol, TFT_BLACK);
        tft.drawString(String(batPct) + "%", px, py, 4);
    }
}

void streamScreenBmp(WiFiClient &client) {
    static const uint8_t bmpHeader[54] = {
        'B', 'M',
        0x36, 0xA3, 0x02, 0x00, // 172,854 bytes total file size
        0x00, 0x00, 0x00, 0x00,
        0x36, 0x00, 0x00, 0x00, // Offset to image data (54 bytes)
        0x28, 0x00, 0x00, 0x00, // Header size (40 bytes)
        0xF0, 0x00, 0x00, 0x00, // Width 240
        0xF0, 0x00, 0x00, 0x00, // Height 240 (bottom-to-top)
        0x01, 0x00,             // Planes 1
        0x18, 0x00,             // 24 bits (RGB)
        0x00, 0x00, 0x00, 0x00, // Compression: None
        0x00, 0xA3, 0x02, 0x00, // Image size: 172,800 bytes
        0x13, 0x0B, 0x00, 0x00, // 72 DPI
        0x13, 0x0B, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    client.write(bmpHeader, 54);

    uint8_t rowChunk[720 * 4]; // 4 rows = 2880 bytes
    int chunkRows = 4;
    for (int yStart = 239; yStart >= 0; yStart -= chunkRows) {
        int rCount = (yStart - chunkRows + 1 >= 0) ? chunkRows : (yStart + 1);
        for (int r = 0; r < rCount; r++) {
            int y = yStart - r;
            uint8_t* out = rowChunk + r * 720;
            const uint16_t* srcRow = nullptr;
            if (cdSprite.height() >= 240) {
                uint16_t* ptr = (uint16_t*)cdSprite.getPointer();
                if (ptr) srcRow = ptr + y * 240;
            } else if (y >= HEAD_H) {
                uint16_t* ptr = (uint16_t*)cdSprite.getPointer();
                if (ptr) srcRow = ptr + (y - HEAD_H) * 240;
            } else if (headSpriteReady) {
                uint16_t* ptr = (uint16_t*)headSprite.getPointer();
                if (ptr) srcRow = ptr + y * 240;
            }
            if (srcRow) {
                for (int x = 0; x < 240; x++) {
                    uint16_t c = srcRow[x];
                    out[x * 3 + 0] = (uint8_t)((c & 0x1F) << 3);         // Blue
                    out[x * 3 + 1] = (uint8_t)(((c >> 5) & 0x3F) << 2);  // Green
                    out[x * 3 + 2] = (uint8_t)(((c >> 11) & 0x1F) << 3); // Red
                }
            } else {
                memset(out, 0, 720);
            }
        }
        client.write(rowChunk, rCount * 720);
    }
}
