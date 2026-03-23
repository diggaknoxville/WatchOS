// --- JigglerApp.h ---
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

void showJiggler(TFT_eSprite *spr);
void handleJigglerTouch(int x, int y);