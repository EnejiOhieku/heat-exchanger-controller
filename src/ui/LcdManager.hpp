#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "../config.h"
#include "../core/Thermodynamics.hpp"
#include "../core/SafetyManager.hpp"

#define LCD_I2C_CLOCK_HZ        100000
#define LCD_PAGE_PERIOD_MS      3000
#define LCD_HEALTH_CHECK_MS     2000
#define LCD_REINIT_PERIOD_MS    60000   // Relay EMI can garble HD44780 state; re-init periodically

struct LcdStatus {
    bool heaterOn;
    uint8_t pumpPct;
    uint8_t coldPumpPct;
    const char* heaterModeShort;   // e.g. "PID", "THR"
    float setpointC;
    const char* ip;
};

class LcdManager {
public:
    LcdManager()
        : _lcd(nullptr),
          _address(0),
          _currentPage(0),
          _lastPageSwitchTime(0),
          _lastHealthCheck(0),
          _lastReinit(0),
          _online(false),
          _alarmActive(false) {
        memset(_alarmMsg, 0, sizeof(_alarmMsg));
    }

    void init() {
        Wire.begin(PIN_LCD_SDA, PIN_LCD_SCL);
        Wire.setClock(LCD_I2C_CLOCK_HZ);

        _address = probeAddress();
        if (_address == 0) {
            Serial.printf("[LCD] No PCF8574 backpack found on SDA=%d SCL=%d (scanned 0x20-0x27, 0x38-0x3F)\n",
                          PIN_LCD_SDA, PIN_LCD_SCL);
            return;
        }

        _lcd = new LiquidCrystal_I2C(_address, 16, 2);
        startDisplay();
        writeLine(0, "HEAT EXCHANGER");
        writeLine(1, "SYSTEM STARTING");
        _lastPageSwitchTime = millis();
        Serial.printf("[LCD] 16x2 at I2C 0x%02X on SDA=%d, SCL=%d\n", _address, PIN_LCD_SDA, PIN_LCD_SCL);
    }

    bool isOnline() const { return _online; }
    uint8_t getAddress() const { return _address; }

    void setForcedAlarm(const char* alarmText) {
        _alarmActive = true;
        strncpy(_alarmMsg, alarmText, 16);
        _alarmMsg[16] = '\0';
    }

    void clearAlarm() {
        _alarmActive = false;
        memset(_alarmMsg, 0, sizeof(_alarmMsg));
    }

    void update(const ThermodynamicData& data, const LcdStatus& st) {
        if (_lcd == nullptr) return;
        uint32_t now = millis();

        if (now - _lastHealthCheck >= LCD_HEALTH_CHECK_MS) {
            _lastHealthCheck = now;
            bool ack = deviceAcks(_address);
            if (ack && (!_online || now - _lastReinit >= LCD_REINIT_PERIOD_MS)) {
                if (!_online) Serial.println("[LCD] Display back online, re-initialising");
                startDisplay();
            } else if (!ack && _online) {
                Serial.println("[LCD] Display stopped acknowledging on I2C");
                _online = false;
            }
        }
        if (!_online) return;

        if (Safety.isTripped()) {
            renderAlarm(Safety.getShortString());
            return;
        }
        if (_alarmActive) {
            renderAlarm(_alarmMsg);
            return;
        }

        if (now - _lastPageSwitchTime >= LCD_PAGE_PERIOD_MS) {
            _currentPage = (_currentPage + 1) % 4;
            _lastPageSwitchTime = now;
        }

        char l1[24], l2[24], a[8], b[8];
        switch (_currentPage) {
            case 0:
                snprintf(l1, sizeof(l1), "Hi%s Ho%s", fmtTemp(a, data.thIn), fmtTemp(b, data.thOut));
                snprintf(l2, sizeof(l2), "Ci%s Co%s", fmtTemp(a, data.tcIn), fmtTemp(b, data.tcOut));
                break;
            case 1:
                snprintf(l1, sizeof(l1), "Fh%4.1f Fc%4.1f", data.flowRateLPM, data.flowRateColdLPM);
                snprintf(l2, sizeof(l2), "Q%5.0fW E%3.0f%%", data.qHotWatts, data.effectiveness);
                break;
            case 2:
                snprintf(l1, sizeof(l1), "Htr %-3s %-3s%4.1fC", st.heaterOn ? "ON" : "OFF",
                         st.heaterModeShort, st.setpointC);
                snprintf(l2, sizeof(l2), "Ph%3u%% Pc%3u%%", st.pumpPct, st.coldPumpPct);
                break;
            default:
                snprintf(l1, sizeof(l1), "%s", (st.ip && st.ip[0]) ? st.ip : "192.168.4.1");
                snprintf(l2, sizeof(l2), "%s", DEFAULT_MDNS_HOST ".local");
                break;
        }
        writeLine(0, l1);
        writeLine(1, l2);
    }

private:
    static bool deviceAcks(uint8_t addr) {
        Wire.beginTransmission(addr);
        return Wire.endTransmission() == 0;
    }

    // PCF8574 backpacks answer at 0x20-0x27, PCF8574A at 0x38-0x3F.
    static uint8_t probeAddress() {
        const uint8_t preferred[] = { 0x27, 0x3F };
        for (uint8_t a : preferred) {
            if (deviceAcks(a)) return a;
        }
        for (uint8_t a = 0x20; a <= 0x27; a++) if (deviceAcks(a)) return a;
        for (uint8_t a = 0x38; a <= 0x3F; a++) if (deviceAcks(a)) return a;
        return 0;
    }

    void startDisplay() {
        _lcd->init();
        _lcd->backlight();
        _lcd->clear();
        _online = true;
        _lastReinit = millis();
    }

    static const char* fmtTemp(char* buf, float t) {
        if (t <= SENSOR_DISCONNECT_VAL + 1.0f) {
            strcpy(buf, " --.-");
        } else {
            snprintf(buf, 8, "%5.1f", t);
        }
        return buf;
    }

    // Overwrite the full row (padded to 16) instead of clear(): no flicker.
    void writeLine(uint8_t row, const char* text) {
        char buf[17];
        snprintf(buf, sizeof(buf), "%-16.16s", text);
        _lcd->setCursor(0, row);
        _lcd->print(buf);
    }

    void renderAlarm(const char* text) {
        bool blink = (millis() / 500) & 1;
        writeLine(0, blink ? "*** ALARM ***" : "");
        writeLine(1, text);
    }

    LiquidCrystal_I2C* _lcd;
    uint8_t _address;
    uint8_t _currentPage;
    uint32_t _lastPageSwitchTime;
    uint32_t _lastHealthCheck;
    uint32_t _lastReinit;
    bool _online;
    bool _alarmActive;
    char _alarmMsg[17];
};

inline LcdManager Display;
