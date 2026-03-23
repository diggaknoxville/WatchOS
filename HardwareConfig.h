// --- HardwareConfig.h ---
#pragma once
#include <Arduino.h>

void updateZielZeit();
void loadPreferences();
void initHardware();
void showBootSequence();
void playBootSound(int type);
int getBattery(); // NEU: Die smarte Akku-Abfrage