#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "../config.h"
#include "../core/Thermodynamics.hpp"
#include "NetworkManager.hpp"

struct GeminiRecommendation {
    String analysis;
    float efficiencyScore;
    uint8_t recommendedPumpPwm;
    uint8_t recommendedHeaterDuty;
    String safetyWarning;
    String suggestedAction;
    uint32_t lastAnalysisTimeSec;
    bool hasValidRecommendation;
};

class GeminiClient {
public:
    GeminiClient()
        : _autoAnalysis(false),
          _analysisIntervalMs(DEFAULT_GEMINI_INTERVAL_MS),
          _lastQueryTime(0),
          _autonomousControl(false),
          _isBusy(false) {
        memset(_lastError, 0, sizeof(_lastError));
        _recommendation.hasValidRecommendation = false;
        _recommendation.efficiencyScore = 0.0f;
        _recommendation.recommendedPumpPwm = 50;
        _recommendation.recommendedHeaterDuty = 0;
        _recommendation.lastAnalysisTimeSec = 0;
    }

    void init() {
        _prefs.begin("hx_ai", false);
        _apiKey = _prefs.getString("key", "");
        _model = _prefs.getString("model", DEFAULT_GEMINI_MODEL);
        _autoAnalysis = _prefs.getBool("auto", false);
        _autonomousControl = _prefs.getBool("ctrl", false);

        Serial.printf("[Gemini] Initialized. Model: %s, API Key: %s, Auto-Analysis: %s, Closed-Loop: %s\n",
                      _model.c_str(),
                      _apiKey.length() > 0 ? "CONFIGURED" : "NOT SET",
                      _autoAnalysis ? "YES" : "NO",
                      _autonomousControl ? "YES" : "NO");
    }

    void setApiKey(const char* key) {
        _apiKey = key ? key : "";
        _prefs.putString("key", _apiKey);
    }
    String getApiKey() const { return _apiKey; }
    bool hasApiKey() const { return _apiKey.length() > 10; }

    void setModel(const char* model) {
        if (model && strlen(model) > 0) {
            _model = model;
            _prefs.putString("model", _model);
            Serial.printf("[Gemini] Model updated to: %s\n", _model.c_str());
        }
    }
    String getModel() const { return _model; }

    void setAutoAnalysis(bool enabled, uint32_t intervalMs = DEFAULT_GEMINI_INTERVAL_MS) {
        _autoAnalysis = enabled;
        _analysisIntervalMs = intervalMs;
        _prefs.putBool("auto", _autoAnalysis);
    }
    bool isAutoAnalysisEnabled() const { return _autoAnalysis; }

    void setAutonomousControl(bool enabled) {
        _autonomousControl = enabled;
        _prefs.putBool("ctrl", _autonomousControl);
    }
    bool isAutonomousControlEnabled() const { return _autonomousControl; }

    const GeminiRecommendation& getLastRecommendation() const { return _recommendation; }
    const char* getLastError() const { return _lastError; }

    void update(const ThermodynamicData& data, uint8_t currentPumpPct, bool heaterOn, float targetTemp) {
        if (!_autoAnalysis || !hasApiKey() || !NetManager.isStaConnected() || _isBusy) {
            return;
        }

        uint32_t now = millis();
        if (now - _lastQueryTime >= _analysisIntervalMs) {
            _lastQueryTime = now;
            query(data, currentPumpPct, heaterOn, targetTemp);
        }
    }

