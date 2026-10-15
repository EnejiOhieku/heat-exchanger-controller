#include "HeaterController.h"

HeaterController Heater;

HeaterController::HeaterController()
    : _mode(HEATER_MODE_OFF),
      _manualState(false),
      _targetTempC(50.0f),
      _hysteresisC(1.0f),
      _dutyPercent(0),
      _relayState(false),
      _windowStartTime(0) {}

void HeaterController::init() {
    pinMode(PIN_RELAY_HEATER, OUTPUT);
    applyRelayState(false);
    _windowStartTime = millis();
    Serial.printf("[Heater] Initialized on Relay GPIO %d (Active Level: %s)\n",
                  PIN_RELAY_HEATER, (RELAY_ACTIVE_LEVEL == HIGH) ? "HIGH" : "LOW");
}

void HeaterController::applyRelayState(bool energize) {
    _relayState = energize;
    if (_relayState) {
        digitalWrite(PIN_RELAY_HEATER, RELAY_ACTIVE_LEVEL);
    } else {
        digitalWrite(PIN_RELAY_HEATER, (RELAY_ACTIVE_LEVEL == HIGH) ? LOW : HIGH);
    }
}

void HeaterController::forceOff() {
    _mode = HEATER_MODE_OFF;
    _manualState = false;
    _dutyPercent = 0;
    applyRelayState(false);
}

void HeaterController::setMode(HeaterMode mode) {
    _mode = mode;
    if (_mode == HEATER_MODE_OFF) {
        applyRelayState(false);
    } else if (_mode == HEATER_MODE_TIME_PWM) {
        _windowStartTime = millis();
    }
}

HeaterMode HeaterController::getMode() const {
    return _mode;
}

void HeaterController::setManualState(bool on) {
    _manualState = on;
}

bool HeaterController::getManualState() const {
    return _manualState;
}

void HeaterController::setTargetTemp(float targetC) {
    if (targetC >= 10.0f && targetC <= (MAX_SAFE_TEMP_C - 2.0f)) {
        _targetTempC = targetC;
    }
}

float HeaterController::getTargetTemp() const {
    return _targetTempC;
}

void HeaterController::setHysteresis(float hystC) {
    if (hystC >= 0.2f && hystC <= 5.0f) {
        _hysteresisC = hystC;
    }
}

float HeaterController::getHysteresis() const {
    return _hysteresisC;
}

void HeaterController::setDutyPercent(uint8_t dutyPct) {
    if (dutyPct > 100) dutyPct = 100;
    _dutyPercent = dutyPct;
}

uint8_t HeaterController::getDutyPercent() const {
    return _dutyPercent;
}

bool HeaterController::isRelayEnergized() const {
    return _relayState;
}

void HeaterController::update(float currentControlTemp) {
    // Crucial: Check Safety Interlocks first!
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
            // Bang-bang with hysteresis:
            // Turn ON when temp falls below (target - hysteresis)
            // Turn OFF when temp rises above (target + hysteresis)
            if (currentControlTemp < (_targetTempC - _hysteresisC)) {
                applyRelayState(true);
            } else if (currentControlTemp > (_targetTempC + _hysteresisC)) {
                applyRelayState(false);
            }
            break;

        case HEATER_MODE_TIME_PWM: {
            // Slow-cycle time proportioning across 5000ms window
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
