#include "AppManager.h"
#include "Globals.h"
#include "Display.h"
#include "Logic.h"
#include "LoraManager.h"
#include <FFat.h>
#include <algorithm>
#include <map>

static std::vector<AppPackage> registry;
static std::map<String, int> dynamicState;

int AppManager::prodMenuPage = 0;
int AppManager::secMenuPage = 0;
int AppManager::goodMenuPage = 0;
int AppManager::badMenuPage = 0;

static std::vector<String> getUninstalledList() {
    std::vector<String> list;
    if (!littleFsReady || !FFat.exists("/apps/uninstalled.txt")) return list;
    fs::File f = FFat.open("/apps/uninstalled.txt", "r");
    if (!f) return list;
    String cur = "";
    while (f.available()) {
        char c = (char)f.read();
        if (c == '\n' || c == '\r') {
            cur.trim();
            if (cur.length() > 0) list.push_back(cur);
            cur = "";
        } else {
            cur += c;
        }
    }
    cur.trim();
    if (cur.length() > 0) list.push_back(cur);
    f.close();
    return list;
}

static void saveUninstalledList(const std::vector<String>& list) {
    if (!littleFsReady) return;
    if (!FFat.exists("/apps")) FFat.mkdir("/apps");
    fs::File f = FFat.open("/apps/uninstalled.txt", "w");
    if (!f) return;
    for (const auto& id : list) {
        f.println(id);
    }
    f.close();
}

