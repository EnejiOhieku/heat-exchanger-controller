#pragma once

#include <Arduino.h>
#include "../config.h"

class FlowSensor {
public:
    FlowSensor()
        : _pulseCount(0),
          _lastPulseTimeMicros(0),
          _lastCalcTimeMillis(0),
          _lastCalcPulseCount(0),
          _flowRateLPM(0.0f),
          _filteredFlowRateLPM(0.0f),
          _totalLiters(0.0f),
          _kFactor(DEFAULT_FLOW_K_FACTOR) {}

    void init();

    void IRAM_ATTR handlePulse() {
        uint32_t now = micros();
        if (now - _lastPulseTimeMicros >= 500) {
            _pulseCount++;
            _lastPulseTimeMicros = now;
        }
    }

    void update() {
        uint32_t now = millis();
        uint32_t dt = now - _lastCalcTimeMillis;

        if (dt >= 250) {
            noInterrupts();
            uint32_t currentPulses = _pulseCount;
            interrupts();

            uint32_t deltaPulses = currentPulses - _lastCalcPulseCount;
            _lastCalcPulseCount = currentPulses;
            _lastCalcTimeMillis = now;

            float freqHz = (float)deltaPulses / ((float)dt / 1000.0f);

            if (_kFactor > 0.001f) {
                _flowRateLPM = freqHz / _kFactor;
            } else {
                _flowRateLPM = 0.0f;
            }

            _filteredFlowRateLPM = (0.3f * _flowRateLPM) + (0.7f * _filteredFlowRateLPM);
            if (_filteredFlowRateLPM < 0.05f) _filteredFlowRateLPM = 0.0f;

            if (_kFactor > 0.001f) {
                _totalLiters += (float)deltaPulses / (_kFactor * 60.0f);
            }
        }
    }

    float getFlowRateLPM() const { return _filteredFlowRateLPM; }
    float getTotalLiters() const { return _totalLiters; }
    uint32_t getTotalPulses() const { return _pulseCount; }
    void resetTotalVolume() { _totalLiters = 0.0f; }

    void setKFactor(float k) {
        if (k > 0.001f) _kFactor = k;
    }
    float getKFactor() const { return _kFactor; }

private:
    volatile uint32_t _pulseCount;
    volatile uint32_t _lastPulseTimeMicros;
    uint32_t _lastCalcTimeMillis;
    uint32_t _lastCalcPulseCount;

    float _flowRateLPM;
    float _filteredFlowRateLPM;
    float _totalLiters;
    float _kFactor;
};

inline FlowSensor Flow;

inline void IRAM_ATTR flowSensorISR() {
    Flow.handlePulse();
}

inline void FlowSensor::init() {
    pinMode(PIN_FLOW_SENSOR, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_FLOW_SENSOR), flowSensorISR, RISING);
    _lastCalcTimeMillis = millis();
    _lastCalcPulseCount = 0;
    _pulseCount = 0;
    _totalLiters = 0.0f;
    _flowRateLPM = 0.0f;
    _filteredFlowRateLPM = 0.0f;
    Serial.printf("[Flow] Initialized on GPIO %d (K-Factor: %.2f Hz/LPM)\n", PIN_FLOW_SENSOR, _kFactor);
}
