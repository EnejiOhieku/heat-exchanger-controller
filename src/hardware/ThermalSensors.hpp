#pragma once

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "../config.h"

enum ProbeIndex : uint8_t {
    PROBE_HOT_IN = 0,
    PROBE_HOT_OUT = 1,
    PROBE_COLD_IN = 2,
    PROBE_COLD_OUT = 3,
    NUM_PROBES = 4
};

class ThermalSensors {
public:
    ThermalSensors()
        : _oneWire(PIN_ONEWIRE_BUS),
          _sensors(&_oneWire),
          _devicesFound(0),
          _lastRequestTime(0),
          _conversionPending(false) {
        for (int i = 0; i < NUM_PROBES; i++) {
            _temperatures[i] = SENSOR_DISCONNECT_VAL;
            memset(_addresses[i], 0, sizeof(DeviceAddress));
        }
    }

    void init() {
        _sensors.begin();
        _sensors.setWaitForConversion(false);
        _devicesFound = _sensors.getDeviceCount();

        Serial.printf("[Sensors] Found %d DS18B20 sensor(s) on 1-Wire bus (GPIO %d)\n", _devicesFound, PIN_ONEWIRE_BUS);

        for (uint8_t i = 0; i < NUM_PROBES; i++) {
            if (i < _devicesFound) {
                if (_sensors.getAddress(_addresses[i], i)) {
                    _sensors.setResolution(_addresses[i], 11);
                    char addrStr[17];
                    getAddressHex((ProbeIndex)i, addrStr);
                    Serial.printf("  Probe %d: ROM %s\n", i, addrStr);
                }
            }
        }

        requestReadings();
    }

    void requestReadings() {
        _sensors.requestTemperatures();
        _lastRequestTime = millis();
        _conversionPending = true;
    }

    void updateReadings() {
        if (_conversionPending && (millis() - _lastRequestTime >= 400)) {
            for (uint8_t i = 0; i < NUM_PROBES; i++) {
                if (i < _devicesFound) {
                    float t = _sensors.getTempC(_addresses[i]);
                    _temperatures[i] = t;
                } else {
                    _temperatures[i] = SENSOR_DISCONNECT_VAL;
                }
            }
            _conversionPending = false;
        }
    }

    float getTempC(ProbeIndex probe) const {
        if (probe >= NUM_PROBES) return SENSOR_DISCONNECT_VAL;
        return _temperatures[probe];
    }

    float getHotIn() const { return _temperatures[PROBE_HOT_IN]; }
    float getHotOut() const { return _temperatures[PROBE_HOT_OUT]; }
    float getColdIn() const { return _temperatures[PROBE_COLD_IN]; }
    float getColdOut() const { return _temperatures[PROBE_COLD_OUT]; }

    uint8_t getDeviceCount() const { return _devicesFound; }

    void getAddressHex(ProbeIndex probe, char* outHexStr) const {
        if (probe >= NUM_PROBES || outHexStr == nullptr) return;
        for (uint8_t i = 0; i < 8; i++) {
            sprintf(outHexStr + (i * 2), "%02X", _addresses[probe][i]);
        }
        outHexStr[16] = '\0';
    }

    bool isConversionPending() const { return _conversionPending; }

private:
    OneWire _oneWire;
    DallasTemperature _sensors;
    DeviceAddress _addresses[NUM_PROBES];
    float _temperatures[NUM_PROBES];
    uint8_t _devicesFound;
    uint32_t _lastRequestTime;
    bool _conversionPending;
};

inline ThermalSensors Sensors;
