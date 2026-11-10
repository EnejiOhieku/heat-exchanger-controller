#pragma once

#include <Arduino.h>

// Discrete PID with the usual practical fixes for slow thermal/flow plants:
// - derivative on measurement (no kick on setpoint changes), low-pass filtered
// - conditional integration anti-windup (integrator frozen while saturated
//   in the direction that would deepen the saturation)
// - bumpless start: reset() preloads the integrator to a given output
class Pid {
public:
    Pid(float kp, float ki, float kd, float outMin, float outMax)
        : _kp(kp), _ki(ki), _kd(kd),
          _outMin(outMin), _outMax(outMax),
          _integral(0.0f), _prevMeas(0.0f), _dFiltered(0.0f),
          _output(0.0f), _primed(false) {}

    void setGains(float kp, float ki, float kd) {
        // Rescale the integrator so the output does not jump when Ki changes.
        if (ki > 1e-6f && _ki > 1e-6f) _integral *= _ki / ki;
        _kp = kp; _ki = ki; _kd = kd;
    }
    float kp() const { return _kp; }
    float ki() const { return _ki; }
    float kd() const { return _kd; }

    void reset(float initialOutput = 0.0f) {
        _integral = (_ki > 1e-6f) ? constrain(initialOutput, _outMin, _outMax) / _ki : 0.0f;
        _dFiltered = 0.0f;
        _primed = false;
        _output = initialOutput;
    }

    // dtS in seconds. Returns the clamped output.
    float update(float setpoint, float measurement, float dtS) {
        if (dtS <= 0.0f) return _output;
        if (!_primed) {
            _prevMeas = measurement;
            _primed = true;
        }

        float error = setpoint - measurement;

        float dMeas = (measurement - _prevMeas) / dtS;
        _prevMeas = measurement;
        _dFiltered += 0.2f * (dMeas - _dFiltered);   // ~5-sample low-pass

        float p = _kp * error;
        float d = -_kd * _dFiltered;
        float unsat = p + _ki * _integral + d;

        bool pushingHigh = (unsat >= _outMax) && (error > 0.0f);
        bool pushingLow = (unsat <= _outMin) && (error < 0.0f);
        if (!pushingHigh && !pushingLow) {
            _integral += error * dtS;
        }

        _output = constrain(p + _ki * _integral + d, _outMin, _outMax);
        return _output;
    }

    float output() const { return _output; }

private:
    float _kp, _ki, _kd;
    float _outMin, _outMax;
    float _integral;
    float _prevMeas;
    float _dFiltered;
    float _output;
    bool _primed;
};
