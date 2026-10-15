// --- TotpAuth.cpp ---
#include "TotpAuth.h"
#include <Preferences.h>
#include "mbedtls/md.h"

static TotpAccount accounts[MAX_TOTP_ACCOUNTS];
static int accountCount = 0;

static int base32Decode(const char* in, uint8_t* out, int maxOut) {
    int buffer = 0;
    int bitsLeft = 0;
    int count = 0;
    for (const char* p = in; *p; p++) {
        char ch = *p;
        if (ch == ' ' || ch == '-' || ch == '=') continue;
        int val = -1;
        if (ch >= 'A' && ch <= 'Z') val = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') val = ch - 'a';
        else if (ch >= '2' && ch <= '7') val = ch - '2' + 26;
        if (val < 0) continue;
        buffer = (buffer << 5) | val;
        bitsLeft += 5;
        if (bitsLeft >= 8) {
            if (count < maxOut) {
                out[count++] = (uint8_t)((buffer >> (bitsLeft - 8)) & 0xFF);
            }
            bitsLeft -= 8;
        }
    }
    return count;
}

static bool hmacSha1(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen, uint8_t* outHash) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!mdInfo) {
        mbedtls_md_free(&ctx);
        return false;
    }
    if (mbedtls_md_setup(&ctx, mdInfo, 1) != 0) {
        mbedtls_md_free(&ctx);
        return false;
    }
    if (mbedtls_md_hmac_starts(&ctx, key, keyLen) != 0) {
        mbedtls_md_free(&ctx);
        return false;
    }
    if (mbedtls_md_hmac_update(&ctx, data, dataLen) != 0) {
        mbedtls_md_free(&ctx);
        return false;
    }
    if (mbedtls_md_hmac_finish(&ctx, outHash) != 0) {
        mbedtls_md_free(&ctx);
        return false;
    }
    mbedtls_md_free(&ctx);
    return true;
}

void TotpAuth::init() {
    loadFromPrefs();
}

int TotpAuth::getAccountCount() {
    return accountCount;
}

bool TotpAuth::getAccount(int index, TotpAccount &acc) {
    if (index < 0 || index >= accountCount) return false;
    acc = accounts[index];
    return true;
}

bool TotpAuth::setAccount(int index, const char* name, const char* secret) {
    if (index < 0 || index >= MAX_TOTP_ACCOUNTS || !name || !secret) return false;
    strncpy(accounts[index].name, name, sizeof(accounts[index].name) - 1);
    accounts[index].name[sizeof(accounts[index].name) - 1] = '\0';
    strncpy(accounts[index].secret, secret, sizeof(accounts[index].secret) - 1);
    accounts[index].secret[sizeof(accounts[index].secret) - 1] = '\0';
    if (index >= accountCount) accountCount = index + 1;
    saveToPrefs();
    return true;
}

bool TotpAuth::removeAccount(int index) {
    if (index < 0 || index >= accountCount) return false;
    for (int i = index; i < accountCount - 1; i++) {
        accounts[i] = accounts[i + 1];
    }
    memset(&accounts[accountCount - 1], 0, sizeof(TotpAccount));
    accountCount--;
    saveToPrefs();
    return true;
}

uint32_t TotpAuth::generateCode(int index, time_t nowTime) {
    if (index < 0 || index >= accountCount || strlen(accounts[index].secret) == 0) return 0;
    uint8_t key[48];
    int keyLen = base32Decode(accounts[index].secret, key, sizeof(key));
    if (keyLen <= 0) return 0;

    uint64_t counter = (uint64_t)(nowTime / 30);
    uint8_t counterBytes[8];
    for (int i = 7; i >= 0; i--) {
        counterBytes[i] = (uint8_t)(counter & 0xFF);
        counter >>= 8;
    }

    uint8_t hash[20];
    if (!hmacSha1(key, keyLen, counterBytes, 8, hash)) return 0;

    int offset = hash[19] & 0x0F;
    uint32_t binary = ((hash[offset] & 0x7F) << 24) |
                      ((hash[offset + 1] & 0xFF) << 16) |
                      ((hash[offset + 2] & 0xFF) << 8) |
                      (hash[offset + 3] & 0xFF);

    return binary % 1000000;
}

int TotpAuth::getRemainingSeconds(time_t nowTime) {
    int rem = 30 - (int)(nowTime % 30);
    if (rem <= 0) rem = 30;
    return rem;
}

void TotpAuth::loadFromPrefs() {
    Preferences p;
    accountCount = 0;
    if (p.begin("clock", true)) {
        for (int i = 0; i < MAX_TOTP_ACCOUNTS; i++) {
            char kName[16], kSec[16];
            snprintf(kName, sizeof(kName), "totp%d_n", i);
            snprintf(kSec, sizeof(kSec), "totp%d_s", i);
            String n = p.getString(kName, "");
            String s = p.getString(kSec, "");
            if (n.length() > 0 && s.length() > 0) {
                strncpy(accounts[accountCount].name, n.c_str(), sizeof(accounts[accountCount].name) - 1);
                accounts[accountCount].name[sizeof(accounts[accountCount].name) - 1] = '\0';
                strncpy(accounts[accountCount].secret, s.c_str(), sizeof(accounts[accountCount].secret) - 1);
                accounts[accountCount].secret[sizeof(accounts[accountCount].secret) - 1] = '\0';
                accountCount++;
            }
        }
        p.end();
    }
    // Falls noch keine Accounts vorhanden sind, ein Demokonto anlegen
    if (accountCount == 0) {
        strncpy(accounts[0].name, "GitHub 2FA", sizeof(accounts[0].name));
        strncpy(accounts[0].secret, "JBSWY3DPEHPK3PXP", sizeof(accounts[0].secret));
        accountCount = 1;
    }
}

void TotpAuth::saveToPrefs() {
    Preferences p;
    if (p.begin("clock", false)) {
        for (int i = 0; i < MAX_TOTP_ACCOUNTS; i++) {
            char kName[16], kSec[16];
            snprintf(kName, sizeof(kName), "totp%d_n", i);
            snprintf(kSec, sizeof(kSec), "totp%d_s", i);
            if (i < accountCount) {
                p.putString(kName, accounts[i].name);
                p.putString(kSec, accounts[i].secret);
            } else {
                p.remove(kName);
                p.remove(kSec);
            }
        }
        p.end();
    }
}

