// --- Display.h ---
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <time.h>

extern TFT_eSPI tft;
extern TFT_eSprite cdSprite;

uint16_t hex2rgb565(String hex);
void applyTheme();
void setBrightness(int b);
time_t stringToTime(String dateStr, String timeStr);

void draw7Segment(TFT_eSprite &spr, int x, int y, int num, int h, int w, uint16_t color);
int draw7SegString(TFT_eSprite &spr, String text, int x, int y, int h, int w, uint16_t color, int spacing);
int get7SegWidth(String text, int h, int w, int spacing);

void drawXButton(TFT_eSprite &spr);
void drawBattery(int x, int y, int pct, bool charging);
void drawHeader(struct tm *aktuell, int akku, bool charging);

void renderPage(int page, TFT_eSprite *spr, struct tm *aktuell);
void slideToPage(int newPage, int dx, int dy);
void renderUI(unsigned long currentMillis);
void renderAOD();