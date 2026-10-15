#include "Thermodynamics.h"

Thermodynamics Thermo;

Thermodynamics::Thermodynamics() {
    memset(&_data, 0, sizeof(_data));
}

void Thermodynamics::compute(float thIn, float thOut, float tcIn, float tcOut, float flowLPM, float coldFlowLPM) {
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
    // If cold flow is not measured separately, default to equal mass flow
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
        if (_data.heatBalance > 150.0f) _data.heatBalance = 150.0f; // clamp outliers
    } else {
        _data.heatBalance = 0.0f;
    }

    // 6. Logarithmic Mean Temperature Difference (LMTD) for Counter-flow
    // ΔT1 = Th,in - Tc,out
    // ΔT2 = Th,out - Tc,in
    float dt1 = thIn - tcOut;
    float dt2 = thOut - tcIn;

    if (dt1 > 0.05f && dt2 > 0.05f) {
        float diff = dt1 - dt2;
        if (fabsf(diff) < 0.1f) {
            // Equal temperature differences: limit ln(x)/diff -> arithmetic mean
            _data.lmtd = (dt1 + dt2) / 2.0f;
        } else {
            _data.lmtd = diff / logf(dt1 / dt2);
        }
    } else {
        _data.lmtd = 0.0f;
    }

    // 7. Heat Exchanger Effectiveness (ε)
    // ε = (Th,in - Th,out) / (Th,in - Tc,in)
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

const ThermodynamicData& Thermodynamics::getData() const {
    return _data;
}
