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
    Thermodynamics() {
        memset(&_data, 0, sizeof(_data));
    }

    void compute(float thIn, float thOut, float tcIn, float tcOut, float flowLPM, float coldFlowLPM = -1.0f) {
        _data.thIn = thIn;
        _data.thOut = thOut;
        _data.tcIn = tcIn;
        _data.tcOut = tcOut;
        _data.flowRateLPM = flowLPM;

        // 1. Temperature differences
        _data.deltaTh = thIn - thOut;
        _data.deltaTc = tcOut - tcIn;

        // 2. Mass flow rate on hot side: kg/s = (L/min / 60) * rho
        _data.massFlowKgS = (flowLPM / 60.0f) * WATER_DENSITY;

        // 3. Heat Transfer Rate Hot Side (Q_hot = m_dot * Cp * deltaTh)
        if (_data.massFlowKgS > 0.0001f && _data.deltaTh > 0.0f) {
            _data.qHotWatts = _data.massFlowKgS * WATER_SPECIFIC_HEAT * _data.deltaTh;
        } else {
            _data.qHotWatts = 0.0f;
        }
        _data.qHotKW = _data.qHotWatts / 1000.0f;

        // 4. Heat Absorption Cold Side (Q_cold = m_dot_c * Cp * deltaTc)
        float actualColdFlow = (coldFlowLPM >= 0.0f) ? coldFlowLPM : flowLPM;
        float coldMassFlow = (actualColdFlow / 60.0f) * WATER_DENSITY;

        if (coldMassFlow > 0.0001f && _data.deltaTc > 0.0f) {
            _data.qColdWatts = coldMassFlow * WATER_SPECIFIC_HEAT * _data.deltaTc;
        } else {
            _data.qColdWatts = 0.0f;
        }
        _data.qColdKW = _data.qColdWatts / 1000.0f;

        // 5. Heat Balance ratio
        if (_data.qHotWatts > 10.0f) {
            _data.heatBalance = (_data.qColdWatts / _data.qHotWatts) * 100.0f;
            if (_data.heatBalance > 150.0f) _data.heatBalance = 150.0f;
        } else {
            _data.heatBalance = 0.0f;
        }

        // 6. Logarithmic Mean Temperature Difference (LMTD) for Counter-flow
        float dt1 = thIn - tcOut;
        float dt2 = thOut - tcIn;

        if (dt1 > 0.05f && dt2 > 0.05f) {
            float diff = dt1 - dt2;
            if (fabsf(diff) < 0.1f) {
                _data.lmtd = (dt1 + dt2) / 2.0f;
            } else {
                _data.lmtd = diff / logf(dt1 / dt2);
            }
        } else {
            _data.lmtd = 0.0f;
        }

        // 7. Heat Exchanger Effectiveness (ε)
        float maxDeltaT = thIn - tcIn;
        if (maxDeltaT > 0.5f && _data.deltaTh >= 0.0f) {
            float eff = (_data.deltaTh / maxDeltaT) * 100.0f;
            if (eff < 0.0f) eff = 0.0f;
            if (eff > 100.0f) eff = 100.0f;
            _data.effectiveness = eff;
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
