#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "config.h"
#include "Thermodynamics.h"

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
    GeminiClient();

    void init();
    void update(const ThermodynamicData& data, uint8_t currentPumpPct, bool heaterOn, float targetTemp);

    bool query(const ThermodynamicData& data, uint8_t currentPumpPct, bool heaterOn, float targetTemp);

    void setApiKey(const char* key);
    String getApiKey() const;
    bool hasApiKey() const;

    void setAutoAnalysis(bool enabled, uint32_t intervalMs = DEFAULT_GEMINI_INTERVAL_MS);
    bool isAutoAnalysisEnabled() const;

    void setAutonomousControl(bool enabled);
    bool isAutonomousControlEnabled() const;

    const GeminiRecommendation& getLastRecommendation() const;
    const char* getLastError() const;

private:
    bool sendRequest(const String& promptText, String& responseJson);
    bool parseGeminiResponse(const String& rawResponse);

    Preferences _prefs;
    String _apiKey;
    bool _autoAnalysis;
    uint32_t _analysisIntervalMs;
    uint32_t _lastQueryTime;
    bool _autonomousControl;

    GeminiRecommendation _recommendation;
    char _lastError[64];
    bool _isBusy;
};

extern GeminiClient Gemini;
