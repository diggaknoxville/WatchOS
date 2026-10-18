#pragma once
#include <Arduino.h>

// BMA423 I2C Beschleunigungssensor & Pedometer OS-Treiber
void bmaInit();
void bmaReadAccel(float &ax, float &ay, float &az);
void bmaInvalidate();
void updateMotion(unsigned long now);
void loadMotionPrefs();
void saveMotionPrefs();

extern RTC_DATA_ATTR int stepHourly[24];

