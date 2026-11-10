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

#define MAX_BUS_DEVICES 8

// One DS18B20 slot on the shared 1-Wire bus. `_address` is the 64-bit ROM the
// slot is bound to; the wiring no longer fixes the position, so the mapping is
// resolved from ROM codes (deterministic on first scan, then kept stable).
class TempProbe {
public:
    explicit TempProbe(const char* name)
        : _name(name),
          _assigned(false),
          _present(false),
          _valid(false),
          _tempC(SENSOR_DISCONNECT_VAL),
          _errorCount(0),
          _totalErrors(0) {
        memset(_address, 0, sizeof(_address));
    }

    // DallasTemperature holds pointers, so copying probes would dangle.
    TempProbe(const TempProbe&) = delete;
    TempProbe& operator=(const TempProbe&) = delete;

    bool isAssigned() const { return _assigned; }
    bool matches(const DeviceAddress addr) const { return _assigned && memcmp(_address, addr, 8) == 0; }

    void bind(const DeviceAddress addr) {
        memcpy(_address, addr, 8);
        _assigned = true;
        _present = true;
        _valid = false;
        _errorCount = 0;
    }

    void setPresent(bool on) {
        _present = on;
        if (!on) {
            _valid = false;
            _tempC = SENSOR_DISCONNECT_VAL;
        }
    }

    void invalidate() {
        _valid = false;
        _tempC = SENSOR_DISCONNECT_VAL;
        _errorCount = 0;
    }

    void clearBinding() {
        memset(_address, 0, sizeof(_address));
        _assigned = false;
        _present = false;
        invalidate();
    }

    void applyResolution(DallasTemperature& bus) {
        if (_assigned) bus.setResolution(_address, TEMP_RESOLUTION_BITS);
    }

    void readConversion(DallasTemperature& bus) {
        if (!_present) {
            markError();
            return;
        }
        float t = bus.getTempC(_address);
        // -127 = no presence pulse or CRC failure. 85.0 exactly is the power-on
        // reset value: the probe browned out and never converted.
        bool bad = (t <= SENSOR_DISCONNECT_VAL) || (t == 85.0f) ||
                   (t < MIN_PLAUSIBLE_TEMP_C) || (t > MAX_PLAUSIBLE_TEMP_C);
        if (bad) {
            markError();
        } else {
            _tempC = t;
            _valid = true;
            _errorCount = 0;
        }
    }

    float getTempC() const { return _valid ? _tempC : SENSOR_DISCONNECT_VAL; }
    bool isPresent() const { return _present; }
    bool isValid() const { return _valid; }
    uint8_t getPin() const { return PIN_TEMP_BUS; }
    const char* getName() const { return _name; }
    uint32_t getTotalErrors() const { return _totalErrors; }

    void getAddressHex(char* out) const {
        for (uint8_t i = 0; i < 8; i++) sprintf(out + i * 2, "%02X", _address[i]);
        out[16] = '\0';
    }

    // Exchange ROM bindings; presence and validity are re-evaluated by the
    // next bus scan.
    void swapBindingsWith(TempProbe& other) {
        for (uint8_t i = 0; i < 8; i++) {
            uint8_t t = _address[i];
            _address[i] = other._address[i];
            other._address[i] = t;
        }
        bool ta = _assigned;
        _assigned = other._assigned;
        other._assigned = ta;
        _present = _assigned;
        other._present = other._assigned;
        invalidate();
        other.invalidate();
    }

private:
    // A single glitch holds the last good value; repeated failures invalidate.
    void markError() {
        _totalErrors++;
        if (_errorCount < 255) _errorCount++;
        if (_errorCount >= TEMP_MAX_CONSECUTIVE_ERRORS) {
            _valid = false;
            _tempC = SENSOR_DISCONNECT_VAL;
        }
    }

    const char* _name;
    DeviceAddress _address;
    bool _assigned;
    bool _present;
    bool _valid;
    float _tempC;
    uint8_t _errorCount;
    uint32_t _totalErrors;
};

// A single 1-Wire bus (GPIO4 / D4) carrying all four DS18B20s. One broadcast
// Convert T starts every sensor at once; each slot then reads its own ROM.
class ThermalSensors {
public:
    ThermalSensors()
        : _oneWire(PIN_TEMP_BUS),
          _dallas(&_oneWire),
          _probes{ TempProbe("Hot In"),
                   TempProbe("Hot Out"),
                   TempProbe("Cold In"),
                   TempProbe("Cold Out") },
          _cycleStart(0),
          _lastRescan(0),
          _conversionPending(false),
          _sampleCount(0),
          _lastPresentMask(0) {}

