#pragma once

#include <Arduino.h>
#include <math.h>
#include "../config.h"

// Lumped-parameter model of the rig, driven by the real controller outputs.
//
//   heater --> [hot tank, M_tank] --hot pump--> HX hot in --> HX hot out --> tank
//   cold supply (T_supply) --cold pump--> HX cold in --> HX cold out --> drain
//
// Each loop has its own pump and flow meter, so the two sides have independent
// capacity rates. The exchanger is modelled as a counter-flow epsilon-NTU unit:
//   Cr  = C_min / C_max,  NTU = UA / C_min
//   eps = (1 - exp(-NTU(1-Cr))) / (1 - Cr*exp(-NTU(1-Cr)))   (Cr < 1)
//         NTU / (1 + NTU)                                      (Cr = 1)
//   Q   = eps * C_min * (Th_in - Tc_in)
// Tank: M*Cp*dT/dt = P_heater - Q - UA_loss*(T - T_amb)
// Probes: first-order lag, small noise, DS18B20 12-bit quantisation.
#define SIM_HEATER_POWER_W     2000.0f
#define SIM_TANK_MASS_KG       3.0f
#define SIM_HX_UA_W_PER_K      180.0f
#define SIM_TANK_LOSS_W_PER_K  4.0f
#define SIM_AMBIENT_C          25.0f
#define SIM_COLD_SUPPLY_C      20.0f
#define SIM_MAX_FLOW_LPM       8.0f
#define SIM_FLOW_TAU_S         1.5f
#define SIM_PROBE_TAU_S        3.0f

enum SimMode : uint8_t {
    SIM_MODE_OFF = 0,    // Real hardware only
    SIM_MODE_ON = 1,     // Always simulate
    SIM_MODE_AUTO = 2    // Simulate if probes are missing/faulty at boot or at runtime
};

enum SimFault : uint8_t {
    SIM_FAULT_NONE = 0,
    SIM_FAULT_FLOW_BLOCKED = 1 << 0,   // hot-loop flow blocked
    SIM_FAULT_PROBE_TH_IN = 1 << 1,
    SIM_FAULT_STUCK_HEATER = 1 << 2,   // relay welded shut
    SIM_FAULT_COLD_FLOW_BLOCKED = 1 << 3
};

class PlantSimulator {
public:
    PlantSimulator()
        : _mode(SIM_MODE_AUTO), _active(false), _faults(SIM_FAULT_NONE),
          _tankC(SIM_AMBIENT_C), _flowLPM(0.0f), _flowColdLPM(0.0f),
          _totalLiters(0.0f), _totalLitersCold(0.0f),
          _lastUpdate(0), _heaterPowerW(0.0f) {
        for (int i = 0; i < 4; i++) _probe[i] = SIM_AMBIENT_C;
        _probe[2] = _probe[3] = SIM_COLD_SUPPLY_C;
    }

    void setMode(uint8_t m) { if (m <= SIM_MODE_AUTO) _mode = (SimMode)m; }
    SimMode getMode() const { return _mode; }

    // Decide whether to run on the model.
    //   realProbes    = DS18B20s found
    //   hardwareFault = any probe missing/invalid at runtime
    // AUTO simulates when no probe answers or a hardware fault is present, and
    // automatically returns to real hardware once the fault clears.
    void resolve(uint8_t realProbes, bool hardwareFault = false) {
        bool want;
        if (_mode == SIM_MODE_ON) {
            want = true;
        } else if (_mode == SIM_MODE_OFF) {
            want = false;
        } else {
            want = (realProbes == 0) || hardwareFault;
        }
        if (want && !_active) reset();
        _active = want;
        Serial.printf("[Sim] Mode %s -> %s\n",
                      _mode == SIM_MODE_ON ? "ON" : (_mode == SIM_MODE_AUTO ? "AUTO" : "OFF"),
                      _active ? "SIMULATING (physical outputs inhibited)" : "real hardware");
    }
    bool isActive() const { return _active; }

    // True when AUTO switched to the model because of a runtime hardware fault
    // (as opposed to having no probes at boot). Used by the UI status tab.
    bool isFaultFallback() const { return _active && _mode == SIM_MODE_AUTO && _faultFallback; }
    void setFaultFallback(bool on) { _faultFallback = on; }

    void setFaults(uint8_t f) { _faults = f; }
    uint8_t getFaults() const { return _faults; }

    void reset() {
        _tankC = SIM_AMBIENT_C;
        _flowLPM = 0.0f;
        _flowColdLPM = 0.0f;
        _totalLiters = 0.0f;
        _totalLitersCold = 0.0f;
        _probe[0] = _probe[1] = SIM_AMBIENT_C;
        _probe[2] = _probe[3] = SIM_COLD_SUPPLY_C;
        _lastUpdate = millis();
    }

    // Pump curves: no flow below ~10% (dead head), roughly linear above.
    static float pumpCurve(float pumpPct) {
        return (pumpPct < 10.0f) ? 0.0f : SIM_MAX_FLOW_LPM * (pumpPct - 10.0f) / 90.0f;
    }