static uint16_t parseHexColor(const String &hex) {
    if (hex.length() < 7 || hex.charAt(0) != '#') return 0x07FF;
    long rgb = strtol(hex.substring(1).c_str(), NULL, 16);
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void AppManager::reassignDynamicPages() {
    for (size_t i = 0; i < registry.size(); i++) {
        registry[i].pageId = DYNAMIC_PAGE_BASE + (int)i;
    }
}

String AppManager::getDefaultAppPackageJson(const String &id) {
    if (id == "stopwatch") {
        return R"rawliteral({
  "id": "stopwatch",
  "name": "Stoppuhr",
  "category": "productivity",
  "color": "#ffea00",
  "icon": "SW",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Präzisions-Stoppuhr mit Rundenzeiten und 7-Segment-Anzeige",
  "driver": "stopwatch",
  "ui": {
    "title": "Stoppuhr",
    "display": {
      "type": "7segment",
      "format": "mm:ss.cc",
      "y": 25,
      "color": "#00e5ff"
    },
    "laps": {
      "type": "lap_list",
      "max": 10,
      "y": 62,
      "row_height": 18
    },
    "buttons": [
      {
        "id": "start_stop",
        "text": "Start/Stop",
        "x": 5,
        "y": 150,
        "w": 72,
        "h": 40,
        "action": "toggle",
        "color": "#00ff00",
        "active_color": "#ff9900"
      },
      {
        "id": "lap",
        "text": "Runde",
        "x": 84,
        "y": 150,
        "w": 72,
        "h": 40,
        "action": "lap",
        "color": "#ffea00"
      },
      {
        "id": "reset",
        "text": "Reset",
        "x": 163,
        "y": 150,
        "w": 72,
        "h": 40,
        "action": "reset",
        "color": "#333333"
      }
    ]
  },
  "logic": {
    "engine": "precision_timer",
    "precision_ms": 10,
    "max_laps": 10,
    "events": {
      "on_toggle": "stopwatch.toggle",
      "on_lap": "stopwatch.lap",
      "on_reset": "stopwatch.reset"
    }
  }
})rawliteral";
    }
    if (id == "timer") {
        return R"rawliteral({
  "id": "timer",
  "name": "Timer",
  "category": "productivity",
  "color": "#00ff00",
  "icon": "TI",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Countdown-Timer mit Schnellwahltasten und Alarmton",
  "driver": "timer",
  "ui": {
    "title": "Timer",
    "presets": [
      {
        "text": "1m",
        "minutes": 1,
        "x": 20,
        "y": 80,
        "w": 60,
        "h": 50
      },
      {
        "text": "3m",
        "minutes": 3,
        "x": 90,
        "y": 80,
        "w": 60,
        "h": 50
      },
      {
        "text": "5m",
        "minutes": 5,
        "x": 160,
        "y": 80,
        "w": 60,
        "h": 50
      }
    ],
    "display": {
      "type": "7segment",
      "format": "mm:ss",
      "y": 70,
      "color": "#ff0000"
    },
    "button_cancel": {
      "text": "Abbruch",
      "x": 75,
      "y": 150,
      "w": 90,
      "h": 40,
      "color": "#ff0000"
    }
  },
  "logic": {
    "engine": "countdown_timer",
    "presets_min": [
      1,
      3,
      5
    ],
    "alarm": {
      "haptic": true,
      "buzzer": true
    }
  }
})rawliteral";
    }
    if (id == "mic") {
        return R"rawliteral({
  "id": "mic",
  "name": "Dezibel",
  "category": "productivity",
  "color": "#00ff00",
  "icon": "DB",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Schallpegelmesser mit I2S-Mikrofon und RMS-Balkengrafik",
  "driver": "mic",
  "ui": {
    "title": "Dezibel-Meter",
    "readout": {
      "type": "db_spl",
      "y": 50,
      "size": 6,
      "color": "#ffffff"
    },
    "rms_label": {
      "type": "label",
      "format": "RMS: {rms}",
      "y": 85,
      "color": "#888888"
    },
    "meter": {
      "type": "bar_graph",
      "x": 60,
      "y": 74,
      "w": 120,
      "h": 120,
      "zones": [
        {
          "max": 65,
          "color": "#00ff00"
        },
        {
          "max": 90,
          "color": "#ffff00"
        },
        {
          "max": 120,
          "color": "#ff0000"
        }
      ]
    }
  },
  "logic": {
    "engine": "i2s_microphone",
    "sample_rate": 16000,
    "unit": "dB SPL",
    "auto_power_save": true
  }
})rawliteral";
    }
    if (id == "calc") {
        return R"rawliteral({
  "id": "calc",
  "name": "Rechner",
  "category": "productivity",
  "color": "#ffffff",
  "icon": "CA",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Taschenrechner mit Touch-Bedienung und 4 Grundrechenarten",
  "driver": "calc",
  "ui": {
    "title": "Rechner",
    "display": {
      "type": "lcd",
      "y": 5,
      "h": 35,
      "bg": "#111111",
      "color": "#00ff00"
    },
    "keypad": {
      "rows": 4,
      "cols": 4,
      "y": 45,
      "layout": [
        [
          "7",
          "8",
          "9",
          "/"
        ],
        [
          "4",
          "5",
          "6",
          "*"
        ],
        [
          "1",
          "2",
          "3",
          "-"
        ],
        [
          "C",
          "0",
          "=",
          "+"
        ]
      ]
    }
  },
  "logic": {
    "engine": "calculator_alu",
    "operations": [
      "+",
      "-",
      "*",
      "/"
    ],
    "precision": 8
  }
})rawliteral";
    }
    if (id == "jiggler") {
        return R"rawliteral({
  "id": "jiggler",
  "name": "Jiggler",
  "category": "productivity",
  "color": "#00ffff",
  "icon": "MJ",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "USB HID Mouse Mover gegen Bildschirmschoner",
  "driver": "jiggler",
  "ui": {
    "title": "Mouse Jiggler",
    "status": {
      "type": "badge",
      "y": 55,
      "active_text": "JIGGLER AKTIV",
      "idle_text": "BEREIT"
    },
    "button_toggle": {
      "text": "Start / Stop",
      "x": 40,
      "y": 130,
      "w": 160,
      "h": 45,
      "color": "#00ffff"
    }
  },
  "logic": {
    "engine": "usb_hid_mouse",
    "motion": "circle_subtle",
    "interval_ms": 1000,
    "delta_px": 2
  }
})rawliteral";
    }
    if (id == "dice") {
        return R"rawliteral({
  "id": "dice",
  "name": "Wuerfel",
  "category": "productivity",
  "color": "#ffff00",
  "icon": "D6",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Würfel und Münzwurf per Schüttelgeste",
  "driver": "dice",
  "ui": {
    "title": "Würfel & Münze",
    "tabs": [
      "WÜRFEL",
      "MÜNZE"
    ],
    "display": {
      "type": "die_face",
      "y": 115,
      "color": "#ffff00"
    },
    "button_roll": {
      "text": "Würfeln / Schütteln",
      "x": 30,
      "y": 175,
      "w": 180,
      "h": 40,
      "color": "#ffff00"
    }
  },
  "logic": {
    "engine": "random_generator",
    "shake_trigger": true,
    "sensor": "bma423_accel",
    "modes": [
      "d6",
      "coin"
    ]
  }
})rawliteral";
    }
    if (id == "level") {
        return R"rawliteral({
  "id": "level",
  "name": "Waage",
  "category": "productivity",
  "color": "#ffb600",
  "icon": "LV",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "2D-Wasserwaage per BMA423 Gyro/Beschleunigung",
  "driver": "level",
  "ui": {
    "title": "Wasserwaage",
    "crosshair": {
      "type": "circle_target",
      "cx": 120,
      "cy": 110,
      "r": 70,
      "color": "#333333"
    },
    "bubble": {
      "type": "bubble",
      "r": 14,
      "color": "#00ff00"
    },
    "angles": {
      "type": "readout",
      "format": "X: {roll}°  Y: {pitch}°",
      "y": 195,
      "color": "#ffffff"
    }
  },
  "logic": {
    "engine": "bma423_level",
    "axes": [
      "roll",
      "pitch"
    ],
    "damping": 0.8
  }
})rawliteral";
    }
    if (id == "totp") {
        return R"rawliteral({
  "id": "totp",
  "name": "2FA TOTP",
  "category": "productivity",
  "color": "#00ffff",
  "icon": "2F",
  "version": "1.1.0",
  "author": "WatchOS",
  "description": "Zwei-Faktor-Authenticator nach RFC 6238",
  "driver": "totp",
  "ui": {
    "title": "2FA Authenticator",
    "service": {
      "type": "label",
      "source": "service_name",
      "y": 50,
      "size": 2,
      "color": "#ffffff"
    },
    "code": {
      "type": "totp_display",
      "format": "000 000",
      "y": 85,
      "size": 6,
      "color": "#00ffff"
    },
    "progress": {
      "type": "countdown_ring",
      "period": 30,
      "y": 150,
      "color": "#00e5ff"
    }
  },
  "logic": {
    "engine": "rfc6238_authenticator",
    "period_sec": 30,
    "digits": 6,
    "algorithm": "SHA1"
  }
})rawliteral";
    }
    if (id == "lora") {
        return R"rawliteral({
  "id": "lora",
  "name": "LoRa Funk",
  "category": "productivity",
  "color": "#00ffff",
  "icon": "RF",
  "version": "2.0.0",
  "author": "WatchOS",
  "description": "Off-Grid P2P Messenger & Beacon auf 868MHz",
  "driver": "lora",
  "ui": {
    "title": "LoRa Funk",
    "tabs": [
      "Chat",
      "Beacon"
    ],
    "presets": [
      {
        "text": "Hallo!",
        "x": 10,
        "y": 160,
        "w": 52,
        "h": 36
      },
      {
        "text": "OK",
        "x": 66,
        "y": 160,
        "w": 52,
        "h": 36
      },
      {
        "text": "Ping?",
        "x": 122,
        "y": 160,
        "w": 52,
        "h": 36
      },
      {
        "text": "Hilfe!",
        "x": 178,
        "y": 160,
        "w": 52,
        "h": 36
      }
    ],
    "beacon_button": {
      "text": "Beacon An/Aus",
      "y": 150,
      "color": "#00ffff"
    }
  },
  "logic": {
    "engine": "sx1262_lora",
    "frequency_mhz": 868.0,
    "bandwidth_khz": 125.0,
    "spreading_factor": 7,
    "tx_power_dbm": 22
  }
})rawliteral";
    }
    if (id == "notif") {
        return R"rawliteral({
  "id": "notif",
  "name": "Mitteilung",
  "category": "productivity",
  "color": "#9cbeff",
  "icon": "NT",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Push-Mitteilungszentrale vom Smartphone",
  "driver": "notif",
  "ui": {
    "title": "Mitteilungen",
    "list": {
      "type": "notification_cards",
      "y": 35,
      "max": 4,
      "empty_text": "Keine Mitteilungen"
    },
    "button_clear": {
      "text": "Alle gelesen",
      "y": 190,
      "color": "#9cbeff"
    }
  },
  "logic": {
    "engine": "notification_center",
    "buffer_size": 10,
    "ble_sync": true
  }
})rawliteral";
    }
    if (id == "battery") {
        return R"rawliteral({
  "id": "battery",
  "name": "Akku-Info",
  "category": "productivity",
  "color": "#31ff00",
  "icon": "BA",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Detaillierte Telemetrie und Ladehistorie",
  "driver": "battery",
  "ui": {
    "title": "Akku Telemetrie",
    "telemetry_card": {
      "type": "glass_card",
      "x": 8,
      "y": 4,
      "w": 224,
      "h": 64,
      "metrics": [
        "pct",
        "volt",
        "charging",
        "runtime"
      ]
    },
    "graph": {
      "type": "discharge_curve",
      "x": 8,
      "y": 76,
      "w": 224,
      "h": 115,
      "samples": 200
    }
  },
  "logic": {
    "engine": "axp2101_pmu",
    "voltage_adc": true,
    "history_interval_sec": 600
  }
})rawliteral";
    }
    if (id == "flashlight") {
        return R"rawliteral({
  "id": "flashlight",
  "name": "Lampe",
  "category": "productivity",
  "color": "#ffffff",
  "icon": "FL",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Helles Display-Flutlicht mit maximaler Helligkeit",
  "driver": "flashlight",
  "ui": {
    "title": "Taschenlampe",
    "display": {
      "type": "fullscreen",
      "color": "#ffffff"
    }
  },
  "logic": {
    "engine": "backlight_boost",
    "brightness": 255,
    "lock_awake": true,
    "dismiss_on_tap": true
  }
})rawliteral";
    }
    if (id == "phishing") {
        return R"rawliteral({
  "id": "phishing",
  "name": "Phishing",
  "category": "security",
  "color": "#ff0000",
  "icon": "EV",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Captive Portal zur Demonstration von Phishing",
  "driver": "phishing",
  "ui": {
    "title": "Evil Portal",
    "status": {
      "type": "badge",
      "y": 40,
      "active": "PORTAL AKTIV",
      "idle": "INAKTIV"
    },
    "creds": {
      "type": "credentials_log",
      "y": 70,
      "max": 3
    },
    "button_toggle": {
      "text": "Portal An/Aus",
      "x": 40,
      "y": 160,
      "w": 160,
      "h": 40,
      "color": "#ff0000"
    }
  },
  "logic": {
    "engine": "dns_captive_portal",
    "softap_ssid": "Free_WiFi_Login",
    "dns_hijack": true
  }
})rawliteral";
    }
    if (id == "badusb") {
        return R"rawliteral({
  "id": "badusb",
  "name": "Ducky",
  "category": "security",
  "color": "#ffff00",
  "icon": "DU",
  "version": "1.2.0",
  "author": "WatchOS",
  "description": "Rubber Ducky USB HID Tastatur-Emulation",
  "driver": "badusb",
  "ui": {
    "title": "Rubber Ducky",
    "script_picker": {
      "type": "carousel",
      "y": 45,
      "scripts": [
        "Fake Update",
        "System Info",
        "WiFi Passwords"
      ]
    },
    "button_inject": {
      "text": "START INJEKTION",
      "x": 50,
      "y": 95,
      "w": 140,
      "h": 45,
      "color": "#ff0000"
    }
  },
  "logic": {
    "engine": "usb_hid_keyboard",
    "layout": "DE_ISO",
    "scripts": [
      {
        "id": 0,
        "name": "Fake Update Prank",
        "trigger": "gui_r"
      },
      {
        "id": 1,
        "name": "System Info Dump",
        "trigger": "powershell"
      },
      {
        "id": 2,
        "name": "WiFi Password Grabber",
        "trigger": "netsh"
      }
    ]
  }
})rawliteral";
    }
    if (id == "wifispam") {
        return R"rawliteral({
  "id": "wifispam",
  "name": "WiFi Spam",
  "category": "security",
  "color": "#ff0000",
  "icon": "WS",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Aussenden von hunderten gefälschten WiFi-Beacons",
  "driver": "wifispam",
  "ui": {
    "title": "WiFi Spam",
    "radar": {
      "type": "beacon_animation",
      "y": 50
    },
    "button_toggle": {
      "text": "SPAM AN/AUS",
      "x": 55,
      "y": 85,
      "w": 130,
      "h": 45,
      "color": "#ff0000"
    }
  },
  "logic": {
    "engine": "80211_beacon_flooder",
    "packet_type": "raw_mgmt_beacon",
    "channels": [
      1,
      6,
      11
    ]
  }
})rawliteral";
    }
    if (id == "blespam") {
        return R"rawliteral({
  "id": "blespam",
  "name": "BLE Spam",
  "category": "security",
  "color": "#ff00ff",
  "icon": "BS",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "BLE Advertisement Flooding & Notification Popups",
  "driver": "blespam",
  "ui": {
    "title": "BLE Spam",
    "ping": {
      "type": "pulse_animation",
      "y": 50
    },
    "button_toggle": {
      "text": "BLE SPAM START",
      "x": 55,
      "y": 85,
      "w": 130,
      "h": 45,
      "color": "#ff00ff"
    }
  },
  "logic": {
    "engine": "ble_adv_flooder",
    "targets": [
      "Apple",
      "Samsung",
      "Windows"
    ],
    "tx_power": 9
  }
})rawliteral";
    }
    if (id == "nmap") {
        return R"rawliteral({
  "id": "nmap",
  "name": "Nmap",
  "category": "security",
  "color": "#00ff00",
  "icon": "NM",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Multi-Port Netzwerk-Portscanner",
  "driver": "nmap",
  "ui": {
    "title": "Portscanner",
    "network_picker": {
      "type": "carousel",
      "y": 45
    },
    "button_scan": {
      "text": "SCAN STARTEN",
      "x": 50,
      "y": 95,
      "w": 140,
      "h": 45,
      "color": "#00ff00"
    },
    "results": {
      "type": "port_list",
      "y": 145
    }
  },
  "logic": {
    "engine": "tcp_port_scanner",
    "ports": [
      21,
      22,
      23,
      80,
      443,
      8080
    ],
    "timeout_ms": 300
  }
})rawliteral";
    }
    if (id == "deauth") {
        return R"rawliteral({
  "id": "deauth",
  "name": "Deauth",
  "category": "security",
  "color": "#ff0000",
  "icon": "DA",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "802.11 Deauthentication Frame Generator",
  "driver": "deauth",
  "ui": {
    "title": "Deauther",
    "target_picker": {
      "type": "carousel",
      "y": 45
    },
    "button_attack": {
      "text": "DEAUTH ATTACKE",
      "x": 50,
      "y": 85,
      "w": 140,
      "h": 35,
      "color": "#ff0000"
    },
    "button_scan": {
      "text": "Netze Scannen",
      "x": 60,
      "y": 125,
      "w": 120,
      "h": 30
    }
  },
  "logic": {
    "engine": "80211_deauther",
    "frame_type": "mgmt_deauth",
    "reason": 7
  }
})rawliteral";
    }
    if (id == "probemon") {
        return R"rawliteral({
  "id": "probemon",
  "name": "Probe Mon",
  "category": "security",
  "color": "#00ffff",
  "icon": "PM",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Erfassen von Probe Requests mobiler Geräte",
  "driver": "probemon",
  "ui": {
    "title": "Probe Monitor",
    "log": {
      "type": "packet_list",
      "y": 15,
      "lines": 4
    },
    "button_toggle": {
      "text": "Sniffer Start/Stop",
      "x": 40,
      "y": 145,
      "w": 160,
      "h": 40,
      "color": "#00ffff"
    }
  },
  "logic": {
    "engine": "80211_probe_sniffer",
    "ring_buffer_capacity": 32,
    "promiscuous": true
  }
})rawliteral";
    }
    if (id == "blescan") {
        return R"rawliteral({
  "id": "blescan",
  "name": "BLE Scan",
  "category": "security",
  "color": "#ff00ff",
  "icon": "BC",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "BLE Umgebungsscanner & Device Discovery",
  "driver": "blescan",
  "ui": {
    "title": "BLE Scanner",
    "device_list": {
      "type": "rssi_list",
      "y": 15,
      "lines": 4
    },
    "button_toggle": {
      "text": "Scan Start/Stop",
      "x": 40,
      "y": 145,
      "w": 160,
      "h": 40,
      "color": "#ff00ff"
    }
  },
  "logic": {
    "engine": "ble_device_scanner",
    "scan_interval": 5,
    "active_scan": true
  }
})rawliteral";
    }
    return "";
}

