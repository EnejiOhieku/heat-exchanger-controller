#include "PumpController.h"
#include <esp_arduino_version.h>

PumpController Pump;

PumpController::PumpController()
    : _targetSpeedPct(0),
      _currentSpeedPct(0.0f),
      _enabled(false),
      _lastRampTime(0) {}

void PumpController::init() {
    pinMode(PIN_PUMP_IN1, OUTPUT);
    pinMode(PIN_PUMP_IN2, OUTPUT);

    // Direction configuration: Forward rotation
    digitalWrite(PIN_PUMP_IN1, LOW);
    digitalWrite(PIN_PUMP_IN2, LOW);

    // Initialize PWM on ENA pin
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

void PumpController::setSpeedPercent(uint8_t speedPct) {
    if (speedPct > 100) speedPct = 100;
    _targetSpeedPct = speedPct;
    if (_targetSpeedPct > 0) {
        _enabled = true;
    }
}

uint8_t PumpController::getSpeedPercent() const {
    return (uint8_t)(_currentSpeedPct + 0.5f);
}

uint8_t PumpController::getTargetSpeedPercent() const {
    return _targetSpeedPct;
}

void PumpController::setEnabled(bool enabled) {
    _enabled = enabled;
    if (!_enabled) {
        _targetSpeedPct = 0;
    }
}

bool PumpController::isEnabled() const {
    return _enabled && (_currentSpeedPct > 0.5f);
}

void PumpController::stop() {
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

void PumpController::update() {
    uint32_t now = millis();
    uint32_t dt = now - _lastRampTime;

    if (dt >= 20) { // Update ramping every 20ms
        _lastRampTime = now;

        float target = _enabled ? (float)_targetSpeedPct : 0.0f;
        float diff = target - _currentSpeedPct;

        if (fabsf(diff) > 0.2f) {
            // Slew rate limit: max ~100% change in 500ms -> 4% per 20ms
            float step = (diff > 0.0f) ? 4.0f : -4.0f;
            if (fabsf(diff) < fabsf(step)) {
                _currentSpeedPct = target;
            } else {
                _currentSpeedPct += step;
            }
        } else {
            _currentSpeedPct = target;
        }

        // Apply direction and PWM
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
