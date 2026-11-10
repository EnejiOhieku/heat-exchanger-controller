#pragma once

#include <Arduino.h>
#include <vector>
#include "../config.h"
#include "../core/ProcessIO.hpp"
#include "../core/Thermodynamics.hpp"
#include "../core/SafetyManager.hpp"
#include "../core/CommandExec.hpp"
#include "../hardware/PumpController.hpp"
#include "../hardware/HeaterController.hpp"
#include "GeminiClient.hpp"

// Operator chat: a small offline command language that always works (no
// internet needed), with everything else forwarded to Gemini together with a
// live telemetry snapshot. Gemini-proposed commands run through
// Commands::run(CMD_SRC_AI) and only execute automatically if the operator
// enabled auto-execute; otherwise the UI shows them with an Execute button.
class ChatBot {
public:
    // Plain-text plant snapshot shared by chat and periodic analysis.
    String buildContext() const {
        const ThermodynamicData& d = Thermo.getData();
        char buf[1500];
        auto t = [](float v, char* o) { if (v <= SENSOR_DISCONNECT_VAL + 1) strcpy(o, "FAULT"); else snprintf(o, 10, "%.2f", v); return o; };
        char a[12], b[12], c[12], e[12];
        snprintf(buf, sizeof(buf),
                 "source=%s uptime_s=%lu\n"
                 "Th_in=%s Th_out=%s Tc_in=%s Tc_out=%s C\n"
                 "flow_hot=%.2f L/min  flow_cold=%.2f L/min  Q_hot=%.0f W  Q_cold=%.0f W  heat_balance=%.0f%%\n"
                 "LMTD=%.2f C  UA=%.1f W/K  effectiveness=%.1f%%\n"
                 "pump_hot: mode=%s speed=%u%% target=%u%% flow_setpoint=%.2f L/min\n"
                 "pump_cold: mode=%s speed=%u%% target=%u%% flow_setpoint=%.2f L/min\n"
                 "heater: mode=%s relay=%s demand=%s duty=%.0f%% setpoint=%.1f C control_probe=%s "
                 "PID kp=%.2f ki=%.3f kd=%.2f\n"
                 "safety: %s%s  warnings:%s%s%s%s\n"
                 "limits: over_temp_trip=%.0f C, min_flow_for_heater=%.2f L/min",
                 PV::simulated() ? "SIMULATION (virtual plant, hardware outputs off)" : "real hardware",
                 (unsigned long)(millis() / 1000),
                 t(d.thIn, a), t(d.thOut, b), t(d.tcIn, c), t(d.tcOut, e),
                 d.flowRateLPM, d.flowRateColdLPM, d.qHotWatts, d.qColdWatts, d.heatBalance,
                 d.lmtd, d.uaWPerK, d.effectiveness,
                 Pump.getMode() == PUMP_MODE_FLOW ? "flow_control" : "manual",
                 Pump.getSpeedPercent(), Pump.getTargetSpeedPercent(), Pump.getFlowSetpoint(),
                 ColdPump.getMode() == PUMP_MODE_FLOW ? "flow_control" : "manual",
                 ColdPump.getSpeedPercent(), ColdPump.getTargetSpeedPercent(), ColdPump.getFlowSetpoint(),
                 Heater.getModeShort(), Heater.isRelayEnergized() ? "ON" : "OFF",
                 Heater.isDemanding() ? "yes" : "no", Heater.getDutyPercent(), Heater.getTargetTemp(),
                 probeName(Heater.getControlProbe()), Heater.getKp(), Heater.getKi(), Heater.getKd(),
                 Safety.isTripped() ? "TRIPPED " : "normal", Safety.isTripped() ? Safety.getErrorString() : "",
                 (Safety.getWarnings() & SAFETY_WARN_NO_FLOW) ? " heater_waiting_for_flow" : "",
                 (Safety.getWarnings() & SAFETY_WARN_PUMP_DRY_RUN) ? " pump_running_no_flow" : "",
                 (Safety.getWarnings() & SAFETY_WARN_COLD_SENSOR) ? " cold_side_probe_fault" : "",
                 (Safety.getWarnings() & SAFETY_WARN_NO_COLD_FLOW) ? " cold_loop_no_flow" : "",
                 MAX_SAFE_TEMP_C, MIN_FLOW_LPM_FOR_HEATER);
        return String(buf);
    }

    // Entry point for a message typed by the operator.
    void handleUserMessage(String text) {
        text.trim();
        if (text.length() == 0) return;
        if (text.length() > 500) text = text.substring(0, 500);
        Gemini.addMessage(CHAT_USER, text);

        String reply;
        if (handleLocal(text, reply)) {
            Gemini.addMessage(CHAT_SYSTEM, reply);
            return;
        }
        const char* why = Gemini.unavailableReason();
        if (why) {
            Gemini.addMessage(CHAT_SYSTEM, String("AI unavailable: ") + why +
                              ". Offline commands still work - type 'help'.");
            return;
        }
        Gemini.requestChat(text, buildContext());
    }

