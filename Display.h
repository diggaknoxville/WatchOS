// --- Display.h ---
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <time.h>

// --- Global Design Tokens (RGB565) ---
#define COLOR_BG_DARK      0x0842  // #0A0D12 Deep canvas background
#define COLOR_SURFACE      0x10E4  // #141923 Card surface
#define COLOR_SURFACE_HI   0x1926  // #1C2331 Highlighted card surface
#define COLOR_BORDER       0x2188  // #263043 Subtle card border
#define COLOR_BORDER_HI    0x3A2E  // #3A465C Focused / active border
#define COLOR_TEXT_MAIN    0xFFFF  // #FFFFFF Primary text
#define COLOR_TEXT_MUTED   0x8450  // #7E8B9F Secondary / muted text
#define COLOR_ACCENT_TIME  0xFD80  // #FFB319 Warm Gold / Time Accent
#define COLOR_PROD_GREEN   0x072E  // #00E676 Productivity Green
#define COLOR_SEC_RED      0xF9E9  // #FF3D57 Security / Bad Red
#define COLOR_CYAN_TECH    0x073F  // #00E5FF Tech / LoRa Cyan
#define COLOR_AMBER_WARN   0xFC80  // #FF9100 Amber Warning / Brightness
#define COLOR_WARN         0xFC80  // Alias for Amber Warning

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

extern TFT_eSPI tft;
extern TFT_eSprite cdSprite;

uint16_t hex2rgb565(String hex);
void applyTheme();
void setBrightness(int b);
time_t stringToTime(String dateStr, String timeStr);

void draw7Segment(TFT_eSPI &spr, int x, int y, int num, int h, int w, uint16_t color);
int draw7SegString(TFT_eSPI &spr, String text, int x, int y, int h, int w, uint16_t color, int spacing);
int get7SegWidth(String text, int h, int w, int spacing);

void drawXButton(TFT_eSprite &spr);
void drawBattery(int x, int y, int pct, bool charging);
void drawHeader(struct tm *aktuell, int akku, bool charging);
void drawGlassCard(TFT_eSprite *spr, int x, int y, int w, int h, int r, uint16_t borderCol, uint16_t fillTop = 0x1CE7, uint16_t fillBot = 0x10A2);
void drawUltraDashboard(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg);
void drawChronographWatchface(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg);
void drawRetroCasioWatchface(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg);
void drawLifetimeWatchface(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg);
void drawBauhausWatchface(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg);
void drawCyberpunkWatchface(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg);
void drawWordClockWatchface(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg);
void drawFinanceWatchface(TFT_eSprite *spr, struct tm *aktuell);
void drawChildrenWatchface(TFT_eSprite *spr, struct tm *aktuell);
void drawParentsWatchface(TFT_eSprite *spr, struct tm *aktuell);
void drawPilotFliegerWatchface(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg);
void drawDiverProWatchface(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg);
void drawNeonHorizonWatchface(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg);
void drawRadarTacticalWatchface(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg);
void drawAstroOrbitWatchface(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg);
void drawMinimalTypoWatchface(TFT_eSprite *spr, struct tm *aktuell, int akku, bool isChg);
void drawControlCenter(TFT_eSprite *spr, struct tm *aktuell);
void drawLoraMessenger(TFT_eSprite *spr, struct tm *aktuell);

void renderPage(int page, TFT_eSprite *spr, struct tm *aktuell);
void slideToPage(int newPage, int dx, int dy);
void renderUI(unsigned long currentMillis);
void renderAOD();

#include <WiFiClient.h>
void streamScreenBmp(WiFiClient &client);