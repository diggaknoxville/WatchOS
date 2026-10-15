#include "LoraManager.h"
#include "Globals.h"
#include "Logic.h"
#include <SPI.h>
#include <RadioLib.h>

#ifndef LORA_CS
#define LORA_CS 5
#endif
#ifndef LORA_IRQ
#define LORA_IRQ 9
#endif
#ifndef LORA_RST
#define LORA_RST 8
#endif
#ifndef LORA_BUSY
#define LORA_BUSY 7
#endif
#ifndef LORA_SCK
#define LORA_SCK 3
#endif
#ifndef LORA_MISO
#define LORA_MISO 4
#endif
#ifndef LORA_MOSI
#define LORA_MOSI 1
#endif

static SPIClass loraSPI(FSPI);
static Module loraModule(LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY, loraSPI);
static SX1262 loraRadio(&loraModule);

static bool loraReady = false;
static float currentFreq = 868.0f;
static bool beaconActive = false;
static int beaconIntervalSec = 5;
static unsigned long lastBeaconTime = 0;
static int txCount = 0;
static int rxCount = 0;
static float lastRssi = 0.0f;
static float lastSnr = 0.0f;

#define MAX_LORA_MSGS 12
static LoraMessage msgHistory[MAX_LORA_MSGS];
static int msgCount = 0;

static volatile bool loraRxFlag = false;
#if defined(ESP8266) || defined(ESP32)
  ICACHE_RAM_ATTR
#endif
static void setLoraRxFlag(void) {
    loraRxFlag = true;
}

static void addMessage(const char* sender, const char* text, float rssi, float snr, bool isOutgoing) {
    if (msgCount >= MAX_LORA_MSGS) {
        for (int i = 0; i < MAX_LORA_MSGS - 1; i++) {
            msgHistory[i] = msgHistory[i + 1];
        }
        msgCount = MAX_LORA_MSGS - 1;
    }
    LoraMessage &m = msgHistory[msgCount++];
    strncpy(m.sender, sender, sizeof(m.sender) - 1);
    m.sender[sizeof(m.sender) - 1] = '\0';
    strncpy(m.text, text, sizeof(m.text) - 1);
    m.text[sizeof(m.text) - 1] = '\0';
    m.rssi = rssi;
    m.snr = snr;
    m.isOutgoing = isOutgoing;

    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10)) {
        snprintf(m.timeStr, sizeof(m.timeStr), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        snprintf(m.timeStr, sizeof(m.timeStr), "%lus", millis() / 1000UL);
    }
}

bool LoraManager::init() {
    if (loraReady) return true;

    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);

    loraSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);

    int state = loraRadio.begin(currentFreq, 125.0, 7, 7, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 14, 8, 1.6, false);
    if (state != RADIOLIB_ERR_NONE) {
        state = loraRadio.begin(currentFreq, 125.0, 7, 7, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 14, 8, 2.4, false);
    }
    if (state != RADIOLIB_ERR_NONE) {
        state = loraRadio.begin(currentFreq, 125.0, 7, 7, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 14, 8, 0.0, false);
    }

    if (state == RADIOLIB_ERR_NONE) {
        loraRadio.setDio2AsRfSwitch(true);
        loraRadio.setPacketReceivedAction(setLoraRxFlag);
        loraRadio.startReceive();
        loraReady = true;
        Serial.printf("[LORA] SX1262 online auf %.1f MHz (EU868 ISM)\n", currentFreq);
        return true;
    } else {
        Serial.printf("[LORA] SX1262 Init Code: %d\n", state);
        loraReady = false;
        return false;
    }
}