    // Execute the proposed commands attached to an assistant message.
    bool executeProposal(uint32_t msgId, String& summary) {
        std::vector<ChatCommand> cmds;
        if (!Gemini.getPendingCommands(msgId, cmds)) {
            summary = "Nothing to execute (already run or no commands)";
            return false;
        }
        bool allOk = true;
        summary = "";
        for (const auto& c : cmds) {
            String msg;
            bool ok = Commands::run(c.action, c.value, c.arg, msg, CMD_SRC_AI);
            allOk &= ok;
            if (summary.length()) summary += "; ";
            summary += (ok ? "OK " : "FAILED ") + msg;
        }
        Gemini.markExecuted(msgId, summary);
        Gemini.addMessage(CHAT_SYSTEM, String(allOk ? "Executed: " : "Executed with errors: ") + summary);
        return allOk;
    }

    // Loop task: kick periodic analysis and consume finished Gemini jobs.
    void update() {
        if (Gemini.autoAnalysisDue()) Gemini.requestAnalysis(buildContext());

        bool ok = false;
        GeminiJob done = Gemini.takeCompletedJob(&ok);
        if (done == GEMINI_JOB_CHAT && ok && Gemini.isChatAutoExecute()) {
            uint32_t id = Gemini.lastAssistantMessageId();
            String summary;
            std::vector<ChatCommand> tmp;
            if (Gemini.getPendingCommands(id, tmp)) executeProposal(id, summary);
        }
        if (done == GEMINI_JOB_ANALYZE && ok && Gemini.isAutonomousControlEnabled()) {
            applyClosedLoop();
        }
    }

private:
    static const char* probeName(uint8_t p) {
        static const char* n[] = { "Th_in", "Th_out", "Tc_in", "Tc_out" };
        return p < 4 ? n[p] : "?";
    }

    // Guard-rails for AI closed-loop control:
    //  - each recommendation is applied once (sequence number), and only if fresh
    //  - never while tripped or when the model itself flags a safety concern
    //  - pump only in manual mode (does not fight the flow PI loop)
    //  - heater duty only in TIME-PWM mode; AI never switches the heater on from OFF
    //  - changes are slew-limited per recommendation
    void applyClosedLoop() {
        GeminiRecommendation rec = Gemini.getRecommendation();
        if (!rec.valid || rec.seq == _appliedSeq) return;
        _appliedSeq = rec.seq;

        if ((millis() / 1000) - rec.timeSec > GEMINI_REC_MAX_AGE_S) return;
        if (Safety.isTripped()) {
            Gemini.addMessage(CHAT_SYSTEM, "AI closed-loop: skipped (safety trip active)");
            return;
        }
        String warn = rec.safetyWarning;
        warn.toUpperCase();
        if (warn.length() && warn != "NONE") {
            Gemini.addMessage(CHAT_SYSTEM, "AI closed-loop: skipped, model flagged: " + rec.safetyWarning);
            return;
        }

        String applied;
        String msg;
        if (Pump.getMode() == PUMP_MODE_MANUAL) {
            int cur = Pump.getTargetSpeedPercent();
            int tgt = constrain((int)rec.recommendedPumpPct, cur - GEMINI_MAX_PUMP_STEP_PCT, cur + GEMINI_MAX_PUMP_STEP_PCT);
            if (Heater.getMode() != HEATER_MODE_OFF) tgt = max(tgt, 20);   // keep flow while heating
            if (tgt != cur && Commands::run("set_pump_pct", tgt, "", msg, CMD_SRC_AI)) applied += msg + ". ";
        }
        if (ColdPump.getMode() == PUMP_MODE_MANUAL) {
            int cur = ColdPump.getTargetSpeedPercent();
            int tgt = constrain((int)rec.recommendedColdPumpPct, cur - GEMINI_MAX_PUMP_STEP_PCT, cur + GEMINI_MAX_PUMP_STEP_PCT);
            if (Heater.getMode() != HEATER_MODE_OFF) tgt = max(tgt, 20);   // keep cold flow while heating
            if (tgt != cur && Commands::run("set_cold_pump_pct", tgt, "", msg, CMD_SRC_AI)) applied += msg + ". ";
        }
        if (Heater.getMode() == HEATER_MODE_TIME_PWM) {
            int cur = (int)Heater.getManualDutySetting();
            int tgt = constrain((int)rec.recommendedHeaterDuty, cur - GEMINI_MAX_HEATER_STEP_PCT, cur + GEMINI_MAX_HEATER_STEP_PCT);
            if (tgt != cur && Commands::run("set_heater_duty", tgt, "", msg, CMD_SRC_AI)) applied += msg + ". ";
        }
        if (applied.length()) Gemini.addMessage(CHAT_SYSTEM, "AI closed-loop applied: " + applied);
    }

