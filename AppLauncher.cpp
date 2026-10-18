// --- AppLauncher.cpp ---
#include "AppLauncher.h"
#include "Globals.h"
#include "Display.h"
#include "Logic.h"
#include "HardwareConfig.h"
#include "AppManager.h"
#include <vector>

// Filter-Zustand für den einheitlichen App Drawer
// 0 = Alle, 1 = Tools (Produktivität), 2 = Security (Netzwerk & Audit), 3 = System
static int drawerTab = 0;
static int drawerPage = 0;

static std::vector<AppPackage*> getFilteredApps() {
    std::vector<AppPackage*> result;
    auto prodApps = AppManager::getAppsByCategory(CAT_PRODUCTIVITY, true);
    auto secApps = AppManager::getAppsByCategory(CAT_SECURITY, true);
    auto sysApps = AppManager::getAppsByCategory(CAT_SYSTEM, true);

    if (drawerTab == 0) { // Alle
        result.insert(result.end(), prodApps.begin(), prodApps.end());
        result.insert(result.end(), secApps.begin(), secApps.end());
        result.insert(result.end(), sysApps.begin(), sysApps.end());
    } else if (drawerTab == 1) { // Tools
        result = prodApps;
    } else if (drawerTab == 2) { // Security
        result = secApps;
    } else if (drawerTab == 3) { // System
        result = sysApps;
    }
    return result;
}

void renderAppLauncher(TFT_eSprite *spr) {
    if (!spr) return;

    auto apps = getFilteredApps();
    int total = (int)apps.size();
    int perPage = 8;
    int maxPage = (total > 0) ? ((total - 1) / perPage) : 0;
    if (drawerPage > maxPage) drawerPage = maxPage;
    if (drawerPage < 0) drawerPage = 0;

    int curPage = drawerPage;
    int startIdx = curPage * perPage;
    int endIdx = min(startIdx + perPage, total);

    // --- 1. Kategorien-Tabs oben (y = 2 .. 22) ---
    static const char* tabLabels[] = {"Alle", "Tools", "Security", "System"};
    int tabW = 50, tabH = 18, tabY = 3, startX = 15, tabGap = 6;
    for (int t = 0; t < 4; t++) {
        int tx = startX + t * (tabW + tabGap);
        bool isActive = (drawerTab == t);
        uint16_t borderCol = isActive ? COLOR_ACCENT_TIME : COLOR_BORDER;
        uint16_t fillCol = isActive ? COLOR_SURFACE_HI : COLOR_SURFACE;
        drawGlassCard(spr, tx, tabY, tabW, tabH, 4, borderCol, fillCol, COLOR_BG_DARK);
        spr->setTextDatum(MC_DATUM);
        spr->setTextColor(isActive ? COLOR_ACCENT_TIME : COLOR_TEXT_MUTED);
        spr->drawString(tabLabels[t], tx + tabW / 2, tabY + tabH / 2, 1);
    }

    // --- 2. App-Raster (y = 26 .. 190) ---
    int bW = 104, bH = 38, gapY = 4, startY = 26;
    for (int i = startIdx; i < endIdx; i++) {
        int slot = i - startIdx;
        int col = slot % 2;
        int row = slot / 2;
        int bx = (col == 0) ? 12 : 124;
        int by = startY + row * (bH + gapY);

        bool isRunning = (apps[i]->statusFn != nullptr) ? apps[i]->statusFn() : false;

        uint16_t borderCol = isRunning ? apps[i]->color : COLOR_BORDER;
        uint16_t fillCol = isRunning ? blendColor565(apps[i]->color, COLOR_SURFACE, 50) : COLOR_SURFACE;
        drawGlassCard(spr, bx, by, bW, bH, 8, borderCol, fillCol, COLOR_BG_DARK);

        // Icon Badge
        if (apps[i]->iconText.length() > 0) {
            spr->fillRoundRect(bx + 6, by + 9, 20, 20, 5, COLOR_SURFACE_HI);
            spr->drawRoundRect(bx + 6, by + 9, 20, 20, 5, isRunning ? apps[i]->color : COLOR_BORDER);
            spr->setTextColor(apps[i]->color);
            spr->setTextDatum(MC_DATUM);
            spr->drawString(apps[i]->iconText, bx + 16, by + 19, 1);
        }

        // App-Name mit Textkürzung
        String dispName = apps[i]->name;
        int maxTextW = bW - 38;
        if (spr->textWidth(dispName, 2) > maxTextW) {
            while (dispName.length() > 1 && spr->textWidth(dispName + "..", 2) > maxTextW) {
                dispName.remove(dispName.length() - 1);
            }
            dispName += "..";
        }
        spr->setTextColor(COLOR_TEXT_MAIN);
        spr->setTextDatum(ML_DATUM);
        spr->drawString(dispName, bx + 30, by + bH / 2, 2);

        // Status-LED
        if (isRunning) {
            spr->fillCircle(bx + bW - 8, by + 8, 3, COLOR_PROD_GREEN);
            spr->fillCircle(bx + bW - 8, by + 8, 1, COLOR_TEXT_MAIN);
        }
    }

    // Leerer Zustand falls keine Apps in Kategorie
    if (total == 0) {
        spr->setTextDatum(MC_DATUM);
        spr->setTextColor(COLOR_TEXT_MUTED);
        spr->drawString("Keine Apps in dieser Gruppe", 120, 100, 2);
    }

    // --- 3. Paginierungs-Indikator unten (y = 198) ---
    if (maxPage > 0) {
        spr->setTextDatum(MC_DATUM);
        spr->setTextColor(COLOR_TEXT_MUTED);
        spr->drawString("<", 20, 202, 2);
        spr->drawString(">", 220, 202, 2);
        for (int p = 0; p <= maxPage; p++) {
            int dotX = 120 + (p - (float)maxPage / 2.0f) * 14;
            if (p == curPage) spr->fillCircle(dotX, 202, 3, COLOR_ACCENT_TIME);
            else spr->drawCircle(dotX, 202, 3, COLOR_BORDER_HI);
        }
    }
}

