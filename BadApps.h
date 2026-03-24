// --- BadApps.h ---
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

// Bad Apps Menu (Page 23)
void renderBadMenu(TFT_eSprite *spr);
void handleBadMenuTouch(int x, int touchY);

// Evil Portal (Page 12)
void handleEvilRoot();
void handleEvilSubmit();
void handleEvilTOS();
void handleEvilImprint();
void showEvilPortal(TFT_eSprite *spr);
void handleEvilTouch(int x, int y);
void toggleEvilPortal();
