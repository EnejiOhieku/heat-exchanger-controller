#pragma once

#include <Arduino.h>
#include <esp_arduino_version.h>
#include "../config.h"

class PumpController {
public:
    PumpController()
        : _targetSpeedPct(0),
          _currentSpeedPct(0.0f),
          _enabled(false),
          _inverted(PUMP_INVERT_ROTATION),
          _lastRampTime(0) {}

    void init() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
        ledcAttach(PIN_PUMP_IN1, PUMP_PWM_FREQ, PUMP_PWM_RES);
        ledcAttach(PIN_PUMP_IN2, PUMP_PWM_FREQ, PUMP_PWM_RES);
        ledcWrite(PIN_PUMP_IN1, 0);
        ledcWrite(PIN_PUMP_IN2, 0);
#else
        ledcSetup(PUMP_PWM_CHANNEL_1, PUMP_PWM_FREQ, PUMP_PWM_RES);
        ledcAttachPin(PIN_PUMP_IN1, PUMP_PWM_CHANNEL_1);
        ledcSetup(PUMP_PWM_CHANNEL_2, PUMP_PWM_FREQ, PUMP_PWM_RES);
        ledcAttachPin(PIN_PUMP_IN2, PUMP_PWM_CHANNEL_2);
        ledcWrite(PUMP_PWM_CHANNEL_1, 0);
        ledcWrite(PUMP_PWM_CHANNEL_2, 0);
#endif

        _lastRampTime = millis();
        Serial.printf("[Pump] Initialized 2-PWM Driver on IN1=%d, IN2=%d (%d Hz, %d-bit resolution)\n",
                      PIN_PUMP_IN1, PIN_PUMP_IN2, PUMP_PWM_FREQ, PUMP_PWM_RES);
    }

    void setSpeedPercent(uint8_t speedPct) {
        if (speedPct > 100) speedPct = 100;
        _targetSpeedPct = speedPct;
        if (_targetSpeedPct > 0) {
            _enabled = true;
        }
    }

    uint8_t getSpeedPercent() const {
        return (uint8_t)(_currentSpeedPct + 0.5f);
    }

    uint8_t getTargetSpeedPercent() const {
        return _targetSpeedPct;
    }

    void setEnabled(bool enabled) {
        _enabled = enabled;
        if (!_enabled) {
            _targetSpeedPct = 0;
        }
    }

    bool isEnabled() const {
        return _enabled && (_currentSpeedPct > 0.5f);
    }

    void setInverted(bool inverted) {
        _inverted = inverted;
    }

    bool isInverted() const {
        return _inverted;
    }

    void stop() {
        _targetSpeedPct = 0;
        _currentSpeedPct = 0.0f;
        _enabled = false;

        writePwmOutputs(0, 0);
    }

    void update() {
        uint32_t now = millis();
        uint32_t dt = now - _lastRampTime;

        if (dt >= 20) {
            _lastRampTime = now;

            float target = _enabled ? (float)_targetSpeedPct : 0.0f;
            float diff = target - _currentSpeedPct;

            if (fabsf(diff) > 0.2f) {
                float step = (diff > 0.0f) ? 4.0f : -4.0f;
                if (fabsf(diff) < fabsf(step)) {
                    _currentSpeedPct = target;
                } else {
                    _currentSpeedPct += step;
                }
            } else {
                _currentSpeedPct = target;
            }

            uint8_t duty = (uint8_t)((_currentSpeedPct / 100.0f) * 255.0f);

            if (duty == 0) {
                writePwmOutputs(0, 0);
            } else {
                if (!_inverted) {
                    writePwmOutputs(duty, 0);
                } else {
                    writePwmOutputs(0, duty);
                }
            }
        }
    }

private:
    void writePwmOutputs(uint8_t duty1, uint8_t duty2) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
        ledcWrite(PIN_PUMP_IN1, duty1);
        ledcWrite(PIN_PUMP_IN2, duty2);
#else
        ledcWrite(PUMP_PWM_CHANNEL_1, duty1);
        ledcWrite(PUMP_PWM_CHANNEL_2, duty2);
#endif
    }

    uint8_t _targetSpeedPct;
    float _currentSpeedPct;
    bool _enabled;
    bool _inverted;
    uint32_t _lastRampTime;
};

inline PumpController Pump;
