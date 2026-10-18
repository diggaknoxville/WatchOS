// --- SensorDriver.cpp ---
#include "SensorDriver.h"
#include "Globals.h"
#include "HardwareConfig.h"
#include "Display.h"
#include "Logic.h"
#include "AppManager.h"
#include <Wire.h>
#include <math.h>
#include <time.h>

// =====================================================================
// BMA423 über I2C (Addr 0x19 auf Wire = PMU Bus)
// =====================================================================
#define BMA423_ADDR 0x19
#define BMA4_ACCEL_X_LSB_ADDR 0x12

static bool bmaOK = false;

void bmaInit() {
    Wire.beginTransmission(BMA423_ADDR);
    Wire.write(0x7D); Wire.write(0x04); // ACC enable
    Wire.endTransmission();
    delay(5);
    // Accelerometer Range: ±2g (0x00), ODR 100Hz (0x08)  
    Wire.beginTransmission(BMA423_ADDR);
    Wire.write(0x40); Wire.write(0x28); // acc_conf: odr=100Hz, bwp=norm
    Wire.endTransmission();
    Wire.beginTransmission(BMA423_ADDR);
    Wire.write(0x41); Wire.write(0x00); // acc_range: ±2g
    Wire.endTransmission();
    bmaOK = true;
}

void bmaReadAccel(float &ax, float &ay, float &az) {
    if (!bmaOK) bmaInit();
    uint8_t buf[6];
    Wire.beginTransmission(BMA423_ADDR);
    Wire.write(BMA4_ACCEL_X_LSB_ADDR);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)BMA423_ADDR, (uint8_t)6);
    for (int i = 0; i < 6; i++) buf[i] = Wire.available() ? Wire.read() : 0;
    
    int16_t rawX = (int16_t)((buf[1] << 8) | buf[0]) >> 4; // 12-bit
    int16_t rawY = (int16_t)((buf[3] << 8) | buf[2]) >> 4;
    int16_t rawZ = (int16_t)((buf[5] << 8) | buf[4]) >> 4;
    
    // ±2g Range: 1 LSB = 0.98 mg  -> /1024 für g
    ax = rawX / 1024.0;
    ay = rawY / 1024.0;
    az = rawZ / 1024.0;
}

void bmaInvalidate() {
    bmaOK = false;
}

// =====================================================================
// Bewegungssensor: Schrittzähler (Software-Pedometer) + Handgelenk-Wake
// =====================================================================
RTC_DATA_ATTR long stepCount = 0;                  // Schritte seit Tagesbeginn
RTC_DATA_ATTR int stepHourly[24] = {0};            // Schritte je Stunde des laufenden Tages (fuers Web-Dashboard)
RTC_DATA_ATTR int stepDay = -1;                     // tm_yday des aktuellen Zähltags
RTC_DATA_ATTR bool stepRtcInitialized = false;

static float stepMagAvg = 1.0f;      // gleitender Mittelwert der Beschleunigung (g)
static bool stepArmed = true;        // bereit für die nächste Aufwärtsflanke
static unsigned long lastStepMs = 0;
static unsigned long lastMotionSample = 0;
static unsigned long lastStepPersistMs = 0;
static long lastPersistedSteps = -1;
static float wakeLastAz = 1.0f;

void saveMotionPrefs() {
    lastPersistedSteps = stepCount;
    prefs.begin("clock", false);
    prefs.putInt("steps", (int)stepCount);
    prefs.putInt("stepDay", stepDay);
    prefs.end();
}

void loadMotionPrefs() {
    // Nur bei Kaltstart (RTC-RAM uninitialisiert) aus dem NVS-Flash nachladen
    if (!stepRtcInitialized || stepDay == -1) {
        prefs.begin("clock", true);
        stepCount = prefs.getInt("steps", 0);
        stepDay   = prefs.getInt("stepDay", -1);
        prefs.end();
        stepRtcInitialized = true;
    }
    lastPersistedSteps = stepCount;
}

