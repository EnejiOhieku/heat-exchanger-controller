#include "FlowSensor.h"

FlowSensor Flow;

static void IRAM_ATTR flowSensorISR() {
    Flow.handlePulse();
}

FlowSensor::FlowSensor()
    : _pulseCount(0),
      _lastPulseTimeMicros(0),
      _lastCalcTimeMillis(0),
      _lastCalcPulseCount(0),
      _flowRateLPM(0.0f),
      _filteredFlowRateLPM(0.0f),
      _totalLiters(0.0f),
      _kFactor(DEFAULT_FLOW_K_FACTOR) {}

void FlowSensor::init() {
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

void IRAM_ATTR FlowSensor::handlePulse() {
    uint32_t now = micros();
    // 500 microsecond software debounce (corresponds to max 2000 Hz, far exceeding standard water meter 100-300 Hz)
    if (now - _lastPulseTimeMicros >= 500) {
        _pulseCount++;
        _lastPulseTimeMicros = now;
    }
}

void FlowSensor::update() {
    uint32_t now = millis();
    uint32_t dt = now - _lastCalcTimeMillis;

    // Run calculation if at least 250ms elapsed
    if (dt >= 250) {
        noInterrupts();
        uint32_t currentPulses = _pulseCount;
        interrupts();

        uint32_t deltaPulses = currentPulses - _lastCalcPulseCount;
        _lastCalcPulseCount = currentPulses;
        _lastCalcTimeMillis = now;

        // Frequency in Hz = pulses / dt(seconds)
        float freqHz = (float)deltaPulses / ((float)dt / 1000.0f);

        // Flow rate (L/min) = freqHz / K_Factor
        if (_kFactor > 0.001f) {
            _flowRateLPM = freqHz / _kFactor;
        } else {
            _flowRateLPM = 0.0f;
        }

        // Low-pass exponential smoothing filter: alpha = 0.3
        _filteredFlowRateLPM = (0.3f * _flowRateLPM) + (0.7f * _filteredFlowRateLPM);
        if (_filteredFlowRateLPM < 0.05f) _filteredFlowRateLPM = 0.0f;

        // Total Liters accumulated: deltaPulses / (K_factor * 60)
        if (_kFactor > 0.001f) {
            _totalLiters += (float)deltaPulses / (_kFactor * 60.0f);
        }
    }
}

float FlowSensor::getFlowRateLPM() const {
    return _filteredFlowRateLPM;
}

float FlowSensor::getTotalLiters() const {
    return _totalLiters;
}

uint32_t FlowSensor::getTotalPulses() const {
    return _pulseCount;
}

void FlowSensor::resetTotalVolume() {
    _totalLiters = 0.0f;
}

void FlowSensor::setKFactor(float k) {
    if (k > 0.001f) {
        _kFactor = k;
    }
}

float FlowSensor::getKFactor() const {
    return _kFactor;
}
