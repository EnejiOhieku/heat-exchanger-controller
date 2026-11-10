#pragma once

#include <Arduino.h>

// ==============================================================================
// PIN DEFINITIONS (ESP32 DevKitC V4 / ESP32-WROOM-32E)
// Reference: Espressif ESP32-DevKitC V4 user guide (J2 = left header,
// J3 = right header). Strapping pins (0, 2, 5, 12, 15) and flash pins (6-11)
// are avoided for I/O.
// ==============================================================================
#define PIN_LCD_SDA          21      // J3-6
#define PIN_LCD_SCL          22      // J3-3

// All four DS18B20 probes share ONE 1-Wire bus on GPIO4 (the pin labelled D4 on
// most ESP32 dev boards; IO4 on the official DevKitC header, J3-13). A single
// bus needs one 4.7 kOhm pull-up to 3.3 V. Breakout modules usually carry a
// pull-up already, so keep only one of them in circuit (parallel 4.7 kOhms
// would load the bus). Positions are resolved from each probe's 64-bit ROM.
#define PIN_TEMP_BUS         4       // J3-13 (D4)

// Flow meters (pulse inputs)
#define PIN_FLOW_HOT         23      // J2-11  hot-loop flow meter
#define PIN_FLOW_COLD        27      // J2-12  cold-loop flow meter

// Actuators
#define PIN_RELAY_HEATER     25      // J2-15  30 A relay (heater)
#define PIN_PUMP_HOT_PWM     5      // J3-8   hot pump: one-way single-PWM driver
#define PIN_PUMP_COLD_PWM    18      // J3-9   cold pump: one-way single-PWM driver
#define PIN_STATUS_LED       2       // on-board LED
// Free for expansion: GPIO 16, 17, 23, 25, 26, 32, 33 (34-39 are input-only).

// ==============================================================================
// THERMODYNAMIC CONSTANTS
// ==============================================================================
#define WATER_SPECIFIC_HEAT  4184.0f  // J / (kg * K)
#define WATER_DENSITY        1.0f     // kg / L

// ==============================================================================
// TEMPERATURE SENSORS (DS18B20)
// ==============================================================================
#define TEMP_RESOLUTION_BITS          12      // 0.0625 C steps, 750 ms conversion
#define TEMP_SAMPLE_PERIOD_MS         1000    // One conversion cycle per second
#define TEMP_MAX_CONSECUTIVE_ERRORS   3       // Bad reads before a probe is declared faulty
#define TEMP_RESCAN_INTERVAL_MS       5000    // Retry detection of missing probes

// ==============================================================================
// FLOW SENSOR CALIBRATION (e.g. YF-S201)
// Frequency (Hz) = K_FACTOR * FlowRate (L/min)
// ==============================================================================
#define DEFAULT_FLOW_K_FACTOR         7.5f
#define DEFAULT_FLOW_K_FACTOR_COLD    7.5f    // Cold-loop meter (may differ from the hot side)
#define FLOW_DEBOUNCE_US              500     // Reject edges closer than this (max 2 kHz)
#define FLOW_ZERO_TIMEOUT_MS          2000    // No pulse for this long => 0 LPM

// ==============================================================================
// SAFETY THRESHOLDS & INTERLOCKS
// ==============================================================================
#define MIN_FLOW_LPM_FOR_HEATER       0.3f    // Flow permissive for the heater
#define MIN_COLD_FLOW_LPM             0.3f    // Cold-loop flow expected while the hot side runs
#define ZERO_FLOW_TIMEOUT_MS          1500    // Flow lost while heating for this long => latched trip
#define MAX_SAFE_TEMP_C               75.0f   // Emergency thermal trip point
#define MIN_PLAUSIBLE_TEMP_C          -10.0f  // Plausible range to catch wiring faults
#define MAX_PLAUSIBLE_TEMP_C          110.0f
#define SENSOR_DISCONNECT_VAL         -127.0f // DallasTemperature DEVICE_DISCONNECTED_C
#define SAFETY_BOOT_GRACE_MS          4000    // Allow first conversions before judging sensors
#define PUMP_DRY_RUN_TIMEOUT_MS       15000   // Pump running but no flow => warning

