// --- NotificationManager.cpp ---
#include "NotificationManager.h"
#include "HardwareConfig.h"
#include "Display.h"
#include "Globals.h"
#include "Logic.h"
#include "SystemEvents.h"
#include <time.h>

static WatchNotification items[MAX_NOTIFICATIONS];
static int itemCount = 0;
static uint32_t nextId = 1;
static bool newAlertFlag = false;

void NotificationManager::init() {
    itemCount = 0;
    newAlertFlag = false;
    memset(items, 0, sizeof(items));
}

void NotificationManager::add(const char* title, const char* message) {
    if (!title || !message) return;

    // Falls Puffer voll ist, schieben wir nach oben
    if (itemCount >= MAX_NOTIFICATIONS) {
        for (int i = 0; i < MAX_NOTIFICATIONS - 1; i++) {
            items[i] = items[i + 1];
        }
        itemCount = MAX_NOTIFICATIONS - 1;
    }

    WatchNotification &n = items[itemCount];
    n.id = nextId++;
    time(&n.timestamp);
    strncpy(n.title, title, sizeof(n.title) - 1);
    n.title[sizeof(n.title) - 1] = '\0';
    strncpy(n.message, message, sizeof(n.message) - 1);
    n.message[sizeof(n.message) - 1] = '\0';
    n.read = false;
    itemCount++;

    newAlertFlag = true;
    SystemEvents::post(SYS_EVT_NOTIFICATION, &n);

    // Haptisches Feedback & Display-Toast
    triggerVibration();
    String tStr = String(title);
    if (tStr.length() > 16) tStr = tStr.substring(0, 14) + "..";
    String mStr = String(message);
    if (mStr.length() > 24) mStr = mStr.substring(0, 22) + "..";
    showToastColor(tStr + ": " + mStr, 3500, TFT_CYAN);
}

int NotificationManager::getCount() {
    return itemCount;
}

int NotificationManager::getUnreadCount() {
    int unread = 0;
    for (int i = 0; i < itemCount; i++) {
        if (!items[i].read) unread++;
    }
    return unread;
}

bool NotificationManager::get(int index, WatchNotification &notif) {
    if (index < 0 || index >= itemCount) return false;
    // Letzte Benachrichtigung zuerst (LIFO)
    int revIdx = (itemCount - 1) - index;
    notif = items[revIdx];
    return true;
}

void NotificationManager::markAllRead() {
    for (int i = 0; i < itemCount; i++) {
        items[i].read = true;
    }
    newAlertFlag = false;
}

void NotificationManager::clear() {
    itemCount = 0;
    newAlertFlag = false;
    memset(items, 0, sizeof(items));
}

bool NotificationManager::hasNewAlert() {
    return newAlertFlag;
}

void NotificationManager::clearNewAlert() {
    newAlertFlag = false;
}

String NotificationManager::toJson() {
    String json = "{\"count\":" + String(itemCount) + ",\"unread\":" + String(getUnreadCount()) + ",\"items\":[";
    for (int i = 0; i < itemCount; i++) {
        // LIFO Reihenfolge
        int revIdx = (itemCount - 1) - i;
        const WatchNotification &n = items[revIdx];
        if (i > 0) json += ",";
        json += "{\"id\":" + String(n.id) + ",";
        json += "\"time\":" + String((long)n.timestamp) + ",";
        json += "\"title\":\"" + jsonEscape(String(n.title)) + "\",";
        json += "\"msg\":\"" + jsonEscape(String(n.message)) + "\",";
        json += "\"read\":" + String(n.read ? "true" : "false") + "}";
    }
    json += "]}";
    return json;
}

