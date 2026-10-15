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
          _lastRampTime(0) {}

    void init() {
        pinMode(PIN_PUMP_IN1, OUTPUT);
        pinMode(PIN_PUMP_IN2, OUTPUT);

        digitalWrite(PIN_PUMP_IN1, LOW);
        digitalWrite(PIN_PUMP_IN2, LOW);

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
        ledcAttach(PIN_PUMP_ENA, PUMP_PWM_FREQ, PUMP_PWM_RES);
        ledcWrite(PIN_PUMP_ENA, 0);
#else
        ledcSetup(PUMP_PWM_CHANNEL, PUMP_PWM_FREQ, PUMP_PWM_RES);
        ledcAttachPin(PIN_PUMP_ENA, PUMP_PWM_CHANNEL);
        ledcWrite(PUMP_PWM_CHANNEL, 0);
#endif

        _lastRampTime = millis();
        Serial.printf("[Pump] Initialized on ENA=%d (PWM), IN1=%d, IN2=%d\n", PIN_PUMP_ENA, PIN_PUMP_IN1, PIN_PUMP_IN2);
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

    void stop() {
        _targetSpeedPct = 0;
        _currentSpeedPct = 0.0f;
        _enabled = false;

        digitalWrite(PIN_PUMP_IN1, LOW);
        digitalWrite(PIN_PUMP_IN2, LOW);

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
        ledcWrite(PIN_PUMP_ENA, 0);
#else
        ledcWrite(PUMP_PWM_CHANNEL, 0);
#endif
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

            if (duty > 0) {
                digitalWrite(PIN_PUMP_IN1, HIGH);
                digitalWrite(PIN_PUMP_IN2, LOW);
            } else {
                digitalWrite(PIN_PUMP_IN1, LOW);
                digitalWrite(PIN_PUMP_IN2, LOW);
            }

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
            ledcWrite(PIN_PUMP_ENA, duty);
#else
            ledcWrite(PUMP_PWM_CHANNEL, duty);
#endif
        }
    }

private:
    uint8_t _targetSpeedPct;
    float _currentSpeedPct;
    bool _enabled;
    uint32_t _lastRampTime;
};

inline PumpController Pump;
