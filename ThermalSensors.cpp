#include "ThermalSensors.h"

ThermalSensors Sensors;

ThermalSensors::ThermalSensors()
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

void ThermalSensors::init() {
    _sensors.begin();
    _sensors.setWaitForConversion(false); // Non-blocking async reading
    _devicesFound = _sensors.getDeviceCount();

    Serial.printf("[Sensors] Found %d DS18B20 sensor(s) on 1-Wire bus (GPIO %d)\n", _devicesFound, PIN_ONEWIRE_BUS);

    for (uint8_t i = 0; i < NUM_PROBES; i++) {
        if (i < _devicesFound) {
            if (_sensors.getAddress(_addresses[i], i)) {
                _sensors.setResolution(_addresses[i], 11); // 11-bit: 375ms conversion, 0.125°C res
                char addrStr[17];
                getAddressHex((ProbeIndex)i, addrStr);
                Serial.printf("  Probe %d: ROM %s\n", i, addrStr);
            }
        }
    }

    // Trigger initial conversion
    requestReadings();
}

void ThermalSensors::requestReadings() {
    _sensors.requestTemperatures();
    _lastRequestTime = millis();
    _conversionPending = true;
}

void ThermalSensors::updateReadings() {
    // 11-bit conversion requires 375 ms maximum
    if (_conversionPending && (millis() - _lastRequestTime >= 400)) {
        for (uint8_t i = 0; i < NUM_PROBES; i++) {
            if (i < _devicesFound) {
                float t = _sensors.getTempC(_addresses[i]);
                _temperatures[i] = t;
            } else {
                // If fewer than 4 physical probes connected during testing/bench testing,
                // keep reasonable placeholder or disconnected value
                _temperatures[i] = SENSOR_DISCONNECT_VAL;
            }
        }
        _conversionPending = false;
    }
}

float ThermalSensors::getTempC(ProbeIndex probe) const {
    if (probe >= NUM_PROBES) return SENSOR_DISCONNECT_VAL;
    return _temperatures[probe];
}

float ThermalSensors::getHotIn() const {
    return _temperatures[PROBE_HOT_IN];
}

float ThermalSensors::getHotOut() const {
    return _temperatures[PROBE_HOT_OUT];
}

float ThermalSensors::getColdIn() const {
    return _temperatures[PROBE_COLD_IN];
}

float ThermalSensors::getColdOut() const {
    return _temperatures[PROBE_COLD_OUT];
}

uint8_t ThermalSensors::getDeviceCount() const {
    return _devicesFound;
}

void ThermalSensors::getAddressHex(ProbeIndex probe, char* outHexStr) const {
    if (probe >= NUM_PROBES || outHexStr == nullptr) return;
    for (uint8_t i = 0; i < 8; i++) {
        sprintf(outHexStr + (i * 2), "%02X", _addresses[probe][i]);
    }
    outHexStr[16] = '\0';
}

bool ThermalSensors::isConversionPending() const {
    return _conversionPending;
}
