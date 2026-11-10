#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <vector>
#include "../config.h"
#include "NetworkManager.hpp"

// ==============================================================================
// Gemini REST client running in its own FreeRTOS task.
//
// The HTTPS round trip (TLS handshake + model latency) takes 2-40 s. Running
// it on the Arduino loop task would freeze safety checks, the heater relay and
// the web server for that whole time, so requests are queued here and the loop
// collects results with takeCompletedJob().
//
// Robustness measures:
//  - HTTPClient (handles chunked transfer-encoding; the old raw socket did not)
//  - HTTP status checked, Google error.message surfaced to the UI
//  - structured output (responseMimeType + responseSchema), so no markdown
//    stripping; automatic retry without schema/thinking config on HTTP 400
//  - one retry on 500/503, backoff on 429
//  - "thought" parts skipped; finishReason / blockReason reported
//  - retired model IDs stored in NVS are migrated to the default
// ==============================================================================

struct GeminiRecommendation {
    String analysis;
    float efficiencyScore = 0.0f;
    uint8_t recommendedPumpPct = 0;
    uint8_t recommendedColdPumpPct = 0;
    uint8_t recommendedHeaterDuty = 0;
    String safetyWarning;
    String suggestedAction;
    uint32_t timeSec = 0;
    uint32_t seq = 0;          // increments on every new valid recommendation
    bool valid = false;
};

struct ChatCommand {
    String action;
    float value = 0.0f;
    String arg;
};

enum ChatRole : uint8_t { CHAT_USER = 0, CHAT_ASSISTANT = 1, CHAT_SYSTEM = 2 };

struct ChatMessage {
    uint32_t id = 0;
    ChatRole role = CHAT_SYSTEM;
    String text;
    std::vector<ChatCommand> commands;
    bool executed = false;
    String results;
    uint32_t timeSec = 0;
};

enum GeminiJob : uint8_t { GEMINI_JOB_NONE = 0, GEMINI_JOB_ANALYZE = 1, GEMINI_JOB_CHAT = 2 };

#define GEMINI_CHAT_HISTORY_MAX   24   // messages kept for the UI
#define GEMINI_CHAT_CONTEXT_TURNS 8    // messages sent back to the model

class GeminiClient {
public:
    void init() {
        _mutex = xSemaphoreCreateMutex();
        _prefs.begin("hx_ai", false);
        _apiKey = _prefs.getString("key", "");
        _model = _prefs.getString("model", DEFAULT_GEMINI_MODEL);
        _autoAnalysis = _prefs.getBool("auto", false);
        _intervalMs = _prefs.getUInt("intv", DEFAULT_GEMINI_INTERVAL_MS);
        _autonomousControl = _prefs.getBool("ctrl", false);
        _chatAutoExec = _prefs.getBool("chatx", false);

        if (isRetiredModel(_model)) {
            Serial.printf("[Gemini] Stored model '%s' is retired, migrating to %s\n", _model.c_str(), DEFAULT_GEMINI_MODEL);
            _model = DEFAULT_GEMINI_MODEL;
            _prefs.putString("model", _model);
        }
        if (_intervalMs < 15000) _intervalMs = DEFAULT_GEMINI_INTERVAL_MS;

        xTaskCreatePinnedToCore(taskEntry, "gemini", GEMINI_TASK_STACK, this, 1, &_task, 0);

        Serial.printf("[Gemini] Model: %s, key: %s, auto: %s (%lus), closed-loop: %s\n",
                      _model.c_str(), hasApiKey() ? "set" : "NOT SET", _autoAnalysis ? "on" : "off",
                      (unsigned long)(_intervalMs / 1000), _autonomousControl ? "on" : "off");
    }

    // ---- Configuration (loop task) --------------------------------------
    void setApiKey(const char* key) {
        String k = key ? String(key) : String();
        k.trim();
        lock();
        _apiKey = k;
        unlock();
        _prefs.putString("key", k);
    }
    bool hasApiKey() const { return _apiKey.length() >= 20; }
    String getKeyHint() const {
        return hasApiKey() ? String("...") + _apiKey.substring(_apiKey.length() - 4) : String();
    }

    bool setModel(const char* model) {
        if (!isValidModelId(model)) return false;
        lock();
        _model = model;
        unlock();
        _prefs.putString("model", _model);
        Serial.printf("[Gemini] Model set to %s\n", model);
        return true;
    }
    String getModel() const { return _model; }

