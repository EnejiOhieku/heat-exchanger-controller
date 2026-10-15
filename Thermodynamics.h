#pragma once

#include <Arduino.h>
#include <math.h>
#include "config.h"

struct ThermodynamicData {
    float thIn;           // °C
    float thOut;          // °C
    float tcIn;           // °C
    float tcOut;          // °C
    float deltaTh;        // °C (Th,in - Th,out)
    float deltaTc;        // °C (Tc,out - Tc,in)
    float flowRateLPM;    // L/min
    float massFlowKgS;    // kg/s
    float qHotWatts;      // Watts
    float qHotKW;         // kW
    float qColdWatts;     // Watts
    float qColdKW;        // kW
    float lmtd;           // °C
    float effectiveness;  // % (0 - 100)
    float heatBalance;    // % (Qcold / Qhot * 100)
};

class Thermodynamics {
public:
    Thermodynamics();

    void compute(float thIn, float thOut, float tcIn, float tcOut, float flowLPM, float coldFlowLPM = -1.0f);
    const ThermodynamicData& getData() const;

private:
    ThermodynamicData _data;
};

extern Thermodynamics Thermo;
