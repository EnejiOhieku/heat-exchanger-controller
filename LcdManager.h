#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "config.h"
#include "Thermodynamics.h"
#include "SafetyManager.h"

class LcdManager {
public:
    LcdManager();

    void init();
    void update(const ThermodynamicData& data, bool heaterOn, uint8_t pumpSpeedPct, const char* ipStr);
    void setForcedAlarm(const char* alarmText);
    void clearAlarm();

private:
    void renderPage0(const ThermodynamicData& data);
    void renderPage1(const ThermodynamicData& data);
    void renderPage2(bool heaterOn, uint8_t pumpSpeedPct, const char* ipStr);
    void renderAlarm(const char* text);

    LiquidCrystal_I2C _lcd;
    uint8_t _currentPage;
    uint32_t _lastPageSwitchTime;
    bool _alarmActive;
    char _alarmMsg[17];
    bool _initialized;
};

extern LcdManager Display;