    void setAutoAnalysis(bool enabled, uint32_t intervalMs) {
        _autoAnalysis = enabled;
        if (intervalMs >= 15000) _intervalMs = intervalMs;
        _prefs.putBool("auto", _autoAnalysis);
        _prefs.putUInt("intv", _intervalMs);
    }
    bool isAutoAnalysisEnabled() const { return _autoAnalysis; }
    uint32_t getIntervalMs() const { return _intervalMs; }

    void setAutonomousControl(bool enabled) {
        _autonomousControl = enabled;
        _prefs.putBool("ctrl", enabled);
    }
    bool isAutonomousControlEnabled() const { return _autonomousControl; }

    void setChatAutoExecute(bool enabled) {
        _chatAutoExec = enabled;
        _prefs.putBool("chatx", enabled);
    }
    bool isChatAutoExecute() const { return _chatAutoExec; }

    // ---- Status ---------------------------------------------------------
    bool isBusy() const { return _job != GEMINI_JOB_NONE; }
    GeminiJob currentJob() const { return _job; }
    const char* getPhase() const { return _phase; }
    String getLastError() const {
        lock();
        String e = _lastError;
        unlock();
        return e;
    }
    uint32_t getLastLatencyMs() const { return _lastLatencyMs; }
    uint32_t getRequestCount() const { return _requests; }
    uint32_t getErrorCount() const { return _errors; }
    bool isBackingOff() const { return _backoffUntil != 0 && (int32_t)(millis() - _backoffUntil) < 0; }

    // Why a request cannot be sent right now, or nullptr if it can.
    const char* unavailableReason() const {
        if (!hasApiKey()) return "Gemini API key is not set (Assistant tab)";
        if (!NetManager.isStaConnected()) return "Station Wi-Fi has no internet connection (Settings tab)";
        if (isBusy()) return "Gemini is still working on the previous request";
        return nullptr;
    }

    // ---- Requests (loop task) -------------------------------------------
    bool requestAnalysis(const String& plantContext) {
        if (unavailableReason()) return false;
        lock();
        _jobContext = plantContext;
        _jobUserText = "";
        unlock();
        startJob(GEMINI_JOB_ANALYZE);
        return true;
    }

    // The user message must already be in the history (addMessage).
    bool requestChat(const String& userText, const String& plantContext) {
        if (unavailableReason()) return false;
        lock();
        _jobContext = plantContext;
        _jobUserText = userText;
        unlock();
        startJob(GEMINI_JOB_CHAT);
        return true;
    }

    // Periodic auto-analysis gate; true when it is time to request one.
    bool autoAnalysisDue() {
        if (!_autoAnalysis || unavailableReason() || isBackingOff()) return false;
        if (millis() - _lastAutoQuery < _intervalMs) return false;
        _lastAutoQuery = millis();
        return true;
    }

    // Returns the job that just finished (once), or GEMINI_JOB_NONE.
    GeminiJob takeCompletedJob(bool* ok = nullptr) {
        GeminiJob j = _completed;
        if (j != GEMINI_JOB_NONE) {
            if (ok) *ok = _completedOk;
            _completed = GEMINI_JOB_NONE;
        }
        return j;
    }

    GeminiRecommendation getRecommendation() const {
        lock();
        GeminiRecommendation r = _rec;
        unlock();
        return r;
    }

    // ---- Chat history (shared, mutex-protected) -------------------------
    uint32_t addMessage(ChatRole role, const String& text, const std::vector<ChatCommand>& cmds = {}) {
        lock();
        ChatMessage m;
        m.id = ++_msgSeq;
        m.role = role;
        m.text = text;
        m.commands = cmds;
        m.timeSec = millis() / 1000;
        _chat.push_back(m);
        while (_chat.size() > GEMINI_CHAT_HISTORY_MAX) _chat.erase(_chat.begin());
        unlock();
        return m.id;
    }

    // Copies the proposed commands of message `id` if it has unexecuted ones.
    bool getPendingCommands(uint32_t id, std::vector<ChatCommand>& out) {
        lock();
        bool found = false;
        for (auto& m : _chat) {
            if (m.id == id && !m.executed && !m.commands.empty()) {
                out = m.commands;
                found = true;
            }
        }
        unlock();
        return found;
    }

    void markExecuted(uint32_t id, const String& results) {
        lock();
        for (auto& m : _chat) {
            if (m.id == id) {
                m.executed = true;
                m.results = results;
            }
        }
        unlock();
    }

    uint32_t lastAssistantMessageId() const {
        lock();
        uint32_t id = 0;
        for (auto& m : _chat) if (m.role == CHAT_ASSISTANT) id = m.id;
        unlock();
        return id;
    }

