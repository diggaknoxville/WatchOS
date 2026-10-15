// --- SystemEvents.cpp ---
#include "SystemEvents.h"

static SystemEventHandler handlers[MAX_EVENT_HANDLERS];
static int handlerCount = 0;

void SystemEvents::init() {
    handlerCount = 0;
    for (int i = 0; i < MAX_EVENT_HANDLERS; i++) handlers[i] = nullptr;
}

bool SystemEvents::registerHandler(SystemEventHandler handler) {
    if (!handler || handlerCount >= MAX_EVENT_HANDLERS) return false;
    handlers[handlerCount++] = handler;
    return true;
}

void SystemEvents::post(SystemEventType evt, void* data) {
    for (int i = 0; i < handlerCount; i++) {
        if (handlers[i]) {
            handlers[i](evt, data);
        }
    }
}

