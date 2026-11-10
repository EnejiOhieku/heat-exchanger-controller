#pragma once

#include <Arduino.h>
#include "../config.h"

enum SafetyError : uint8_t {
    SAFETY_OK = 0,
    SAFETY_ERR_DRY_FIRE = 1,
    SAFETY_ERR_OVER_TEMP = 2,
    SAFETY_ERR_SENSOR_FAULT = 3,
    SAFETY_ERR_ESTOP = 4
};

enum SafetyWarning : uint8_t {
    SAFETY_WARN_NONE = 0,
    SAFETY_WARN_NO_FLOW = 1 << 0,         // Heater wants heat but flow permissive is missing
    SAFETY_WARN_PUMP_DRY_RUN = 1 << 1,    // Pump running, no flow detected
    SAFETY_WARN_COLD_SENSOR = 1 << 2,     // Cold-side probe invalid (calculations degraded)
    SAFETY_WARN_NO_COLD_FLOW = 1 << 3     // Hot side running but the cold loop has no flow
};

struct SafetyInputs {
    float flowLPM;
    float coldFlowLPM;
    uint8_t probeFaultMask;   // bit i = ProbeIndex i invalid
    float maxValidTempC;      // hottest valid probe
    bool heaterDemand;        // controller wants heat
    bool relayEnergized;      // relay actually on (previous tick)
    bool pumpRunning;
};

// Two layers:
//  1. Permissive (non-latching): the heater may only energise with flow present.
//     Missing flow simply holds the heater off; it is not a fault.
//  2. Trips (latching until operator reset): over-temperature, hot-side probe
//     failure, flow lost *while the relay was energised* (real dry-fire), E-stop.
class SafetyManager {
public:
    SafetyManager()
        : _currentError(SAFETY_OK),
          _warnings(SAFETY_WARN_NONE),
          _tripDetail(0),
          _lowFlowStartTime(0),
          _dryRunStartTime(0),
          _bootTime(0),
          _tripCount(0),
          _permissive(false),
          _lastInputs{} {}

    void init() {
        _currentError = SAFETY_OK;
        _bootTime = millis();
    }

    void update(const SafetyInputs& in) {
        _lastInputs = in;
        uint32_t now = millis();
        bool flowOk = in.flowLPM >= MIN_FLOW_LPM_FOR_HEATER;
        _warnings = SAFETY_WARN_NONE;

        // Warnings (informational, never latch)
        if (in.probeFaultMask & 0b1100) _warnings |= SAFETY_WARN_COLD_SENSOR;
        if (in.heaterDemand && !flowOk) _warnings |= SAFETY_WARN_NO_FLOW;
        if ((in.heaterDemand || in.pumpRunning) && in.coldFlowLPM < MIN_COLD_FLOW_LPM) {
            _warnings |= SAFETY_WARN_NO_COLD_FLOW;
        }
        if (in.pumpRunning && in.flowLPM < 0.05f) {
            if (_dryRunStartTime == 0) _dryRunStartTime = now;
            if (now - _dryRunStartTime >= PUMP_DRY_RUN_TIMEOUT_MS) _warnings |= SAFETY_WARN_PUMP_DRY_RUN;
        } else {
            _dryRunStartTime = 0;
        }

        if (_currentError == SAFETY_OK) {
            bool booting = (now - _bootTime) < SAFETY_BOOT_GRACE_MS;

            if (!booting && (in.probeFaultMask & 0b0011)) {
                trip(SAFETY_ERR_SENSOR_FAULT, in.probeFaultMask & 0b0011);
            } else if (in.maxValidTempC >= MAX_SAFE_TEMP_C) {
                trip(SAFETY_ERR_OVER_TEMP, 0);
            } else if (in.relayEnergized && !flowOk) {
                if (_lowFlowStartTime == 0) {
                    _lowFlowStartTime = now;
                } else if (now - _lowFlowStartTime >= ZERO_FLOW_TIMEOUT_MS) {
                    trip(SAFETY_ERR_DRY_FIRE, 0);
                }
            } else {
                _lowFlowStartTime = 0;
            }
        }

        // While flow is lost with the relay on we keep the permissive for the
        // debounce window (a bubble should not cycle the relay) and then trip.
        bool flowGrace = in.relayEnergized && _lowFlowStartTime != 0;
        _permissive = (_currentError == SAFETY_OK) && (flowOk || flowGrace);
    }

    bool isHeaterAllowed() const { return _permissive; }
    bool isTripped() const { return _currentError != SAFETY_OK; }
    uint8_t getWarnings() const { return _warnings; }
    uint32_t getTripCount() const { return _tripCount; }
    uint8_t getTripDetail() const { return _tripDetail; }

    void triggerEStop() { trip(SAFETY_ERR_ESTOP, 0); }

    // Clears the latch only if the cause is gone. Returns false (and keeps the
    // trip) if the condition is still present.
    bool resetFault() {
        const SafetyInputs& in = _lastInputs;
        if (_currentError == SAFETY_ERR_SENSOR_FAULT && (in.probeFaultMask & 0b0011)) return false;
        if (_currentError == SAFETY_ERR_OVER_TEMP && in.maxValidTempC >= MAX_SAFE_TEMP_C - 5.0f) return false;
        _currentError = SAFETY_OK;
        _tripDetail = 0;
        _lowFlowStartTime = 0;
        return true;
    }

    SafetyError getError() const { return _currentError; }

    const char* getErrorString() const {
        switch (_currentError) {
            case SAFETY_OK:               return "SYSTEM NORMAL";
            case SAFETY_ERR_DRY_FIRE:     return "DRY-FIRE: FLOW LOST WHILE HEATING";
            case SAFETY_ERR_OVER_TEMP:    return "OVER-TEMPERATURE SHUTDOWN";
            case SAFETY_ERR_SENSOR_FAULT: return "HOT-SIDE SENSOR FAULT";
            case SAFETY_ERR_ESTOP:        return "EMERGENCY STOP";
            default:                      return "UNKNOWN FAULT";
        }
    }

    // <= 16 characters for the LCD.
    const char* getShortString() const {
        switch (_currentError) {
            case SAFETY_ERR_DRY_FIRE:     return "DRY-FIRE TRIP";
            case SAFETY_ERR_OVER_TEMP:    return "OVER-TEMP TRIP";
            case SAFETY_ERR_SENSOR_FAULT: return (_tripDetail & 0b01) ? "SENSOR Th,in" : "SENSOR Th,out";
            case SAFETY_ERR_ESTOP:        return "E-STOP ACTIVE";
            default:                      return "NORMAL";
        }
    }

    float getMaxObservedTemp() const { return _lastInputs.maxValidTempC; }

private:
    void trip(SafetyError err, uint8_t detail) {
        if (_currentError == SAFETY_OK) _tripCount++;
        _currentError = err;
        _tripDetail = detail;
        _permissive = false;
        Serial.printf("[Safety] TRIP: %s\n", getErrorString());
    }

    SafetyError _currentError;
    uint8_t _warnings;
    uint8_t _tripDetail;
    uint32_t _lowFlowStartTime;
    uint32_t _dryRunStartTime;
    uint32_t _bootTime;
    uint32_t _tripCount;
    bool _permissive;
    SafetyInputs _lastInputs;
};

inline SafetyManager Safety;