    bool query(const ThermodynamicData& data, uint8_t currentPumpPct, bool heaterOn, float targetTemp) {
        if (!hasApiKey()) {
            snprintf(_lastError, sizeof(_lastError), "Gemini API key is not set");
            return false;
        }
        if (!NetManager.isStaConnected()) {
            snprintf(_lastError, sizeof(_lastError), "Wi-Fi Station is not connected to Internet");
            return false;
        }

        _isBusy = true;
        snprintf(_lastError, sizeof(_lastError), "Analyzing...");

        char promptBuf[1024];
        snprintf(promptBuf, sizeof(promptBuf),
                 "Act as an industrial thermodynamic process engineer. "
                 "Analyze this heat exchanger telemetry:\n"
                 "- Hot Inlet: %.2f C, Hot Outlet: %.2f C (Delta Th = %.2f C)\n"
                 "- Cold Inlet: %.2f C, Cold Outlet: %.2f C (Delta Tc = %.2f C)\n"
                 "- Flow Rate: %.2f LPM (Mass Flow: %.4f kg/s)\n"
                 "- Hot Side Power: %.1f W (%.2f kW)\n"
                 "- Cold Side Absorption: %.1f W (%.2f kW)\n"
                 "- LMTD: %.2f C, Effectiveness: %.1f%%\n"
                 "- Actuator Status: Pump PWM = %u%%, Heater = %s, Target Temp = %.1f C\n\n"
                 "Return ONLY valid JSON (no markdown formatting, no backticks):\n"
                 "{\"analysis\":\"short assessment\",\"efficiency_score\":number_0_to_100,"
                 "\"recommended_pump_pwm\":integer_0_to_100,\"recommended_heater_duty\":integer_0_to_100,"
                 "\"safety_warning\":\"NONE or risk description\",\"action\":\"MAINTAIN|INCREASE_FLOW|DECREASE_FLOW|INCREASE_HEAT|DECREASE_HEAT\"}",
                 data.thIn, data.thOut, data.deltaTh,
                 data.tcIn, data.tcOut, data.deltaTc,
                 data.flowRateLPM, data.massFlowKgS,
                 data.qHotWatts, data.qHotKW,
                 data.qColdWatts, data.qColdKW,
                 data.lmtd, data.effectiveness,
                 currentPumpPct, heaterOn ? "ON" : "OFF", targetTemp);

        String responseJson;
        bool reqOk = sendRequest(String(promptBuf), responseJson);

        if (reqOk) {
            bool parseOk = parseGeminiResponse(responseJson);
            _isBusy = false;
            return parseOk;
        }

        _isBusy = false;
        return false;
    }

private:
    bool sendRequest(const String& promptText, String& responseJson) {
        WiFiClientSecure client;
        client.setInsecure();
        client.setTimeout(15000);

        Serial.printf("[Gemini] Connecting to %s:%d (Model: %s)...\n", GEMINI_API_HOST, GEMINI_API_PORT, _model.c_str());
        if (!client.connect(GEMINI_API_HOST, GEMINI_API_PORT)) {
            snprintf(_lastError, sizeof(_lastError), "Connection to Gemini host failed");
            Serial.println("[Gemini] Connection failed!");
            return false;
        }

        JsonDocument doc;
        JsonObject content = doc["contents"].add<JsonObject>();
        JsonObject part = content["parts"].add<JsonObject>();
        part["text"] = promptText;

        String requestBody;
        serializeJson(doc, requestBody);

        String path = "/v1beta/models/" + _model + ":generateContent?key=" + _apiKey;

        client.print("POST " + path + " HTTP/1.1\r\n");
        client.print("Host: " + String(GEMINI_API_HOST) + "\r\n");
        client.print("User-Agent: ESP32-HeatExchanger/1.0\r\n");
        client.print("Content-Type: application/json\r\n");
        client.print("Connection: close\r\n");
        client.print("Content-Length: " + String(requestBody.length()) + "\r\n\r\n");
        client.print(requestBody);

        while (client.connected()) {
            String line = client.readStringUntil('\n');
            if (line == "\r") {
                break;
            }
        }

        responseJson = client.readString();
        client.stop();

        if (responseJson.length() == 0) {
            snprintf(_lastError, sizeof(_lastError), "Empty response from Gemini");
            return false;
        }

        return true;
    }

    bool parseGeminiResponse(const String& rawResponse) {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, rawResponse);

        if (error) {
            snprintf(_lastError, sizeof(_lastError), "JSON parse error: %s", error.c_str());
            Serial.printf("[Gemini] Parse error: %s\n", error.c_str());
            return false;
        }

        const char* textContent = doc["candidates"][0]["content"]["parts"][0]["text"];
        if (!textContent) {
            snprintf(_lastError, sizeof(_lastError), "No candidates returned from Gemini");
            return false;
        }

        String cleanText = String(textContent);
        cleanText.trim();
        if (cleanText.startsWith("```json")) {
            cleanText = cleanText.substring(7);
        } else if (cleanText.startsWith("```")) {
            cleanText = cleanText.substring(3);
        }
        if (cleanText.endsWith("```")) {
            cleanText = cleanText.substring(0, cleanText.length() - 3);
        }
        cleanText.trim();

        JsonDocument aiDoc;
        DeserializationError innerErr = deserializeJson(aiDoc, cleanText);

        if (innerErr) {
            _recommendation.analysis = cleanText;
            _recommendation.efficiencyScore = 50.0f;
            _recommendation.recommendedPumpPwm = 50;
            _recommendation.recommendedHeaterDuty = 0;
            _recommendation.safetyWarning = "Parsing structured JSON failed";
            _recommendation.suggestedAction = "MAINTAIN";
            _recommendation.lastAnalysisTimeSec = millis() / 1000;
            _recommendation.hasValidRecommendation = true;
            snprintf(_lastError, sizeof(_lastError), "Extracted unstructured text");
            return true;
        }

        _recommendation.analysis = aiDoc["analysis"] | "No analysis provided";
        _recommendation.efficiencyScore = aiDoc["efficiency_score"] | 0.0f;
        _recommendation.recommendedPumpPwm = aiDoc["recommended_pump_pwm"] | 50;
        _recommendation.recommendedHeaterDuty = aiDoc["recommended_heater_duty"] | 0;
        _recommendation.safetyWarning = aiDoc["safety_warning"] | "NONE";
        _recommendation.suggestedAction = aiDoc["action"] | "MAINTAIN";
        _recommendation.lastAnalysisTimeSec = millis() / 1000;
        _recommendation.hasValidRecommendation = true;

        snprintf(_lastError, sizeof(_lastError), "OK: %s", _recommendation.suggestedAction.c_str());
        Serial.printf("[Gemini] AI Analysis Received! Score: %.1f%%, Recommended Pump: %u%%, Heater: %u%%\n",
                      _recommendation.efficiencyScore, _recommendation.recommendedPumpPwm, _recommendation.recommendedHeaterDuty);
        return true;
    }

    Preferences _prefs;
    String _apiKey;
    String _model;
    bool _autoAnalysis;
    uint32_t _analysisIntervalMs;
    uint32_t _lastQueryTime;
    bool _autonomousControl;

    GeminiRecommendation _recommendation;
    char _lastError[64];
    bool _isBusy;
};

inline GeminiClient Gemini;