    void init() {
        _dallas.begin();
        _dallas.setWaitForConversion(false);
        Serial.printf("[Sensors] 1-Wire bus on GPIO %d (D4)\n", PIN_TEMP_BUS);
        scanAndAssign(true);
        _lastPresentMask = presentMask();
        logMapping();
        startCycle(millis());
    }

    // Non-blocking conversion state machine; call every loop iteration.
    void update() {
        uint32_t now = millis();

        if (_conversionPending && (now - _cycleStart >= conversionTimeMs())) {
            for (uint8_t i = 0; i < NUM_PROBES; i++) _probes[i].readConversion(_dallas);
            _conversionPending = false;
            _sampleCount++;
        }

        if (!_conversionPending && (now - _cycleStart >= TEMP_SAMPLE_PERIOD_MS)) {
            if (now - _lastRescan >= TEMP_RESCAN_INTERVAL_MS) {
                _lastRescan = now;
                scanAndAssign(true);
                notifyChanges();
            }
            startCycle(now);
        }
    }

    float getTempC(ProbeIndex probe) const {
        return (probe < NUM_PROBES) ? _probes[probe].getTempC() : SENSOR_DISCONNECT_VAL;
    }
    float getHotIn() const { return _probes[PROBE_HOT_IN].getTempC(); }
    float getHotOut() const { return _probes[PROBE_HOT_OUT].getTempC(); }
    float getColdIn() const { return _probes[PROBE_COLD_IN].getTempC(); }
    float getColdOut() const { return _probes[PROBE_COLD_OUT].getTempC(); }

    bool isValid(ProbeIndex probe) const { return probe < NUM_PROBES && _probes[probe].isValid(); }
    bool allValid() const {
        for (uint8_t i = 0; i < NUM_PROBES; i++) {
            if (!_probes[i].isValid()) return false;
        }
        return true;
    }
    // Bitmask of probes that are currently invalid (bit i = ProbeIndex i).
    uint8_t faultMask() const {
        uint8_t m = 0;
        for (uint8_t i = 0; i < NUM_PROBES; i++) {
            if (!_probes[i].isValid()) m |= (1 << i);
        }
        return m;
    }
    uint8_t getDeviceCount() const {
        uint8_t n = 0;
        for (uint8_t i = 0; i < NUM_PROBES; i++) n += _probes[i].isPresent() ? 1 : 0;
        return n;
    }
    // True once every probe has had at least one full conversion cycle.
    bool hasFirstSample() const { return _sampleCount > 0; }
    const TempProbe& probe(ProbeIndex p) const { return _probes[p]; }

    // Manually re-bind a slot to a ROM shown in the status page / serial log.
    bool assignSlot(uint8_t slot, const char* romHex) {
        if (slot >= NUM_PROBES || !romHex || strlen(romHex) != 16) return false;
        DeviceAddress addr;
        for (uint8_t i = 0; i < 8; i++) {
            int hi = hexNibble(romHex[i * 2]);
            int lo = hexNibble(romHex[i * 2 + 1]);
            if (hi < 0 || lo < 0) return false;
            addr[i] = (uint8_t)((hi << 4) | lo);
        }
        // A ROM may only occupy one slot.
        for (uint8_t i = 0; i < NUM_PROBES; i++) {
            if (i != slot && _probes[i].matches(addr)) _probes[i].clearBinding();
        }
        _probes[slot].bind(addr);
        scanAndAssign(false);
        logMapping();
        return _probes[slot].isPresent();
    }

