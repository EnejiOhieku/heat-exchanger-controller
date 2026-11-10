#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "../config.h"
#include "web_dashboard.hpp"
#include "../core/SafetyManager.hpp"
#include "../core/Thermodynamics.hpp"
#include "../core/ProcessIO.hpp"
#include "../core/CommandExec.hpp"
#include "../core/SettingsStore.hpp"
#include "../hardware/ThermalSensors.hpp"
#include "../hardware/FlowSensor.hpp"
#include "../hardware/PumpController.hpp"
#include "../hardware/HeaterController.hpp"
#include "../sim/PlantSimulator.hpp"
#include "../ui/DataLogger.hpp"
#include "../ui/LcdManager.hpp"
#include "NetworkManager.hpp"
#include "GeminiClient.hpp"
#include "ChatBot.hpp"

// Buffers Print output and forwards it as HTTP chunks via sendContent().
// Writing straight to the WiFiClient after a CONTENT_LENGTH_UNKNOWN response
// bypasses the chunk framing and corrupts the download.
class ChunkedPrint : public Print {
public:
    explicit ChunkedPrint(WebServer& s) : _s(s), _len(0) {}
    size_t write(uint8_t c) override {
        _buf[_len++] = (char)c;
        if (_len == sizeof(_buf)) flush();
        return 1;
    }
    void flush() override {
        if (_len) _s.sendContent(_buf, _len);
        _len = 0;
    }
private:
    WebServer& _s;
    char _buf[1024];
    size_t _len;
};

class WebServerManager {
public:
    WebServerManager()
        : _server(DEFAULT_WEB_PORT) {}

    void init() {
        _server.on("/", HTTP_GET, [this]() { handleRoot(); });
        _server.on("/api/telemetry", HTTP_GET, [this]() { handleGetTelemetry(); });
        _server.on("/api/control", HTTP_POST, [this]() { handlePostControl(); });
        _server.on("/api/config", HTTP_POST, [this]() { handlePostConfig(); });
        _server.on("/api/ai/config", HTTP_POST, [this]() { handlePostAiConfig(); });
        _server.on("/api/ai/query", HTTP_POST, [this]() { handlePostAiQuery(); });
        _server.on("/api/chat", HTTP_GET, [this]() { handleGetChat(); });
        _server.on("/api/chat", HTTP_POST, [this]() { handlePostChat(); });
        _server.on("/api/chat/execute", HTTP_POST, [this]() { handlePostChatExecute(); });
        _server.on("/api/chat/clear", HTTP_POST, [this]() { Gemini.clearChat(); sendOk("Chat cleared"); });
        _server.on("/api/wifi/scan", HTTP_GET, [this]() { handleGetWifiScan(); });
        _server.on("/api/export.csv", HTTP_GET, [this]() { handleGetExportCsv(); });
        _server.on("/api/logging/clear", HTTP_POST, [this]() { Logger.clear(); sendOk("Log cleared"); });
        _server.onNotFound([this]() { _server.send(404, "text/plain", "404 Not Found"); });

        _server.begin();
        Serial.printf("[Web] Web server listening on port %d\n", DEFAULT_WEB_PORT);
    }

    void handleClient() {
        _server.handleClient();
    }

private:
    void handleRoot() {
        _server.sendHeader("Content-Encoding", "gzip");
        _server.sendHeader("Cache-Control", "no-cache");
        _server.send_P(200, "text/html", (PGM_P)PAGE_DASHBOARD_GZ, PAGE_DASHBOARD_GZ_LEN);
    }

