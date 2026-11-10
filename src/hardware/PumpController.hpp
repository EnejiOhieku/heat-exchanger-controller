#pragma once

#include <Arduino.h>
#include <esp_arduino_version.h>
#include "../config.h"
#include "../core/Pid.hpp"

enum PumpMode : uint8_t {
    PUMP_MODE_MANUAL = 0,   // Operator sets speed %
    PUMP_MODE_FLOW = 1      // PI loop holds a flow setpoint (L/min)
};

// One-way pump on a single PWM pin (MOSFET / one-direction motor module).
// No direction pin and no H-bridge decay handling: the output is just a duty
// cycle. Two instances exist, Pump (hot loop) and ColdPump (cold loop), each
// with its own soft ramp, dead-band compensation, kick-start and flow PI loop.
class PumpController {
public:
    PumpController(uint8_t pin, uint8_t ledcChannel, const char* name)
        : _pin(pin),
          _ledcChannel(ledcChannel),
          _name(name),
          _mode(PUMP_MODE_MANUAL),
          _targetSpeedPct(0.0f),
          _currentSpeedPct(0.0f),
          _flowSetpointLPM(0.0f),
          _minDutyPct(PUMP_MIN_DUTY_PCT),
          _kickUntil(0),
          _lastRampTime(0),
          _lastFlowLoopTime(0),
          _lastDuty(UINT32_MAX),
          _flowPid(DEFAULT_FLOW_KP, DEFAULT_FLOW_KI, 0.0f, 0.0f, 100.0f) {}

    void init() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
        ledcAttach(_pin, PUMP_PWM_FREQ, PUMP_PWM_RES);
#else
        ledcSetup(_ledcChannel, PUMP_PWM_FREQ, PUMP_PWM_RES);
        ledcAttachPin(_pin, _ledcChannel);
#endif
        writePwm(0);
        _lastRampTime = millis();
        Serial.printf("[Pump] %s: one-way PWM on GPIO %d, %d Hz, %d-bit, min duty %u%%\n",
                      _name, _pin, PUMP_PWM_FREQ, PUMP_PWM_RES, _minDutyPct);
    }

    // ---- Commands -------------------------------------------------------
    void setSpeedPercent(float speedPct) {
        _mode = PUMP_MODE_MANUAL;
        _targetSpeedPct = constrain(speedPct, 0.0f, 100.0f);
    }

    void setFlowSetpoint(float lpm) {
        lpm = constrain(lpm, 0.0f, 30.0f);
        if (_mode != PUMP_MODE_FLOW) _flowPid.reset(_currentSpeedPct);   // bumpless transfer
        _mode = PUMP_MODE_FLOW;
        _flowSetpointLPM = lpm;
    }

    void stop() {
        _mode = PUMP_MODE_MANUAL;
        _targetSpeedPct = 0.0f;
        _currentSpeedPct = 0.0f;
        _kickUntil = 0;
        _driving = false;
        writePwm(0);
    }

    // Simulation: the control logic keeps running but the pin stays LOW.
    void setOutputsInhibited(bool inhibit) {
        _outputsInhibited = inhibit;
        if (inhibit) writePwm(0);
    }

    void setMinDutyPercent(uint8_t pct) { _minDutyPct = constrain(pct, 0, 80); }
    uint8_t getMinDutyPercent() const { return _minDutyPct; }

    void setFlowGains(float kp, float ki) { _flowPid.setGains(kp, ki, 0.0f); }
    float getFlowKp() const { return _flowPid.kp(); }
    float getFlowKi() const { return _flowPid.ki(); }

    // ---- State ----------------------------------------------------------
    PumpMode getMode() const { return _mode; }
    float getFlowSetpoint() const { return _flowSetpointLPM; }
    uint8_t getSpeedPercent() const { return (uint8_t)(_currentSpeedPct + 0.5f); }
    uint8_t getTargetSpeedPercent() const { return (uint8_t)(_targetSpeedPct + 0.5f); }
    bool isRunning() const { return _currentSpeedPct > 0.5f; }
    uint32_t getDutyRaw() const { return pctToDuty(_currentSpeedPct); }
    uint8_t getPin() const { return _pin; }
    const char* getName() const { return _name; }

    // ---- Periodic -------------------------------------------------------
    // Closed-loop flow control; call at a fixed rate (10 Hz) with filtered flow.
    void updateFlowLoop(float flowLPM) {
        uint32_t now = millis();
        float dt = (now - _lastFlowLoopTime) / 1000.0f;
        _lastFlowLoopTime = now;
        if (_mode != PUMP_MODE_FLOW || dt > 1.0f) return;

        if (_flowSetpointLPM < 0.05f) {
            _targetSpeedPct = 0.0f;
            _flowPid.reset(0.0f);
        } else {
            _targetSpeedPct = _flowPid.update(_flowSetpointLPM, flowLPM, dt);
        }
    }

    // Soft ramp + output stage; call every loop iteration.
    void update() {
        uint32_t now = millis();
        uint32_t dtMs = now - _lastRampTime;
        if (dtMs < 10) return;
        _lastRampTime = now;

        float maxStep = PUMP_RAMP_PCT_PER_S * dtMs / 1000.0f;
        float diff = _targetSpeedPct - _currentSpeedPct;
        if (fabsf(diff) <= maxStep) {
            _currentSpeedPct = _targetSpeedPct;
        } else {
            _currentSpeedPct += (diff > 0.0f) ? maxStep : -maxStep;
        }

        uint32_t duty = pctToDuty(_currentSpeedPct);
        if (duty == 0) {
            _kickUntil = 0;
            _driving = false;
            writePwm(0);
            return;
        }
        // Starting from standstill: a brief full-duty pulse breaks static friction.
        if (!_driving && PUMP_KICKSTART_MS > 0) _kickUntil = now + PUMP_KICKSTART_MS;
        _driving = true;
        if (_kickUntil != 0 && (int32_t)(now - _kickUntil) < 0) {
            duty = maxDuty();
        } else {
            _kickUntil = 0;
        }
        writePwm(duty);
    }

private:
    static constexpr uint32_t maxDuty() { return (1UL << PUMP_PWM_RES) - 1; }

    // 0% is off; 1..100% maps onto [minDuty, 100%] so the bottom of the slider
    // is not a dead band where the motor hums without turning.
    uint32_t pctToDuty(float pct) const {
        if (pct < 0.5f) return 0;
        float eff = _minDutyPct + (100.0f - _minDutyPct) * (pct / 100.0f);
        return (uint32_t)(eff / 100.0f * maxDuty() + 0.5f);
    }

    void writePwm(uint32_t duty) {
        if (_outputsInhibited) duty = 0;
        if (duty == _lastDuty) return;
        _lastDuty = duty;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
        ledcWrite(_pin, duty);
#else
        ledcWrite(_ledcChannel, duty);
#endif
    }

    uint8_t _pin;
    uint8_t _ledcChannel;
    const char* _name;
    PumpMode _mode;
    float _targetSpeedPct;
    float _currentSpeedPct;
    float _flowSetpointLPM;
    uint8_t _minDutyPct;
    uint32_t _kickUntil;
    uint32_t _lastRampTime;
    uint32_t _lastFlowLoopTime;
    uint32_t _lastDuty;
    bool _outputsInhibited = false;
    bool _driving = false;
    Pid _flowPid;
};

inline PumpController Pump(PIN_PUMP_HOT_PWM, PUMP_HOT_PWM_CHANNEL, "hot");
inline PumpController ColdPump(PIN_PUMP_COLD_PWM, PUMP_COLD_PWM_CHANNEL, "cold");