    // Swap two slots when the auto-mapping put the physical probes in the
    // wrong order (e.g. hot-in and hot-out reversed).
    void swapSlots(uint8_t a, uint8_t b) {
        if (a >= NUM_PROBES || b >= NUM_PROBES || a == b) return;
        _probes[a].swapBindingsWith(_probes[b]);
        scanAndAssign(false);
        logMapping();
    }

private:
    static int hexNibble(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    static bool validTempFamily(uint8_t family) {
        return family == 0x10 || family == 0x28 || family == 0x22 ||
               family == 0x3B || family == 0x42;
    }

    uint8_t presentMask() const {
        uint8_t m = 0;
        for (uint8_t i = 0; i < NUM_PROBES; i++) {
            if (_probes[i].isPresent()) m |= (1 << i);
        }
        return m;
    }

    void notifyChanges() {
        uint8_t mask = presentMask();
        uint8_t changed = mask ^ _lastPresentMask;
        if (!changed) return;
        for (uint8_t i = 0; i < NUM_PROBES; i++) {
            if (!(changed & (1 << i))) continue;
            if (mask & (1 << i)) {
                Serial.printf("[Sensors] %s reconnected (slot %u)\n", _probes[i].getName(), i);
            } else {
                Serial.printf("[Sensors] %s lost (slot %u)\n", _probes[i].getName(), i);
            }
        }
        _lastPresentMask = mask;
    }

    void logMapping() const {
        for (uint8_t i = 0; i < NUM_PROBES; i++) {
            char rom[17];
            _probes[i].getAddressHex(rom);
            Serial.printf("[Sensors] %-8s slot %u GPIO %d: %s %s\n",
                          _probes[i].getName(), i, PIN_TEMP_BUS,
                          _probes[i].isPresent() ? "OK " : "MISSING",
                          _probes[i].isAssigned() ? rom : "");
        }
    }

    void startCycle(uint32_t now) {
        _dallas.requestTemperatures();   // one broadcast Convert T for the whole bus
        _cycleStart = now;
        _conversionPending = true;
    }

    static constexpr uint32_t conversionTimeMs() {
        // 93.75 ms at 9 bits, doubling per extra bit, plus margin.
        return (94UL << (TEMP_RESOLUTION_BITS - 9)) + 30;
    }

    // Reconcile the bus with the four slots. Existing bindings are kept so a
    // missing probe does not shift the others; new devices take a free slot.
    void scanAndAssign(bool applyConfig) {
        // DallasTemperature caches its device list from begin(), so enumerate the
        // bus directly. This keeps hot-plug detection cheap (no 50 ms begin()).
        DeviceAddress found[MAX_BUS_DEVICES];
        uint8_t foundCount = 0;
        _oneWire.reset_search();
        DeviceAddress addr;
        while (foundCount < MAX_BUS_DEVICES && _oneWire.search(addr)) {
            if (validTempFamily(addr[0])) memcpy(found[foundCount++], addr, 8);
        }

        for (uint8_t i = 0; i < NUM_PROBES; i++) {
            if (!_probes[i].isAssigned()) continue;
            bool on = false;
            for (uint8_t j = 0; j < foundCount; j++) {
                if (_probes[i].matches(found[j])) { on = true; break; }
            }
            _probes[i].setPresent(on);
        }

        // Deterministic first assignment: sort the ROMs before binding.
        for (uint8_t a = 1; a < foundCount; a++) {
            DeviceAddress key;
            memcpy(key, found[a], 8);
            int8_t b = (int8_t)a - 1;
            while (b >= 0 && memcmp(found[b], key, 8) > 0) {
                memcpy(found[b + 1], found[b], 8);
                b--;
            }
            memcpy(found[b + 1], key, 8);
        }

        for (uint8_t j = 0; j < foundCount; j++) {
            bool known = false;
            for (uint8_t i = 0; i < NUM_PROBES; i++) {
                if (_probes[i].matches(found[j])) { known = true; break; }
            }
            if (known) continue;

            int8_t slot = -1;
            for (uint8_t i = 0; i < NUM_PROBES; i++) if (!_probes[i].isAssigned()) { slot = i; break; }
            if (slot < 0) {
                for (uint8_t i = 0; i < NUM_PROBES; i++) if (!_probes[i].isPresent()) { slot = i; break; }
            }
            if (slot < 0) {
                Serial.println("[Sensors] more than 4 devices on the bus - extra ignored");
                break;
            }
            _probes[slot].bind(found[j]);
        }

        if (applyConfig) {
            for (uint8_t i = 0; i < NUM_PROBES; i++) {
                if (_probes[i].isPresent()) _probes[i].applyResolution(_dallas);
            }
        }
    }

    OneWire _oneWire;
    DallasTemperature _dallas;
    TempProbe _probes[NUM_PROBES];
    uint32_t _cycleStart;
    uint32_t _lastRescan;
    bool _conversionPending;
    uint32_t _sampleCount;
    uint8_t _lastPresentMask;
};

inline ThermalSensors Sensors;