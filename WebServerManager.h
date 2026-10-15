#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "config.h"
#include "web_dashboard.h"
#include "Thermodynamics.h"
#include "ThermalSensors.h"
#include "FlowSensor.h"
#include "PumpController.h"
#include "HeaterController.h"
#include "SafetyManager.h"
#include "NetworkManager.h"
#include "GeminiClient.h"
#include "DataLogger.h"

class WebServerManager {
public:
    WebServerManager();

    void init();
    void handleClient();

private:
    void handleRoot();
    void handleGetTelemetry();
    void handlePostControl();
    void handlePostConfig();
    void handlePostAiConfig();
    void handlePostAiQuery();
    void handleGetExportCsv();
    void handlePostClearLog();
    void handleNotFound();

    WebServer _server;
};

extern WebServerManager Web;
