#pragma once

#include <Arduino.h>
#include "../config.h"
#include "../core/Pid.hpp"

enum HeaterMode : uint8_t {
    HEATER_MODE_OFF = 0,
    HEATER_MODE_MANUAL = 1,         // Operator relay on/off
    HEATER_MODE_THERMOSTATIC = 2,   // On/off with hysteresis and min switch time
    HEATER_MODE_TIME_PWM = 3,       // Fixed duty, time-proportioned over a window
    HEATER_MODE_PID = 4,            // PID -> duty -> time-proportioned relay
    HEATER_MODE_COUNT
};

// The controller computes a *demand* every tick; the relay follows the demand
// only while the safety layer grants the permissive. Keeping demand and relay
// state separate lets the UI show "waiting for flow" instead of a silent OFF.
class HeaterController {
public:
    HeaterController()
        : _mode(HEATER_MODE_OFF),
          _manualState(false),
          _targetTempC(DEFAULT_HEATER_TARGET_C),
          _hysteresisC(DEFAULT_HEATER_HYST_C),
          _dutyPercent(0),
          _controlProbe(0),
          _demand(false),
          _relayState(false),
          _lastSwitchTime(0),
          _windowStartTime(0),
          _lastPidTime(0),
          _pid(DEFAULT_HEATER_KP, DEFAULT_HEATER_KI, DEFAULT_HEATER_KD, 0.0f, 100.0f) {}

    void init() {
        pinMode(PIN_RELAY_HEATER, OUTPUT);
        applyRelayState(false);
        _windowStartTime = millis();
        Serial.printf("[Heater] Relay on GPIO %d (active %s)\n",
                      PIN_RELAY_HEATER, (RELAY_ACTIVE_LEVEL == HIGH) ? "HIGH" : "LOW");
    }

    void forceOff() {
        _mode = HEATER_MODE_OFF;
        _manualState = false;
        _dutyPercent = 0;
        _demand = false;
        applyRelayState(false);
    }

    bool setMode(uint8_t mode) {
        if (mode >= HEATER_MODE_COUNT) return false;
        if (mode == _mode) return true;
        _mode = (HeaterMode)mode;
        _windowStartTime = millis();
        if (_mode == HEATER_MODE_PID) {
            _pid.reset(0.0f);
            _lastPidTime = 0;
        }
        if (_mode == HEATER_MODE_OFF) {
            _demand = false;
            applyRelayState(false);
        }
        return true;
    }
    HeaterMode getMode() const { return _mode; }
    const char* getModeShort() const {
        static const char* names[] = { "OFF", "MAN", "THR", "TPW", "PID" };
        return names[_mode];
    }

    void setManualState(bool on) { _manualState = on; }
    bool getManualState() const { return _manualState; }

    bool setTargetTemp(float targetC) {
        if (targetC < 10.0f || targetC > (MAX_SAFE_TEMP_C - 2.0f)) return false;
        _targetTempC = targetC;
        return true;
    }
    float getTargetTemp() const { return _targetTempC; }

    bool setHysteresis(float hystC) {
        if (hystC < 0.2f || hystC > 5.0f) return false;
        _hysteresisC = hystC;
        return true;
    }
    float getHysteresis() const { return _hysteresisC; }

    void setDutyPercent(float dutyPct) { _dutyPercent = constrain(dutyPct, 0.0f, 100.0f); }
    // Duty actually being applied (PID output in PID mode).
    float getDutyPercent() const {
        if (_mode == HEATER_MODE_PID) return _pid.output();
        if (_mode == HEATER_MODE_TIME_PWM) return _dutyPercent;
        return _relayState ? 100.0f : 0.0f;
    }
    float getManualDutySetting() const { return _dutyPercent; }

    void setPidGains(float kp, float ki, float kd) {
        _pid.setGains(max(kp, 0.0f), max(ki, 0.0f), max(kd, 0.0f));
    }
    float getKp() const { return _pid.kp(); }
    float getKi() const { return _pid.ki(); }
    float getKd() const { return _pid.kd(); }

