// --- NotificationManager.h ---
#pragma once
#include <Arduino.h>

struct WatchNotification {
    uint32_t id;
    time_t timestamp;
    char title[32];
    char message[96];
    bool read;
};

#define MAX_NOTIFICATIONS 8

class NotificationManager {
public:
    static void init();
    static void add(const char* title, const char* message);
    static int getCount();
    static int getUnreadCount();
    static bool get(int index, WatchNotification &notif);
    static void markAllRead();
    static void clear();
    static bool hasNewAlert();
    static void clearNewAlert();
    static String toJson();
};