    // ------------------------------------------------------------------
    void handleGetTelemetry() {
        const ThermodynamicData& d = Thermo.getData();

        JsonDocument doc;
        doc["uptime_s"] = millis() / 1000;
        doc["simulated"] = PV::simulated();
        doc["th_in"] = d.thIn;
        doc["th_out"] = d.thOut;
        doc["tc_in"] = d.tcIn;
        doc["tc_out"] = d.tcOut;
        doc["delta_th"] = d.deltaTh;
        doc["delta_tc"] = d.deltaTc;
        doc["thermo_valid"] = d.valid;

        doc["flow_lpm"] = d.flowRateLPM;
        doc["flow_cold_lpm"] = d.flowRateColdLPM;
        doc["total_liters"] = PV::totalLiters();
        doc["total_liters_cold"] = PV::totalLitersCold();
        doc["q_hot_w"] = d.qHotWatts;
        doc["q_cold_w"] = d.qColdWatts;
        doc["lmtd"] = d.lmtd;
        doc["ua"] = d.uaWPerK;
        doc["effectiveness"] = d.effectiveness;
        doc["heat_balance"] = d.heatBalance;

        JsonArray probes = doc["probes"].to<JsonArray>();
        for (uint8_t i = 0; i < NUM_PROBES; i++) {
            const TempProbe& p = Sensors.probe((ProbeIndex)i);
            JsonObject o = probes.add<JsonObject>();
            o["name"] = p.getName();
            o["pin"] = p.getPin();
            o["present"] = p.isPresent();
            o["valid"] = PV::valid((ProbeIndex)i);
            o["errors"] = p.getTotalErrors();
            char rom[17];
            p.getAddressHex(rom);
            o["rom"] = rom;
        }

        JsonObject pump = doc["pump"].to<JsonObject>();
        pump["pct"] = Pump.getSpeedPercent();
        pump["target_pct"] = Pump.getTargetSpeedPercent();
        pump["mode"] = (uint8_t)Pump.getMode();
        pump["flow_setpoint"] = Pump.getFlowSetpoint();
        pump["duty_raw"] = Pump.getDutyRaw();

        JsonObject cpump = doc["pump_cold"].to<JsonObject>();
        cpump["pct"] = ColdPump.getSpeedPercent();
        cpump["target_pct"] = ColdPump.getTargetSpeedPercent();
        cpump["mode"] = (uint8_t)ColdPump.getMode();
        cpump["flow_setpoint"] = ColdPump.getFlowSetpoint();
        cpump["duty_raw"] = ColdPump.getDutyRaw();

        JsonObject htr = doc["heater"].to<JsonObject>();
        htr["on"] = Heater.isRelayEnergized();
        htr["demand"] = Heater.isDemanding();
        htr["mode"] = (uint8_t)Heater.getMode();
        htr["manual"] = Heater.getManualState();
        htr["duty"] = Heater.getDutyPercent();
        htr["duty_setting"] = Heater.getManualDutySetting();
        htr["target"] = Heater.getTargetTemp();
        htr["permitted"] = Safety.isHeaterAllowed();

        JsonObject saf = doc["safety"].to<JsonObject>();
        saf["tripped"] = Safety.isTripped();
        saf["error"] = Safety.getErrorString();
        saf["code"] = (uint8_t)Safety.getError();
        saf["warnings"] = Safety.getWarnings();
        saf["trips"] = Safety.getTripCount();

        JsonObject net = doc["net"].to<JsonObject>();
        net["sta_connected"] = NetManager.isStaConnected();
        net["sta_ip"] = NetManager.getStaIP();
        net["ap_ip"] = NetManager.getApIP();
        net["ssid"] = NetManager.getStoredSsid();
        net["rssi"] = NetManager.isStaConnected() ? WiFi.RSSI() : 0;

        JsonObject sys = doc["sys"].to<JsonObject>();
        sys["heap"] = ESP.getFreeHeap();
        sys["lcd"] = Display.isOnline();
        sys["log_count"] = Logger.getCount();
        sys["log_capacity"] = Logger.getCapacity();
        sys["sim_mode"] = (uint8_t)Sim.getMode();
        sys["sim_faults"] = Sim.getFaults();
        sys["sim_fault_fallback"] = Sim.isFaultFallback();
        sys["sim_tank"] = Sim.getTankC();

        JsonObject cfg = doc["cfg"].to<JsonObject>();
        cfg["k_factor"] = FlowHot.getKFactor();
        cfg["cold_k_factor"] = FlowCold.getKFactor();
        cfg["hyst"] = Heater.getHysteresis();
        cfg["kp"] = Heater.getKp();
        cfg["ki"] = Heater.getKi();
        cfg["kd"] = Heater.getKd();
        cfg["control_probe"] = Heater.getControlProbe();
        cfg["pump_min_duty"] = Pump.getMinDutyPercent();
        cfg["flow_kp"] = Pump.getFlowKp();
        cfg["flow_ki"] = Pump.getFlowKi();
        cfg["cold_pump_min_duty"] = ColdPump.getMinDutyPercent();
        cfg["cold_flow_kp"] = ColdPump.getFlowKp();
        cfg["cold_flow_ki"] = ColdPump.getFlowKi();
        cfg["max_temp"] = MAX_SAFE_TEMP_C;

        GeminiRecommendation ai = Gemini.getRecommendation();
        JsonObject aiObj = doc["ai"].to<JsonObject>();
        aiObj["model"] = Gemini.getModel();
        aiObj["has_key"] = Gemini.hasApiKey();
        aiObj["key_hint"] = Gemini.getKeyHint();
        aiObj["auto_enabled"] = Gemini.isAutoAnalysisEnabled();
        aiObj["interval_ms"] = Gemini.getIntervalMs();
        aiObj["closed_loop"] = Gemini.isAutonomousControlEnabled();
        aiObj["chat_auto_exec"] = Gemini.isChatAutoExecute();
        aiObj["busy"] = Gemini.isBusy();
        aiObj["phase"] = Gemini.getPhase();
        aiObj["last_error"] = Gemini.getLastError();
        aiObj["latency_ms"] = Gemini.getLastLatencyMs();
        aiObj["requests"] = Gemini.getRequestCount();
        aiObj["errors"] = Gemini.getErrorCount();
        aiObj["chat_rev"] = Gemini.chatRevision();
        const char* why = Gemini.unavailableReason();
        aiObj["unavailable"] = (why && !Gemini.isBusy()) ? why : "";
        aiObj["has_rec"] = ai.valid;
        if (ai.valid) {
            aiObj["analysis"] = ai.analysis;
            aiObj["efficiency_score"] = ai.efficiencyScore;
            aiObj["recommended_pump_pct"] = ai.recommendedPumpPct;
            aiObj["recommended_cold_pump_pct"] = ai.recommendedColdPumpPct;
            aiObj["recommended_heater_duty"] = ai.recommendedHeaterDuty;
            aiObj["safety_warning"] = ai.safetyWarning;
            aiObj["suggested_action"] = ai.suggestedAction;
            aiObj["age_s"] = millis() / 1000 - ai.timeSec;
        }

        String response;
        serializeJson(doc, response);
        _server.send(200, "application/json", response);
    }

