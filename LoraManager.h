#pragma once
#include <Arduino.h>

struct LoraMessage {
    char sender[16];
    char text[64];
    char timeStr[12];
    float rssi;
    float snr;
    bool isOutgoing;
};

class LoraManager {
public:
    static bool init();
    static void loop();
    static bool sendMessage(const String &text, const String &sender = "WatchOS");
    static bool sendPing();
    static void setBeacon(bool enable, int intervalSec = 5);
    static bool isBeaconActive();
    static bool isReady();
    static int getMessageCount();
    static const LoraMessage* getMessages();
    static void clearMessages();
    static float getFrequency();
    static void setFrequency(float freq);
    static int getTxCount();
    static int getRxCount();
    static float getLastRssi();
    static float getLastSnr();
    static String getStatusJson();
};

