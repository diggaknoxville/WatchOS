// --- Display.cpp ---
#include "Display.h"
#include "Globals.h" 
#include "GoodApps.h" 
#include "BadApps.h"
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

inline uint16_t blendColor565(uint16_t c1, uint16_t c2, uint8_t alpha) {
    uint8_t r1 = (c1 >> 11) & 0x1F;
    uint8_t g1 = (c1 >> 5) & 0x3F;
    uint8_t b1 = c1 & 0x1F;
    uint8_t r2 = (c2 >> 11) & 0x1F;
    uint8_t g2 = (c2 >> 5) & 0x3F;
    uint8_t b2 = c2 & 0x1F;
    uint8_t r = (r1 * (255 - alpha) + r2 * alpha) >> 8;
    uint8_t g = (g1 * (255 - alpha) + g2 * alpha) >> 8;
    uint8_t b = (b1 * (255 - alpha) + b2 * alpha) >> 8;
    return (r << 11) | (g << 5) | b;
}

void drawGlassCard(TFT_eSprite *spr, int x, int y, int w, int h, int r, uint16_t borderCol, uint16_t fillTop, uint16_t fillBot) {
    if (!spr || spr->getPointer() == nullptr) return;
    spr->fillRoundRect(x, y, w, h, r, fillBot);
    int topBandH = h / 3;
    if (topBandH > r) {
        spr->fillRoundRect(x + 1, y + 1, w - 2, topBandH, r, fillTop);
        spr->fillRect(x + 1, y + r, w - 2, topBandH - r + 2, blendColor565(fillTop, fillBot, 140));
    }
    // Specular gloss reflection line along the top inner edge
    spr->drawFastHLine(x + r, y + 1, w - 2 * r, 0xCE79);
    spr->drawFastHLine(x + r, y + 2, w - 2 * r, 0x52AA);
    // Subtle ambient shadow at the bottom
    spr->drawFastHLine(x + r, y + h, w - 2 * r, 0x0000);
    // Bevel border
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
    
    TFT_eSprite &hs = headSpriteReady ? headSprite : cdSprite;
    hs.fillRect(0, 0, 240, HEAD_H, 0x0821);
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
        hs.fillRect(wx - 8, wy + 1, 16, 8, 0x0821);
        curX += 16;
    }
    
    // 3b. BACKGROUND APP INDICATOR — Zahnrad
    bool anyAppRunning = jigglerActive || badUsbActive || wifiSpamActive || bleSpamActive || deauthActive || nmapActive || evilActive || probeMonActive || bleScanActive || swRunning || timerRunning;
    if (anyAppRunning) {
        int gx = curX + 5, gy = cy + 6;
        hs.fillCircle(gx, gy, 4, 0xFD20);
        hs.fillCircle(gx, gy, 2, 0x0821);
        hs.fillRect(gx - 1, gy - 6, 2, 3, 0xFD20);
        hs.fillRect(gx - 1, gy + 3, 2, 3, 0xFD20);
        hs.fillRect(gx - 6, gy - 1, 3, 2, 0xFD20);
        hs.fillRect(gx + 3, gy - 1, 3, 2, 0xFD20);
        curX += 13;
    }
    
    // 4. KOPFZEILE RECHTS: Kontext-sensitiv
    // Auf Page 0 (Zifferblatt) KEINE redundante Mini-Uhrzeit/Datum anzeigen!
    if (currentPage == 0) {
        hs.setTextDatum(TR_DATUM);
        if (wlanActive) {
            hs.setTextColor(0x07E0, 0x0821);
            if (wlanClientMode && WiFi.status() == WL_CONNECTED) {
                hs.drawString(WiFi.localIP().toString(), 236, cy + 1, 2);
            } else {
                hs.drawString("AP: 192.168.4.1", 236, cy + 1, 2);
            }
        } else if (phoneMac.length() > 0) {
            hs.setTextColor(0x551F, 0x0821);
            String pInfo = phoneName.length() > 0 ? phoneName : "Phone";
            if (pInfo.length() > 10) pInfo = pInfo.substring(0, 10);
            hs.drawString(pInfo, 236, cy + 1, 2);
        } else if (charging) {
            hs.setTextColor(0xFFE0, 0x0821);
            hs.drawString("Laden ⚡", 236, cy + 1, 2);
        } else {
            hs.setTextColor(0x8CD1, 0x0821);
            hs.drawString("WatchOS", 236, cy + 1, 2);
        }
    } else {
        // Auf App- und Einstellungsseiten: Saubere Uhrzeit rechts oben
        hs.setTextDatum(TR_DATUM);
        hs.setTextColor(cAcc1, 0x0821);
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

void drawControlCenter(TFT_eSprite *spr, struct tm *aktuell) {
    if (!spr || spr->getPointer() == nullptr) return;

    // 1. TOP MINI STATUS & GRAB HANDLE (y = 0 .. 22)
    spr->fillRoundRect(100, 2, 40, 4, 2, 0x5ACB);

    // Left: Mini Time
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(0xFFE0, cBG);
    char tMini[8] = "--:--";
    if (aktuell) snprintf(tMini, sizeof(tMini), "%02d:%02d", aktuell->tm_hour, aktuell->tm_min);
    spr->drawString(tMini, 12, 4, 2);

    // Right: Battery & Connection
    spr->setTextDatum(TR_DATUM);
    int bat = getBattery();
    bool isChg = isChargingCached(false);
    String batTxt = String(bat) + "%" + (isChg ? " \xE2\x9A\xA1" : "");
    spr->setTextColor(isChg ? 0x07FF : (bat > 20 ? 0x07E0 : 0xF800), cBG);
    spr->drawString(batTxt, 228, 4, 2);

    // 2. LIVE BRIGHTNESS SLIDER (y = 24 .. 62)
    drawGlassCard(spr, 10, 24, 220, 38, 8, 0x39E7, 0x1CE7, 0x0821);
    
    // Sun Icons
    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(0x8CD1);
    spr->drawString("-", 20, 43, 2);
    spr->setTextColor(0xFFE0);
    spr->drawString("+", 220, 43, 2);

    // Slider Track (x = 32 to 208, width = 176)
    int sX = 32, sW = 176, sY = 38, sH = 10;
    spr->fillRoundRect(sX, sY, sW, sH, 5, 0x0821);
    spr->drawRoundRect(sX, sY, sW, sH, 5, 0x2125);
    
    int fillW = map(maxBright, 10, 255, 6, sW);
    if (fillW > sW) fillW = sW;
    if (fillW > 2) {
        spr->fillRoundRect(sX, sY, fillW, sH, 5, 0xFFE0);
    }
    // Glowing thumb
    int thumbX = sX + fillW;
    if (thumbX > sX + sW - 5) thumbX = sX + sW - 5;
    spr->fillCircle(thumbX, sY + sH / 2, 7, 0xFFFF);
    spr->drawCircle(thumbX, sY + sH / 2, 7, 0xFD20);

    // Brightness percentage
    int bPct = map(maxBright, 10, 255, 4, 100);
    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(0xFFFF);
    spr->drawString(String(bPct) + "%", 120, 43, 1);

    // 3. FOUR APPLE-STYLE SCHNELLTOGGLES (2x2 Grid, y = 66 .. 156)
    int tW = 106, tH = 42;
    int c0 = 10, c1 = 124;
    int r0 = 66, r1 = 112;

    auto drawCtrlTile = [&](int x, int y, const char* title, const String &sub, bool active, uint16_t activeCol, const char* icon) {
        drawGlassCard(spr, x, y, tW, tH, 8, active ? activeCol : 0x2945, active ? 0x2184 : 0x1423, 0x0C12);
        
        spr->fillCircle(x + tW - 9, y + 9, active ? 3 : 2, active ? activeCol : 0x5ACB);
        if (active) spr->fillCircle(x + tW - 9, y + 9, 1, 0xFFFF);

        spr->setTextDatum(TL_DATUM);
        spr->setTextColor(TFT_WHITE);
        spr->drawString(String(icon) + " " + title, x + 6, y + 5, 2);

        spr->setTextColor(active ? activeCol : 0x8BD0);
        spr->drawString(sub, x + 6, y + 23, 1);
    };

    // Toggle 1: Hotspot / WLAN
    String wSub = wlanActive ? (wlanClientMode ? "Client" : "192.168.4.1") : "Aus";
    drawCtrlTile(c0, r0, "Hotspot", wSub, wlanActive, wlanClientMode ? 0x07FF : 0x07E0, "WLAN");

    // Toggle 2: Stummschaltung / DND
    bool isDnd = !vibActive;
    drawCtrlTile(c1, r0, isDnd ? "Lautlos" : "Haptik", isDnd ? "DND Aktiv" : "Vibra An", isDnd, 0xFD20, isDnd ? "DND" : "VIB");

    // Toggle 3: Taschenlampe (Flashlight)
    drawCtrlTile(c0, r1, "Licht", "100% Weiss", false, 0xFFE0, "LED");

    // Toggle 4: USB Modus
    drawCtrlTile(c1, r1, "USB", usbHidActive ? "HID (Maus)" : "CDC (COM)", usbHidActive, 0x91F7, "USB");

    // 4. BOTTOM ACTION PILLS (y = 158 .. 198)
    int bRowY = 158;
    // Left: LoRa P2P Messenger
    bool loraOn = LoraManager::isReady();
    drawGlassCard(spr, c0, bRowY, tW, 40, 8, loraOn ? 0x07FF : 0x3186, loraOn ? 0x0A66 : 0x1423, 0x0C12);
    spr->fillCircle(c0 + tW - 9, bRowY + 9, 3, loraOn ? 0x07FF : 0x5ACB);
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(TFT_WHITE);
    spr->drawString("LoRa Funk", c0 + 6, bRowY + 5, 2);
    spr->setTextColor(loraOn ? 0x07FF : 0x8CD1);
    spr->drawString(loraOn ? "868 MHz P2P" : "SX1262 Aus", c0 + 6, bRowY + 23, 1);

    // Right: Stop Apps
    bool anyRun = jigglerActive || badUsbActive || wifiSpamActive || bleSpamActive || deauthActive || nmapActive || evilActive || probeMonActive || bleScanActive || swRunning || timerRunning;
    drawGlassCard(spr, c1, bRowY, tW, 40, 8, anyRun ? 0xF800 : 0x2945, anyRun ? 0x4800 : 0x1423, 0x0C12);
    spr->fillCircle(c1 + tW - 9, bRowY + 9, 3, anyRun ? 0xF800 : 0x5ACB);
    spr->setTextDatum(TL_DATUM);
    spr->setTextColor(TFT_WHITE);
    spr->drawString("Stop Apps", c1 + 6, bRowY + 5, 2);
    spr->setTextColor(anyRun ? 0xF800 : 0x8CD1);
    spr->drawString(anyRun ? "Apps stoppen!" : "Inaktiv", c1 + 6, bRowY + 23, 1);
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
    drawGlassCard(spr, 10, 26, tabW, tabH, 4, loraTabMode == 0 ? 0x07FF : 0x2125, loraTabMode == 0 ? 0x1CE7 : 0x0821, 0x0821);
    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(loraTabMode == 0 ? 0xFFFF : 0x8CD1);
    spr->drawString("Chat (P2P)", 63, 37, 2);

    // Tab 1
    drawGlassCard(spr, 124, 26, tabW, tabH, 4, loraTabMode == 1 ? 0x07FF : 0x2125, loraTabMode == 1 ? 0x1CE7 : 0x0821, 0x0821);
    spr->setTextDatum(MC_DATUM);
    spr->setTextColor(loraTabMode == 1 ? 0xFFFF : 0x8CD1);
    spr->drawString("Ping Beacon", 177, 37, 2);

    if (loraTabMode == 0) {
        // === TAB 0: CHAT FEED & PRESETS ===
        // Message Feed Card (y = 52 .. 156, height = 104)
        drawGlassCard(spr, 10, 52, 220, 104, 8, 0x2125, 0x0C12, 0x0821);
        
        int nMsgs = LoraManager::getMessageCount();
        const LoraMessage* msgs = LoraManager::getMessages();

        if (nMsgs == 0) {
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(0x5ACB);
            spr->drawString("Warte auf Funk-Pakete...", 120, 95, 2);
            spr->drawString("Kanal: 868.0 MHz  SF7", 120, 115, 1);
        } else {
            int startIdx = nMsgs > 3 ? nMsgs - 3 : 0;
            int mY = 56;
            for (int i = startIdx; i < nMsgs; i++) {
                const LoraMessage &m = msgs[i];
                spr->setTextDatum(TL_DATUM);
                if (m.isOutgoing) {
                    spr->setTextColor(0xFFE0);
                    spr->drawString("> " + String(m.sender) + " (" + String(m.timeStr) + "):", 16, mY, 1);
                    spr->setTextColor(0xFFFF);
                    spr->drawString(m.text, 22, mY + 12, 2);
                } else {
                    spr->setTextColor(0x07FF);
                    String sig = String((int)m.rssi) + "dBm";
                    spr->drawString("< " + String(m.sender) + " [" + sig + "]:", 16, mY, 1);
                    spr->setTextColor(0x07E0);
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
            drawGlassCard(spr, px, 162, pW, pH, 6, 0x07FF, 0x1CE7, 0x0821);
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(0xFFFF);
            spr->drawString(presets[i], px + pW / 2, 179, 1);
        }
    } else {
        // === TAB 1: PING BEACON & TELEMETRY ===
        drawGlassCard(spr, 10, 52, 220, 96, 8, 0x2125, 0x0C12, 0x0821);
        
        spr->setTextDatum(TL_DATUM);
        spr->setTextColor(0x07FF);
        spr->drawString("LoRa Telemetrie (SX1262)", 18, 58, 2);

        spr->setTextColor(0xFFFF);
        spr->drawString("TX Pakete: " + String(LoraManager::getTxCount()), 18, 76, 2);
        spr->drawString("RX Pakete: " + String(LoraManager::getRxCount()), 18, 94, 2);
        
        String sigStr = "Signal: " + String((int)LoraManager::getLastRssi()) + " dBm / SNR: " + String(LoraManager::getLastSnr(), 1) + " dB";
        spr->setTextColor(0x8CD1);
        spr->drawString(sigStr, 18, 114, 1);
        spr->drawString("Frequenz: 868.0 MHz  BW: 125 kHz", 18, 128, 1);

        // Big Beacon Button (y = 154 .. 198)
        bool bActive = LoraManager::isBeaconActive();
        drawGlassCard(spr, 10, 154, 220, 44, 8, bActive ? 0xF800 : 0x07E0, bActive ? 0x4800 : 0x1463, 0x0C12);
        spr->setTextDatum(MC_DATUM);
        spr->setTextColor(0xFFFF);
        spr->drawString(bActive ? "BEACON STOPPEN (5s)" : "BEACON STARTEN (5s)", 120, 176, 2);
    }
}

static void renderApp(AppPackage* app, TFT_eSprite *spr, struct tm *aktuell) {
    if (!app || !spr) return;
    String target = (app->driver.length() > 0) ? app->driver : app->id;
    if (app->isCustom && target.length() == 0) {
        AppManager::renderDynamicApp(app, spr);
        return;
    }
    if (target == "stopwatch") {
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Stoppuhr", 120, 0, 2); long ms = swRunning ? (swElapsed + (millis() - swStartTime)) : swElapsed;
        char swBuf[20]; snprintf(swBuf, sizeof(swBuf), "%02d:%02d.%02d", (int)(ms/60000), (int)((ms/1000)%60), (int)((ms%1000)/10));
        int bW = get7SegWidth(String(swBuf), 22, 12, 4); draw7SegString(*spr, String(swBuf), 120 - (bW/2), 25, 22, 12, cAcc1, 4);
        if (swLapCount > 0) {
            spr->setTextDatum(MC_DATUM); spr->setTextColor(cBatBG);
            int showStart = (swLapCount > 3) ? swLapCount - 3 : 0;
            for (int i = showStart; i < swLapCount; i++) {
                int row = i - showStart;
                long lapMs = swLaps[i];
                char lapBuf[30]; snprintf(lapBuf, sizeof(lapBuf), "R%d  %02d:%02d.%02d", i+1, (int)(lapMs/60000), (int)((lapMs/1000)%60), (int)((lapMs%1000)/10));
                spr->setTextColor((i == swLapCount - 1) ? cAcc2 : cBatBG);
                spr->drawString(lapBuf, 120, 62 + row * 18, 2);
            }
        }
        spr->setTextDatum(MC_DATUM);
        spr->fillRoundRect(5, 150, 72, 40, 6, swRunning ? TFT_ORANGE : TFT_GREEN); spr->setTextColor(TFT_BLACK); spr->drawString(swRunning ? "Stop" : "Start", 41, 170, 2);
        spr->fillRoundRect(84, 150, 72, 40, 6, (swRunning && swLapCount < MAX_LAPS) ? cAcc2 : cBatBG); spr->setTextColor(swRunning ? TFT_BLACK : TFT_DARKGREY); spr->drawString("Runde", 120, 170, 2);
        spr->fillRoundRect(163, 150, 72, 40, 6, cBatBG); spr->setTextColor(cFG); spr->drawString("Reset", 199, 170, 2);
    }
    else if (target == "timer") {
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Timer", 120, 0, 2); spr->setTextDatum(MC_DATUM); 
        if (!timerRunning && timerRemaining == 0) { spr->setTextColor(cAcc2); spr->drawString("Waehlen:", 120, 50, 4); spr->fillRoundRect(20, 80, 60, 50, 6, cBatBG); spr->setTextColor(cFG); spr->drawString(String(t1)+"m", 50, 105, 2); spr->fillRoundRect(90, 80, 60, 50, 6, cBatBG); spr->drawString(String(t2)+"m", 120, 105, 2); spr->fillRoundRect(160, 80, 60, 50, 6, cBatBG); spr->drawString(String(t3)+"m", 190, 105, 2); } else { char tBuf[16]; snprintf(tBuf, sizeof(tBuf), "%02d:%02d", (int)(timerRemaining/60), (int)(timerRemaining%60)); int bW = get7SegWidth(String(tBuf), 40, 24, 6); draw7SegString(*spr, String(tBuf), 120 - (bW/2), 70, 40, 24, TFT_RED, 6); spr->fillRoundRect(75, 150, 90, 40, 6, TFT_RED); spr->setTextColor(TFT_WHITE); spr->drawString("Abbruch", 120, 170, 2); }
    }
    else if (target == "mic") {
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Dezibel-Meter", 120, 0, 2); int db = getDecibels(); spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_WHITE); spr->drawString(String(db) + " dB", 120, 50, 6); spr->setTextColor(cBatBG); spr->drawString("RMS: " + String(debug_rms, 1), 120, 85, 2); int barH = db; if(barH > 120) barH = 120; uint16_t bCol = (db >= 90) ? TFT_RED : ((db >= 65) ? TFT_YELLOW : TFT_GREEN); spr->fillRect(60, 195 - barH, 120, barH, bCol); spr->drawRect(59, 74, 122, 122, TFT_WHITE);
    }
    else if (target == "battery") {
        int batPct = getBattery();
        float batVolt = getBatteryVoltageCached(false);
        bool isCharging = isChargingCached(false);

        drawGlassCard(spr, 8, 4, 224, 64, 8, 0x39E7, 0x1425, 0x0821);
        spr->setTextDatum(TL_DATUM);
        spr->setTextColor(0xFFFF);
        char pctBuf[16]; snprintf(pctBuf, sizeof(pctBuf), "%d%%", batPct);
        spr->drawString(pctBuf, 20, 10, 4);

        spr->setTextDatum(TR_DATUM);
        spr->setTextColor(0x07FF);
        char vBuf[16]; snprintf(vBuf, sizeof(vBuf), "%.2f V", batVolt);
        spr->drawString(vBuf, 220, 12, 4);

        spr->setTextDatum(MC_DATUM);
        if (isCharging) {
            spr->fillRoundRect(20, 40, 94, 20, 10, 0x03E0);
            spr->setTextColor(0xFFFF);
            spr->drawString("⚡ LADEN", 67, 50, 2);
        } else {
            spr->fillRoundRect(20, 40, 94, 20, 10, 0x18C3);
            spr->setTextColor(0xFD20);
            spr->drawString("🔋 AKKU", 67, 50, 2);
        }

        String estStr = isCharging ? "Voll in ca. 20m" : "Laufzeit: Berechne...";
        if (!isCharging) {
            if (historyCount >= 2) {
                int drop = (batHistoryPct[0] % 100) - (batHistoryPct[historyCount - 1] % 100);
                if (drop > 0) {
                    int elapsedMins = (historyCount - 1) * (historyInterval / 60000);
                    float dropPerMin = (float)drop / (float)max(1, elapsedMins);
                    float remainingMins = batPct / max(0.001f, dropPerMin);
                    estStr = "Noch " + String((int)(remainingMins / 60)) + "h " + String((int)remainingMins % 60) + "m";
                } else {
                    estStr = "Noch > 24 Std.";
                }
            } else {
                estStr = "Noch > 18 Std.";
            }
        }
        spr->setTextDatum(TR_DATUM);
        spr->setTextColor(0x8BD0);
        spr->drawString(estStr, 220, 42, 2);

        drawGlassCard(spr, 8, 72, 224, 76, 8, 0x18C3, 0x0C61, 0x0820);
        spr->setTextDatum(TL_DATUM);
        spr->setTextColor(0x8BD0);
        spr->drawString("Verlauf (letzte 24h)", 16, 76, 1);

        int gx = 16, gy = 88, gw = 208, gh = 52;
        spr->drawFastHLine(gx, gy + gh, gw, 0x2104);
        spr->drawFastHLine(gx, gy + gh/2, gw, 0x18C3);

        if (historyCount > 1) {
            int prevX = gx;
            int prevY = gy + gh - (batHistoryPct[0] * gh / 100);
            for (int i = 1; i < historyCount; i++) {
                int cx = gx + (i * gw) / (historyCount - 1);
                int cy = gy + gh - (batHistoryPct[i] * gh / 100);
                uint16_t lCol = isCharging ? 0x07E0 : ((batHistoryPct[i] < 20) ? 0xF800 : 0x07FF);
                spr->drawLine(prevX, prevY, cx, cy, lCol);
                spr->fillCircle(cx, cy, 2, lCol);
                prevX = cx; prevY = cy;
            }
        } else {
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(0x52AA);
            spr->drawString("Sammle Daten...", 120, 114, 2);
        }

        drawGlassCard(spr, 8, 152, 224, 48, 8, 0x18C3, 0x0C61, 0x0820);
        spr->setTextDatum(TL_DATUM);
        spr->setTextColor(0x8BD0);
        spr->drawString("Normal: ~18h", 16, 158, 2);
        spr->drawString("Hacking-Tools: ~4-6h", 16, 176, 2);
        spr->setTextDatum(TR_DATUM);
        spr->setTextColor(0x07E0);
        spr->drawString("USB 5V OK", 220, 168, 2);
    }
    else if (target == "flashlight") {
        spr->fillSprite(TFT_WHITE);
    }
    else if (target == "phishing") {
        showEvilPortal(spr);
    }
    else if (target == "calc") {
        showRechner(spr);
    }
    else if (target == "jiggler") {
        showJiggler(spr);
    }
    else if (target == "badusb") {
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Rubber Ducky", 120, 10, 2);
        spr->fillRoundRect(15, 35, 30, 30, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString("<", 30, 42, 2);
        spr->fillRoundRect(195, 35, 30, 30, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString(">", 210, 42, 2);
        spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_YELLOW); spr->drawString(badUsbNames[currentBadUsbScript], 120, 50, 2);
        spr->fillRoundRect(55, 85, 130, 45, 10, badUsbActive ? TFT_RED : cBatBG);
        spr->setTextColor(TFT_WHITE); spr->drawString(badUsbActive ? "SCRIPT LÄUFT" : "START SCRIPT", 120, 108, 2);
    }
    else if (target == "wifispam") {
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("WiFi Spammer", 120, 10, 2);
        spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_YELLOW); spr->drawString("Gefakte Netzwerke:", 120, 40, 2);
        spr->fillRoundRect(55, 80, 130, 45, 10, wifiSpamActive ? TFT_RED : cBatBG);
        spr->setTextColor(TFT_WHITE); spr->drawString(wifiSpamActive ? "SPAMMING (Stop)" : "START SPAM", 120, 103, 2);
    }
    else if (target == "blespam") {
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("BLE Spammer", 120, 10, 2);
        spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_YELLOW); spr->drawString("Apple Popups", 120, 40, 2);
        spr->fillRoundRect(55, 80, 130, 45, 10, bleSpamActive ? TFT_RED : cBatBG);
        spr->setTextColor(TFT_WHITE); spr->drawString(bleSpamActive ? "SPAMMING (Stop)" : "START SPAM", 120, 103, 2);
    }
    else if (target == "nmap") {
        spr->setTextDatum(TC_DATUM); spr->setTextColor(TFT_GREEN); spr->drawString("Nmap Port 80", 120, 5, 2);
        spr->fillRoundRect(15, 35, 30, 30, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString("<", 30, 42, 2);
        spr->fillRoundRect(195, 35, 30, 30, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString(">", 210, 42, 2);
        spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_YELLOW); 
        spr->drawString(targetSSIDs[currentTargetNmap].length() > 0 ? targetSSIDs[currentTargetNmap] : "Leer", 120, 50, 2);
        if (nmapState == 0 || nmapState == 3 || nmapState == 4) {
            spr->fillRoundRect(55, 85, 130, 35, 10, nmapActive ? TFT_RED : cBatBG);
            spr->setTextColor(TFT_WHITE); spr->drawString(nmapActive ? "STOPPING..." : "CONNECT & SCAN", 120, 103, 2);
        } else {
            spr->fillRoundRect(55, 85, 130, 35, 10, TFT_RED);
            spr->setTextColor(TFT_WHITE); spr->drawString("SCANNING (Stop)", 120, 103, 2);
            spr->setTextColor(TFT_YELLOW); spr->drawString("IP: " + String(nmapIP), 120, 140, 2);
        }
        spr->setTextColor(cBatBG);
        if (nmapState == 1) spr->drawString("Verbinde...", 120, 170, 2);
        else if (nmapState == 3) spr->drawString("Abgeschlossen!", 120, 170, 2);
        else if (nmapState == 4) spr->drawString("Fehler!", 120, 170, 2);
        else if (nmapLog != "") spr->drawString("Ergebnis im Webportal", 120, 170, 2);
    }
    else if (target == "deauth") {
        spr->setTextDatum(TC_DATUM); spr->setTextColor(TFT_RED); spr->drawString("WiFi Deauther", 120, 5, 2);
        spr->fillRoundRect(15, 35, 30, 30, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString("<", 30, 42, 2);
        spr->fillRoundRect(195, 35, 30, 30, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString(">", 210, 42, 2);
        spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_YELLOW); 
        if (numScannedNetworks > 0) spr->drawString(scannedSSIDs[currentDeauthTarget], 120, 50, 2);
        else spr->drawString("Nichts gescannt", 120, 50, 2);
        spr->fillRoundRect(55, 75, 130, 35, 10, deauthActive ? TFT_RED : cBatBG);
        spr->setTextColor(TFT_WHITE); spr->drawString(deauthActive ? "ATTACKING..." : "START DEAUTH", 120, 93, 2);
        spr->fillRoundRect(65, 125, 110, 28, 6, isScanningAir ? TFT_ORANGE : cBatBG);
        spr->setTextColor(TFT_WHITE); spr->drawString(isScanningAir ? "Scanne..." : "Scan Umgebung", 120, 139, 2);
        spr->setTextColor(cBatBG);
        if (deauthState == 2) spr->drawString(scannedMACs[currentDeauthTarget], 120, 185, 2);
    }
    else if (target == "probemon") {
        spr->setTextDatum(TC_DATUM); spr->setTextColor(TFT_CYAN); spr->drawString("Probe Monitor", 120, 0, 2);
        spr->setTextDatum(TL_DATUM); spr->setTextColor(cBatBG); spr->drawString("Gefunden: " + String(probeCount), 10, 18, 2);
        int yStart = 35; int lineH = 28;
        for (int i = probeScrollOffset; i < probeCount && i < probeScrollOffset + 4; i++) {
            int yPos = yStart + (i - probeScrollOffset) * lineH;
            spr->setTextColor(TFT_WHITE); spr->drawString(probeMacs[i].substring(0, 11), 5, yPos, 1);
            spr->setTextColor(TFT_YELLOW); spr->drawString(probeSSIDs[i].substring(0, 14), 75, yPos, 1);
            spr->setTextColor(TFT_GREEN); spr->drawString(String(probeRSSI[i]) + "dB", 195, yPos, 1);
        }
        if (probeScrollOffset > 0) { spr->setTextDatum(TC_DATUM); spr->setTextColor(cBatBG); spr->drawString("^ Hoch", 120, 25, 1); }
        if (probeScrollOffset < probeCount - 4) { spr->setTextDatum(BC_DATUM); spr->setTextColor(cBatBG); spr->drawString("v Runter", 120, 138, 1); }
        spr->setTextDatum(MC_DATUM);
        spr->fillRoundRect(45, 150, 150, 35, 10, probeMonActive ? TFT_RED : TFT_CYAN);
        spr->setTextColor(probeMonActive ? TFT_WHITE : TFT_BLACK);
        spr->drawString(probeMonActive ? "STOP MONITOR" : "START MONITOR", 120, 168, 2);
    }
    else if (target == "blescan") {
        spr->setTextDatum(TC_DATUM); spr->setTextColor(TFT_MAGENTA); spr->drawString("BLE Scanner", 120, 0, 2);
        spr->setTextDatum(TL_DATUM); spr->setTextColor(cBatBG); spr->drawString("Gefunden: " + String(bleScanCount), 10, 18, 2);
        int yStart = 35; int lineH = 28;
        for (int i = bleScanScrollOffset; i < bleScanCount && i < bleScanScrollOffset + 4; i++) {
            int yPos = yStart + (i - bleScanScrollOffset) * lineH;
            spr->setTextColor(TFT_WHITE); spr->drawString(bleScanNames[i].substring(0, 14), 5, yPos, 1);
            spr->setTextColor(TFT_GREEN); spr->drawString(String(bleScanRssi[i]) + "dB", 195, yPos, 1);
        }
        if (bleScanScrollOffset > 0) { spr->setTextDatum(TC_DATUM); spr->setTextColor(cBatBG); spr->drawString("^ Hoch", 120, 25, 1); }
        if (bleScanScrollOffset < bleScanCount - 4) { spr->setTextDatum(BC_DATUM); spr->setTextColor(cBatBG); spr->drawString("v Runter", 120, 138, 1); }
        spr->setTextDatum(MC_DATUM);
        spr->fillRoundRect(45, 150, 150, 35, 10, bleScanActive ? TFT_RED : TFT_MAGENTA);
        spr->setTextColor(TFT_WHITE);
        spr->drawString(bleScanActive ? "STOP SCAN" : "START SCAN", 120, 168, 2);
    }
    else if (target == "dice") {
        showDice(spr);
    }
    else if (target == "level") {
        showLevel(spr);
    }
    else if (target == "totp") {
        showTotpAuth(spr);
    }
    else if (target == "notif") {
        showNotificationCenter(spr);
    }
    else if (target == "lora") {
        drawLoraMessenger(spr, aktuell);
    }
    else {
        AppManager::renderDynamicApp(app, spr);
    }
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

        if (watchfaceMode == 0) {
            // === 1. TOP PILL: Datum & Wochentag (3D Glassmorphic) ===
            static const char* monKurz[] = {"Jan", "Feb", "Mrz", "Apr", "Mai", "Jun", "Jul", "Aug", "Sep", "Okt", "Nov", "Dez"};
            char dPill[32];
            snprintf(dPill, sizeof(dPill), "%s, %02d. %s", wochentage[wday], mday, monKurz[mon]);
            
            drawGlassCard(spr, 28, 4, 184, 22, 11, 0x3186, 0x1CE7, 0x10A2);
            spr->fillCircle(40, 15, 3, 0x07FF); // Glowing cyan beacon
            spr->fillCircle(40, 15, 1, 0xFFFF);
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(0xFFE0); // Bright Gold
            spr->drawString(dPill, 126, 15, 2);

            // === 2. HERO TIME: Apple Watch Ultra Digital Clock ===
            char tBuf[8];
            snprintf(tBuf, sizeof(tBuf), "%02d:%02d", aktuell->tm_hour, aktuell->tm_min);
            spr->setTextDatum(MR_DATUM);
            spr->setTextColor(0xFFFF, cBG);
            spr->drawString(tBuf, 148, 48, 7);

            char sBuf[6];
            snprintf(sBuf, sizeof(sBuf), ":%02d", aktuell->tm_sec);
            spr->setTextDatum(ML_DATUM);
            spr->setTextColor(0x07FF, cBG); // Electric Cyan
            spr->drawString(sBuf, 152, 50, 4);

            // === 3. 3D CHROMATIC ACTIVITY RINGS ===
            int goal = (stepGoal > 0) ? stepGoal : 8000;
            float stepRatio = (goal > 0) ? ((float)stepCount / (float)goal) : 0.0f;
            float batRatio  = (float)getBattery() / 100.0f;
            float timeRatio = (float)(aktuell->tm_hour * 60 + aktuell->tm_min) / 1440.0f;
            drawActivityRings(spr, 48, 114, stepRatio, batRatio, timeRatio);

            // === 4. COMPLICATION CARDS (Rechts neben den Ringen) ===
            int cX = 98, cW = 132;
            
            // Schritte Card
            drawGlassCard(spr, cX, 74, cW, 36, 8, 0x0540, 0x1463, 0x0A22);
            spr->fillCircle(cX + 12, 88, 4, 0x07E0); // Glowing emerald
            spr->fillCircle(cX + 12, 88, 1, 0xFFFF);
            spr->setTextDatum(TL_DATUM);
            spr->setTextColor(0xFFFF);
            spr->drawString(String(stepCount) + " Steps", cX + 22, 78, 2);
            int sBarW = cW - 28;
            int sFillW = (int)(stepRatio * sBarW);
            if (sFillW > sBarW) sFillW = sBarW;
            spr->fillRoundRect(cX + 22, 98, sBarW, 4, 2, 0x18C4);
            if (sFillW > 1) spr->fillRoundRect(cX + 22, 98, sFillW, 4, 2, 0x07E0);

            // Akku & Power Card
            drawGlassCard(spr, cX, 116, cW, 36, 8, 0x62C0, 0x2180, 0x10A2);
            spr->fillCircle(cX + 12, 130, 4, 0xFD20); // Glowing amber
            spr->fillCircle(cX + 12, 130, 1, 0xFFFF);
            spr->setTextDatum(TL_DATUM);
            spr->setTextColor(0xFFFF);
            spr->drawString(String(getBattery()) + "% Akku", cX + 22, 120, 2);
            spr->setTextColor(0xFD20);
            bool isChg = isChargingCached(false);
            String voltPwr = String(getBatteryVoltageCached(false), 2) + "V  " + (isChg ? "Laden" : "Akku");
            spr->drawString(voltPwr, cX + 22, 136, 1);
            if (isChg) {
                int lX = cX + cW - 14, lY = 124;
                spr->drawLine(lX + 3, lY, lX, lY + 5, 0xFFE0);
                spr->drawLine(lX, lY + 5, lX + 4, lY + 5, 0xFFE0);
                spr->drawLine(lX + 4, lY + 5, lX + 1, lY + 10, 0xFFFF);
            }

            // === 5. BOTTOM PILL: Lebensziel-Vorschau (3D Glassmorphic) ===
            drawGlassCard(spr, 12, 160, 216, 26, 13, 0x2A67, 0x1425, 0x0821);
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(0x07FF);
            char goalPill[44];
            snprintf(goalPill, sizeof(goalPill), "Lebensziel: %dJ %d Tage  >", yearsLeft, daysLeft);
            spr->drawString(goalPill, 120, 173, 2);
        } else if (watchfaceMode == 1) {
            // === MODE 1: CLASSIC SWISS CHRONOGRAPH ===
            drawChronographWatchface(spr, aktuell, getBattery(), isChargingCached(false));
        } else if (watchfaceMode == 2) {
            // === MODE 2: RETRO CASIO F-91W / ILLUMINATOR ===
            drawRetroCasioWatchface(spr, aktuell, getBattery(), isChargingCached(false));
        } else {
            // === MODE 3: MODERNISIERTE LEBENSUHR MIT 3D CARDS ===
            int vals[] = {(int)(diff / 31536000L), (int)((diff % 31536000L) / 2592000L), (int)((diff % 2592000L) / 86400L), (int)((diff % 86400L) / 3600L), (int)((diff % 3600L) / 60L), (int)(diff % 60L)};
            const char* labs[] = {"Jahre", "Monate", "Tage", "Stunden", "Minuten", "Sekunden"};
            int rowH = 28; 
            int sH = 20; int sW = 12; 
            for(int i = 0; i < 6; i++) { 
                int y = 4 + i * (rowH + 3);
                drawGlassCard(spr, 8, y, 196, rowH, 6, (i < 3) ? 0x62C0 : 0x2184, 0x1CE7, 0x10A2);
                char buf[5]; snprintf(buf, sizeof(buf), "%02d", vals[i]); 
                draw7SegString(*spr, String(buf), 16, y + 4, sH, sW, (i < 3) ? 0xFFFF : 0xFFE0, 3); 
                spr->setTextDatum(ML_DATUM); 
                spr->setTextColor((i < 3) ? 0x07FF : 0x8BD0); 
                spr->drawString(labs[i], 66, y + rowH/2, 2); 
            }
            
            // Lebensziel Progress Bar Rechts mit 3D Säule
            float aktAlter = (float)(aktuell->tm_year + 1900 - gJahr) + (float)aktuell->tm_yday/365.0f; 
            float prozent = (zielAlter > 0) ? (aktAlter / (float)zielAlter) : 0.0f; 
            if (prozent > 1.0f) prozent = 1.0f;
            if (prozent < 0.0f) prozent = 0.0f;
            int fullBarH = 183; 
            int fillH = (int)(fullBarH * prozent); 
            drawGlassCard(spr, 212, 4, 18, fullBarH, 9, 0x3186, 0x1CE7, 0x0821);
            uint16_t pCol = (prozent > 0.8f) ? TFT_RED : (prozent > 0.5f ? TFT_ORANGE : 0x07E0);
            if (fillH > 4) {
                spr->fillRoundRect(214, (4 + fullBarH) - fillH, 14, fillH, 7, pCol);
                spr->drawFastHLine(215, (4 + fullBarH) - fillH + 1, 12, 0xFFFF); // Specular head
            }
        }
    } 
    else if (page == 1) {
        spr->setTextDatum(TC_DATUM);
        spr->setTextColor(0xFFE0); // Gold
        spr->drawString("Opportunitaet", 120, 10, 4);
        
        float dailyNet = (netSalary * 12.0) / 365.0;
        float fraction = (millis() % 1000) / 1000.0;
        long secToday = aktuell->tm_hour * 3600 + aktuell->tm_min * 60 + aktuell->tm_sec;
        float earnedToday = dailyNet * ((secToday + fraction) / 86400.0);
        
        // 3D Glass-Karte: Heutiger Verdienst
        drawGlassCard(spr, 12, 40, 216, 64, 10, 0x62C0, 0x2180, 0x10A2);
        
        spr->setTextDatum(TL_DATUM);
        spr->setTextColor(0xAD55);
        spr->drawString("Heute verdient", 22, 46, 2);
        
        char moneyBuf[16];
        snprintf(moneyBuf, sizeof(moneyBuf), "%.4f", earnedToday);
        int mH = 26; int mW = 15; int mSpace = 4;
        int totalW = get7SegWidth(String(moneyBuf), mH, mW, mSpace);
        draw7SegString(*spr, String(moneyBuf), 22, 68, mH, mW, 0xFFFF, mSpace);
        
        spr->setTextDatum(TL_DATUM);
        spr->setTextColor(0x07E0); // Emerald EUR
        spr->drawString("EUR", 22 + totalW + 6, 74, 2);

        // 3D Glass-Karte: Echter Stundenlohn
        float workDaysPerYear = workDaysYear - vacationDays - sickDays;
        float workHoursPerYear = workDaysPerYear * workHoursPerDay * (partTimePct / 100.0);
        float trueHourlyWage = 0;
        if (workHoursPerYear > 0) trueHourlyWage = (netSalary * 12.0) / workHoursPerYear;
        
        drawGlassCard(spr, 12, 112, 216, 64, 10, 0x0540, 0x1CE7, 0x10A2);
        
        spr->setTextDatum(TL_DATUM);
        spr->setTextColor(0xAD55);
        spr->drawString("Echter Stundenlohn", 22, 118, 2);
        
        char hrBuf[16];
        snprintf(hrBuf, sizeof(hrBuf), "%.2f", trueHourlyWage); 
        int hH = 24; int hW = 14; int hSpace = 3;
        int hTotalW = get7SegWidth(String(hrBuf), hH, hW, hSpace);
        draw7SegString(*spr, String(hrBuf), 22, 140, hH, hW, 0x07FF, hSpace);
        
        spr->setTextDatum(TL_DATUM);
        spr->setTextColor(0x07FF);
        spr->drawString("EUR/h", 22 + hTotalW + 6, 146, 2);
    }
    else if (page == 2) {
        int activeKids = 0; String kNames[3]; String kDates[3]; String kTimes[3];
        if(child1Name.length() > 0 && child1Date.length() == 10) { kNames[activeKids] = child1Name; kDates[activeKids] = child1Date; kTimes[activeKids] = child1Time; activeKids++; }
        if(child2Name.length() > 0 && child2Date.length() == 10) { kNames[activeKids] = child2Name; kDates[activeKids] = child2Date; kTimes[activeKids] = child2Time; activeKids++; }
        if(child3Name.length() > 0 && child3Date.length() == 10) { kNames[activeKids] = child3Name; kDates[activeKids] = child3Date; kTimes[activeKids] = child3Time; activeKids++; }
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
            int64_t myAgeSec = (int64_t)jetzt - (int64_t)uTime;
            time_t targetDate = (time_t)((int64_t)pTime + myAgeSec);
            struct tm tgt;
            localtime_r(&targetDate, &tgt);
            const char* monate[] = {"Januar", "Februar", "Maerz", "April", "Mai", "Juni", "Juli", "August", "September", "Oktober", "November", "Dezember"};
            char dBuf[30]; snprintf(dBuf, sizeof(dBuf), "%02d. %s %04d", tgt.tm_mday, monate[tgt.tm_mon], tgt.tm_year+1900);
            struct tm uTm;
            localtime_r(&uTime, &uTm);
            int yearsThen = (tgt.tm_year + 1900) - (uTm.tm_year + 1900);
            if (tgt.tm_mon < uTm.tm_mon || (tgt.tm_mon == uTm.tm_mon && tgt.tm_mday < uTm.tm_mday)) yearsThen--;
            spr->setTextDatum(MC_DATUM); spr->setTextColor(cBatBG); spr->drawString("Heute war für " + parentName, 120, 30, 2); spr->drawString("in deinem Alter der", 120, 50, 2);
            spr->setTextColor(cFG); spr->drawString(dBuf, 120, 85, 4); spr->setTextColor(cBatBG); spr->drawString("An diesem Tag warst Du", 120, 135, 2);
            if (yearsThen < 0) { spr->setTextColor(cAcc2); spr->drawString("Noch nicht", 120, 160, 4); spr->drawString("geboren.", 120, 185, 4); } else { spr->setTextColor(cAcc2); spr->drawString(String(yearsThen) + " Jahre", 120, 175, 4); }
        } else { spr->setTextDatum(MC_DATUM); spr->setTextColor(cBatBG); spr->drawString("Daten fehlen", 120, 100, 2); }
    }
    // === Page 4 reserviert (nicht mehr in Watchface-Rotation) ===
    else if (page == 4) {
        // Leer — AOD wird nur im Web-Dashboard konfiguriert
    }
    else if (page == 5) {
        drawControlCenter(spr, aktuell);
    }
    else if (page == 6) { 
        spr->setTextDatum(TC_DATUM);
        spr->setTextColor(0xFFFF);
        spr->drawString("Apps & Packages", 120, 2, 2);
        
        // PRODUCTIVITY APPS CARD (Links) - 3D Glass
        drawGlassCard(spr, 12, 26, 104, 170, 12, 0x07E0, 0x1463, 0x0A22);
        
        spr->setTextDatum(MC_DATUM);
        spr->fillCircle(64, 54, 14, tft.color565(15, 45, 20));
        spr->drawCircle(64, 54, 14, 0x07E0);
        spr->fillCircle(64, 54, 1, 0xFFFF);
        spr->setTextColor(0x07E0);
        spr->drawString("+", 64, 54, 4);
        
        spr->setTextColor(0x07E0);
        spr->drawString("PROD", 64, 88, 4);
        spr->setTextColor(0xFFFF);
        int prodCount = AppManager::getAppCount(CAT_PRODUCTIVITY, true);
        char prodBuf[20]; snprintf(prodBuf, sizeof(prodBuf), "%d Apps", prodCount);
        spr->drawString(prodBuf, 64, 114, 2);
        spr->setTextColor(0x8BD0);
        spr->drawString("Stoppuhr, LoRa", 64, 138, 1);
        spr->drawString("2FA, Rechner", 64, 150, 1);
        spr->drawString("Timer, Waage", 64, 162, 1);

        // SECURITY APPS CARD (Rechts) - 3D Glass
        drawGlassCard(spr, 124, 26, 104, 170, 12, TFT_RED, 0x3841, 0x1800);
        
        spr->setTextDatum(MC_DATUM);
        spr->fillCircle(176, 54, 14, tft.color565(55, 15, 15));
        spr->drawCircle(176, 54, 14, TFT_RED);
        spr->fillCircle(176, 54, 1, 0xFFFF);
        spr->setTextColor(TFT_RED);
        spr->drawString("!", 176, 54, 4);
        
        spr->setTextColor(TFT_RED);
        spr->drawString("SEC", 176, 88, 4);
        spr->setTextColor(0xFFFF);
        int secCount = AppManager::getAppCount(CAT_SECURITY, true);
        char secBuf[20]; snprintf(secBuf, sizeof(secBuf), "%d Apps", secCount);
        spr->drawString(secBuf, 176, 114, 2);
        spr->setTextColor(0x8BD0);
        spr->drawString("Rubber Ducky", 176, 138, 1);
        spr->drawString("WiFi/BLE Spam", 176, 150, 1);
        spr->drawString("Deauth, Portal", 176, 162, 1);
        
        bool anyBadRun = badUsbActive || wifiSpamActive || bleSpamActive || deauthActive || nmapActive || evilActive || probeMonActive || bleScanActive;
        if (anyBadRun) {
            spr->fillCircle(176, 185, 4, TFT_RED);
            spr->fillCircle(176, 185, 1, 0xFFFF);
            spr->setTextColor(TFT_RED);
            spr->drawString("AKTIV", 176, 177, 1);
        }
    }
    else if (page == 7) { 
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Stoppuhr", 120, 0, 2); long ms = swRunning ? (swElapsed + (millis() - swStartTime)) : swElapsed;
        char swBuf[20]; snprintf(swBuf, sizeof(swBuf), "%02d:%02d.%02d", (int)(ms/60000), (int)((ms/1000)%60), (int)((ms%1000)/10));
        int bW = get7SegWidth(String(swBuf), 22, 12, 4); draw7SegString(*spr, String(swBuf), 120 - (bW/2), 25, 22, 12, cAcc1, 4);
        // Rundenzeiten anzeigen (letzte 3)
        if (swLapCount > 0) {
            spr->setTextDatum(MC_DATUM); spr->setTextColor(cBatBG);
            int showStart = (swLapCount > 3) ? swLapCount - 3 : 0;
            for (int i = showStart; i < swLapCount; i++) {
                int row = i - showStart;
                long lapMs = swLaps[i];
                char lapBuf[30]; snprintf(lapBuf, sizeof(lapBuf), "R%d  %02d:%02d.%02d", i+1, (int)(lapMs/60000), (int)((lapMs/1000)%60), (int)((lapMs%1000)/10));
                spr->setTextColor((i == swLapCount - 1) ? cAcc2 : cBatBG);
                spr->drawString(lapBuf, 120, 62 + row * 18, 2);
            }
        }
        // Buttons: Start/Stop | Runde | Reset
        spr->setTextDatum(MC_DATUM);
        spr->fillRoundRect(5, 150, 72, 40, 6, swRunning ? TFT_ORANGE : TFT_GREEN); spr->setTextColor(TFT_BLACK); spr->drawString(swRunning ? "Stop" : "Start", 41, 170, 2);
        spr->fillRoundRect(84, 150, 72, 40, 6, (swRunning && swLapCount < MAX_LAPS) ? cAcc2 : cBatBG); spr->setTextColor(swRunning ? TFT_BLACK : TFT_DARKGREY); spr->drawString("Runde", 120, 170, 2);
        spr->fillRoundRect(163, 150, 72, 40, 6, cBatBG); spr->setTextColor(cFG); spr->drawString("Reset", 199, 170, 2);
    }
    else if (page == 8) { 
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Timer", 120, 0, 2); spr->setTextDatum(MC_DATUM); 
        if (!timerRunning && timerRemaining == 0) { spr->setTextColor(cAcc2); spr->drawString("Waehlen:", 120, 50, 4); spr->fillRoundRect(20, 80, 60, 50, 6, cBatBG); spr->setTextColor(cFG); spr->drawString(String(t1)+"m", 50, 105, 2); spr->fillRoundRect(90, 80, 60, 50, 6, cBatBG); spr->drawString(String(t2)+"m", 120, 105, 2); spr->fillRoundRect(160, 80, 60, 50, 6, cBatBG); spr->drawString(String(t3)+"m", 190, 105, 2); } else { char tBuf[16]; snprintf(tBuf, sizeof(tBuf), "%02d:%02d", (int)(timerRemaining/60), (int)(timerRemaining%60)); int bW = get7SegWidth(String(tBuf), 40, 24, 6); draw7SegString(*spr, String(tBuf), 120 - (bW/2), 70, 40, 24, TFT_RED, 6); spr->fillRoundRect(75, 150, 90, 40, 6, TFT_RED); spr->setTextColor(TFT_WHITE); spr->drawString("Abbruch", 120, 170, 2); }
    }
    else if (page == 9) { 
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Dezibel-Meter", 120, 0, 2); int db = getDecibels(); spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_WHITE); spr->drawString(String(db) + " dB", 120, 50, 6); spr->setTextColor(cBatBG); spr->drawString("RMS: " + String(debug_rms, 1), 120, 85, 2); int barH = db; if(barH > 120) barH = 120; uint16_t bCol = (db >= 90) ? TFT_RED : ((db >= 65) ? TFT_YELLOW : TFT_GREEN); spr->fillRect(60, 195 - barH, 120, barH, bCol); spr->drawRect(59, 74, 122, 122, TFT_WHITE);
    }
    else if (page == 10) {
        int batPct = getBattery();
        float batVolt = getBatteryVoltageCached(false);
        bool isCharging = isChargingCached(false);

        // 1. Top 3D Glass Hero Card (y=4, h=64)
        drawGlassCard(spr, 8, 4, 224, 64, 8, 0x39E7, 0x1425, 0x0821);

        spr->setTextDatum(TL_DATUM);
        spr->setTextColor(0xFFFF);
        char pctBuf[16]; snprintf(pctBuf, sizeof(pctBuf), "%d%%", batPct);
        spr->drawString(pctBuf, 20, 10, 4);

        spr->setTextDatum(TR_DATUM);
        spr->setTextColor(0x07FF);
        char vBuf[16]; snprintf(vBuf, sizeof(vBuf), "%.2f V", batVolt);
        spr->drawString(vBuf, 220, 12, 4);

        // Status Pill inside Card
        spr->setTextDatum(MC_DATUM);
        if (isCharging) {
            spr->fillRoundRect(20, 40, 94, 20, 10, 0x03E0);
            spr->setTextColor(0xFFFF);
            spr->drawString("⚡ LADEN", 67, 50, 2);
        } else {
            spr->fillRoundRect(20, 40, 94, 20, 10, 0x18C3);
            spr->setTextColor(0xFD20);
            spr->drawString("🔋 AKKU", 67, 50, 2);
        }

        // Restlaufzeit / ETA
        String estStr = isCharging ? "Voll in ca. 20m" : "Laufzeit: Berechne...";
        if (!isCharging) {
            if (historyCount >= 2) {
                int drop = (batHistoryPct[0] % 100) - (batHistoryPct[historyCount - 1] % 100);
                if (drop > 0) {
                    int elapsedMins = (historyCount - 1) * (historyInterval / 60000);
                    float dropPerMin = (float)drop / (float)max(1, elapsedMins);
                    float remainingMins = batPct / max(0.001f, dropPerMin);
                    estStr = "Noch " + String((int)(remainingMins / 60)) + "h " + String((int)remainingMins % 60) + "m";
                } else {
                    estStr = "Noch > 24 Std.";
                }
            } else {
                estStr = "Noch > 18 Std.";
            }
        }
        spr->setTextDatum(TR_DATUM);
        spr->setTextColor(0xFFE0);
        spr->drawString(estStr, 220, 42, 2);

        // 2. Middle 3D Battery Graph Card (y=72, h=76)
        drawGlassCard(spr, 8, 72, 224, 76, 8, 0x2104, 0x1082, 0x0000);
        int gX = 18, gY = 78, gW = 204, gH = 62;
        spr->drawFastHLine(gX, gY, gW, 0x18C3);
        spr->drawFastHLine(gX, gY + gH / 2, gW, 0x18C3);
        spr->drawFastHLine(gX, gY + gH - 1, gW, 0x18C3);

        if (historyCount > 1) {
            float stepX = (float)gW / max(1, historyCount - 1);
            for (int i = 0; i < historyCount - 1; i++) {
                uint16_t raw1 = batHistoryPct[i]; uint16_t raw2 = batHistoryPct[i + 1];
                int p1 = raw1 % 100; if (raw1 >= 200 && p1 == 0 && raw1 != 200) p1 = 100;
                int p2 = raw2 % 100; if (raw2 >= 200 && p2 == 0 && raw2 != 200) p2 = 100;
                int px1 = gX + (int)(i * stepX);
                int px2 = gX + (int)((i + 1) * stepX);
                int py1 = gY + gH - (p1 * gH / 100);
                int py2 = gY + gH - (p2 * gH / 100);
                if (py1 < gY) py1 = gY; if (py1 > gY + gH - 1) py1 = gY + gH - 1;
                if (py2 < gY) py2 = gY; if (py2 > gY + gH - 1) py2 = gY + gH - 1;

                uint16_t col = (raw1 >= 200) ? 0x07FF : ((p1 >= 50) ? 0x07E0 : (p1 >= 25 ? 0xFD20 : 0xF800));
                spr->fillTriangle(px1, gY + gH - 1, px2, gY + gH - 1, px1, py1, blendColor565(col, 0x0000, 100));
                spr->fillTriangle(px1, py1, px2, py2, px2, gY + gH - 1, blendColor565(col, 0x0000, 100));
                spr->drawLine(px1, py1, px2, py2, col);
            }
            int lastPx = gX + gW - 2;
            int lastP = batPct;
            int lastPy = gY + gH - (lastP * gH / 100);
            spr->fillCircle(lastPx, lastPy, 4, 0x07FF);
            spr->fillCircle(lastPx, lastPy, 2, 0xFFFF);
        } else {
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(0x5ACB);
            spr->drawString("Historie sammelt Daten...", 120, gY + gH / 2, 2);
        }

        // 3. Bottom Steps / Health Complication Card (y=152, h=50)
        drawGlassCard(spr, 8, 152, 224, 50, 8, 0x1442, 0x0A22, 0x0821);
        spr->setTextDatum(TL_DATUM);
        spr->setTextColor(0x8CD1);
        spr->drawString("Schritte heute", 20, 157, 2);

        spr->setTextDatum(TR_DATUM);
        spr->setTextColor(0x07E0);
        spr->drawString(String(stepCount) + " / " + String(stepGoal), 220, 157, 2);

        int sBarW = 200;
        float sProg = (stepGoal > 0) ? min(1.0f, (float)stepCount / (float)stepGoal) : 0.0f;
        int sFill = (int)(sBarW * sProg);
        spr->fillRoundRect(20, 180, sBarW, 8, 4, 0x18C3);
        if (sFill > 2) {
            spr->fillRoundRect(20, 180, sFill, 8, 4, 0x07E0);
            spr->drawFastHLine(21, 181, sFill - 2, 0xFFFF);
        }
    }
    else if (page == 11) { spr->fillSprite(TFT_WHITE); }
    else if (page == 12) { showEvilPortal(spr); } 
    else if (page == 13) { showRechner(spr); }
    else if (page == 14) { showJiggler(spr); }
    else if (page == 15) { 
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("Rubber Ducky", 120, 10, 2);
        spr->fillRoundRect(15, 35, 30, 30, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString("<", 30, 42, 2);
        spr->fillRoundRect(195, 35, 30, 30, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString(">", 210, 42, 2);
        spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_YELLOW); spr->drawString(badUsbNames[currentBadUsbScript], 120, 50, 2);
        spr->fillRoundRect(55, 85, 130, 45, 10, badUsbActive ? TFT_RED : cBatBG);
        spr->setTextColor(TFT_WHITE); spr->drawString(badUsbActive ? "SCRIPT LÄUFT" : "START SCRIPT", 120, 108, 2);
    }
    else if (page == 16) { 
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("WiFi Spammer", 120, 10, 2);
        spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_YELLOW); spr->drawString("Gefakte Netzwerke:", 120, 40, 2);
        spr->fillRoundRect(55, 80, 130, 45, 10, wifiSpamActive ? TFT_RED : cBatBG);
        spr->setTextColor(TFT_WHITE); spr->drawString(wifiSpamActive ? "SPAMMING (Stop)" : "START SPAM", 120, 103, 2);
    }
    else if (page == 17) { 
        spr->setTextDatum(TC_DATUM); spr->setTextColor(cFG); spr->drawString("BLE Spammer", 120, 10, 2);
        spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_YELLOW); spr->drawString("Apple Popups", 120, 40, 2);
        spr->fillRoundRect(55, 80, 130, 45, 10, bleSpamActive ? TFT_RED : cBatBG);
        spr->setTextColor(TFT_WHITE); spr->drawString(bleSpamActive ? "SPAMMING (Stop)" : "START SPAM", 120, 103, 2);
    }
    else if (page == 18) {
        spr->setTextDatum(TC_DATUM); spr->setTextColor(TFT_GREEN); spr->drawString("Nmap Port 80", 120, 5, 2);
        spr->fillRoundRect(15, 35, 30, 30, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString("<", 30, 42, 2);
        spr->fillRoundRect(195, 35, 30, 30, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString(">", 210, 42, 2);
        
        spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_YELLOW); 
        spr->drawString(targetSSIDs[currentTargetNmap].length() > 0 ? targetSSIDs[currentTargetNmap] : "Leer", 120, 50, 2);

        if (nmapState == 0 || nmapState == 3 || nmapState == 4) {
            spr->fillRoundRect(55, 85, 130, 35, 10, nmapActive ? TFT_RED : cBatBG);
            spr->setTextColor(TFT_WHITE); spr->drawString(nmapActive ? "STOPPING..." : "CONNECT & SCAN", 120, 103, 2);
        } else {
            spr->fillRoundRect(55, 85, 130, 35, 10, TFT_RED);
            spr->setTextColor(TFT_WHITE); spr->drawString("SCANNING (Stop)", 120, 103, 2);
            spr->setTextColor(TFT_YELLOW); spr->drawString("IP: " + String(nmapIP), 120, 140, 2);
        }
        
        spr->setTextColor(cBatBG);
        if (nmapState == 1) spr->drawString("Verbinde...", 120, 170, 2);
        else if (nmapState == 3) spr->drawString("Abgeschlossen!", 120, 170, 2);
        else if (nmapState == 4) spr->drawString("Fehler!", 120, 170, 2);
        else if (nmapLog != "") spr->drawString("Ergebnis im Webportal", 120, 170, 2);
        
    }
    else if (page == 19) {
        spr->setTextDatum(TC_DATUM); spr->setTextColor(TFT_RED); spr->drawString("WiFi Deauther", 120, 5, 2);
        spr->fillRoundRect(15, 35, 30, 30, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString("<", 30, 42, 2);
        spr->fillRoundRect(195, 35, 30, 30, 5, cBatBG); spr->setTextColor(TFT_WHITE); spr->drawString(">", 210, 42, 2);
        
        spr->setTextDatum(MC_DATUM); spr->setTextColor(TFT_YELLOW); 
        if (numScannedNetworks > 0) spr->drawString(scannedSSIDs[currentDeauthTarget], 120, 50, 2);
        else spr->drawString("Nichts gescannt", 120, 50, 2);

        spr->fillRoundRect(55, 75, 130, 35, 10, deauthActive ? TFT_RED : cBatBG);
        spr->setTextColor(TFT_WHITE); spr->drawString(deauthActive ? "ATTACKING..." : "START DEAUTH", 120, 93, 2);

        spr->fillRoundRect(65, 125, 110, 28, 6, isScanningAir ? TFT_ORANGE : cBatBG);
        spr->setTextColor(TFT_WHITE); spr->drawString(isScanningAir ? "Scanne..." : "Scan Umgebung", 120, 139, 2);

        spr->setTextColor(cBatBG);
        if (deauthState == 2) spr->drawString(scannedMACs[currentDeauthTarget], 120, 185, 2);

    }
    else if (page == 20) {
        // Probe Request Monitor
        spr->setTextDatum(TC_DATUM); spr->setTextColor(TFT_CYAN); spr->drawString("Probe Monitor", 120, 0, 2);
        spr->setTextDatum(TL_DATUM); spr->setTextColor(cBatBG); spr->drawString("Gefunden: " + String(probeCount), 10, 18, 2);
        int yStart = 35; int lineH = 28;
        for (int i = probeScrollOffset; i < probeCount && i < probeScrollOffset + 4; i++) {
            int yPos = yStart + (i - probeScrollOffset) * lineH;
            spr->setTextColor(TFT_WHITE); spr->drawString(probeMacs[i].substring(0, 11), 5, yPos, 1);
            spr->setTextColor(TFT_YELLOW); spr->drawString(probeSSIDs[i].substring(0, 14), 75, yPos, 1);
            spr->setTextColor(TFT_GREEN); spr->drawString(String(probeRSSI[i]) + "dB", 195, yPos, 1);
        }
        if (probeScrollOffset > 0) { spr->setTextDatum(TC_DATUM); spr->setTextColor(cBatBG); spr->drawString("^ Hoch", 120, 25, 1); }
        if (probeScrollOffset < probeCount - 4) { spr->setTextDatum(BC_DATUM); spr->setTextColor(cBatBG); spr->drawString("v Runter", 120, 138, 1); }
        spr->setTextDatum(MC_DATUM);
        spr->fillRoundRect(45, 150, 150, 35, 10, probeMonActive ? TFT_RED : TFT_CYAN);
        spr->setTextColor(probeMonActive ? TFT_WHITE : TFT_BLACK);
        spr->drawString(probeMonActive ? "STOP MONITOR" : "START MONITOR", 120, 168, 2);
    }
    else if (page == 21) {
        // BLE Scanner
        spr->setTextDatum(TC_DATUM); spr->setTextColor(TFT_MAGENTA); spr->drawString("BLE Scanner", 120, 0, 2);
        spr->setTextDatum(TL_DATUM); spr->setTextColor(cBatBG); spr->drawString("Gefunden: " + String(bleScanCount), 10, 18, 2);
        int yStart = 35; int lineH = 28;
        for (int i = bleScanScrollOffset; i < bleScanCount && i < bleScanScrollOffset + 4; i++) {
            int yPos = yStart + (i - bleScanScrollOffset) * lineH;
            spr->setTextColor(TFT_WHITE); spr->drawString(bleScanNames[i].substring(0, 14), 5, yPos, 1);
            spr->setTextColor(TFT_GREEN); spr->drawString(String(bleScanRssi[i]) + "dB", 195, yPos, 1);
        }
        if (bleScanScrollOffset > 0) { spr->setTextDatum(TC_DATUM); spr->setTextColor(cBatBG); spr->drawString("^ Hoch", 120, 25, 1); }
        if (bleScanScrollOffset < bleScanCount - 4) { spr->setTextDatum(BC_DATUM); spr->setTextColor(cBatBG); spr->drawString("v Runter", 120, 138, 1); }
        spr->setTextDatum(MC_DATUM);
        spr->fillRoundRect(45, 150, 150, 35, 10, bleScanActive ? TFT_RED : TFT_MAGENTA);
        spr->setTextColor(TFT_WHITE);
        spr->drawString(bleScanActive ? "STOP SCAN" : "START SCAN", 120, 168, 2);
    }
    else if (page == 22) { renderGoodMenu(spr); }
    else if (page == 23) { renderBadMenu(spr); }
    else if (page == 24) { showDice(spr); }
    else if (page == 25) { showLevel(spr); }
    else if (page == 26) { showTotpAuth(spr); }
    else if (page == 27) { showNotificationCenter(spr); }
    else if (page == Pages::LoraMessenger) { drawLoraMessenger(spr, aktuell); }
    
    // Page indicator dots for watchfaces (pages 0-3 only, Page 4 = AOD Manager Info)
    if (page >= 0 && page <= 3) {
        int dotY = spr->height() - 7;
        int enabledPages[4]; int numEnabled = 0;
        if (enP0) enabledPages[numEnabled++] = 0;
        if (enP1) enabledPages[numEnabled++] = 1;
        if (enP2) enabledPages[numEnabled++] = 2;
        if (enP3) enabledPages[numEnabled++] = 3;
        if (numEnabled > 1) {
            int totalW = numEnabled * 16;
            int startX = 120 - totalW / 2;
            for (int i = 0; i < numEnabled; i++) {
                int dotX = startX + i * 16;
                if (enabledPages[i] == page) spr->fillRoundRect(dotX, dotY - 2, 12, 4, 2, cAcc1);
                else spr->fillCircle(dotX + 6, dotY, 2, cBatBG);
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

    if (millis() < toastEndTime) { int tW = spr->textWidth(toastMsg, 2) + 40; int tX = (240 - tW) / 2; spr->fillRoundRect(tX, 165, tW, 30, 15, toastColor); spr->setTextColor(TFT_WHITE, toastColor); spr->drawCentreString(toastMsg, 120, 173, 2); }
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

    if (currentPage != 11) drawHeader(aktuell, getBattery(), isCharging);
    cdSprite.fillSprite(currentPage == 11 ? TFT_WHITE : cBG);
    renderPage(currentPage, &cdSprite, aktuell);
    if (currentPage != 11) cdSprite.pushSprite(0, HEAD_H); else cdSprite.pushSprite(0, 0);

    Serial.printf("[SLIDE] OK -> page %d (heap=%d)\n", currentPage, ESP.getFreeHeap());
}

void renderUI(unsigned long currentMillis) {
    time_t jetzt; time(&jetzt); struct tm *aktuell = localtime(&jetzt); bool isCharging = isChargingCached(false); static unsigned long lastFrame = 0;

    bool fastRefresh = (currentPage == 1 || currentPage == 7 || currentPage == 9 || currentPage == 25); 
    bool forceRender = (aktuell->tm_sec != lastSecond) 
                    || (fastRefresh && (currentMillis - lastFrame > 50)) 
                    || (currentMillis < toastEndTime) 
                    || (hotspotPromptActive && (currentMillis - lastFrame > 40))
                    || (currentPage >= 13 && currentPage <= 25);
    if (forceRender) { 
        if (cdSprite.getPointer() == nullptr) {
            Serial.println("[RENDER] cdSprite ist NULL - recreate!");
            void* p = cdSprite.createSprite(240, 240 - HEAD_H);
            if (!p) { Serial.println("[RENDER] KRITISCH: cdSprite alloc FAIL!"); return; }
        }
        lastSecond = aktuell->tm_sec; 
        lastFrame = currentMillis; 
        if(currentPage != 11) drawHeader(aktuell, getBattery(), isCharging); 
        cdSprite.fillSprite(currentPage == 11 ? TFT_WHITE : cBG); 
        renderPage(currentPage, &cdSprite, aktuell); 
        if (hotspotPromptActive) {
            drawHotspotPrompt(&cdSprite);
        }
        if(currentPage != 11) cdSprite.pushSprite(0, HEAD_H); else cdSprite.pushSprite(0, 0); 
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
                // 7-Segment: Basis 40px Höhe bei Scale 1.0
                int sH = (int)(40 * wfTimeScale); int sW = (int)(sH * 0.6); int sp = (int)(sH * 0.15);
                int totalW = get7SegWidth(String(aodBuf), sH, sW, sp);
                // 7seg braucht einen Sprite — zeichne als text fallback im AOD
                tft.drawString(aodBuf, px, py, wfTimeScale >= 1.2 ? 7 : (wfTimeScale >= 0.7 ? 6 : 4));
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
            if (isCharging) {
                // Lade-Modus: Prozent pulsierend anzeigen (ohne Blitz)
                uint8_t bri = 100 + animPhase * 50; // 100..250 pulsierend
                uint16_t pulseCol = tft.color565(bri, bri, 0);
                tft.setTextColor(pulseCol, TFT_BLACK);
                tft.drawString(String(batPct) + "%", px, py, 4);
            } else {
                uint16_t bCol = (batPct > 50) ? TFT_GREEN : (batPct > 20) ? TFT_YELLOW : TFT_RED;
                tft.setTextColor(bCol, TFT_BLACK);
                tft.drawString(String(batPct) + "%", px, py, 4);
            }
        }

        // === Schritte ===
        if (wfShowSteps) {
            int px = wfStepsX + offX, py = wfStepsY + offY;
            tft.setTextDatum(MC_DATUM); tft.setTextColor(cAodAcc, TFT_BLACK);
            tft.drawString("Schritte: " + String(stepCount), px, py, 2);
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