void AppManager::init() {
    registry.clear();
    prodMenuPage = 0;
    secMenuPage = 0;
    goodMenuPage = 0;
    badMenuPage = 0;

    auto uninstalled = getUninstalledList();
    auto isUninstalled = [&](const char* id) {
        for (const auto& u : uninstalled) {
            if (u.equalsIgnoreCase(id)) return true;
        }
        return false;
    };

    Preferences p;
    p.begin("packages", true);

    auto add = [&](const char* id, const char* name, AppCategory cat, uint16_t col, const char* icon, const char* ver, const char* desc, AppStatusFn statusFn = nullptr) {
        if (isUninstalled(id)) return; // Vom Nutzer deinstalliert
        AppPackage pkg;
        pkg.id = id;
        pkg.name = name;
        pkg.category = cat;
        pkg.color = col;
        pkg.iconText = icon;
        pkg.pageId = DYNAMIC_PAGE_BASE + (int)registry.size(); // Dynamisch zugewiesene Page-ID!
        pkg.enabled = p.getBool(id, true);
        pkg.isCustom = false;
        pkg.version = ver;
        pkg.author = "WatchOS";
        pkg.description = desc;
        pkg.order = p.getInt((String(id) + "_ord").c_str(), (int)registry.size() + 1);
        pkg.driver = id;
        pkg.drawFn = nullptr;
        pkg.touchFn = nullptr;
        pkg.statusFn = statusFn;
        pkg.stopFn = nullptr;
        pkg.uiJson = "";
        pkg.logicJson = "";
        pkg.configJson = "";
        registry.push_back(pkg);

        // Auf FFat sicherstellen: Wenn Datei /apps/<id>.app noch nicht existiert, volles .app Paket schreiben
        if (littleFsReady) {
            if (!FFat.exists("/apps")) FFat.mkdir("/apps");
            String path = "/apps/" + String(id) + ".app";
            if (!FFat.exists(path)) {
                fs::File f = FFat.open(path, "w");
                if (f) {
                    f.print(getDefaultAppPackageJson(id));
                    f.close();
                }
            }
        }
    };

    // --- Productivity Apps (Produktivitaet, Sensoren & Werkzeuge) ---
    add("stopwatch",   "Stoppuhr",   CAT_PRODUCTIVITY, cAcc1,      "SW", "1.0.0", "Präzisions-Stoppuhr mit Rundenzeiten", [](){ return swRunning; });
    add("timer",       "Timer",      CAT_PRODUCTIVITY, cAcc2,      "TI", "1.0.0", "Countdown-Timer mit Alarmton", [](){ return timerRunning; });
    add("mic",         "Dezibel",    CAT_PRODUCTIVITY, TFT_GREEN,  "DB", "1.0.0", "Schallpegelmesser mit I2S-Mikrofon", [](){ return micActive; });
    add("calc",        "Rechner",    CAT_PRODUCTIVITY, cFG,        "CA", "1.0.0", "Taschenrechner mit Touch-Bedienung", nullptr);
    add("jiggler",     "Jiggler",    CAT_PRODUCTIVITY, TFT_CYAN,   "MJ", "1.0.0", "USB HID Mouse Mover gegen Bildschirmschoner", [](){ return jigglerActive; });
    add("dice",        "Wuerfel",    CAT_PRODUCTIVITY, TFT_YELLOW, "D6", "1.0.0", "Würfel und Münzwurf per Schüttelgeste", [](){ return diceRolling; });
    add("level",       "Waage",      CAT_PRODUCTIVITY, TFT_ORANGE, "LV", "1.0.0", "2D-Wasserwaage per BMA423 Gyro/Beschleunigung", nullptr);
    add("totp",        "2FA TOTP",   CAT_PRODUCTIVITY, 0x07FF,     "2F", "1.1.0", "Zwei-Faktor-Authenticator nach RFC 6238", nullptr);
    add("lora",        "LoRa Funk",  CAT_PRODUCTIVITY, 0x07FF,     "RF", "2.0.0", "Off-Grid P2P Messenger & Beacon auf 868MHz", [](){ return LoraManager::isBeaconActive(); });
    add("notif",       "Mitteilung", CAT_PRODUCTIVITY, 0x9DFF,     "NT", "1.0.0", "Push-Mitteilungszentrale vom Smartphone", nullptr);
    add("battery",     "Akku-Info",  CAT_PRODUCTIVITY, 0x37E0,     "BA", "1.0.0", "Detaillierte Telemetrie und Ladehistorie", nullptr);
    add("flashlight",  "Lampe",      CAT_PRODUCTIVITY, TFT_WHITE,  "FL", "1.0.0", "Helles Display-Flutlicht", nullptr);

    // --- Security Apps (Security & Hacker Tools) ---
    add("phishing",    "Phishing",   CAT_SECURITY, TFT_RED,     "EV", "1.0.0", "Captive Portal zur Demonstration von Phishing", [](){ return evilActive; });
    add("badusb",      "Ducky",      CAT_SECURITY, TFT_YELLOW,  "DU", "1.2.0", "Rubber Ducky USB HID Tastatur-Emulation", [](){ return badUsbActive; });
    add("wifispam",    "WiFi Spam",  CAT_SECURITY, TFT_RED,     "WS", "1.0.0", "Aussenden von hunderten gefälschten WiFi-Beacons", [](){ return wifiSpamActive; });
    add("blespam",     "BLE Spam",   CAT_SECURITY, TFT_MAGENTA, "BS", "1.0.0", "BLE Advertisement Flooding & Notification Popups", [](){ return bleSpamActive; });
    add("nmap",        "Nmap",       CAT_SECURITY, TFT_GREEN,   "NM", "1.0.0", "Multi-Port Netzwerk-Portscanner", [](){ return nmapActive; });
    add("deauth",      "Deauth",     CAT_SECURITY, TFT_RED,     "DA", "1.0.0", "802.11 Deauthentication Frame Generator", [](){ return deauthActive; });
    add("probemon",    "Probe Mon",  CAT_SECURITY, TFT_CYAN,    "PM", "1.0.0", "Erfassen von Probe Requests mobiler Geräte", [](){ return probeMonActive; });
    add("blescan",     "BLE Scan",   CAT_SECURITY, TFT_MAGENTA, "BC", "1.0.0", "BLE Umgebungsscanner & Device Discovery", [](){ return bleScanActive; });

    p.end();

    // Dynamisch installierte Pakete aus FFat laden
    loadInstalledPackages();
}

