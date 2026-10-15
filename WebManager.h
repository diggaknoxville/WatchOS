// --- WebManager.h ---
#pragma once
#include <Arduino.h>
#include <FS.h>
using namespace fs;
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

extern DNSServer dnsServer;
extern WebServer server;
extern const byte DNS_PORT;

void setupServerRoutes(); // NEU: Routen-Weiche
void toggleWLAN();
void startHotspotAP();
void stopHotspotAP();
void handleWlanIdleTimeout(); // schaltet ungenutzten Hotspot nach Timeout ab