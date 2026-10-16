#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "../config.h"
#include "web_dashboard.hpp"
#include "../core/SafetyManager.hpp"
#include "../core/Thermodynamics.hpp"
#include "../hardware/ThermalSensors.hpp"
#include "../hardware/FlowSensor.hpp"
#include "../hardware/PumpController.hpp"
#include "../hardware/HeaterController.hpp"
#include "NetworkManager.hpp"
#include "GeminiClient.hpp"
#include "../ui/DataLogger.hpp"

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
        _server.on("/api/export.csv", HTTP_GET, [this]() { handleGetExportCsv(); });
        _server.on("/api/logging/clear", HTTP_POST, [this]() { handlePostClearLog(); });
        _server.onNotFound([this]() { handleNotFound(); });

        _server.begin();
        Serial.printf("[Web] Web server listening on port %d\n", DEFAULT_WEB_PORT);
    }

    void handleClient() {
        _server.handleClient();
    }

private:
    void handleRoot() {
        _server.send_P(200, "text/html", PAGE_DASHBOARD);
    }

    void handleGetTelemetry() {
        const ThermodynamicData& d = Thermo.getData();
        const GeminiRecommendation& ai = Gemini.getLastRecommendation();

        JsonDocument doc;
        doc["th_in"] = d.thIn;
        doc["th_out"] = d.thOut;
        doc["tc_in"] = d.tcIn;
        doc["tc_out"] = d.tcOut;
        doc["delta_th"] = d.deltaTh;
        doc["delta_tc"] = d.deltaTc;

        doc["flow_lpm"] = d.flowRateLPM;
        doc["total_liters"] = Flow.getTotalLiters();
        doc["q_hot_w"] = d.qHotWatts;
        doc["q_hot_kw"] = d.qHotKW;
        doc["q_cold_w"] = d.qColdWatts;
        doc["q_cold_kw"] = d.qColdKW;
        doc["lmtd"] = d.lmtd;
        doc["effectiveness"] = d.effectiveness;
        doc["heat_balance"] = d.heatBalance;

        doc["pump_pct"] = Pump.getSpeedPercent();
        doc["pump_target_pct"] = Pump.getTargetSpeedPercent();
        doc["heater_on"] = Heater.isRelayEnergized();
        doc["heater_mode"] = (uint8_t)Heater.getMode();
        doc["heater_target_temp"] = Heater.getTargetTemp();

        doc["safety_tripped"] = Safety.isTripped();
        doc["safety_error"] = Safety.getErrorString();
        doc["safety_code"] = (uint8_t)Safety.getError();

        doc["sta_connected"] = NetManager.isStaConnected();
        doc["sta_ip"] = NetManager.getStaIP();
        doc["ap_ip"] = NetManager.getApIP();

        doc["log_count"] = Logger.getCount();
        doc["log_capacity"] = Logger.getCapacity();

        JsonObject aiObj = doc["ai"].to<JsonObject>();
        aiObj["has_rec"] = ai.hasValidRecommendation;
        if (ai.hasValidRecommendation) {
            aiObj["analysis"] = ai.analysis;
            aiObj["efficiency_score"] = ai.efficiencyScore;
            aiObj["recommended_pump_pwm"] = ai.recommendedPumpPwm;
            aiObj["recommended_heater_duty"] = ai.recommendedHeaterDuty;
            aiObj["safety_warning"] = ai.safetyWarning;
            aiObj["suggested_action"] = ai.suggestedAction;
        }

        String response;
        serializeJson(doc, response);
        _server.send(200, "application/json", response);
    }

    void handlePostControl() {
        if (!_server.hasArg("plain")) {
            _server.send(400, "application/json", "{\"error\":\"Missing body\"}");
            return;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, _server.arg("plain"));
        if (err) {
            _server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }

        if (doc["estop"].is<bool>() && doc["estop"].as<bool>()) {
            Safety.triggerEStop();
            Pump.stop();
            Heater.forceOff();
            _server.send(200, "application/json", "{\"status\":\"E-STOP TRIGGERED\"}");
            return;
        }

        if (doc["reset_fault"].is<bool>() && doc["reset_fault"].as<bool>()) {
            Safety.resetFault();
        }

        if (doc["pump_pct"].is<int>()) {
            uint8_t pct = (uint8_t)doc["pump_pct"].as<int>();
            Pump.setSpeedPercent(pct);
        }

        if (doc["heater_mode"].is<int>()) {
            HeaterMode mode = (HeaterMode)doc["heater_mode"].as<int>();
            Heater.setMode(mode);
        }

        if (doc["heater_manual"].is<bool>()) {
            Heater.setManualState(doc["heater_manual"].as<bool>());
        }

        if (doc["heater_duty"].is<int>()) {
            Heater.setDutyPercent((uint8_t)doc["heater_duty"].as<int>());
        }

        if (doc["target_temp"].is<float>()) {
            Heater.setTargetTemp(doc["target_temp"].as<float>());
        }

        _server.send(200, "application/json", "{\"status\":\"OK\"}");
    }

    void handlePostConfig() {
        if (!_server.hasArg("plain")) {
            _server.send(400, "application/json", "{\"error\":\"Missing body\"}");
            return;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, _server.arg("plain"));
        if (err) {
            _server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }

        if (doc["wifi_ssid"].is<const char*>()) {
            const char* ssid = doc["wifi_ssid"].as<const char*>();
            const char* pass = doc["wifi_pass"] | "";
            NetManager.setStaCredentials(ssid, pass);
        }

        if (doc["flow_k_factor"].is<float>()) {
            Flow.setKFactor(doc["flow_k_factor"].as<float>());
        }

        _server.send(200, "application/json", "{\"status\":\"Config updated\"}");
    }

    void handlePostAiConfig() {
        if (!_server.hasArg("plain")) {
            _server.send(400, "application/json", "{\"error\":\"Missing body\"}");
            return;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, _server.arg("plain"));
        if (err) {
            _server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }

        if (doc["api_key"].is<const char*>()) {
            Gemini.setApiKey(doc["api_key"].as<const char*>());
        }

        if (doc["auto_interval_ms"].is<int>()) {
            int interval = doc["auto_interval_ms"].as<int>();
            Gemini.setAutoAnalysis(interval > 0, interval > 0 ? interval : DEFAULT_GEMINI_INTERVAL_MS);
        }

        if (doc["closed_loop"].is<bool>()) {
            Gemini.setAutonomousControl(doc["closed_loop"].as<bool>());
        }

        _server.send(200, "application/json", "{\"status\":\"AI config updated\"}");
    }

    void handlePostAiQuery() {
        const ThermodynamicData& d = Thermo.getData();
        bool ok = Gemini.query(d, Pump.getSpeedPercent(), Heater.isRelayEnergized(), Heater.getTargetTemp());

        if (ok) {
            const GeminiRecommendation& rec = Gemini.getLastRecommendation();
            JsonDocument doc;
            doc["status"] = "OK";
            doc["analysis"] = rec.analysis;
            doc["score"] = rec.efficiencyScore;
            doc["recommended_pump"] = rec.recommendedPumpPwm;
            doc["recommended_heater"] = rec.recommendedHeaterDuty;
            doc["action"] = rec.suggestedAction;

            String res;
            serializeJson(doc, res);
            _server.send(200, "application/json", res);
        } else {
            String err = "{\"status\":\"ERROR\",\"message\":\"" + String(Gemini.getLastError()) + "\"}";
            _server.send(500, "application/json", err);
        }
    }

    void handleGetExportCsv() {
        WiFiClient client = _server.client();
        _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        _server.sendHeader("Content-Type", "text/csv");
        _server.sendHeader("Content-Disposition", "attachment; filename=\"heatexchanger_telemetry.csv\"");
        _server.sendHeader("Connection", "close");
        _server.send(200);

        Logger.streamCSV(client);
        client.stop();
    }

    void handlePostClearLog() {
        Logger.clear();
        _server.send(200, "application/json", "{\"status\":\"Log cleared\"}");
    }

    void handleNotFound() {
        _server.send(404, "text/plain", "404 Not Found");
    }

    WebServer _server;
};

inline WebServerManager Web;
