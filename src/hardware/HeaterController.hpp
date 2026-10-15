#pragma once

#include <Arduino.h>
#include "../config.h"
#include "../core/SafetyManager.hpp"

enum HeaterMode : uint8_t {
    HEATER_MODE_OFF = 0,
    HEATER_MODE_MANUAL = 1,
    HEATER_MODE_THERMOSTATIC = 2,
    HEATER_MODE_TIME_PWM = 3
};

class HeaterController {
public:
    HeaterController()
        : _mode(HEATER_MODE_OFF),
          _manualState(false),
          _targetTempC(50.0f),
          _hysteresisC(1.0f),
          _dutyPercent(0),
          _relayState(false),
          _windowStartTime(0) {}

    void init() {
        pinMode(PIN_RELAY_HEATER, OUTPUT);
        applyRelayState(false);
        _windowStartTime = millis();
        Serial.printf("[Heater] Initialized on Relay GPIO %d (Active Level: %s)\n",
                      PIN_RELAY_HEATER, (RELAY_ACTIVE_LEVEL == HIGH) ? "HIGH" : "LOW");
    }

    void forceOff() {
        _mode = HEATER_MODE_OFF;
        _manualState = false;
        _dutyPercent = 0;
        applyRelayState(false);
    }

    void setMode(HeaterMode mode) {
        _mode = mode;
        if (_mode == HEATER_MODE_OFF) {
            applyRelayState(false);
        } else if (_mode == HEATER_MODE_TIME_PWM) {
            _windowStartTime = millis();
        }
    }

    HeaterMode getMode() const { return _mode; }

    void setManualState(bool on) { _manualState = on; }
    bool getManualState() const { return _manualState; }

    void setTargetTemp(float targetC) {
        if (targetC >= 10.0f && targetC <= (MAX_SAFE_TEMP_C - 2.0f)) {
            _targetTempC = targetC;
        }
    }
    float getTargetTemp() const { return _targetTempC; }

    void setHysteresis(float hystC) {
        if (hystC >= 0.2f && hystC <= 5.0f) {
            _hysteresisC = hystC;
        }
    }
    float getHysteresis() const { return _hysteresisC; }

    void setDutyPercent(uint8_t dutyPct) {
        if (dutyPct > 100) dutyPct = 100;
        _dutyPercent = dutyPct;
    }
    uint8_t getDutyPercent() const { return _dutyPercent; }

    bool isRelayEnergized() const { return _relayState; }

    void update(float currentControlTemp) {
        if (!Safety.isHeaterAllowed()) {
            applyRelayState(false);
            return;
        }

        uint32_t now = millis();

        switch (_mode) {
            case HEATER_MODE_OFF:
                applyRelayState(false);
                break;

            case HEATER_MODE_MANUAL:
                applyRelayState(_manualState);
                break;

            case HEATER_MODE_THERMOSTATIC:
                if (currentControlTemp < (_targetTempC - _hysteresisC)) {
                    applyRelayState(true);
                } else if (currentControlTemp > (_targetTempC + _hysteresisC)) {
                    applyRelayState(false);
                }
                break;

            case HEATER_MODE_TIME_PWM: {
                if (now - _windowStartTime >= HEATER_CYCLE_WINDOW_MS) {
                    _windowStartTime += HEATER_CYCLE_WINDOW_MS;
                }
                uint32_t onDuration = (HEATER_CYCLE_WINDOW_MS * _dutyPercent) / 100;
                if ((now - _windowStartTime) < onDuration) {
                    applyRelayState(true);
                } else {
                    applyRelayState(false);
                }
                break;
            }
        }
    }

private:
    void applyRelayState(bool energize) {
        _relayState = energize;
        if (_relayState) {
            digitalWrite(PIN_RELAY_HEATER, RELAY_ACTIVE_LEVEL);
        } else {
            digitalWrite(PIN_RELAY_HEATER, (RELAY_ACTIVE_LEVEL == HIGH) ? LOW : HIGH);
        }
    }

    HeaterMode _mode;
    bool _manualState;
    float _targetTempC;
    float _hysteresisC;
    uint8_t _dutyPercent;
    bool _relayState;
    uint32_t _windowStartTime;
};

inline HeaterController Heater;
