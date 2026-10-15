#include "DataLogger.h"

DataLogger Logger;

DataLogger::DataLogger()
    : _head(0),
      _count(0),
      _startTimeSec(0) {}

void DataLogger::init() {
    _head = 0;
    _count = 0;
    _startTimeSec = millis() / 1000;
}

void DataLogger::clear() {
    _head = 0;
    _count = 0;
    _startTimeSec = millis() / 1000;
}

void DataLogger::record(const ThermodynamicData& data, uint8_t pumpPct, bool heaterState) {
    LogEntry& entry = _buffer[_head];
    entry.timestampSec = (millis() / 1000) - _startTimeSec;
    entry.thIn = data.thIn;
    entry.thOut = data.thOut;
    entry.tcIn = data.tcIn;
    entry.tcOut = data.tcOut;
    entry.flowLPM = data.flowRateLPM;
    entry.qHotWatts = data.qHotWatts;
    entry.lmtd = data.lmtd;
    entry.effectiveness = data.effectiveness;
    entry.pumpPct = pumpPct;
    entry.heaterState = heaterState;

    _head = (_head + 1) % MAX_LOG_RECORDS;
    if (_count < MAX_LOG_RECORDS) {
        _count++;
    }
}

size_t DataLogger::getCount() const {
    return _count;
}

size_t DataLogger::getCapacity() const {
    return MAX_LOG_RECORDS;
}

void DataLogger::streamCSV(Print& out) const {
    // Header
    out.println("Time_sec,Th_in_C,Th_out_C,Tc_in_C,Tc_out_C,Flow_LPM,Q_hot_Watts,LMTD_C,Effectiveness_pct,Pump_pct,Heater_State");

    if (_count == 0) return;

    size_t startIdx = (_count < MAX_LOG_RECORDS) ? 0 : _head;

    for (size_t i = 0; i < _count; i++) {
        size_t idx = (startIdx + i) % MAX_LOG_RECORDS;
        const LogEntry& e = _buffer[idx];

        char line[128];
        snprintf(line, sizeof(line),
                 "%u,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.2f,%.1f,%u,%s",
                 e.timestampSec,
                 e.thIn, e.thOut, e.tcIn, e.tcOut,
                 e.flowLPM, e.qHotWatts, e.lmtd, e.effectiveness,
                 e.pumpPct, e.heaterState ? "ON" : "OFF");
        out.println(line);
    }
}
