#pragma once

#include <Arduino.h>

// ==============================================================================
// ESP32 HEAT EXCHANGER CONTROL SYSTEM - MASTER HEADER
// ==============================================================================

// 1. Configuration & Constants
#include "config.h"

// 2. Core Safety, Control & Physics
#include "core/Pid.hpp"
#include "core/SafetyManager.hpp"
#include "core/Thermodynamics.hpp"

// 3. Physical Hardware Peripherals & Virtual Plant
#include "hardware/ThermalSensors.hpp"
#include "hardware/FlowSensor.hpp"
#include "hardware/PumpController.hpp"
#include "hardware/HeaterController.hpp"
#include "sim/PlantSimulator.hpp"
#include "core/ProcessIO.hpp"
#include "core/SettingsStore.hpp"

// 4. Local Display & Logging
#include "ui/LcdManager.hpp"
#include "ui/DataLogger.hpp"

// 5. Command layer (shared by REST, chat and AI)
#include "core/CommandExec.hpp"

// 6. Dual Networking, Embedded Web Server, Gemini AI & Chat
#include "network/NetworkManager.hpp"
#include "network/GeminiClient.hpp"
#include "network/ChatBot.hpp"
#include "network/WebServerManager.hpp"
