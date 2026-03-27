// --- HardwareConfig.h ---
#pragma once
#include <Arduino.h>

void updateZielZeit();
void loadPreferences();
void initHardware();
void toggleUsbHid();
void showBootSequence();
void playBootSound(int type);
int getBattery();
String readFileFromFS(String path, String fallback);
void writeFileToFS(String path, String content);