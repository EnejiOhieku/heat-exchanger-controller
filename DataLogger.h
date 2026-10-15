#pragma once

#include <Arduino.h>
#include "config.h"
#include "Thermodynamics.h"

struct LogEntry {
    uint32_t timestampSec;
    float thIn;
    float thOut;
    float tcIn;
    float tcOut;
    float flowLPM;
    float qHotWatts;
    float lmtd;
    float effectiveness;
    uint8_t pumpPct;
    bool heaterState;
};

class DataLogger {
public:
    DataLogger();

    void init();
    void record(const ThermodynamicData& data, uint8_t pumpPct, bool heaterState);
    void clear();

    size_t getCount() const;
    size_t getCapacity() const;

    void streamCSV(Print& out) const;

private:
    LogEntry _buffer[MAX_LOG_RECORDS];
    size_t _head;
    size_t _count;
    uint32_t _startTimeSec;
};

extern DataLogger Logger;
