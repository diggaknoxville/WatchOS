#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

void renderAppLauncher(TFT_eSprite *spr);
void handleAppLauncherTouch(int x, int touchY);

// Abwärtskompatible Weiterleitungen
void renderGoodMenu(TFT_eSprite *spr);
void handleGoodMenuTouch(int x, int touchY);
void renderBadMenu(TFT_eSprite *spr);
void handleBadMenuTouch(int x, int touchY);

