// --- EvilPortalApp.h ---
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

void handleEvilRoot();
void handleEvilSubmit();
void handleEvilTOS();     // NEU: Nutzungsbedingungen
void handleEvilImprint(); // NEU: Impressum

void showEvilPortal(TFT_eSprite *spr);
void handleEvilTouch(int x, int y);
void toggleEvilPortal();