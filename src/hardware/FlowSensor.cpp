#include "FlowSensor.hpp"

namespace flow_isr {

Channel hot;
Channel cold;

void IRAM_ATTR onPulseHot() {
    uint32_t now = micros();
    portENTER_CRITICAL_ISR(&hot.mux);
    if (now - hot.lastPulseMicros >= FLOW_DEBOUNCE_US) {
        hot.pulseCount = hot.pulseCount + 1;
        hot.lastPulseMicros = now;
    }
    portEXIT_CRITICAL_ISR(&hot.mux);
}

void IRAM_ATTR onPulseCold() {
    uint32_t now = micros();
    portENTER_CRITICAL_ISR(&cold.mux);
    if (now - cold.lastPulseMicros >= FLOW_DEBOUNCE_US) {
        cold.pulseCount = cold.pulseCount + 1;
        cold.lastPulseMicros = now;
    }
    portEXIT_CRITICAL_ISR(&cold.mux);
}

}  // namespace flow_isr