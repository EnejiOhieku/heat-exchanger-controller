#pragma once

#include <Arduino.h>
#include "config.h"

class PumpController {
public:
    PumpController();

    void init();
    void update(); // Called frequently to handle smooth ramping

    void setSpeedPercent(uint8_t speedPct);
    uint8_t getSpeedPercent() const;
    uint8_t getTargetSpeedPercent() const;

    void setEnabled(bool enabled);
    bool isEnabled() const;

    void stop();

private:
    uint8_t _targetSpeedPct;
    float _currentSpeedPct;
    bool _enabled;
    uint32_t _lastRampTime;
};

extern PumpController Pump;