    // ------------------------------------------------------------------
    bool parseBody(JsonDocument& doc) {
        if (!_server.hasArg("plain")) {
            sendError(400, "Missing body");
            return false;
        }
        if (deserializeJson(doc, _server.arg("plain"))) {
            sendError(400, "Invalid JSON");
            return false;
        }
        return true;
    }

    void sendOk(const String& msg) {
        JsonDocument d;
        d["ok"] = true;
        d["message"] = msg;
        String s;
        serializeJson(d, s);
        _server.send(200, "application/json", s);
    }

    void sendError(int code, const String& msg) {
        JsonDocument d;
        d["ok"] = false;
        d["message"] = msg;
        String s;
        serializeJson(d, s);
        _server.send(code, "application/json", s);
    }

    // Runs a command and accumulates its outcome for the response.
    void run(const char* action, float value, const String& arg, bool& allOk, String& log) {
        String msg;
        bool ok = Commands::run(action, value, arg, msg, CMD_SRC_OPERATOR);
        allOk &= ok;
        if (log.length()) log += "; ";
        log += msg;
    }

    void handlePostControl() {
        JsonDocument doc;
        if (!parseBody(doc)) return;
        bool ok = true;
        String log;

        if (doc["estop"] | false) {
            run("estop", 0, "", ok, log);
            sendOk(log);
            return;
        }
        if (doc["reset_fault"] | false) run("reset_fault", 0, "", ok, log);
        if (doc["pump_stop"] | false) run("pump_stop", 0, "", ok, log);
        if (doc["pump_pct"].is<float>()) run("set_pump_pct", doc["pump_pct"].as<float>(), "", ok, log);
        if (doc["flow_setpoint"].is<float>()) run("set_flow_setpoint", doc["flow_setpoint"].as<float>(), "", ok, log);
        if (doc["cold_pump_stop"] | false) run("cold_pump_stop", 0, "", ok, log);
        if (doc["cold_pump_pct"].is<float>()) run("set_cold_pump_pct", doc["cold_pump_pct"].as<float>(), "", ok, log);
        if (doc["cold_flow_setpoint"].is<float>()) run("set_cold_flow_setpoint", doc["cold_flow_setpoint"].as<float>(), "", ok, log);
        if (doc["heater_mode"].is<int>()) run("set_heater_mode", doc["heater_mode"].as<int>(), "", ok, log);
        if (doc["heater_manual"].is<bool>()) run("set_heater_manual", doc["heater_manual"].as<bool>() ? 1 : 0, "", ok, log);
        if (doc["heater_duty"].is<float>()) run("set_heater_duty", doc["heater_duty"].as<float>(), "", ok, log);
        if (doc["target_temp"].is<float>()) run("set_target_temp", doc["target_temp"].as<float>(), "", ok, log);
        if (doc["swap_probes"].is<JsonArray>()) {
            JsonArray a = doc["swap_probes"].as<JsonArray>();
            if (a.size() == 2) {
                run("swap_probes", a[0].as<float>(), String(a[1].as<int>()), ok, log);
            } else {
                ok = false; log += "swap_probes expects [a, b]. ";
            }
        }
        if (doc["assign_probe"].is<JsonArray>()) {
            JsonArray a = doc["assign_probe"].as<JsonArray>();
            if (a.size() == 2) {
                run("assign_probe", a[0].as<float>(), a[1].as<const char*>(), ok, log);
            } else {
                ok = false; log += "assign_probe expects [slot, ROM]. ";
            }
        }

        if (ok) sendOk(log.length() ? log : String("No change"));
        else sendError(409, log);
    }

