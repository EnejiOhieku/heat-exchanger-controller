#pragma once

#include <Arduino.h>

// ==============================================================================
// PIN DEFINITIONS (ESP32 DevKitC)
// ==============================================================================
#define PIN_LCD_SDA          21
#define PIN_LCD_SCL          22
#define PIN_ONEWIRE_BUS      4
#define PIN_FLOW_SENSOR      27
#define PIN_RELAY_HEATER     25
#define PIN_PUMP_IN1         19      // Motor Driver PWM Input 1 (Forward PWM)
#define PIN_PUMP_IN2         23      // Motor Driver PWM Input 2 (Reverse PWM / Held LOW)
#define PIN_STATUS_LED       2
// Note: GPIO 18 (formerly PIN_PUMP_ENA) is now unassigned and available for expansion.

// ==============================================================================
// THERMODYNAMIC CONSTANTS
// ==============================================================================
#define WATER_SPECIFIC_HEAT  4184.0f  // J / (kg * K)
#define WATER_DENSITY        1.0f     // kg / L

// ==============================================================================
// FLOW SENSOR CALIBRATION (e.g. YF-S201)
// Frequency (Hz) = K_FACTOR * FlowRate (L/min)
// ==============================================================================
#define DEFAULT_FLOW_K_FACTOR 7.5f

// ==============================================================================
// SAFETY THRESHOLDS & INTERLOCKS
// ==============================================================================
#define MIN_FLOW_LPM_FOR_HEATER       0.3f    // Flow required to operate heater
#define ZERO_FLOW_TIMEOUT_MS          1500    // Cutoff delay if flow drops below min
#define MAX_SAFE_TEMP_C               75.0f   // Emergency thermal trip point
#define MIN_PLAUSIBLE_TEMP_C          -10.0f  // Plausible range to catch disconnects
#define MAX_PLAUSIBLE_TEMP_C          110.0f  // Plausible range
#define SENSOR_DISCONNECT_VAL         -127.0f // Dallas 1-Wire error code
#define SENSOR_POR_RESET_VAL          85.0f   // Dallas 1-Wire power-on reset state

// ==============================================================================
// ACTUATOR SETTINGS
// ==============================================================================
#define RELAY_ACTIVE_LEVEL            HIGH    // Set to HIGH if HIGH turns relay ON, LOW if active-low
#define PUMP_PWM_FREQ                 5000    // 5 kHz PWM (audible noise suppression & motor efficiency)
#define PUMP_PWM_RES                  8       // 8-bit resolution (0 - 255)
#define PUMP_PWM_CHANNEL_1            0       // ESP32 LEDC channel for IN1 (Core v2 fallback)
#define PUMP_PWM_CHANNEL_2            1       // ESP32 LEDC channel for IN2 (Core v2 fallback)
#define PUMP_INVERT_ROTATION          false   // Set true to swap IN1/IN2 if physical pump rotates backwards
#define HEATER_CYCLE_WINDOW_MS        5000    // 5-second slow-cycle time-proportional PWM window

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
#define GEMINI_API_PORT               443
#define DEFAULT_GEMINI_MODEL          "gemini-2.0-flash"
#define DEFAULT_GEMINI_INTERVAL_MS    30000   // 30-second periodic analysis if auto-enabled

// ==============================================================================
// DATA LOGGER BUFFER SIZE
// ==============================================================================
#define MAX_LOG_RECORDS               600     // 10 minutes at 1 record/second
