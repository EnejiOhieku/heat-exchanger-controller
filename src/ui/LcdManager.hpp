#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "../config.h"
#include "../core/Thermodynamics.hpp"
#include "../core/SafetyManager.hpp"

class LcdManager {
public:
    LcdManager()
        : _lcd(0x27, 16, 2),
          _currentPage(0),
          _lastPageSwitchTime(0),
          _alarmActive(false),
          _initialized(false) {
        memset(_alarmMsg, 0, sizeof(_alarmMsg));
    }

    void init() {
        Wire.begin(PIN_LCD_SDA, PIN_LCD_SCL);
        _lcd.init();
        _lcd.backlight();
        _lcd.clear();

        _lcd.setCursor(0, 0);
        _lcd.print("HEAT EXCHANGER");
        _lcd.setCursor(0, 1);
        _lcd.print("SYSTEM STARTING");

        _initialized = true;
        _lastPageSwitchTime = millis();
        Serial.printf("[LCD] Initialized 16x2 on SDA=%d, SCL=%d\n", PIN_LCD_SDA, PIN_LCD_SCL);
    }

    void setForcedAlarm(const char* alarmText) {
        _alarmActive = true;
        strncpy(_alarmMsg, alarmText, 16);
        _alarmMsg[16] = '\0';
    }

    void clearAlarm() {
        _alarmActive = false;
        memset(_alarmMsg, 0, sizeof(_alarmMsg));
    }

    void update(const ThermodynamicData& data, bool heaterOn, uint8_t pumpSpeedPct, const char* ipStr) {
        if (!_initialized) return;

        if (Safety.isTripped()) {
            renderAlarm(Safety.getErrorString());
            return;
        } else if (_alarmActive) {
            renderAlarm(_alarmMsg);
            return;
        }

        uint32_t now = millis();
        if (now - _lastPageSwitchTime >= 3000) {
            _currentPage = (_currentPage + 1) % 3;
            _lastPageSwitchTime = now;
            _lcd.clear();
        }

        switch (_currentPage) {
            case 0: renderPage0(data); break;
            case 1: renderPage1(data); break;
            case 2: renderPage2(heaterOn, pumpSpeedPct, ipStr); break;
        }
    }

private:
    void renderPage0(const ThermodynamicData& data) {
        char line1[17];
        char line2[17];
        snprintf(line1, sizeof(line1), "H:I%4.1f O%4.1f", data.thIn, data.thOut);
        snprintf(line2, sizeof(line2), "C:I%4.1f O%4.1f", data.tcIn, data.tcOut);

        _lcd.setCursor(0, 0);
        _lcd.print(line1);
        _lcd.setCursor(0, 1);
        _lcd.print(line2);
    }

    void renderPage1(const ThermodynamicData& data) {
        char line1[17];
        char line2[17];
        snprintf(line1, sizeof(line1), "F:%4.1fL/m %4.0fW", data.flowRateLPM, data.qHotWatts);
        snprintf(line2, sizeof(line2), "Eff:%2.0f%% L:%4.1fC", data.effectiveness, data.lmtd);

        _lcd.setCursor(0, 0);
        _lcd.print(line1);
        _lcd.setCursor(0, 1);
        _lcd.print(line2);
    }

    void renderPage2(bool heaterOn, uint8_t pumpSpeedPct, const char* ipStr) {
        char line1[17];
        char line2[17];
        snprintf(line1, sizeof(line1), "%-16.16s", (ipStr && strlen(ipStr) > 0) ? ipStr : "192.168.4.1");
        snprintf(line2, sizeof(line2), "Htr:%-3s  Pmp:%3d%%", heaterOn ? "ON" : "OFF", pumpSpeedPct);

        _lcd.setCursor(0, 0);
        _lcd.print(line1);
        _lcd.setCursor(0, 1);
        _lcd.print(line2);
    }

    void renderAlarm(const char* text) {
        _lcd.setCursor(0, 0);
        _lcd.print("*** ALARM ***   ");
        _lcd.setCursor(0, 1);
        char buf[17];
        snprintf(buf, sizeof(buf), "%-16.16s", text);
        _lcd.print(buf);
    }

    LiquidCrystal_I2C _lcd;
    uint8_t _currentPage;
    uint32_t _lastPageSwitchTime;
    bool _alarmActive;
    char _alarmMsg[17];
    bool _initialized;
};

inline LcdManager Display;
