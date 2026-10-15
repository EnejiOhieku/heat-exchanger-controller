#pragma once

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "config.h"

enum ProbeIndex : uint8_t {
    PROBE_HOT_IN = 0,
    PROBE_HOT_OUT = 1,
    PROBE_COLD_IN = 2,
    PROBE_COLD_OUT = 3,
    NUM_PROBES = 4
};

class ThermalSensors {
public:
    ThermalSensors();

    void init();
    void requestReadings();  // Asynchronous non-blocking request
    void updateReadings();   // Reads result after conversion time elapsed

    float getTempC(ProbeIndex probe) const;
    float getHotIn() const;
    float getHotOut() const;
    float getColdIn() const;
    float getColdOut() const;

    uint8_t getDeviceCount() const;
    void getAddressHex(ProbeIndex probe, char* outHexStr) const;
    bool isConversionPending() const;

private:
    OneWire _oneWire;
    DallasTemperature _sensors;
    DeviceAddress _addresses[NUM_PROBES];
    float _temperatures[NUM_PROBES];
    uint8_t _devicesFound;
    uint32_t _lastRequestTime;
    bool _conversionPending;
};

extern ThermalSensors Sensors;
