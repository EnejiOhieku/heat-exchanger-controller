#pragma once

#include <Arduino.h>

// ==============================================================================
// ESP32 HEAT EXCHANGER CONTROL SYSTEM - MASTER HEADER
// ==============================================================================

// 1. Configuration & Constants
#include "config.h"

// 2. Core Safety & Physics
#include "core/SafetyManager.hpp"
#include "core/Thermodynamics.hpp"

// 3. Physical Hardware Peripherals
#include "hardware/ThermalSensors.hpp"
#include "hardware/FlowSensor.hpp"
#include "hardware/PumpController.hpp"
#include "hardware/HeaterController.hpp"

// 4. Local Display & Logging
#include "ui/LcdManager.hpp"
#include "ui/DataLogger.hpp"

// 5. Dual Networking, Embedded Web Server & Gemini AI
#include "network/NetworkManager.hpp"
#include "network/GeminiClient.hpp"
#include "network/WebServerManager.hpp"
