#pragma once

#include <Arduino.h>
#include <math.h>
#include "../config.h"

struct ThermodynamicData {
    float thIn;           // °C
    float thOut;          // °C
    float tcIn;           // °C
    float tcOut;          // °C
    float deltaTh;        // °C (Th,in - Th,out)
    float deltaTc;        // °C (Tc,out - Tc,in)
    float flowRateLPM;        // L/min (hot loop)
    float flowRateColdLPM;    // L/min (cold loop)
    float massFlowKgS;        // kg/s (hot loop)
    float qHotWatts;      // W
    float qHotKW;         // kW
    float qColdWatts;     // W
    float qColdKW;        // kW
    float lmtd;           // °C
    float uaWPerK;        // W/K  (Q_hot / LMTD) - falls as the exchanger fouls
    float effectiveness;  // % (0 - 100)
    float heatBalance;    // % (Qcold / Qhot * 100)
    bool valid;           // all four probes valid
};

class Thermodynamics {
public:
    Thermodynamics() {
        memset(&_data, 0, sizeof(_data));
    }

    // coldFlowLPM < 0 means "same flow as hot side" (single flow meter rigs).
    void compute(float thIn, float thOut, float tcIn, float tcOut, float flowLPM, float coldFlowLPM = -1.0f) {
        _data.thIn = thIn;
        _data.thOut = thOut;
        _data.tcIn = tcIn;
        _data.tcOut = tcOut;
        _data.flowRateLPM = flowLPM;
        _data.flowRateColdLPM = (coldFlowLPM >= 0.0f) ? coldFlowLPM : flowLPM;
        _data.massFlowKgS = (flowLPM / 60.0f) * WATER_DENSITY;

        auto ok = [](float t) { return t > SENSOR_DISCONNECT_VAL + 1.0f; };
        _data.valid = ok(thIn) && ok(thOut) && ok(tcIn) && ok(tcOut);
        if (!_data.valid) {
            _data.deltaTh = _data.deltaTc = 0.0f;
            _data.qHotWatts = _data.qHotKW = _data.qColdWatts = _data.qColdKW = 0.0f;
            _data.lmtd = _data.uaWPerK = _data.effectiveness = _data.heatBalance = 0.0f;
            return;
        }

        _data.deltaTh = thIn - thOut;
        _data.deltaTc = tcOut - tcIn;

        // Q_hot = m_dot * Cp * dTh
        _data.qHotWatts = (_data.massFlowKgS > 0.0001f && _data.deltaTh > 0.0f)
                              ? _data.massFlowKgS * WATER_SPECIFIC_HEAT * _data.deltaTh : 0.0f;
        _data.qHotKW = _data.qHotWatts / 1000.0f;

        // Q_cold = m_dot_c * Cp * dTc
        float coldMassFlow = (((coldFlowLPM >= 0.0f) ? coldFlowLPM : flowLPM) / 60.0f) * WATER_DENSITY;
        _data.qColdWatts = (coldMassFlow > 0.0001f && _data.deltaTc > 0.0f)
                               ? coldMassFlow * WATER_SPECIFIC_HEAT * _data.deltaTc : 0.0f;
        _data.qColdKW = _data.qColdWatts / 1000.0f;

        if (_data.qHotWatts > 10.0f) {
            _data.heatBalance = fminf((_data.qColdWatts / _data.qHotWatts) * 100.0f, 150.0f);
        } else {
            _data.heatBalance = 0.0f;
        }

        // Counter-flow LMTD, arithmetic mean when dT1 ~ dT2
        float dt1 = thIn - tcOut;
        float dt2 = thOut - tcIn;
        if (dt1 > 0.05f && dt2 > 0.05f) {
            float diff = dt1 - dt2;
            _data.lmtd = (fabsf(diff) < 0.1f) ? (dt1 + dt2) / 2.0f : diff / logf(dt1 / dt2);
        } else {
            _data.lmtd = 0.0f;
        }
        _data.uaWPerK = (_data.lmtd > 0.2f) ? _data.qHotWatts / _data.lmtd : 0.0f;

        // Effectiveness = Q / (C_min * (Th,in - Tc,in)). The two loops have
        // independent flows, so C_min is the smaller of the two capacity rates.
        float cHot = _data.massFlowKgS * WATER_SPECIFIC_HEAT;
        float cCold = coldMassFlow * WATER_SPECIFIC_HEAT;
        float cMin = fminf(cHot, cCold);
        float maxDeltaT = thIn - tcIn;
        if (cMin > 0.0001f && maxDeltaT > 0.5f) {
            _data.effectiveness = constrain(_data.qHotWatts / (cMin * maxDeltaT) * 100.0f, 0.0f, 100.0f);
        } else {
            _data.effectiveness = 0.0f;
        }
    }

    const ThermodynamicData& getData() const {
        return _data;
    }

private:
    ThermodynamicData _data;
};

inline Thermodynamics Thermo;
