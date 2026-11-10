#pragma once

#include <Arduino.h>
#include "../config.h"
#include "SafetyManager.hpp"
#include "SettingsStore.hpp"
#include "../hardware/ThermalSensors.hpp"
#include "../hardware/PumpController.hpp"
#include "../hardware/HeaterController.hpp"
#include "../sim/PlantSimulator.hpp"
#include "../ui/DataLogger.hpp"

enum CommandSource : uint8_t {
    CMD_SRC_OPERATOR = 0,   // Web UI buttons / typed chat command
    CMD_SRC_AI = 1          // Gemini chat or closed-loop
};

// Every actuator command from every source (REST, chat, AI closed loop) goes
// through here so validation and limits live in one place. Runs on the loop
// task only. Safety interlocks still sit below this layer.
namespace Commands {

// Switching between real and simulated plant always drops to a safe state:
// otherwise a heater that was "on" in the model would energise the real relay.
// A hardware fault (missing or invalid probe) forces the virtual plant when the
// operator selected AUTO; the same call returns to hardware once it clears.
inline bool hardwareFaultActive() {
    if (millis() < SAFETY_BOOT_GRACE_MS) return Sensors.getDeviceCount() < NUM_PROBES;
    return Sensors.faultMask() != 0 || Sensors.getDeviceCount() < NUM_PROBES;
}

inline void applySimMode(uint8_t mode) {
    bool wasActive = Sim.isActive();
    bool fault = hardwareFaultActive();
    Sim.setMode(mode);
    Sim.resolve(Sensors.getDeviceCount(), fault);
    Sim.setFaultFallback(Sim.isActive() && fault && Sensors.getDeviceCount() > 0);
    if (Sim.isActive() != wasActive) {
        Heater.forceOff();
        Pump.stop();
        ColdPump.stop();
    }
    Heater.setOutputsInhibited(Sim.isActive());
    Pump.setOutputsInhibited(Sim.isActive());
    ColdPump.setOutputsInhibited(Sim.isActive());
}

// Called periodically from the loop. Only AUTO reacts: enter the virtual plant
// on any hardware fault and return to real hardware automatically when clear.
inline void updateSimSupervisor() {
    if (Sim.getMode() != SIM_MODE_AUTO) return;
    bool want = hardwareFaultActive() || (Sensors.getDeviceCount() == 0);
    if (want != Sim.isActive()) applySimMode(SIM_MODE_AUTO);
}

inline int heaterModeFromName(const String& n) {
    if (n == "off") return HEATER_MODE_OFF;
    if (n == "manual") return HEATER_MODE_MANUAL;
    if (n == "thermostatic" || n == "thermo") return HEATER_MODE_THERMOSTATIC;
    if (n == "time_pwm" || n == "pwm") return HEATER_MODE_TIME_PWM;
    if (n == "pid") return HEATER_MODE_PID;
    return -1;
}

// action: snake_case verb, value: numeric argument, arg: string argument.
// Returns true on success; msg receives a human-readable outcome.
inline bool run(String action, float value, String arg, String& msg, CommandSource src) {
    action.toLowerCase();
    arg.toLowerCase();
    bool ai = (src == CMD_SRC_AI);

    if (action == "estop") {
        Safety.triggerEStop();
        Pump.stop();
        ColdPump.stop();
        Heater.forceOff();
        msg = "E-STOP: heater off, pumps stopped";
        return true;
    }
    if (action == "reset_fault") {
        if (ai) { msg = "Refused: only an operator can reset a safety trip"; return false; }
        bool ok = Safety.resetFault();
        msg = ok ? "Safety trip cleared" : String("Cannot reset: ") + Safety.getErrorString() + " condition still present";
        return ok;
    }
    if (Safety.getError() == SAFETY_ERR_ESTOP && action != "pump_stop" && action != "set_heater_mode") {
        msg = "Refused: E-STOP active, reset it first";
        return false;
    }
    if (action == "set_pump_pct") {
        if (value < 0 || value > 100) { msg = "Pump % must be 0-100"; return false; }
        Pump.setSpeedPercent(value);
        msg = "Pump set to " + String(value, 0) + "%";
        return true;
    }
    if (action == "set_flow_setpoint") {
        if (value < 0 || value > 30) { msg = "Flow setpoint must be 0-30 L/min"; return false; }
        Pump.setFlowSetpoint(value);
        msg = "Pump in flow control, setpoint " + String(value, 2) + " L/min";
        return true;
    }
    if (action == "pump_stop") {
        Pump.setSpeedPercent(0);
        msg = "Pump ramping to stop";
        return true;
    }
    if (action == "set_cold_pump_pct") {
        if (value < 0 || value > 100) { msg = "Cold pump % must be 0-100"; return false; }
        ColdPump.setSpeedPercent(value);
        msg = "Cold pump set to " + String(value, 0) + "%";
        return true;
    }
    if (action == "set_cold_flow_setpoint") {
        if (value < 0 || value > 30) { msg = "Cold flow setpoint must be 0-30 L/min"; return false; }
        ColdPump.setFlowSetpoint(value);
        msg = "Cold pump in flow control, setpoint " + String(value, 2) + " L/min";
        return true;
    }
    if (action == "cold_pump_stop") {
        ColdPump.setSpeedPercent(0);
        msg = "Cold pump ramping to stop";
        return true;
    }
    if (action == "set_heater_mode") {
        int m = arg.length() ? heaterModeFromName(arg) : (int)value;
        if (m < 0 || !Heater.setMode((uint8_t)m)) { msg = "Unknown heater mode"; return false; }
        if (m == HEATER_MODE_MANUAL) Heater.setManualState(false);   // never arm straight to ON
        msg = String("Heater mode ") + Heater.getModeShort();
        return true;
    }
    if (action == "set_heater_manual") {
        bool on = (arg == "on" || arg == "true" || value >= 0.5f);
        if (on && Heater.getMode() != HEATER_MODE_MANUAL) {
            msg = "Switch heater to MANUAL mode first";
            return false;
        }
        Heater.setManualState(on);
        msg = String("Manual heater ") + (on ? "ON" : "OFF");
        return true;
    }
    if (action == "set_target_temp") {
        if (!Heater.setTargetTemp(value)) {
            msg = "Setpoint must be 10-" + String(MAX_SAFE_TEMP_C - 2.0f, 0) + " C";
            return false;
        }
        Settings.save();
        msg = "Heater setpoint " + String(value, 1) + " C";
        return true;
    }
    if (action == "set_heater_duty") {
        if (value < 0 || value > 100) { msg = "Duty must be 0-100%"; return false; }
        Heater.setDutyPercent(value);
        msg = "Time-PWM duty " + String(value, 0) + "%";
        return true;
    }
    if (action == "set_sim_mode") {
        if (ai) { msg = "Refused: simulation mode is operator-only"; return false; }
        int m = (arg == "off") ? SIM_MODE_OFF : (arg == "on") ? SIM_MODE_ON : (arg == "auto") ? SIM_MODE_AUTO : (int)value;
        if (m < SIM_MODE_OFF || m > SIM_MODE_AUTO) { msg = "Sim mode must be off/on/auto"; return false; }
        applySimMode(m);
        Settings.save();
        msg = String("Simulation ") + (Sim.isActive() ? "ACTIVE" : "inactive");
        return true;
    }
    if (action == "clear_log") {
        Logger.clear();
        msg = "Log buffer cleared";
        return true;
    }
    if (action == "assign_probe") {
        int slot = (int)value;
        if (!Sensors.assignSlot((uint8_t)slot, arg.c_str())) {
            msg = "assign_probe needs slot 0-3 and a 16-character ROM";
            return false;
        }
        msg = "Probe slot " + String(slot) + " mapped to " + arg;
        return true;
    }
    if (action == "swap_probes") {
        int a = (int)value;
        int b = arg.toInt();
        if (a < 0 || a >= NUM_PROBES || b < 0 || b >= NUM_PROBES || a == b) {
            msg = "swap_probes needs two different slots 0-3";
            return false;
        }
        Sensors.swapSlots((uint8_t)a, (uint8_t)b);
        msg = "Swapped probe slots " + String(a) + " <-> " + String(b);
        return true;
    }
    msg = "Unknown command: " + action;
    return false;
}

}  // namespace Commands
