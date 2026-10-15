// --- SystemEvents.h ---
#pragma once
#include <Arduino.h>

enum SystemEventType {
    SYS_EVT_NONE = 0,
    SYS_EVT_NOTIFICATION,
    SYS_EVT_BATTERY_UPDATE,
    SYS_EVT_TILT_WAKE,
    SYS_EVT_WIFI_TOGGLE,
    SYS_EVT_APP_STATE
};

typedef void (*SystemEventHandler)(SystemEventType evt, void* data);

#define MAX_EVENT_HANDLERS 8

class SystemEvents {
public:
    static void init();
    static bool registerHandler(SystemEventHandler handler);
    static void post(SystemEventType evt, void* data = nullptr);
};

