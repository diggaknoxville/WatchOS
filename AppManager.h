#pragma once
#include <Arduino.h>
#include <vector>
#include <TFT_eSPI.h>

enum AppCategory {
    CAT_PRODUCTIVITY = 0,   // Werkzeuge & Produktivität (ehemals Good)
    CAT_SECURITY     = 1,   // Security & Hacker Tools (ehemals Bad)
    CAT_SYSTEM       = 2    // System-Tools
};

// Aliase für Abwärtskompatibilität
#define CAT_GOOD CAT_PRODUCTIVITY
#define CAT_BAD  CAT_SECURITY

constexpr int DYNAMIC_PAGE_BASE = 50;

typedef void (*AppDrawFn)(TFT_eSprite* spr);
typedef void (*AppTouchFn)(int x, int y);
typedef bool (*AppStatusFn)();
typedef void (*AppStopFn)();

struct AppPackage {
    String id;              // Eindeutige ID z.B. "lora", "totp", "dice"
    String name;            // Anzeigename z.B. "LoRa Chat", "2FA Authenticator"
    AppCategory category;   // CAT_PRODUCTIVITY, CAT_SECURITY, CAT_SYSTEM
    uint16_t color;         // Akzentfarbe fürs Icon/Button
    String iconText;        // 2-3 Zeichen Kurzsymbol ("RF", "2F", "SW", etc.)
    int pageId;             // Dynamisch zugewiesene Page ID (ab DYNAMIC_PAGE_BASE)
    bool enabled;           // Aktiviert im Launcher
    bool isCustom;          // true = hochgeladenes Paket, false = eingebautes System-Paket
    String version;         // z.B. "1.0.0"
    String author;          // Entwickler / Ersteller
    String description;     // Kurzbeschreibung
    int order;              // Sortier-Reihenfolge
    String driver;          // Native Driver / Engine Bindung (z.B. "stopwatch", "timer", "lora", etc.)
    AppDrawFn drawFn;       // Optionaler Direkt-Renderer
    AppTouchFn touchFn;     // Optionaler Touch-Handler
    AppStatusFn statusFn;   // Prüft ob die App gerade im Hintergrund läuft
    AppStopFn stopFn;       // Stoppt die App bei Not-Aus
    String uiJson;          // UI-Descriptor für die App
    String logicJson;       // Logik- / Event-Descriptor
    String configJson;      // Optionale Presets / Skripte
    String jsScript;        // Eingebetteter JavaScript-Code aus .app Datei
    int tickIntervalMs = 0; // Tick-Intervall für onTick() in ms
    unsigned long lastTickMs = 0;
    bool jsInitialized = false;
};

class AppManager {
public:
    static void init();
    static String getDefaultAppPackageJson(const String &id);
    static void registerApp(const AppPackage &pkg);
    
    // Abfragen für GUI Launcher
    static std::vector<AppPackage*> getAppsByCategory(AppCategory cat, bool onlyEnabled = true);
    static int getAppCount(AppCategory cat, bool onlyEnabled = true);
    static AppPackage* getAppById(const String &id);
    static AppPackage* getAppByPage(int pageId);

    // Launcher Steuerung
    static void launchApp(int pageId);
    static void launchAppById(const String &id);

    // Paketverwaltung
    static void setAppEnabled(const String &id, bool enabled);
    static bool isAppEnabled(const String &id);
    static void setAppOrder(const String &id, int order);
    static void reassignDynamicPages();
    
    // Dynamische Installation, Deinstallation und Export
    static bool installPackageFromJson(const String &jsonStr, String &outId, String &outError);
    static bool uninstallPackage(const String &id);
    static String getPackageAppFile(const String &id);
    static void loadInstalledPackages();
    static void resetToDefaults();

    // Dynamische App & JS Ausführung (Rendering, Touch & Tick)
    static bool hasJsScript(const AppPackage *app);
    static void tickDynamicApp(AppPackage *app, unsigned long now);
    static void tickBackgroundApps(unsigned long now);
    static void stopAllJsApps();
    static void renderDynamicApp(AppPackage *app, TFT_eSprite *spr);
    static void handleDynamicAppTouch(AppPackage *app, int x, int touchY);

    // API & JSON Export
    static String getPackagesJson();
    
    // Pagination State für Menüs auf der Uhr (0-basiert)
    static int prodMenuPage;
    static int secMenuPage;
    static int goodMenuPage;
    static int badMenuPage;
};
