#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "config.h"

class NetworkManager {
public:
    NetworkManager();

    void init();
    void update();

    bool isStaConnected() const;
    String getStaIP() const;
    String getApIP() const;
    String getActiveIP() const;

    void setStaCredentials(const char* ssid, const char* pass);
    String getStoredSsid() const;

    String scanNetworksJson();

private:
    void connectSta();

    Preferences _prefs;
    String _staSsid;
    String _staPass;
    bool _staConnecting;
    uint32_t _staConnectStartTime;
    uint32_t _lastReconnectCheck;
};

extern NetworkManager Network;