    void clearChat() {
        lock();
        _chat.clear();
        unlock();
    }

    void chatToJson(JsonArray arr, uint32_t sinceId) const {
        lock();
        for (const auto& m : _chat) {
            if (m.id <= sinceId) continue;
            JsonObject o = arr.add<JsonObject>();
            o["id"] = m.id;
            o["role"] = m.role == CHAT_USER ? "user" : (m.role == CHAT_ASSISTANT ? "assistant" : "system");
            o["text"] = m.text;
            o["t"] = m.timeSec;
            if (!m.commands.empty()) {
                JsonArray c = o["commands"].to<JsonArray>();
                for (const auto& cmd : m.commands) {
                    JsonObject co = c.add<JsonObject>();
                    co["action"] = cmd.action;
                    co["value"] = cmd.value;
                    co["arg"] = cmd.arg;
                }
                o["executed"] = m.executed;
                o["results"] = m.results;
            }
        }
        unlock();
    }

    uint32_t chatRevision() const { return _msgSeq; }

private:
    // ======================== Gemini task =================================
    static void taskEntry(void* self) { static_cast<GeminiClient*>(self)->taskLoop(); }

    void taskLoop() {
        for (;;) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            GeminiJob job = _job;
            if (job == GEMINI_JOB_NONE) continue;

            uint32_t t0 = millis();
            bool ok = runJob(job);
            _lastLatencyMs = millis() - t0;
            _requests = _requests + 1;
            if (!ok) _errors = _errors + 1;
            _phase = ok ? "idle" : "error";

            _completedOk = ok;
            _completed = job;
            _job = GEMINI_JOB_NONE;
        }
    }

    bool runJob(GeminiJob job) {
        lock();
        String key = _apiKey, model = _model, context = _jobContext, userText = _jobUserText;
        std::vector<ChatMessage> history;
        if (job == GEMINI_JOB_CHAT) {
            // Prior turns, excluding the user message being answered (last entry).
            size_t n = _chat.size();
            size_t start = (n > GEMINI_CHAT_CONTEXT_TURNS + 1) ? n - GEMINI_CHAT_CONTEXT_TURNS - 1 : 0;
            for (size_t i = start; i + 1 < n; i++) {
                if (_chat[i].role != CHAT_SYSTEM) history.push_back(_chat[i]);
            }
        }
        unlock();

        String url = String("https://") + GEMINI_API_HOST + "/v1beta/models/" + model + ":generateContent";
        String text, err;
        bool ok = false;

        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            bool compat = (attempt > 0 && _lastHttpCode == 400);
            String body = buildBody(job, model, context, userText, history, compat);
            _phase = "sending";
            String resp;
            int code = httpPost(url, key, body, resp);
            _lastHttpCode = code;

            if (code == 200) {
                ok = extractText(resp, text, err);
                break;
            }
            err = describeHttpError(code, resp);
            Serial.printf("[Gemini] %s\n", err.c_str());
            if (code == 400 && !compat) continue;                // retry without schema/thinking
            if (code == 500 || code == 503 || code < 0) { vTaskDelay(pdMS_TO_TICKS(3000)); continue; }
            if (code == 429) _backoffUntil = millis() + 60000;
            break;
        }

        if (ok) ok = (job == GEMINI_JOB_ANALYZE) ? parseAnalysis(text, err) : parseChat(text, err);

        lock();
        _lastError = ok ? String("OK") : err;
        unlock();
        if (!ok && job == GEMINI_JOB_CHAT) addMessage(CHAT_SYSTEM, "AI request failed: " + err);
        return ok;
    }

    String buildBody(GeminiJob job, const String& model, const String& context, const String& userText,
                     const std::vector<ChatMessage>& history, bool compat) {
        JsonDocument doc;
        doc["systemInstruction"]["parts"][0]["text"] =
            (job == GEMINI_JOB_CHAT) ? CHAT_SYSTEM_PROMPT : ANALYSIS_SYSTEM_PROMPT;

        JsonArray contents = doc["contents"].to<JsonArray>();
        for (const auto& m : history) {
            JsonObject c = contents.add<JsonObject>();
            c["role"] = (m.role == CHAT_USER) ? "user" : "model";
            String t = m.text;
            if (!m.commands.empty()) {
                t += "\n[proposed commands:";
                for (const auto& cmd : m.commands) t += " " + cmd.action + "(" + (cmd.arg.length() ? cmd.arg : String(cmd.value, 1)) + ")";
                t += m.executed ? " -> executed: " + m.results + "]" : " -> not executed]";
            }
            c["parts"][0]["text"] = t;
        }
        JsonObject last = contents.add<JsonObject>();
        last["role"] = "user";
        last["parts"][0]["text"] = (job == GEMINI_JOB_CHAT)
            ? String("LIVE TELEMETRY:\n") + context + "\n\nOPERATOR MESSAGE:\n" + userText
            : String("LIVE TELEMETRY:\n") + context + "\n\nAssess the exchanger and recommend setpoints.";

        JsonObject gen = doc["generationConfig"].to<JsonObject>();
        gen["temperature"] = 0.3;
        gen["responseMimeType"] = "application/json";
        if (!compat) {
            gen["responseSchema"] = serialized((job == GEMINI_JOB_CHAT) ? CHAT_SCHEMA : ANALYSIS_SCHEMA);
            if (model.startsWith("gemini-3")) gen["thinkingConfig"]["thinkingLevel"] = "low";
        }

        String body;
        serializeJson(doc, body);
        return body;
    }

    int httpPost(const String& url, const String& key, const String& body, String& resp) {
        WiFiClientSecure client;
        client.setInsecure();   // Google rotates intermediates; pinning a CA would break silently
        client.setHandshakeTimeout(20);
        HTTPClient http;
        http.setReuse(false);
        http.setConnectTimeout(10000);
        http.setTimeout(GEMINI_HTTP_TIMEOUT_MS);
        if (!http.begin(client, url)) return HTTPC_ERROR_CONNECTION_REFUSED;
        http.addHeader("Content-Type", "application/json");
        http.addHeader("x-goog-api-key", key);   // keeps the key out of the URL
        _phase = "waiting for model";
        int code = http.POST(body);
        if (code > 0) {
            _phase = "reading";
            resp = http.getString();
        }
        http.end();
        return code;
    }

    static String describeHttpError(int code, const String& resp) {
        if (code < 0) return String("Network error: ") + HTTPClient::errorToString(code);
        String msg;
        JsonDocument filter;
        filter["error"]["message"] = true;
        filter["error"]["status"] = true;
        JsonDocument doc;
        if (!deserializeJson(doc, resp, DeserializationOption::Filter(filter))) {
            msg = String(doc["error"]["status"] | "") + ": " + String(doc["error"]["message"] | "");
        }
        String hint;
        if (code == 400) hint = " (bad request / unsupported model or option)";
        else if (code == 401 || code == 403) hint = " (API key invalid or not enabled for Gemini)";
        else if (code == 404) hint = " (model not found or retired - pick another)";
        else if (code == 429) hint = " (rate limit / quota, backing off 60 s)";
        else if (code >= 500) hint = " (Google server busy)";
        String out = "HTTP " + String(code) + hint + (msg.length() > 2 ? " - " + msg : "");
        if (out.length() > 200) out = out.substring(0, 200) + "...";
        return out;
    }

    static bool extractText(const String& resp, String& text, String& err) {
        JsonDocument filter;
        filter["candidates"][0]["content"]["parts"][0]["text"] = true;
        filter["candidates"][0]["content"]["parts"][0]["thought"] = true;
        filter["candidates"][0]["finishReason"] = true;
        filter["promptFeedback"]["blockReason"] = true;
        JsonDocument doc;
        DeserializationError e = deserializeJson(doc, resp, DeserializationOption::Filter(filter));
        if (e) {
            err = String("Response JSON error: ") + e.c_str();
            return false;
        }
        if (doc["promptFeedback"]["blockReason"].is<const char*>()) {
            err = String("Prompt blocked: ") + doc["promptFeedback"]["blockReason"].as<const char*>();
            return false;
        }
        text = "";
        for (JsonObject part : doc["candidates"][0]["content"]["parts"].as<JsonArray>()) {
            if (part["thought"] | false) continue;
            text += part["text"] | "";
        }
        if (text.length() == 0) {
            err = String("Empty answer (finishReason: ") + (doc["candidates"][0]["finishReason"] | "none") + ")";
            return false;
        }
        return true;
    }

    // Trims ```json fences or prose around the outermost {...}.
    static bool isolateJson(String& text) {
        int a = text.indexOf('{');
        int b = text.lastIndexOf('}');
        if (a < 0 || b <= a) return false;
        text = text.substring(a, b + 1);
        return true;
    }

    bool parseAnalysis(String text, String& err) {
        JsonDocument doc;
        if (!isolateJson(text) || deserializeJson(doc, text)) {
            err = "Model reply was not valid JSON";
            return false;
        }
        if (!doc["recommended_pump_pct"].is<float>() || !doc["recommended_heater_duty"].is<float>()) {
            err = "Model reply missing setpoint fields";
            return false;
        }
        GeminiRecommendation r;
        r.analysis = doc["analysis"] | "";
        r.efficiencyScore = constrain(doc["efficiency_score"] | 0.0f, 0.0f, 100.0f);
        r.recommendedPumpPct = (uint8_t)constrain(doc["recommended_pump_pct"].as<float>(), 0.0f, 100.0f);
        float coldRec = doc["recommended_cold_pump_pct"] | doc["recommended_pump_pct"].as<float>();
        r.recommendedColdPumpPct = (uint8_t)constrain(coldRec, 0.0f, 100.0f);
        r.recommendedHeaterDuty = (uint8_t)constrain(doc["recommended_heater_duty"].as<float>(), 0.0f, 100.0f);
        r.safetyWarning = doc["safety_warning"] | "NONE";
        r.suggestedAction = doc["action"] | "MAINTAIN";
        r.timeSec = millis() / 1000;
        r.valid = true;
        lock();
        r.seq = _rec.seq + 1;
        _rec = r;
        unlock();
        Serial.printf("[Gemini] Analysis: score %.0f, pump %u%%/%u%%, heater %u%%, %s\n", r.efficiencyScore,
                      r.recommendedPumpPct, r.recommendedColdPumpPct, r.recommendedHeaterDuty,
                      r.suggestedAction.c_str());
        return true;
    }

    bool parseChat(String text, String& err) {
        JsonDocument doc;
        String reply;
        std::vector<ChatCommand> cmds;
        if (isolateJson(text) && !deserializeJson(doc, text) && doc["reply"].is<const char*>()) {
            reply = doc["reply"].as<const char*>();
            for (JsonObject c : doc["commands"].as<JsonArray>()) {
                ChatCommand cmd;
                cmd.action = c["action"] | "";
                cmd.value = c["value"] | 0.0f;
                cmd.arg = c["arg"] | "";
                if (cmd.action.length()) cmds.push_back(cmd);
                if (cmds.size() >= 6) break;
            }
        } else {
            // Plain-text answer (compat mode): show it, but never act on it.
            reply = text;
        }
        reply.trim();
        if (reply.length() == 0) reply = cmds.empty() ? "(no reply)" : "Proposed commands:";
        addMessage(CHAT_ASSISTANT, reply, cmds);
        return true;
    }

    void startJob(GeminiJob job) {
        _phase = "queued";
        _job = job;
        xTaskNotifyGive(_task);
    }

    static bool isValidModelId(const char* m) {
        if (!m) return false;
        size_t n = strlen(m);
        if (n < 6 || n > 48 || strncmp(m, "gemini-", 7) != 0) return false;
        for (size_t i = 0; i < n; i++) {
            char c = m[i];
            if (!(isalnum((unsigned char)c) || c == '-' || c == '.' || c == '_')) return false;
        }
        return true;
    }

    static bool isRetiredModel(const String& m) {
        static const char* retired[] = {
            "gemini-pro", "gemini-1.0-pro", "gemini-1.5-pro", "gemini-1.5-flash", "gemini-1.5-flash-8b",
            "gemini-2.0-flash", "gemini-2.0-flash-001", "gemini-2.0-flash-lite", "gemini-2.0-flash-lite-001",
            "gemini-2.5-pro", "gemini-2.5-flash", "gemini-3-pro-preview", "gemini-3.5-flash", "gemini-3.7-flash" };
        for (const char* r : retired) if (m == r) return true;
        return !isValidModelId(m.c_str());
    }

    void lock() const { if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY); }
    void unlock() const { if (_mutex) xSemaphoreGive(_mutex); }

    static constexpr const char* ANALYSIS_SYSTEM_PROMPT =
        "You are a thermal process engineer supervising a small liquid-to-liquid counter-flow heat exchanger "
        "test rig. A relay-switched electric heater warms a hot loop driven by one pump; a second pump drives "
        "the cold loop through a counter-flow exchanger. Firmware enforces "
        "interlocks (no heat without flow, 75 C over-temperature trip, sensor fault trip). Using only the telemetry "
        "given, assess heat transfer (Q, LMTD, UA, effectiveness, heat balance), spot anomalies (sensor drift, "
        "heat-balance error, fouling, low flow), and recommend hot-pump %, cold-pump % and heater duty %. Keep "
        "analysis under 60 words. safety_warning is NONE unless there is a concrete risk.";

    static constexpr const char* CHAT_SYSTEM_PROMPT =
        "You are the operator assistant for an ESP32-controlled heat exchanger test rig. Answer questions briefly "
        "and concretely from the LIVE TELEMETRY block; never invent readings. If the operator asks to change the "
        "system, put the changes in `commands` using only these actions: set_pump_pct (value 0-100), "
        "set_flow_setpoint (value L/min 0-30, closed-loop hot flow), pump_stop, "
        "set_cold_pump_pct (value 0-100), set_cold_flow_setpoint (value L/min 0-30, closed-loop cold flow), "
        "cold_pump_stop, set_heater_mode (arg: off, "
        "manual, thermostatic, time_pwm, pid), set_heater_manual (arg on/off; heater must already be in manual "
        "mode), set_target_temp (value C, 10-73), set_heater_duty (value 0-100, time_pwm mode), estop, clear_log. "
        "To heat to a temperature, prefer pid mode with set_target_temp and make sure the pump is running. You "
        "cannot reset safety trips or change simulation mode; tell the operator to do it. If a request is unsafe "
        "or unclear, ask instead of commanding. Questions only => empty commands. Plain text, no markdown, under "
        "100 words.";

    static constexpr const char* ANALYSIS_SCHEMA =
        "{\"type\":\"OBJECT\",\"properties\":{"
        "\"analysis\":{\"type\":\"STRING\"},"
        "\"efficiency_score\":{\"type\":\"NUMBER\"},"
        "\"recommended_pump_pct\":{\"type\":\"INTEGER\"},"
        "\"recommended_cold_pump_pct\":{\"type\":\"INTEGER\"},"
        "\"recommended_heater_duty\":{\"type\":\"INTEGER\"},"
        "\"safety_warning\":{\"type\":\"STRING\"},"
        "\"action\":{\"type\":\"STRING\",\"enum\":[\"MAINTAIN\",\"INCREASE_FLOW\",\"DECREASE_FLOW\",\"INCREASE_HEAT\",\"DECREASE_HEAT\"]}},"
        "\"required\":[\"analysis\",\"efficiency_score\",\"recommended_pump_pct\",\"recommended_heater_duty\",\"safety_warning\",\"action\"]}";

    static constexpr const char* CHAT_SCHEMA =
        "{\"type\":\"OBJECT\",\"properties\":{"
        "\"reply\":{\"type\":\"STRING\"},"
        "\"commands\":{\"type\":\"ARRAY\",\"items\":{\"type\":\"OBJECT\",\"properties\":{"
        "\"action\":{\"type\":\"STRING\",\"enum\":[\"set_pump_pct\",\"set_flow_setpoint\",\"pump_stop\",\"set_cold_pump_pct\","
        "\"set_cold_flow_setpoint\",\"cold_pump_stop\",\"set_heater_mode\","
        "\"set_heater_manual\",\"set_target_temp\",\"set_heater_duty\",\"estop\",\"clear_log\"]},"
        "\"value\":{\"type\":\"NUMBER\"},\"arg\":{\"type\":\"STRING\"}},\"required\":[\"action\"]}}},"
        "\"required\":[\"reply\",\"commands\"]}";

    mutable SemaphoreHandle_t _mutex = nullptr;
    TaskHandle_t _task = nullptr;
    Preferences _prefs;

    String _apiKey;
    String _model;
    bool _autoAnalysis = false;
    uint32_t _intervalMs = DEFAULT_GEMINI_INTERVAL_MS;
    bool _autonomousControl = false;
    bool _chatAutoExec = false;
    uint32_t _lastAutoQuery = 0;

    // Job hand-off (written by loop before notify, read by task)
    volatile GeminiJob _job = GEMINI_JOB_NONE;
    volatile GeminiJob _completed = GEMINI_JOB_NONE;
    volatile bool _completedOk = false;
    String _jobContext;
    String _jobUserText;

    const char* volatile _phase = "idle";
    volatile int _lastHttpCode = 0;
    volatile uint32_t _lastLatencyMs = 0;
    volatile uint32_t _requests = 0;
    volatile uint32_t _errors = 0;
    volatile uint32_t _backoffUntil = 0;
    String _lastError;

    GeminiRecommendation _rec;
    std::vector<ChatMessage> _chat;
    uint32_t _msgSeq = 0;
};

inline GeminiClient Gemini;
