#pragma once

#include <Arduino.h>
#include "config.h"
#include "SafetyManager.h"

enum HeaterMode : uint8_t {
    HEATER_MODE_OFF = 0,
    HEATER_MODE_MANUAL = 1,
    HEATER_MODE_THERMOSTATIC = 2,
    HEATER_MODE_TIME_PWM = 3
};

class HeaterController {
public:
    HeaterController();

    void init();
    void update(float currentControlTemp);

    void setMode(HeaterMode mode);
    HeaterMode getMode() const;

    void setManualState(bool on);
    bool getManualState() const;

    void setTargetTemp(float targetC);
    float getTargetTemp() const;

    void setHysteresis(float hystC);
    float getHysteresis() const;

    void setDutyPercent(uint8_t dutyPct);
    uint8_t getDutyPercent() const;

    bool isRelayEnergized() const;
    void forceOff();

private:
    void applyRelayState(bool energize);

    HeaterMode _mode;
    bool _manualState;
    float _targetTempC;
    float _hysteresisC;
    uint8_t _dutyPercent;
    bool _relayState;
    uint32_t _windowStartTime;
};

extern HeaterController Heater;
