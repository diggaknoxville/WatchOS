// --- HardwareConfig.h ---
#pragma once
#include <Arduino.h>

void updateZielZeit();
void loadPreferences();
void initHardware();
void toggleUsbHid();
void showBootSequence();
void playBootSound(int type);
void playSynthSound(int type, int duration);
void playToneHz(int freqHz, int durationMs);
int getBattery();
int getBatteryCached(bool forceRefresh = false);
bool isChargingCached(bool forceRefresh = false);
float getBatteryVoltageCached(bool forceRefresh = false);
void refreshPowerTelemetry(bool forceRefresh = false);
bool syncNtpTime(int timeoutSeconds = 6);
String readFileFromFS(String path, String fallback);
void writeFileToFS(String path, String content);