// ==============================================================================
// ACTUATOR SETTINGS
// ==============================================================================
#define RELAY_ACTIVE_LEVEL            HIGH    // HIGH if a HIGH input energises the relay, LOW if active-low

// Pumps: both are one-way drivers with a single PWM input (MOSFET / one-
// direction module). No direction pin, no decay mode.
#define PUMP_PWM_FREQ                 50    // Hz. Keep <= 5 kHz for L298N; DRV88xx is fine at 20 kHz
#define PUMP_PWM_RES                  10      // bits (0 - 1023)
#define PUMP_HOT_PWM_CHANNEL          0       // LEDC channel for the hot pump (Core v2 only)
#define PUMP_COLD_PWM_CHANNEL         1       // LEDC channel for the cold pump (Core v2 only)
#define PUMP_MIN_DUTY_PCT             25      // Duty where the pump just starts turning; 1% maps here
#define PUMP_KICKSTART_MS             250     // Full-duty pulse when starting from standstill (0 = off)
#define PUMP_RAMP_PCT_PER_S           50.0f   // Soft-start/stop slew rate

// Pump flow loop (PI, pump % from flow setpoint)
#define DEFAULT_FLOW_KP               8.0f    // % per LPM
#define DEFAULT_FLOW_KI               4.0f    // % per LPM*s

// Cold-side pump defaults (tuned independently in Settings).
#define PUMP_COLD_MIN_DUTY_PCT        25      // Duty where the cold pump just starts turning
#define DEFAULT_COLD_FLOW_KP          8.0f    // % per LPM
#define DEFAULT_COLD_FLOW_KI          4.0f    // % per LPM*s

// Heater
#define HEATER_CYCLE_WINDOW_MS        10000   // Time-proportioning window for TIME-PWM and PID
#define HEATER_MIN_PULSE_MS           1000    // Shorter on/off slices are absorbed (relay life)
#define HEATER_MIN_SWITCH_MS          5000    // Min time between relay toggles in THERMOSTATIC mode
#define DEFAULT_HEATER_TARGET_C       50.0f
#define DEFAULT_HEATER_HYST_C         1.0f
#define DEFAULT_HEATER_KP             8.0f    // % per C
#define DEFAULT_HEATER_KI             0.05f   // % per C*s
#define DEFAULT_HEATER_KD             0.0f    // % per C/s

// ==============================================================================
// NETWORK DEFAULTS
// ==============================================================================
#define DEFAULT_AP_SSID               "HeatExchanger-AP"
#define DEFAULT_AP_PASS               "12345678"
#define DEFAULT_AP_CHANNEL            1
#define DEFAULT_MDNS_HOST             "heatexchanger"
#define DEFAULT_WEB_PORT              80

// ==============================================================================
// GEMINI AI INTEGRATION DEFAULTS
// ==============================================================================
#define GEMINI_API_HOST               "generativelanguage.googleapis.com"
#define DEFAULT_GEMINI_MODEL          "gemini-3.6-flash"
#define DEFAULT_GEMINI_INTERVAL_MS    60000   // Periodic analysis interval when auto is enabled
#define GEMINI_HTTP_TIMEOUT_MS        45000   // Thinking models can take tens of seconds
#define GEMINI_TASK_STACK             16384   // bytes; TLS handshake needs a deep stack
#define GEMINI_REC_MAX_AGE_S          120     // Closed-loop ignores recommendations older than this
#define GEMINI_MAX_PUMP_STEP_PCT      15      // Closed-loop max pump change per recommendation
#define GEMINI_MAX_HEATER_STEP_PCT    20      // Closed-loop max heater duty change per recommendation

// ==============================================================================
// DATA LOGGER BUFFER SIZE
// ==============================================================================
#define MAX_LOG_RECORDS               600     // 10 minutes at 1 record/second