void LoraManager::loop() {
    if (!loraReady) return;

    if (loraRxFlag) {
        loraRxFlag = false;
        String str;
        int state = loraRadio.readData(str);
        if (state == RADIOLIB_ERR_NONE && str.length() > 0) {
            lastRssi = loraRadio.getRSSI();
            lastSnr = loraRadio.getSNR();
            rxCount++;

            String sName = "Node";
            String text = str;
            if (str.startsWith("[") && str.indexOf("]") > 0) {
                int cIdx = str.indexOf("]");
                sName = str.substring(1, cIdx);
                text = str.substring(cIdx + 1);
                text.trim();
            }

            addMessage(sName.c_str(), text.c_str(), lastRssi, lastSnr, false);
            triggerVibration();
            showToastColor("LoRa: " + text.substring(0, 16), 2500, 0x07FF);
        }
        loraRadio.setPacketReceivedAction(setLoraRxFlag);
        loraRadio.startReceive();
    }

    if (beaconActive && (millis() - lastBeaconTime >= (unsigned long)beaconIntervalSec * 1000UL)) {
        lastBeaconTime = millis();
        sendPing();
    }
}

bool LoraManager::sendMessage(const String &text, const String &sender) {
    if (!loraReady) {
        if (!init()) return false;
    }

    String pSender = sender.length() > 0 ? sender : (phoneName.length() > 0 ? phoneName : "WatchOS");
    String payload = "[" + pSender + "] " + text;
    int state = loraRadio.transmit(payload);

    loraRadio.setPacketReceivedAction(setLoraRxFlag);
    loraRadio.startReceive();

    if (state == RADIOLIB_ERR_NONE) {
        txCount++;
        addMessage("Ich", text.c_str(), 0.0f, 0.0f, true);
        triggerVibration();
        return true;
    }
    return false;
}

bool LoraManager::sendPing() {
    static uint32_t pingSeq = 1;
    String pingMsg = "PING #" + String(pingSeq++);
    return sendMessage(pingMsg, "WatchOS");
}

void LoraManager::setBeacon(bool enable, int intervalSec) {
    beaconActive = enable;
    beaconIntervalSec = (intervalSec > 1) ? intervalSec : 5;
    lastBeaconTime = millis() - (beaconIntervalSec * 1000UL); // trigger immediately
}

bool LoraManager::isBeaconActive() {
    return beaconActive;
}

bool LoraManager::isReady() {
    return loraReady;
}

int LoraManager::getMessageCount() {
    return msgCount;
}

const LoraMessage* LoraManager::getMessages() {
    return msgHistory;
}

void LoraManager::clearMessages() {
    msgCount = 0;
}

float LoraManager::getFrequency() {
    return currentFreq;
}

void LoraManager::setFrequency(float freq) {
    if (freq >= 150.0f && freq <= 960.0f) {
        currentFreq = freq;
        if (loraReady) {
            loraRadio.setFrequency(currentFreq);
        }
    }
}

int LoraManager::getTxCount() {
    return txCount;
}

int LoraManager::getRxCount() {
    return rxCount;
}

float LoraManager::getLastRssi() {
    return lastRssi;
}

float LoraManager::getLastSnr() {
    return lastSnr;
}

String LoraManager::getStatusJson() {
    String j = "{";
    j += "\"ready\":" + String(loraReady ? "true" : "false");
    j += ",\"freq\":" + String(currentFreq, 1);
    j += ",\"beacon\":" + String(beaconActive ? "true" : "false");
    j += ",\"tx\":" + String(txCount);
    j += ",\"rx\":" + String(rxCount);
    j += ",\"last_rssi\":" + String(lastRssi, 1);
    j += ",\"last_snr\":" + String(lastSnr, 1);
    j += ",\"messages\":[";
    for (int i = 0; i < msgCount; i++) {
        if (i > 0) j += ",";
        j += "{\"sender\":\"" + jsonEscape(String(msgHistory[i].sender)) + "\"";
        j += ",\"text\":\"" + jsonEscape(String(msgHistory[i].text)) + "\"";
        j += ",\"time\":\"" + String(msgHistory[i].timeStr) + "\"";
        j += ",\"rssi\":" + String(msgHistory[i].rssi, 1);
        j += ",\"snr\":" + String(msgHistory[i].snr, 1);
        j += ",\"out\":" + String(msgHistory[i].isOutgoing ? "true" : "false");
        j += "}";
    }
    j += "]}";
    return j;
}