void AppManager::loadInstalledPackages() {
    if (!littleFsReady) return;
    if (!FFat.exists("/apps")) FFat.mkdir("/apps");

    fs::File root = FFat.open("/apps");
    if (!root || !root.isDirectory()) return;

    Preferences p;
    p.begin("packages", true);

    fs::File file = root.openNextFile();
    while (file) {
        String fn = file.name();
        if (fn.endsWith(".app")) {
            String jsonStr = "";
            while (file.available()) jsonStr += (char)file.read();
            file.close();

            String dummyId, dummyErr;
            installPackageFromJson(jsonStr, dummyId, dummyErr);
        }
        file = root.openNextFile();
    }
    p.end();
}

bool AppManager::installPackageFromJson(const String &jsonStr, String &outId, String &outError) {
    auto extractStr = [&](const String &key, const String &dflt = "") -> String {
        String search = "\"" + key + "\"";
        int idx = jsonStr.indexOf(search);
        if (idx == -1) return dflt;
        int colon = jsonStr.indexOf(':', idx + search.length());
        if (colon == -1) return dflt;
        int quoteStart = jsonStr.indexOf('"', colon + 1);
        if (quoteStart == -1) return dflt;
        for (int i = colon + 1; i < quoteStart; i++) {
            char c = jsonStr.charAt(i);
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return dflt;
        }
        int quoteEnd = jsonStr.indexOf('"', quoteStart + 1);
        if (quoteEnd == -1) return dflt;
        return jsonStr.substring(quoteStart + 1, quoteEnd);
    };

    auto extractInt = [&](const String &key, int dflt = 0) -> int {
        String search = "\"" + key + "\"";
        int idx = jsonStr.indexOf(search);
        if (idx == -1) return dflt;
        int colon = jsonStr.indexOf(':', idx + search.length());
        if (colon == -1) return dflt;
        int start = colon + 1;
        while (start < (int)jsonStr.length() && (jsonStr.charAt(start) == ' ' || jsonStr.charAt(start) == '\t' || jsonStr.charAt(start) == '\r' || jsonStr.charAt(start) == '\n')) start++;
        int end = start;
        while (end < (int)jsonStr.length() && (isDigit(jsonStr.charAt(end)) || jsonStr.charAt(end) == '-')) end++;
        if (start == end) return dflt;
        return (int)jsonStr.substring(start, end).toInt();
    };

    String id = extractStr("id");
    id.trim();
    if (id.length() == 0) {
        outError = "Ungueltige Paketdatei: 'id' fehlt";
        return false;
    }
    outId = id;

    String name = extractStr("name", id);
    String catStr = extractStr("category", "productivity");
    AppCategory cat = (catStr.equalsIgnoreCase("security") || catStr == "1") ? CAT_SECURITY : CAT_PRODUCTIVITY;
    String colStr = extractStr("color", "#00e5ff");
    uint16_t color = parseHexColor(colStr);
    String icon = extractStr("icon", id.substring(0, min((unsigned int)2, id.length())));
    icon.toUpperCase();
    String ver = extractStr("version", "1.0.0");
    String author = extractStr("author", "Community");
    String desc = extractStr("description", "Installierte App");
    String driver = extractStr("driver", "");
    if (driver.length() == 0 && (id == "stopwatch" || id == "timer" || id == "mic" || id == "calc" || id == "jiggler" || id == "dice" || id == "level" || id == "totp" || id == "lora" || id == "notif" || id == "battery" || id == "flashlight" || id == "phishing" || id == "badusb" || id == "wifispam" || id == "blespam" || id == "nmap" || id == "deauth" || id == "probemon" || id == "blescan")) {
        driver = id;
    }

    // UI-Definition extrahieren falls vorhanden
    String uiJson = "";
    int uiIdx = jsonStr.indexOf("\"ui\":");
    if (uiIdx != -1) {
        int start = uiIdx + 5;
        while (start < (int)jsonStr.length() && (jsonStr.charAt(start) == ' ' || jsonStr.charAt(start) == '\n' || jsonStr.charAt(start) == '\r')) start++;
        if (start < (int)jsonStr.length() && (jsonStr.charAt(start) == '[' || jsonStr.charAt(start) == '{')) {
            char openChar = jsonStr.charAt(start);
            char closeChar = (openChar == '[') ? ']' : '}';
            int depth = 0;
            int end = start;
            for (; end < (int)jsonStr.length(); end++) {
                char c = jsonStr.charAt(end);
                if (c == openChar) depth++;
                else if (c == closeChar) {
                    depth--;
                    if (depth == 0) { end++; break; }
                }
            }
            uiJson = jsonStr.substring(start, end);
        }
    }

    // Logic-Definition extrahieren falls vorhanden
    String logicJson = "";
    int logicIdx = jsonStr.indexOf("\"logic\":");
    if (logicIdx != -1) {
        int start = logicIdx + 8;
        while (start < (int)jsonStr.length() && (jsonStr.charAt(start) == ' ' || jsonStr.charAt(start) == '\n' || jsonStr.charAt(start) == '\r')) start++;
        if (start < (int)jsonStr.length() && (jsonStr.charAt(start) == '[' || jsonStr.charAt(start) == '{')) {
            char openChar = jsonStr.charAt(start);
            char closeChar = (openChar == '[') ? ']' : '}';
            int depth = 0;
            int end = start;
            for (; end < (int)jsonStr.length(); end++) {
                char c = jsonStr.charAt(end);
                if (c == openChar) depth++;
                else if (c == closeChar) {
                    depth--;
                    if (depth == 0) { end++; break; }
                }
            }
            logicJson = jsonStr.substring(start, end);
        }
    }

    // Falls App in uninstalled.txt gelistet war: austragen!
    auto uninstalled = getUninstalledList();
    std::vector<String> newUninstalled;
    for (const auto& u : uninstalled) {
        if (!u.equalsIgnoreCase(id)) newUninstalled.push_back(u);
    }
    saveUninstalledList(newUninstalled);

    // Auf FFat persistieren: /apps/<id>.app
    if (littleFsReady) {
        if (!FFat.exists("/apps")) FFat.mkdir("/apps");
        fs::File f = FFat.open("/apps/" + id + ".app", "w");
        if (f) {
            f.print(jsonStr);
            f.close();
        }
    }

    // Falls bereits in registry: aktualisieren
    AppPackage* existing = getAppById(id);
    if (existing) {
        existing->name = name;
        existing->category = cat;
        existing->color = color;
        existing->iconText = icon;
        existing->enabled = true;
        existing->version = ver;
        existing->author = author;
        existing->description = desc;
        existing->driver = driver;
        existing->uiJson = uiJson;
        existing->logicJson = logicJson;
        setAppEnabled(id, true);
        return true;
    }

    // Neu eintragen mit dynamischer Page-ID
    AppPackage pkg;
    pkg.id = id;
    pkg.name = name;
    pkg.category = cat;
    pkg.color = color;
    pkg.iconText = icon;
    pkg.pageId = DYNAMIC_PAGE_BASE + (int)registry.size(); // Dynamisch zugewiesen!
    pkg.enabled = true;
    pkg.isCustom = true;
    pkg.version = ver;
    pkg.author = author;
    pkg.description = desc;
    pkg.driver = driver;
    pkg.uiJson = uiJson;
    pkg.logicJson = logicJson;
    pkg.order = (int)registry.size() + 1;
    pkg.drawFn = nullptr;
    pkg.touchFn = nullptr;
    pkg.statusFn = nullptr;
    pkg.stopFn = nullptr;
    registry.push_back(pkg);

    setAppEnabled(id, true);
    return true;
}

