#pragma once

#include <Arduino.h>
#include "config.h"

enum SafetyError : uint8_t {
    SAFETY_OK = 0,
    SAFETY_ERR_DRY_FIRE = 1,
    SAFETY_ERR_OVER_TEMP = 2,
    SAFETY_ERR_SENSOR_FAULT = 3,
    SAFETY_ERR_ESTOP = 4
};

class SafetyManager {
public:
    SafetyManager();

    void init();
    void update(float flowRateLPM, float thIn, float thOut, float tcIn, float tcOut, bool heaterCommanded);

    bool isHeaterAllowed() const;
    bool isTripped() const;
    void triggerEStop();
    void resetFault();

    SafetyError getError() const;
    const char* getErrorString() const;
    float getMaxObservedTemp() const;

private:
    SafetyError _currentError;
    uint32_t _lowFlowStartTime;
    bool _tripped;
    float _maxObservedTemp;
};

extern SafetyManager Safety;
