// --- UpdateOTA.h ---
#pragma once
#include <Arduino.h>

struct CloudOtaState {
    bool checked = false;
    bool checking = false;
    bool updateAvailable = false;
    String latestVersion = "";
    String latestCommit = "";
    String buildDate = "";
    size_t binSize = 0;
    String sha256 = "";
    String downloadUrl = "";
    String lastCheckTime = "";
    String lastStatus = "Noch nicht geprüft";
    unsigned long lastCheckMs = 0;
};

extern CloudOtaState cloudOta;

void setupOTARoutes();
bool checkGitHubUpdate(bool force = false);
bool performCloudOTA(String customUrl = "");
void checkOtaAutoOnWifi();