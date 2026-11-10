#pragma once

#include <Arduino.h>
#include "../config.h"
#include "../core/Thermodynamics.hpp"

struct LogEntry {
    uint32_t timestampSec;
    float thIn;
    float thOut;
    float tcIn;
    float tcOut;
    float flowLPM;
    float flowColdLPM;
    float qHotWatts;
    float qColdWatts;
    float lmtd;
    float uaWPerK;
    float effectiveness;
    uint8_t pumpPct;
    uint8_t coldPumpPct;
    uint8_t heaterDutyPct;
    bool heaterState;
    bool simulated;
};

class DataLogger {
public:
    DataLogger()
        : _head(0),
          _count(0),
          _startTimeSec(0) {}

    void init() {
        _head = 0;
        _count = 0;
        _startTimeSec = millis() / 1000;
    }

    void clear() {
        _head = 0;
        _count = 0;
        _startTimeSec = millis() / 1000;
    }

    void record(const ThermodynamicData& data, uint8_t pumpPct, uint8_t coldPumpPct,
                bool heaterState, uint8_t heaterDutyPct, bool simulated) {
        LogEntry& entry = _buffer[_head];
        entry.timestampSec = (millis() / 1000) - _startTimeSec;
        entry.thIn = data.thIn;
        entry.thOut = data.thOut;
        entry.tcIn = data.tcIn;
        entry.tcOut = data.tcOut;
        entry.flowLPM = data.flowRateLPM;
        entry.flowColdLPM = data.flowRateColdLPM;
        entry.qHotWatts = data.qHotWatts;
        entry.qColdWatts = data.qColdWatts;
        entry.lmtd = data.lmtd;
        entry.uaWPerK = data.uaWPerK;
        entry.effectiveness = data.effectiveness;
        entry.pumpPct = pumpPct;
        entry.coldPumpPct = coldPumpPct;
        entry.heaterDutyPct = heaterDutyPct;
        entry.heaterState = heaterState;
        entry.simulated = simulated;

        _head = (_head + 1) % MAX_LOG_RECORDS;
        if (_count < MAX_LOG_RECORDS) {
            _count++;
        }
    }

    size_t getCount() const { return _count; }
    size_t getCapacity() const { return MAX_LOG_RECORDS; }

    void streamCSV(Print& out) const {
        out.println("Time_sec,Th_in_C,Th_out_C,Tc_in_C,Tc_out_C,Flow_hot_LPM,Flow_cold_LPM,Q_hot_W,Q_cold_W,"
                    "LMTD_C,UA_W_per_K,Effectiveness_pct,Pump_hot_pct,Pump_cold_pct,Heater_Duty_pct,"
                    "Heater_State,Source");

        if (_count == 0) return;

        size_t startIdx = (_count < MAX_LOG_RECORDS) ? 0 : _head;

        for (size_t i = 0; i < _count; i++) {
            size_t idx = (startIdx + i) % MAX_LOG_RECORDS;
            const LogEntry& e = _buffer[idx];

            char line[192];
            snprintf(line, sizeof(line),
                     "%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.1f,%.2f,%.1f,%.1f,%u,%u,%u,%s,%s",
                     (unsigned long)e.timestampSec,
                     e.thIn, e.thOut, e.tcIn, e.tcOut,
                     e.flowLPM, e.flowColdLPM, e.qHotWatts, e.qColdWatts, e.lmtd, e.uaWPerK,
                     e.effectiveness, e.pumpPct, e.coldPumpPct, e.heaterDutyPct,
                     e.heaterState ? "ON" : "OFF", e.simulated ? "SIM" : "HW");
            out.println(line);
        }
    }

private:
    LogEntry _buffer[MAX_LOG_RECORDS];
    size_t _head;
    size_t _count;
    uint32_t _startTimeSec;
};

inline DataLogger Logger;
