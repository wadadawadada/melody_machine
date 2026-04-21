#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

void spiGuardInit();
bool spiGuardLock(TickType_t timeoutTicks = pdMS_TO_TICKS(20));
void spiGuardUnlock();

class SpiGuardScope {
public:
    explicit SpiGuardScope(TickType_t timeoutTicks = pdMS_TO_TICKS(20))
        : _locked(spiGuardLock(timeoutTicks)) {}

    ~SpiGuardScope() {
        if (_locked) spiGuardUnlock();
    }

    bool locked() const { return _locked; }

private:
    bool _locked;
};