    void handlePostConfig() {
        JsonDocument doc;
        if (!parseBody(doc)) return;
        String log;
        bool ok = true;
        bool save = false;

        if (doc["wifi_ssid"].is<const char*>()) {
            NetManager.setStaCredentials(doc["wifi_ssid"].as<const char*>(), doc["wifi_pass"] | "");
            log += "Wi-Fi credentials saved, connecting. ";
        }
        if (doc["flow_k_factor"].is<float>()) {
            float k = doc["flow_k_factor"].as<float>();
            if (k > 0.1f && k < 500.0f) { FlowHot.setKFactor(k); save = true; } else { ok = false; log += "K-factor out of range. "; }
        }
        if (doc["cold_flow_k_factor"].is<float>()) {
            float k = doc["cold_flow_k_factor"].as<float>();
            if (k > 0.1f && k < 500.0f) { FlowCold.setKFactor(k); save = true; } else { ok = false; log += "Cold K-factor out of range. "; }
        }
        if (doc["hysteresis"].is<float>()) {
            if (Heater.setHysteresis(doc["hysteresis"].as<float>())) save = true; else { ok = false; log += "Hysteresis must be 0.2-5 C. "; }
        }
        if (doc["kp"].is<float>() || doc["ki"].is<float>() || doc["kd"].is<float>()) {
            Heater.setPidGains(doc["kp"] | Heater.getKp(), doc["ki"] | Heater.getKi(), doc["kd"] | Heater.getKd());
            save = true;
        }
        if (doc["control_probe"].is<int>()) {
            Heater.setControlProbe(doc["control_probe"].as<int>());
            save = true;
        }
        if (doc["pump_min_duty"].is<int>()) {
            Pump.setMinDutyPercent(doc["pump_min_duty"].as<int>());
            save = true;
        }
        if (doc["flow_kp"].is<float>() || doc["flow_ki"].is<float>()) {
            Pump.setFlowGains(doc["flow_kp"] | Pump.getFlowKp(), doc["flow_ki"] | Pump.getFlowKi());
            save = true;
        }
        if (doc["cold_pump_min_duty"].is<int>()) {
            ColdPump.setMinDutyPercent(doc["cold_pump_min_duty"].as<int>());
            save = true;
        }
        if (doc["cold_flow_kp"].is<float>() || doc["cold_flow_ki"].is<float>()) {
            ColdPump.setFlowGains(doc["cold_flow_kp"] | ColdPump.getFlowKp(),
                                  doc["cold_flow_ki"] | ColdPump.getFlowKi());
            save = true;
        }
        if (doc["sim_mode"].is<int>()) {
            String m;
            ok &= Commands::run("set_sim_mode", doc["sim_mode"].as<int>(), "", m, CMD_SRC_OPERATOR);
            log += m + ". ";
        }
        if (doc["sim_faults"].is<int>()) {
            Sim.setFaults(doc["sim_faults"].as<int>());
            log += "Simulated faults updated. ";
        }
        if (save) {
            Settings.save();
            log += "Settings saved. ";
        }
        if (ok) sendOk(log.length() ? log : String("No change"));
        else sendError(400, log);
    }