    // pumpHotPct / pumpColdPct: ramped pump speeds; heaterOn: logical relay state.
    void update(float pumpHotPct, float pumpColdPct, bool heaterOn) {
        if (!_active) return;
        uint32_t now = millis();
        float dt = (now - _lastUpdate) / 1000.0f;
        _lastUpdate = now;
        if (dt <= 0.0f || dt > 1.0f) return;

        float flowTargetHot = pumpCurve(pumpHotPct);
        float flowTargetCold = pumpCurve(pumpColdPct);
        if (_faults & SIM_FAULT_FLOW_BLOCKED) flowTargetHot = 0.0f;
        if (_faults & SIM_FAULT_COLD_FLOW_BLOCKED) flowTargetCold = 0.0f;

        float aFlow = fminf(dt / SIM_FLOW_TAU_S, 1.0f);
        _flowLPM += (flowTargetHot - _flowLPM) * aFlow;
        _flowColdLPM += (flowTargetCold - _flowColdLPM) * aFlow;
        if (_flowLPM < 0.01f) _flowLPM = 0.0f;
        if (_flowColdLPM < 0.01f) _flowColdLPM = 0.0f;
        _totalLiters += _flowLPM * dt / 60.0f;
        _totalLitersCold += _flowColdLPM * dt / 60.0f;

        bool heating = heaterOn || (_faults & SIM_FAULT_STUCK_HEATER);
        _heaterPowerW = heating ? SIM_HEATER_POWER_W : 0.0f;

        // Independent capacity rates (W/K) for each loop.
        float cHot = (_flowLPM / 60.0f) * WATER_DENSITY * WATER_SPECIFIC_HEAT;
        float cCold = (_flowColdLPM / 60.0f) * WATER_DENSITY * WATER_SPECIFIC_HEAT;

        float thIn = _tankC, thOut = _tankC, tcOut = SIM_COLD_SUPPLY_C, q = 0.0f;
        if (cHot > 1.0f && cCold > 1.0f) {
            float cMin = fminf(cHot, cCold);
            float cMax = fmaxf(cHot, cCold);
            float cr = cMin / cMax;
            float ntu = SIM_HX_UA_W_PER_K / cMin;
            float eps;
            if (fabsf(cr - 1.0f) < 1e-3f) {
                eps = ntu / (1.0f + ntu);
            } else {
                float e = expf(-ntu * (1.0f - cr));
                eps = (1.0f - e) / (1.0f - cr * e);
            }
            q = eps * cMin * (thIn - SIM_COLD_SUPPLY_C);
            thOut = thIn - q / cHot;
            tcOut = SIM_COLD_SUPPLY_C + q / cCold;
        } else {
            // Stagnant pipe sections drift toward ambient when there is no flow.
            thOut = _probe[1] + (SIM_AMBIENT_C - _probe[1]) * 0.02f;
            tcOut = _probe[3] + (SIM_AMBIENT_C - _probe[3]) * 0.02f;
        }

        float loss = SIM_TANK_LOSS_W_PER_K * (_tankC - SIM_AMBIENT_C);
        _tankC += (_heaterPowerW - q - loss) / (SIM_TANK_MASS_KG * WATER_SPECIFIC_HEAT) * dt;

        float a = fminf(dt / SIM_PROBE_TAU_S, 1.0f);
        _probe[0] += (thIn - _probe[0]) * a;
        _probe[1] += (thOut - _probe[1]) * a;
        _probe[2] += (SIM_COLD_SUPPLY_C - _probe[2]) * a;
        _probe[3] += (tcOut - _probe[3]) * a;
    }

    float getTempC(uint8_t probe) const {
        if (probe >= 4) return SENSOR_DISCONNECT_VAL;
        if (probe == 0 && (_faults & SIM_FAULT_PROBE_TH_IN)) return SENSOR_DISCONNECT_VAL;
        float noisy = _probe[probe] + ((float)(esp_random() % 1000) / 1000.0f - 0.5f) * 0.1f;
        return roundf(noisy * 16.0f) / 16.0f;   // 0.0625 C steps
    }
    bool isProbeValid(uint8_t probe) const {
        return !(probe == 0 && (_faults & SIM_FAULT_PROBE_TH_IN));
    }
    uint8_t faultMask() const { return (_faults & SIM_FAULT_PROBE_TH_IN) ? 0b0001 : 0; }
    float getFlowLPM() const { return _flowLPM; }
    float getFlowColdLPM() const { return _flowColdLPM; }
    float getTotalLiters() const { return _totalLiters; }
    float getTotalLitersCold() const { return _totalLitersCold; }
    float getTankC() const { return _tankC; }
    float getHeaterPowerW() const { return _heaterPowerW; }

private:
    SimMode _mode;
    bool _active;
    bool _faultFallback = false;
    uint8_t _faults;
    float _tankC;
    float _flowLPM;
    float _flowColdLPM;
    float _totalLiters;
    float _totalLitersCold;
    float _probe[4];
    uint32_t _lastUpdate;
    float _heaterPowerW;
};

inline PlantSimulator Sim;