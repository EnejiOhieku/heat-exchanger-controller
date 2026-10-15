#include "SafetyManager.h"

SafetyManager Safety;

SafetyManager::SafetyManager()
    : _currentError(SAFETY_OK),
      _lowFlowStartTime(0),
      _tripped(false),
      _maxObservedTemp(0.0f) {}

void SafetyManager::init() {
    _currentError = SAFETY_OK;
    _lowFlowStartTime = 0;
    _tripped = false;
    _maxObservedTemp = 0.0f;
}

void SafetyManager::update(float flowRateLPM, float thIn, float thOut, float tcIn, float tcOut, bool heaterCommanded) {
    if (_currentError == SAFETY_ERR_ESTOP) {
        _tripped = true;
        return;
    }

    // 1. Check Sensor Disconnection / Error states
    float temps[4] = { thIn, thOut, tcIn, tcOut };
    _maxObservedTemp = -999.0f;

    for (int i = 0; i < 4; i++) {
        float t = temps[i];
        if (t > _maxObservedTemp) _maxObservedTemp = t;

        // Dallas error values or out of plausible range
        if (t <= SENSOR_DISCONNECT_VAL || t == SENSOR_POR_RESET_VAL ||
            t < MIN_PLAUSIBLE_TEMP_C || t > MAX_PLAUSIBLE_TEMP_C) {
            _currentError = SAFETY_ERR_SENSOR_FAULT;
            _tripped = true;
            return;
        }
    }

    // 2. Check Over-Temperature Limit
    if (_maxObservedTemp >= MAX_SAFE_TEMP_C) {
        _currentError = SAFETY_ERR_OVER_TEMP;
        _tripped = true;
        return;
    }

    // 3. Check Zero-Flow Dry-Fire Protection
    if (heaterCommanded) {
        if (flowRateLPM < MIN_FLOW_LPM_FOR_HEATER) {
            if (_lowFlowStartTime == 0) {
                _lowFlowStartTime = millis();
            } else if (millis() - _lowFlowStartTime >= ZERO_FLOW_TIMEOUT_MS) {
                _currentError = SAFETY_ERR_DRY_FIRE;
                _tripped = true;
                return;
            }
        } else {
            _lowFlowStartTime = 0;
        }
    } else {
        _lowFlowStartTime = 0;
    }

    // If no trip condition active and not manually tripped
    if (!_tripped) {
        _currentError = SAFETY_OK;
    }
}

bool SafetyManager::isHeaterAllowed() const {
    return !_tripped && (_currentError == SAFETY_OK);
}

bool SafetyManager::isTripped() const {
    return _tripped;
}

void SafetyManager::triggerEStop() {
    _currentError = SAFETY_ERR_ESTOP;
    _tripped = true;
}

void SafetyManager::resetFault() {
    _currentError = SAFETY_OK;
    _tripped = false;
    _lowFlowStartTime = 0;
}

SafetyError SafetyManager::getError() const {
    return _currentError;
}

const char* SafetyManager::getErrorString() const {
    switch (_currentError) {
        case SAFETY_OK:               return "SYSTEM NORMAL";
        case SAFETY_ERR_DRY_FIRE:     return "DRY-FIRE (LOW FLOW)";
        case SAFETY_ERR_OVER_TEMP:    return "OVER-TEMP SHUTDOWN";
        case SAFETY_ERR_SENSOR_FAULT: return "SENSOR DISCONNECTED";
        case SAFETY_ERR_ESTOP:        return "EMERGENCY STOP (E-STOP)";
        default:                      return "UNKNOWN FAULT";
    }
}

float SafetyManager::getMaxObservedTemp() const {
    return _maxObservedTemp;
}