void updateMotion(unsigned long now) {
    if (now - lastMotionSample < 80) return;
    lastMotionSample = now;

    float ax, ay, az;
    bmaReadAccel(ax, ay, az);
    float mag = sqrtf(ax * ax + ay * ay + az * az);
    if (mag < 0.05f) return; // Sensor liefert nichts -> ignorieren

    // Tageswechsel: Zähler zurücksetzen
    time_t t; time(&t); struct tm *lt = localtime(&t);
    if (lt->tm_yday != stepDay) {
        if (lt->tm_year + 1900 > 2020) {
            saveMotionPrefs(); // Vorigen Tag sichern
            stepDay = lt->tm_yday;
            stepCount = 0;
            for (int i = 0; i < 24; i++) stepHourly[i] = 0;
            saveMotionPrefs();
        }
        else if (stepDay == -1) { stepDay = lt->tm_yday; }
    }

    // --- Schritt-Erkennung: Peak über dynamischem Schwellwert ---
    stepMagAvg = stepMagAvg * 0.9f + mag * 0.1f;
    float dyn = mag - stepMagAvg;
    const float STEP_HI = 0.13f;
    const float STEP_LO = 0.04f;
    if (stepArmed && dyn > STEP_HI && (now - lastStepMs) > 280) {
        stepCount++; stepHourly[lt->tm_hour]++; lastStepMs = now; stepArmed = false;
        AppPackage* curApp = AppManager::getAppByPage(currentPage);
        if (currentPage == 10 || (curApp && curApp->id == "battery")) lastSecond = -1;
    } else if (!stepArmed && dyn < STEP_LO) {
        stepArmed = true;
    }

    // --- Handgelenk-Erkennung (Zuverlaessiger Wake + Automatischer Wrist-Down-Sleep) ---
    static bool armDownArmed = false;
    static unsigned long armDownStartTime = 0;
    static unsigned long lastWakeTime = 0;
    static int wristDownCount = 0;

    // 1. Arm-Down / Tilted-Away Haltung erkennen:
    // Der Arm haengt nach unten oder ist vom Gesicht abgewandt (az < 0.40f oder starke Querbeschleunigung)
    bool isArmDown = (az < 0.40f) || (sqrtf(ax * ax + ay * ay) > 0.65f);

    if (isArmDown) {
        if (!armDownArmed) {
            if (armDownStartTime == 0) armDownStartTime = now;
            else if (now - armDownStartTime > 200) {
                armDownArmed = true; // Arm war mind. 200ms unten -> scharfschalten fuer echten Wake
            }
        }
    } else {
        armDownStartTime = 0;
    }

    // 2. Handgelenk-Wake: wenn zuvor scharfgeschaltet und der Arm in normale Blickposition gedreht wird
    // Blickposition: az > 0.65f (Display zeigt nach oben/zum Nutzer), fabsf(ax) < 0.60f, ay zwischen -0.65f und +0.45f
    bool isViewingAngle = (az > 0.65f) && (fabsf(ax) < 0.60f) && (ay > -0.65f && ay < 0.45f);

    if ((powerState >= 2 && powerState <= 4) && armDownArmed && isViewingAngle && (now - lastWakeTime > 1200)) {
        armDownArmed = false;
        lastWakeTime = now;
        if (powerState == 4) {
            tft.writecommand(0x11);
            delay(120);
        }
        lastTouchTime = now;
        powerState = 1;
        setBrightness(maxBright);
        triggerVibration();
        lastSecond = -1;
    }

    // 3. Wrist-Down / Arm-Senken: Automatisch Dimmen/Schlafen wenn Display weggedreht oder Arm gesenkt wird
    // Verhindert, dass die Uhr 30s unnoetig weiterleuchtet oder auf dem Tisch an bleibt
    AppPackage* curMotionApp = AppManager::getAppByPage(currentPage);
    bool keepAwakeApp = (curMotionApp && (curMotionApp->id == "flashlight" || curMotionApp->driver == "flashlight" || curMotionApp->id == "battery"));
    if (powerState == 1 && !keepAwakeApp && (now - lastTouchTime > 1500)) {
        if (az < 0.20f || (isArmDown && az < 0.35f)) {
            wristDownCount++;
            if (wristDownCount >= 6) { // ~480ms stabile Haltung nach unten
                wristDownCount = 0;
                lastTouchTime = now - time_dim_start - 10; // Sofort Dim/Sleep einleiten
            }
        } else {
            wristDownCount = 0;
        }
    } else {
        wristDownCount = 0;
    }

    // --- Schritte schonend im Flash sichern (alle 60 min statt alle 3 min) ---
    if (stepCount != lastPersistedSteps && (now - lastStepPersistMs) > 3600000UL) {
        lastStepPersistMs = now;
        saveMotionPrefs();
    }
}