void handleAppLauncherTouch(int x, int touchY) {
    // 1. Kategorien-Tabs oben (y <= 24)
    if (touchY <= 24) {
        int oldTab = drawerTab;
        if (x < 68) drawerTab = 0;          // Alle
        else if (x < 124) drawerTab = 1;     // Tools
        else if (x < 182) drawerTab = 2;     // Security
        else drawerTab = 3;                  // System

        if (drawerTab != oldTab) {
            drawerPage = 0;
            triggerVibration();
            lastSecond = -1;
        }
        return;
    }

    auto apps = getFilteredApps();
    int total = (int)apps.size();
    int perPage = 8;
    int maxPage = (total > 0) ? ((total - 1) / perPage) : 0;

    // 2. Bottom Navigation Bar Tap (Page < >)
    if (maxPage > 0 && touchY >= 194) {
        if (x < 80 && drawerPage > 0) {
            drawerPage--;
            triggerVibration();
            lastSecond = -1;
            return;
        } else if (x > 160 && drawerPage < maxPage) {
            drawerPage++;
            triggerVibration();
            lastSecond = -1;
            return;
        }
    }

    // 3. App Kachel Tap
    int bH = 38, gapY = 4, startY = 26;
    int row = (touchY - startY) / (bH + gapY);
    int col = (x < 120) ? 0 : 1;
    int slot = row * 2 + col;

    if (row < 0 || row > 3 || touchY < startY) return;

    int idx = drawerPage * perPage + slot;
    if (idx >= total) return;

    triggerVibration();
    AppManager::launchApp(apps[idx]->pageId);
}

// Abwärtskompatible Weiterleitungen
void renderGoodMenu(TFT_eSprite *spr) {
    drawerTab = 1; // Öffne direkt Tools-Register
    renderAppLauncher(spr);
}
void handleGoodMenuTouch(int x, int touchY) {
    handleAppLauncherTouch(x, touchY);
}
void renderBadMenu(TFT_eSprite *spr) {
    drawerTab = 2; // Öffne direkt Security-Register
    renderAppLauncher(spr);
}
void handleBadMenuTouch(int x, int touchY) {
    handleAppLauncherTouch(x, touchY);
}

