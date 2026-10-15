#pragma once

#include <Arduino.h>
#include "config.h"

class FlowSensor {
public:
    FlowSensor();

    void init();
    void update(); // Called regularly (e.g. every 500ms or 1s)

    float getFlowRateLPM() const;
    float getTotalLiters() const;
    uint32_t getTotalPulses() const;
    void resetTotalVolume();

    void setKFactor(float k);
    float getKFactor() const;

    // Interrupt handler
    void IRAM_ATTR handlePulse();

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

extern FlowSensor Flow;
