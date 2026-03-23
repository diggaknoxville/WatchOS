// --- RechnerApp.h ---
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

void showRechner(TFT_eSprite *spr);
void handleRechnerTouch(int x, int y);