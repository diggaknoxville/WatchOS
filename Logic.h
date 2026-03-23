// --- Logic.h ---
#pragma once
#include <Arduino.h>

void showToast(String msg, int duration = 3500);
void triggerVibration();
void triggerAlarm();
void startMic();
void stopMic();
int getDecibels();
void killAllApps();
void toggleBT();
void toggleVibra();
void checkBackgroundTasks(unsigned long currentMillis);
void handlePowerManagement(unsigned long currentMillis);
void handleTouchInput();