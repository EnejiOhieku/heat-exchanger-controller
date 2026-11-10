#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "../config.h"
#include "../hardware/FlowSensor.hpp"
#include "../hardware/PumpController.hpp"
#include "../hardware/HeaterController.hpp"
#include "../sim/PlantSimulator.hpp"

// Persists operator tuning (calibration, setpoints, gains) in NVS so a reboot
// does not silently revert the rig to compile-time defaults. Modes are NOT
// restored: the heater always boots OFF and the pump stopped.
class SettingsStore {
public:
    void load() {
        _prefs.begin("hx_cfg", false);
        FlowHot.setKFactor(_prefs.getFloat("kfac", DEFAULT_FLOW_K_FACTOR));
        FlowCold.setKFactor(_prefs.getFloat("kfac2", DEFAULT_FLOW_K_FACTOR_COLD));
        Heater.setTargetTemp(_prefs.getFloat("tgt", DEFAULT_HEATER_TARGET_C));
        Heater.setHysteresis(_prefs.getFloat("hyst", DEFAULT_HEATER_HYST_C));
        Heater.setPidGains(_prefs.getFloat("kp", DEFAULT_HEATER_KP),
                           _prefs.getFloat("ki", DEFAULT_HEATER_KI),
                           _prefs.getFloat("kd", DEFAULT_HEATER_KD));
        Heater.setControlProbe(_prefs.getUChar("cprobe", 0));
        Pump.setMinDutyPercent(_prefs.getUChar("pmin", PUMP_MIN_DUTY_PCT));
        Pump.setFlowGains(_prefs.getFloat("fkp", DEFAULT_FLOW_KP), _prefs.getFloat("fki", DEFAULT_FLOW_KI));
        ColdPump.setMinDutyPercent(_prefs.getUChar("pmin2", PUMP_COLD_MIN_DUTY_PCT));
        ColdPump.setFlowGains(_prefs.getFloat("fkp2", DEFAULT_COLD_FLOW_KP),
                              _prefs.getFloat("fki2", DEFAULT_COLD_FLOW_KI));
        Sim.setMode(_prefs.getUChar("sim", SIM_MODE_AUTO));
        Serial.printf("[Settings] Loaded: K=%.2f/%.2f target=%.1fC PID=%.2f/%.3f/%.2f probe=%u\n",
                      FlowHot.getKFactor(), FlowCold.getKFactor(), Heater.getTargetTemp(),
                      Heater.getKp(), Heater.getKi(), Heater.getKd(), Heater.getControlProbe());
    }

    void save() {
        _prefs.putFloat("kfac", FlowHot.getKFactor());
        _prefs.putFloat("kfac2", FlowCold.getKFactor());
        _prefs.putFloat("tgt", Heater.getTargetTemp());
        _prefs.putFloat("hyst", Heater.getHysteresis());
        _prefs.putFloat("kp", Heater.getKp());
        _prefs.putFloat("ki", Heater.getKi());
        _prefs.putFloat("kd", Heater.getKd());
        _prefs.putUChar("cprobe", Heater.getControlProbe());
        _prefs.putUChar("pmin", Pump.getMinDutyPercent());
        _prefs.putFloat("fkp", Pump.getFlowKp());
        _prefs.putFloat("fki", Pump.getFlowKi());
        _prefs.putUChar("pmin2", ColdPump.getMinDutyPercent());
        _prefs.putFloat("fkp2", ColdPump.getFlowKp());
        _prefs.putFloat("fki2", ColdPump.getFlowKi());
        _prefs.putUChar("sim", Sim.getMode());
    }

private:
    Preferences _prefs;
};

inline SettingsStore Settings;
