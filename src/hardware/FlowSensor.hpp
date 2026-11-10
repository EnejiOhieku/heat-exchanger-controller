#pragma once

#include <Arduino.h>
#include "../config.h"

// ISR state and handlers are defined in FlowSensor.cpp so the handlers can live
// in IRAM. A flash-resident ISR crashes ("Cache disabled but cached memory
// region accessed") whenever NVS/Preferences writes disable the flash cache.
// Each meter owns its own channel state and its own non-inline handler, because
// inline IRAM functions in headers trigger Xtensa "l32r: literal placed after
// use" errors.
namespace flow_isr {

struct Channel {
    volatile uint32_t pulseCount = 0;
    volatile uint32_t lastPulseMicros = 0;
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
};

extern Channel hot;
extern Channel cold;

void onPulseHot();    // IRAM_ATTR on the definition
void onPulseCold();   // IRAM_ATTR on the definition

}  // namespace flow_isr

// One pulse flow meter. Two instances exist: FlowHot (heater loop) and
// FlowCold (cooling loop). Frequency is measured edge-to-edge rather than
// pulses / window, which is far less noisy at low flow where only one or two
// pulses land per 250 ms window.
class FlowSensor {
public:
    FlowSensor(flow_isr::Channel& channel, void (*isr)(), uint8_t pin, float defaultKFactor)
        : _channel(channel),
          _isr(isr),
          _pin(pin),
          _lastCalcTimeMillis(0),
          _prevPulseCount(0),
          _prevEdgeMicros(0),
          _haveRefEdge(false),
          _freqHz(0.0f),
          _filteredFlowRateLPM(0.0f),
          _totalLiters(0.0f),
          _kFactor(defaultKFactor) {}

    void init() {
        pinMode(_pin, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(_pin), _isr, RISING);
        _lastCalcTimeMillis = millis();
        Serial.printf("[Flow] Meter on GPIO %d (K-Factor: %.2f Hz/LPM)\n", _pin, _kFactor);
    }

    void update() {
        uint32_t nowMs = millis();
        if (nowMs - _lastCalcTimeMillis < 250) return;
        _lastCalcTimeMillis = nowMs;

        portENTER_CRITICAL(&_channel.mux);
        uint32_t count = _channel.pulseCount;
        uint32_t edgeUs = _channel.lastPulseMicros;
        portEXIT_CRITICAL(&_channel.mux);

        uint32_t deltaPulses = count - _prevPulseCount;
        uint32_t sinceEdgeUs = micros() - edgeUs;

        if (deltaPulses > 0) {
            uint32_t spanUs = edgeUs - _prevEdgeMicros;
            // A stale reference edge (flow just restarted) would give a bogus low reading.
            bool freshRef = _haveRefEdge && spanUs > 0 && spanUs <= (uint32_t)FLOW_ZERO_TIMEOUT_MS * 1000UL;
            if (freshRef) _freqHz = (float)deltaPulses * 1e6f / (float)spanUs;
            _haveRefEdge = true;
        } else {
            if (sinceEdgeUs > (uint32_t)FLOW_ZERO_TIMEOUT_MS * 1000UL) {
                _freqHz = 0.0f;
            } else if (sinceEdgeUs > 0) {
                // No new edge: the true frequency is at most 1 / time-since-last-edge.
                float bound = 1e6f / (float)sinceEdgeUs;
                if (bound < _freqHz) _freqHz = bound;
            }
        }
        if (deltaPulses > 0) {
            _prevEdgeMicros = edgeUs;
            _prevPulseCount = count;
        }

        float lpm = (_kFactor > 0.001f) ? _freqHz / _kFactor : 0.0f;
        _filteredFlowRateLPM = (_freqHz == 0.0f) ? 0.0f : 0.4f * lpm + 0.6f * _filteredFlowRateLPM;

        if (_kFactor > 0.001f) _totalLiters += (float)deltaPulses / (_kFactor * 60.0f);
    }

    float getFlowRateLPM() const { return _filteredFlowRateLPM; }
    float getFrequencyHz() const { return _freqHz; }
    float getTotalLiters() const { return _totalLiters; }
    uint32_t getTotalPulses() const {
        portENTER_CRITICAL(&_channel.mux);
        uint32_t n = _channel.pulseCount;
        portEXIT_CRITICAL(&_channel.mux);
        return n;
    }
    void resetTotalVolume() { _totalLiters = 0.0f; }
    uint8_t getPin() const { return _pin; }

    void setKFactor(float k) {
        if (k > 0.001f && k < 1000.0f) _kFactor = k;
    }
    float getKFactor() const { return _kFactor; }

private:
    flow_isr::Channel& _channel;
    void (*_isr)();
    uint8_t _pin;
    uint32_t _lastCalcTimeMillis;
    uint32_t _prevPulseCount;
    uint32_t _prevEdgeMicros;
    bool _haveRefEdge;
    float _freqHz;
    float _filteredFlowRateLPM;
    float _totalLiters;
    float _kFactor;
};

inline FlowSensor FlowHot(flow_isr::hot, flow_isr::onPulseHot, PIN_FLOW_HOT, DEFAULT_FLOW_K_FACTOR);
inline FlowSensor FlowCold(flow_isr::cold, flow_isr::onPulseCold, PIN_FLOW_COLD, DEFAULT_FLOW_K_FACTOR_COLD);