bool AppManager::uninstallPackage(const String &id) {
    AppPackage* p = getAppById(id);
    if (!p) return false;

    // In die uninstalled.txt Liste aufnehmen
    auto uninstalled = getUninstalledList();
    bool existsInList = false;
    for (const auto& u : uninstalled) {
        if (u.equalsIgnoreCase(id)) { existsInList = true; break; }
    }
    if (!existsInList) {
        uninstalled.push_back(id);
        saveUninstalledList(uninstalled);
    }

    // Datei auf FFat löschen falls vorhanden
    if (littleFsReady && FFat.exists("/apps/" + id + ".app")) {
        FFat.remove("/apps/" + id + ".app");
    }

    // Aus Memory Registry entfernen
    for (auto it = registry.begin(); it != registry.end(); ++it) {
        if (it->id == id) {
            registry.erase(it);
            break;
        }
    }
    reassignDynamicPages();

    // Preferences bereinigen
    Preferences prefs;
    prefs.begin("packages", false);
    prefs.remove(id.c_str());
    prefs.remove((id + "_ord").c_str());
    prefs.end();

    return true;
}

String AppManager::getPackageAppFile(const String &id) {
    if (littleFsReady && FFat.exists("/apps/" + id + ".app")) {
        fs::File f = FFat.open("/apps/" + id + ".app", "r");
        if (f) {
            String content = "";
            while (f.available()) content += (char)f.read();
            f.close();
            if (content.length() > 10) return content;
        }
    }
    String def = getDefaultAppPackageJson(id);
    if (def.length() > 0) return def;

    AppPackage* p = getAppById(id);
    if (!p) return "";

    char hexCol[8];
    uint8_t r = ((p->color >> 11) & 0x1F) * 255 / 31;
    uint8_t g = ((p->color >> 5) & 0x3F) * 255 / 63;
    uint8_t b = (p->color & 0x1F) * 255 / 31;
    snprintf(hexCol, sizeof(hexCol), "#%02x%02x%02x", r, g, b);

    String j = "{\n";
    j += "  \"id\": \"" + jsonEscape(p->id) + "\",\n";
    j += "  \"name\": \"" + jsonEscape(p->name) + "\",\n";
    j += "  \"category\": \"" + String(p->category == CAT_SECURITY ? "security" : "productivity") + "\",\n";
    j += "  \"color\": \"" + String(hexCol) + "\",\n";
    j += "  \"icon\": \"" + jsonEscape(p->iconText) + "\",\n";
    j += "  \"version\": \"" + jsonEscape(p->version) + "\",\n";
    j += "  \"author\": \"" + jsonEscape(p->author) + "\",\n";
    j += "  \"description\": \"" + jsonEscape(p->description) + "\"";
    if (p->driver.length() > 0) j += ",\n  \"driver\": \"" + jsonEscape(p->driver) + "\"";
    if (p->uiJson.length() > 0) j += ",\n  \"ui\": " + p->uiJson;
    if (p->logicJson.length() > 0) j += ",\n  \"logic\": " + p->logicJson;
    j += "\n}\n";
    return j;
}

