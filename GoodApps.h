// --- GoodApps.h ---
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

// Good Apps Menu (Page 22)
void renderGoodMenu(TFT_eSprite *spr);
void handleGoodMenuTouch(int x, int touchY);

// Rechner (Page 13)
void showRechner(TFT_eSprite *spr);
void handleRechnerTouch(int x, int y);

// Jiggler (Page 14)
void showJiggler(TFT_eSprite *spr);
void handleJigglerTouch(int x, int y);

// Würfel (Page 24)
void showDice(TFT_eSprite *spr);
void handleDiceTouch(int x, int y);
void updateDiceAnimation();

// Wasserwaage (Page 25)
void showLevel(TFT_eSprite *spr);

// Bewegungssensor: Schrittzähler + Handgelenk-Wake (läuft im Hintergrund)
void updateMotion(unsigned long now);
void loadMotionPrefs();