    void setControlProbe(uint8_t probe) { if (probe < 4) _controlProbe = probe; }
    uint8_t getControlProbe() const { return _controlProbe; }

    // Simulation: relay logic keeps running, the physical pin stays de-energised.
    void setOutputsInhibited(bool inhibit) {
        _outputsInhibited = inhibit;
        applyRelayState(_relayState);
    }

    bool isDemanding() const { return _demand; }
    bool isRelayEnergized() const { return _relayState; }

    // controlTemp: selected probe (SENSOR_DISCONNECT_VAL if invalid).
    // permitted: safety permissive (flow present, no trips).
    void update(float controlTemp, bool permitted) {
        uint32_t now = millis();
        bool tempValid = controlTemp > SENSOR_DISCONNECT_VAL + 1.0f;

        switch (_mode) {
            case HEATER_MODE_OFF:
                _demand = false;
                break;

            case HEATER_MODE_MANUAL:
                _demand = _manualState;
                break;

            case HEATER_MODE_THERMOSTATIC:
                // Turning OFF is immediate; turning back ON is rate-limited.
                if (!tempValid || controlTemp > _targetTempC + _hysteresisC) {
                    _demand = false;
                } else if (controlTemp < _targetTempC - _hysteresisC &&
                           now - _lastSwitchTime >= HEATER_MIN_SWITCH_MS) {
                    _demand = true;
                }
                break;

            case HEATER_MODE_TIME_PWM:
                _demand = timeProportion(_dutyPercent, now);
                break;

            case HEATER_MODE_PID:
                if (!tempValid) {
                    _pid.reset(0.0f);
                    _demand = false;
                    break;
                }
                if (_lastPidTime == 0 || now - _lastPidTime >= 1000) {
                    float dt = (_lastPidTime == 0) ? 1.0f : (now - _lastPidTime) / 1000.0f;
                    _lastPidTime = now;
                    if (permitted) {
                        _pid.update(_targetTempC, controlTemp, dt);
                    } else {
                        _pid.reset(0.0f);   // Don't wind up while heat is inhibited
                    }
                }
                _demand = timeProportion(_pid.output(), now);
                break;

            default:
                _demand = false;
                break;
        }

        applyRelayState(_demand && permitted);
    }

private:
    // Slow PWM over HEATER_CYCLE_WINDOW_MS. Slices shorter than the minimum
    // pulse are rounded away so the relay never chatters.
    bool timeProportion(float dutyPct, uint32_t now) {
        while (now - _windowStartTime >= HEATER_CYCLE_WINDOW_MS) _windowStartTime += HEATER_CYCLE_WINDOW_MS;
        uint32_t onMs = (uint32_t)(HEATER_CYCLE_WINDOW_MS * dutyPct / 100.0f);
        if (onMs < HEATER_MIN_PULSE_MS) onMs = 0;
        if (HEATER_CYCLE_WINDOW_MS - onMs < HEATER_MIN_PULSE_MS) onMs = HEATER_CYCLE_WINDOW_MS;
        return (now - _windowStartTime) < onMs;
    }

    void applyRelayState(bool energize) {
        if (energize != _relayState) _lastSwitchTime = millis();
        _relayState = energize;
        if (_relayState && !_outputsInhibited) {
            digitalWrite(PIN_RELAY_HEATER, RELAY_ACTIVE_LEVEL);
        } else {
            digitalWrite(PIN_RELAY_HEATER, (RELAY_ACTIVE_LEVEL == HIGH) ? LOW : HIGH);
        }
    }

    HeaterMode _mode;
    bool _manualState;
    float _targetTempC;
    float _hysteresisC;
    float _dutyPercent;
    uint8_t _controlProbe;
    bool _demand;
    bool _relayState;
    uint32_t _lastSwitchTime;
    uint32_t _windowStartTime;
    uint32_t _lastPidTime;
    bool _outputsInhibited = false;
    Pid _pid;
};

inline HeaterController Heater;