std::vector<AppPackage*> AppManager::getAppsByCategory(AppCategory cat, bool onlyEnabled) {
    std::vector<AppPackage*> res;
    for (size_t i = 0; i < registry.size(); i++) {
        if (registry[i].category == cat) {
            if (!onlyEnabled || registry[i].enabled) {
                res.push_back(&registry[i]);
            }
        }
    }
    std::sort(res.begin(), res.end(), [](const AppPackage* a, const AppPackage* b) {
        return a->order < b->order;
    });
    return res;
}

int AppManager::getAppCount(AppCategory cat, bool onlyEnabled) {
    int cnt = 0;
    for (const auto &pkg : registry) {
        if (pkg.category == cat) {
            if (!onlyEnabled || pkg.enabled) cnt++;
        }
    }
    return cnt;
}

AppPackage* AppManager::getAppById(const String &id) {
    for (auto &pkg : registry) {
        if (pkg.id == id) return &pkg;
    }
    return nullptr;
}

AppPackage* AppManager::getAppByPage(int pageId) {
    for (auto &pkg : registry) {
        if (pkg.pageId == pageId) return &pkg;
    }
    return nullptr;
}

void AppManager::launchApp(int pageId) {
    AppPackage* p = getAppByPage(pageId);
    if (!p) {
        slideToPage(pageId, 0, -1);
        return;
    }
    triggerVibration();
    if (p->id == "mic" || p->driver == "mic") startMic();
    if (p->category == CAT_SECURITY) {
        killBadApps(); // Auto-kill laufende Security-App bevor neue gestartet wird
    }
    slideToPage(p->pageId, 0, -1);
}