    static bool parseNumber(const String& s, float& out) {
        if (s.length() == 0) return false;
        char* end = nullptr;
        out = strtof(s.c_str(), &end);
        return end && *end == '\0';
    }

    // Offline commands. Returns true if the text was handled locally.
    bool handleLocal(const String& raw, String& reply) {
        String s = raw;
        s.toLowerCase();
        s.trim();
        int sp = s.indexOf(' ');
        String verb = sp < 0 ? s : s.substring(0, sp);
        String rest = sp < 0 ? String() : s.substring(sp + 1);
        rest.trim();
        rest.replace("%", "");
        rest.replace("c", "");      // "55c" -> "55" (harmless for word args below: matched first)
        float num = 0;
        bool hasNum = parseNumber(rest, num);
        String word = sp < 0 ? String() : s.substring(sp + 1);
        word.trim();
        String msg;

        if (verb == "help" || verb == "?") {
            reply = "Offline commands: status | pump <0-100> | pump off | flow <L/min> | "
                    "coldpump <0-100> | coldpump off | coldflow <L/min> | "
                    "heater off|manual|thermo|pwm|pid | heater on (manual mode) | setpoint <C> | duty <0-100> | "
                    "swap <a> <b> | assign <slot> <ROM> | estop | reset | sim on|off|auto | clear log. "
                    "Anything else is sent to Gemini.";
            return true;
        }
        if (verb == "status") {
            reply = buildContext();
            return true;
        }
        if (verb == "estop" || s == "stop all" || s == "emergency stop") {
            Commands::run("estop", 0, "", reply, CMD_SRC_OPERATOR);
            return true;
        }
        if (verb == "reset") {
            Commands::run("reset_fault", 0, "", reply, CMD_SRC_OPERATOR);
            return true;
        }
        if (verb == "pump") {
            if (word == "off" || word == "stop") { Commands::run("pump_stop", 0, "", reply, CMD_SRC_OPERATOR); return true; }
            if (hasNum) { Commands::run("set_pump_pct", num, "", reply, CMD_SRC_OPERATOR); return true; }
        }
        if (verb == "flow" && hasNum) {
            Commands::run("set_flow_setpoint", num, "", reply, CMD_SRC_OPERATOR);
            return true;
        }
        if (verb == "coldpump") {
            if (word == "off" || word == "stop") { Commands::run("cold_pump_stop", 0, "", reply, CMD_SRC_OPERATOR); return true; }
            if (hasNum) { Commands::run("set_cold_pump_pct", num, "", reply, CMD_SRC_OPERATOR); return true; }
        }
        if (verb == "coldflow" && hasNum) {
            Commands::run("set_cold_flow_setpoint", num, "", reply, CMD_SRC_OPERATOR);
            return true;
        }
        if (verb == "heater") {
            if (word == "on" || word == "off") {
                if (word == "off") {
                    Commands::run("set_heater_mode", 0, "off", reply, CMD_SRC_OPERATOR);
                } else {
                    Commands::run("set_heater_mode", 0, "manual", msg, CMD_SRC_OPERATOR);
                    Commands::run("set_heater_manual", 1, "on", reply, CMD_SRC_OPERATOR);
                    if (!Safety.isHeaterAllowed()) reply += " (held off by interlock: needs flow / no trip)";
                }
                return true;
            }
            if (Commands::heaterModeFromName(word) >= 0) {
                Commands::run("set_heater_mode", 0, word, reply, CMD_SRC_OPERATOR);
                return true;
            }
        }
        if ((verb == "setpoint" || verb == "target" || verb == "sp") && hasNum) {
            Commands::run("set_target_temp", num, "", reply, CMD_SRC_OPERATOR);
            return true;
        }
        if (verb == "duty" && hasNum) {
            Commands::run("set_heater_duty", num, "", reply, CMD_SRC_OPERATOR);
            return true;
        }
        if (verb == "sim" && (word == "on" || word == "off" || word == "auto")) {
            Commands::run("set_sim_mode", 0, word, reply, CMD_SRC_OPERATOR);
            return true;
        }
        if (s == "clear log") {
            Commands::run("clear_log", 0, "", reply, CMD_SRC_OPERATOR);
            return true;
        }
        if (verb == "swap") {
            int a = 0, b = 0;
            if (sscanf(word.c_str(), "%d %d", &a, &b) == 2) {
                Commands::run("swap_probes", (float)a, String(b), reply, CMD_SRC_OPERATOR);
                return true;
            }
        }
        if (verb == "assign") {
            int split = word.indexOf(' ');
            if (split > 0) {
                int slot = word.substring(0, split).toInt();
                String rom = word.substring(split + 1);
                rom.trim();
                Commands::run("assign_probe", (float)slot, rom, reply, CMD_SRC_OPERATOR);
                return true;
            }
        }
        return false;
    }

    uint32_t _appliedSeq = 0;
};

inline ChatBot Chat;
