// --- TotpAuth.h ---
#pragma once
#include <Arduino.h>

struct TotpAccount {
    char name[24];
    char secret[48]; // Base32 encoded key
};

#define MAX_TOTP_ACCOUNTS 4

class TotpAuth {
public:
    static void init();
    static int getAccountCount();
    static bool getAccount(int index, TotpAccount &acc);
    static bool setAccount(int index, const char* name, const char* secret);
    static bool removeAccount(int index);
    static uint32_t generateCode(int index, time_t nowTime);
    static int getRemainingSeconds(time_t nowTime);
    static void loadFromPrefs();
    static void saveToPrefs();
};