void AppManager::launchAppById(const String &id) {
    AppPackage* p = getAppById(id);
    if (p) launchApp(p->pageId);
}

void AppManager::setAppEnabled(const String &id, bool enabled) {
    AppPackage* p = getAppById(id);
    if (p) {
        p->enabled = enabled;
        Preferences prefs;
        prefs.begin("packages", false);
        prefs.putBool(id.c_str(), enabled);
        prefs.end();
    }
}

bool AppManager::isAppEnabled(const String &id) {
    AppPackage* p = getAppById(id);
    return p ? p->enabled : false;
}

void AppManager::setAppOrder(const String &id, int order) {
    AppPackage* p = getAppById(id);
    if (p) {
        p->order = order;
        Preferences prefs;
        prefs.begin("packages", false);
        prefs.putInt((id + "_ord").c_str(), order);
        prefs.end();
    }
}

void AppManager::renderDynamicApp(AppPackage *app, TFT_eSprite *spr) {
    if (!app || !spr) return;
    spr->fillSprite(TFT_BLACK);

    // 1. Top Bar / Header
    spr->fillRoundRect(4, 4, 232, 34, 6, 0x18C3);
    spr->drawRoundRect(4, 4, 232, 34, 6, app->color);

    spr->setTextDatum(ML_DATUM);
    spr->setTextColor(0xFFFF);
    spr->drawString("<", 12, 21, 2);

    spr->fillRoundRect(28, 9, 22, 22, 4, 0x0000);
    spr->setTextColor(app->color);
    spr->setTextDatum(MC_DATUM);
    spr->drawString(app->iconText.length() > 0 ? app->iconText : "APP", 39, 20, 1);

    spr->setTextDatum(ML_DATUM);
    spr->setTextColor(0xFFFF);
    spr->drawString(app->name, 56, 21, 2);

    // Falls keine benutzerdefinierte UI: Standard-Infokarte
    if (app->uiJson.length() == 0) {
        spr->setTextDatum(MC_DATUM);
        spr->setTextColor(app->color);
        spr->drawString(app->name, 120, 85, 4);
        spr->setTextColor(0x8BD0);
        spr->drawString("v" + app->version + " · " + app->author, 120, 115, 2);
        spr->setTextColor(0xFFFF);
        spr->drawString(app->description, 120, 140, 1);

        spr->fillRoundRect(25, 175, 190, 38, 8, app->color);
        spr->setTextColor(TFT_BLACK);
        spr->drawString("Zurueck zum Menue", 120, 194, 2);
        return;
    }

    // Dynamic UI Parsing
    String json = app->uiJson;
    auto extractProp = [&](const String &elem, const String &key, const String &dflt = "") -> String {
        int idx = elem.indexOf("\"" + key + "\":\"");
        if (idx != -1) {
            int end = elem.indexOf("\"", idx + key.length() + 4);
            if (end != -1) return elem.substring(idx + key.length() + 4, end);
        }
        idx = elem.indexOf("\"" + key + "\":");
        if (idx != -1) {
            int start = idx + key.length() + 2;
            int end = start;
            while (end < (int)elem.length() && elem.charAt(end) != ',' && elem.charAt(end) != '}' && elem.charAt(end) != ']') end++;
            String val = elem.substring(start, end);
            val.trim();
            return val;
        }
        return dflt;
    };

    int pos = 0;
    while ((pos = json.indexOf('{', pos)) != -1) {
        int end = json.indexOf('}', pos);
        if (end == -1) break;
        String elem = json.substring(pos, end + 1);
        pos = end + 1;

        String type   = extractProp(elem, "type", "");
        String text   = extractProp(elem, "text", "");
        String id     = extractProp(elem, "id", "");
        int y         = extractProp(elem, "y", "50").toInt();
        int x         = extractProp(elem, "x", "20").toInt();
        int w         = extractProp(elem, "w", "200").toInt();
        int h         = extractProp(elem, "h", "36").toInt();
        int size      = extractProp(elem, "size", "2").toInt();
        String colStr = extractProp(elem, "color", "#ffffff");
        uint16_t col  = parseHexColor(colStr);

        if (type == "label") {
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(col);
            spr->drawString(text, 120, y, size > 0 ? size : 2);
        } else if (type == "value") {
            int curVal = dynamicState[app->id + "_" + id];
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(col);
            spr->drawString(String(curVal), 120, y, size > 0 ? size : 4);
        } else if (type == "card") {
            spr->fillRoundRect(x, y, w, h, 8, 0x18C3);
            spr->drawRoundRect(x, y, w, h, 8, col);
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(col);
            spr->drawString(text, x + w / 2, y + h / 2, size > 0 ? size : 2);
        } else if (type == "button") {
            spr->fillRoundRect(x, y, w, h, 8, col);
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(TFT_BLACK);
            spr->drawString(text, x + w / 2, y + h / 2, size > 0 ? size : 2);
        }
    }
}

