#pragma once

#include <Arduino.h>
#include "../config.h"
#include "../hardware/ThermalSensors.hpp"
#include "../hardware/FlowSensor.hpp"
#include "../sim/PlantSimulator.hpp"

// Single source of process values for control, safety, logging and the UI.
// Routes to the simulator when it is active, otherwise to real hardware.
namespace PV {
inline bool simulated() { return Sim.isActive(); }

inline float temp(ProbeIndex p) { return simulated() ? Sim.getTempC(p) : Sensors.getTempC(p); }
inline bool valid(ProbeIndex p) { return simulated() ? Sim.isProbeValid(p) : Sensors.isValid(p); }
inline uint8_t faultMask() { return simulated() ? Sim.faultMask() : Sensors.faultMask(); }

inline float maxValidTemp() {
    float m = -999.0f;
    for (uint8_t i = 0; i < NUM_PROBES; i++) {
        if (valid((ProbeIndex)i)) m = fmaxf(m, temp((ProbeIndex)i));
    }
    return m;
}

inline float flowLPM() { return simulated() ? Sim.getFlowLPM() : FlowHot.getFlowRateLPM(); }
inline float flowColdLPM() { return simulated() ? Sim.getFlowColdLPM() : FlowCold.getFlowRateLPM(); }
inline float totalLiters() { return simulated() ? Sim.getTotalLiters() : FlowHot.getTotalLiters(); }
inline float totalLitersCold() { return simulated() ? Sim.getTotalLitersCold() : FlowCold.getTotalLiters(); }
}  // namespace PV