    void handlePostAiConfig() {
        JsonDocument doc;
        if (!parseBody(doc)) return;

        if (doc["api_key"].is<const char*>()) Gemini.setApiKey(doc["api_key"].as<const char*>());
        if (doc["model"].is<const char*>() && !Gemini.setModel(doc["model"].as<const char*>())) {
            sendError(400, "Invalid model id (expected e.g. gemini-3.6-flash)");
            return;
        }
        if (doc["auto_interval_ms"].is<int>()) {
            int interval = doc["auto_interval_ms"].as<int>();
            Gemini.setAutoAnalysis(interval > 0, interval > 0 ? interval : Gemini.getIntervalMs());
        }
        if (doc["closed_loop"].is<bool>()) Gemini.setAutonomousControl(doc["closed_loop"].as<bool>());
        if (doc["chat_auto_exec"].is<bool>()) Gemini.setChatAutoExecute(doc["chat_auto_exec"].as<bool>());
        sendOk("AI config updated");
    }

    void handlePostAiQuery() {
        if (Gemini.requestAnalysis(Chat.buildContext())) {
            JsonDocument d;
            d["ok"] = true;
            d["message"] = "Analysis queued";
            String s;
            serializeJson(d, s);
            _server.send(202, "application/json", s);
        } else {
            sendError(409, Gemini.unavailableReason() ? Gemini.unavailableReason() : "Unavailable");
        }
    }

    // ------------------------------------------------------------------
    void handleGetChat() {
        uint32_t since = _server.hasArg("since") ? _server.arg("since").toInt() : 0;
        JsonDocument doc;
        doc["rev"] = Gemini.chatRevision();
        doc["busy"] = Gemini.isBusy() && Gemini.currentJob() == GEMINI_JOB_CHAT;
        doc["phase"] = Gemini.getPhase();
        doc["auto_exec"] = Gemini.isChatAutoExecute();
        Gemini.chatToJson(doc["messages"].to<JsonArray>(), since);
        String s;
        serializeJson(doc, s);
        _server.send(200, "application/json", s);
    }

    void handlePostChat() {
        JsonDocument doc;
        if (!parseBody(doc)) return;
        const char* text = doc["message"] | "";
        if (!text[0]) {
            sendError(400, "Empty message");
            return;
        }
        Chat.handleUserMessage(text);
        sendOk("Received");
    }

    void handlePostChatExecute() {
        JsonDocument doc;
        if (!parseBody(doc)) return;
        String summary;
        bool ok = Chat.executeProposal(doc["id"] | 0, summary);
        if (ok) sendOk(summary);
        else sendError(409, summary);
    }

    // Non-blocking scan: first call starts it, later calls return results.
    void handleGetWifiScan() {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_FAILED) {
            WiFi.scanNetworks(true);
            _server.send(200, "application/json", "{\"scanning\":true}");
            return;
        }
        if (n == WIFI_SCAN_RUNNING) {
            _server.send(200, "application/json", "{\"scanning\":true}");
            return;
        }
        JsonDocument doc;
        doc["scanning"] = false;
        JsonArray arr = doc["networks"].to<JsonArray>();
        for (int i = 0; i < n; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["ssid"] = WiFi.SSID(i);
            o["rssi"] = WiFi.RSSI(i);
            o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
        }
        WiFi.scanDelete();
        String s;
        serializeJson(doc, s);
        _server.send(200, "application/json", s);
    }

    void handleGetExportCsv() {
        _server.sendHeader("Content-Disposition", "attachment; filename=\"heatexchanger_telemetry.csv\"");
        _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        _server.send(200, "text/csv", "");
        ChunkedPrint out(_server);
        Logger.streamCSV(out);
        out.flush();
        _server.sendContent("");   // terminating chunk
    }

    WebServer _server;
};

inline WebServerManager Web;