void AppManager::handleDynamicAppTouch(AppPackage *app, int x, int touchY) {
    if (!app) return;

    // Header Back Button
    if (touchY <= 40) {
        triggerVibration();
        if (app->category == CAT_PRODUCTIVITY) slideToPage(Pages::GoodAppsMenu, -1, 0);
        else slideToPage(Pages::BadAppsMenu, -1, 0);
        return;
    }

    if (app->uiJson.length() == 0) {
        if (touchY >= 170 && touchY <= 220) {
            triggerVibration();
            if (app->category == CAT_PRODUCTIVITY) slideToPage(Pages::GoodAppsMenu, -1, 0);
            else slideToPage(Pages::BadAppsMenu, -1, 0);
        }
        return;
    }

    String json = app->uiJson;
    auto extractProp = [&](const String &elem, const String &key, const String &dflt = "") -> String {
        int idx = elem.indexOf("\"" + key + "\":\"");
        if (idx != -1) {
            int end = elem.indexOf("\"", idx + key.length() + 4);
            if (end != -1) return elem.substring(idx + key.length() + 4, end);
        }
        idx = elem.indexOf("\"" + key + "\":");
        if (idx != -1) {
            int start = idx + key.length() + 2;
            int end = start;
            while (end < (int)elem.length() && elem.charAt(end) != ',' && elem.charAt(end) != '}' && elem.charAt(end) != ']') end++;
            String val = elem.substring(start, end);
            val.trim();
            return val;
        }
        return dflt;
    };

    int pos = 0;
    while ((pos = json.indexOf('{', pos)) != -1) {
        int end = json.indexOf('}', pos);
        if (end == -1) break;
        String elem = json.substring(pos, end + 1);
        pos = end + 1;

        String type   = extractProp(elem, "type", "");
        String action = extractProp(elem, "action", "");
        int btnY      = extractProp(elem, "y", "0").toInt();
        int btnX      = extractProp(elem, "x", "20").toInt();
        int btnW      = extractProp(elem, "w", "200").toInt();
        int btnH      = extractProp(elem, "h", "36").toInt();

        if (type == "button") {
            if (x >= btnX && x <= (btnX + btnW) && touchY >= btnY && touchY <= (btnY + btnH)) {
                triggerVibration();
                if (action.startsWith("inc:")) {
                    String varId = action.substring(4);
                    dynamicState[app->id + "_" + varId]++;
                } else if (action.startsWith("dec:")) {
                    String varId = action.substring(4);
                    dynamicState[app->id + "_" + varId]--;
                } else if (action.startsWith("reset:")) {
                    String varId = action.substring(6);
                    dynamicState[app->id + "_" + varId] = 0;
                } else if (action.startsWith("toast:")) {
                    showToast(action.substring(6));
                } else if (action == "back") {
                    if (app->category == CAT_PRODUCTIVITY) slideToPage(Pages::GoodAppsMenu, -1, 0);
                    else slideToPage(Pages::BadAppsMenu, -1, 0);
                }
                lastSecond = -1;
                return;
            }
        }
    }
}

void AppManager::resetToDefaults() {
    auto uninstalled = getUninstalledList();
    uninstalled.clear();
    saveUninstalledList(uninstalled);

    if (littleFsReady) {
        if (!FFat.exists("/apps")) FFat.mkdir("/apps");
        static const char* appIds[] = {
            "stopwatch", "timer", "mic", "calc", "jiggler", "dice", "level", "totp",
            "lora", "notif", "battery", "flashlight", "phishing", "badusb", "wifispam",
            "blespam", "nmap", "deauth", "probemon", "blescan"
        };
        for (const char* aid : appIds) {
            fs::File f = FFat.open("/apps/" + String(aid) + ".app", "w");
            if (f) {
                f.print(getDefaultAppPackageJson(aid));
                f.close();
            }
        }
    }

    Preferences prefs;
    prefs.begin("packages", false);
    prefs.clear();
    prefs.end();

    init();
}

String AppManager::getPackagesJson() {
    String j = "{\"packages\":[";
    for (size_t i = 0; i < registry.size(); i++) {
        if (i > 0) j += ",";
        const auto &pkg = registry[i];
        bool isRunning = (pkg.statusFn != nullptr) ? pkg.statusFn() : false;
        
        char hexCol[8];
        uint8_t r = ((pkg.color >> 11) & 0x1F) * 255 / 31;
        uint8_t g = ((pkg.color >> 5) & 0x3F) * 255 / 63;
        uint8_t b = (pkg.color & 0x1F) * 255 / 31;
        snprintf(hexCol, sizeof(hexCol), "#%02x%02x%02x", r, g, b);

        j += "{";
        j += "\"id\":\"" + jsonEscape(pkg.id) + "\"";
        j += ",\"name\":\"" + jsonEscape(pkg.name) + "\"";
        j += ",\"cat\":" + String((int)pkg.category);
        j += ",\"cat_name\":\"" + String(pkg.category == CAT_PRODUCTIVITY ? "Productivity" : (pkg.category == CAT_SECURITY ? "Security" : "System")) + "\"";
        j += ",\"color\":\"" + String(hexCol) + "\"";
        j += ",\"icon\":\"" + jsonEscape(pkg.iconText) + "\"";
        j += ",\"page\":" + String(pkg.pageId);
        j += ",\"enabled\":" + String(pkg.enabled ? "true" : "false");
        j += ",\"is_custom\":" + String(pkg.isCustom ? "true" : "false");
        j += ",\"version\":\"" + jsonEscape(pkg.version) + "\"";
        j += ",\"author\":\"" + jsonEscape(pkg.author) + "\"";
        j += ",\"description\":\"" + jsonEscape(pkg.description) + "\"";
        j += ",\"running\":" + String(isRunning ? "true" : "false");
        j += ",\"order\":" + String(pkg.order);
        j += "}";
    }
    j += "]}";
    return j;
}
