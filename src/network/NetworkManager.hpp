#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "../config.h"

class NetworkManager {
public:
    NetworkManager()
        : _staConnecting(false),
          _staConnectStartTime(0),
          _lastReconnectCheck(0) {}

    void init() {
        _prefs.begin("hx_net", false);
        _staSsid = _prefs.getString("ssid", "");
        _staPass = _prefs.getString("pass", "");

        WiFi.mode(WIFI_AP_STA);

        IPAddress apIP(192, 168, 4, 1);
        IPAddress netMsk(255, 255, 255, 0);
        WiFi.softAPConfig(apIP, apIP, netMsk);
        bool apOk = WiFi.softAP(DEFAULT_AP_SSID, DEFAULT_AP_PASS, DEFAULT_AP_CHANNEL);

        if (apOk) {
            Serial.printf("[WiFi] SoftAP started! SSID: '%s', IP: %s\n",
                          DEFAULT_AP_SSID, WiFi.softAPIP().toString().c_str());
        } else {
            Serial.println("[WiFi] Failed to start SoftAP!");
        }

        if (_staSsid.length() > 0) {
            connectSta();
        } else {
            Serial.println("[WiFi] No saved Station credentials. Connect via SoftAP to configure.");
        }

        if (MDNS.begin(DEFAULT_MDNS_HOST)) {
            MDNS.addService("http", "tcp", DEFAULT_WEB_PORT);
            Serial.printf("[mDNS] Responder started: http://%s.local\n", DEFAULT_MDNS_HOST);
        }
    }

    void update() {
        uint32_t now = millis();

        if (_staConnecting) {
            if (WiFi.status() == WL_CONNECTED) {
                _staConnecting = false;
                Serial.printf("[WiFi] Station connected! IP: %s\n", WiFi.localIP().toString().c_str());
            } else if (now - _staConnectStartTime > 15000) {
                _staConnecting = false;
                Serial.println("[WiFi] Station connection timed out. SoftAP remains active.");
            }
        } else if (_staSsid.length() > 0 && WiFi.status() != WL_CONNECTED) {
            if (now - _lastReconnectCheck >= 30000) {
                _lastReconnectCheck = now;
                Serial.println("[WiFi] Reconnecting to Station...");
                WiFi.reconnect();
            }
        }
    }

    bool isStaConnected() const {
        return WiFi.status() == WL_CONNECTED;
    }

    String getStaIP() const {
        if (isStaConnected()) {
            return WiFi.localIP().toString();
        }
        return "";
    }

    String getApIP() const {
        return WiFi.softAPIP().toString();
    }

    String getActiveIP() const {
        if (isStaConnected()) {
            return WiFi.localIP().toString();
        }
        return WiFi.softAPIP().toString();
    }

    void setStaCredentials(const char* ssid, const char* pass) {
        _staSsid = ssid ? ssid : "";
        _staPass = pass ? pass : "";

        _prefs.putString("ssid", _staSsid);
        _prefs.putString("pass", _staPass);

        if (_staSsid.length() > 0) {
            connectSta();
        } else {
            WiFi.disconnect(false);
        }
    }

    String getStoredSsid() const {
        return _staSsid;
    }

    String scanNetworksJson() {
        int n = WiFi.scanNetworks();
        String json = "[";
        for (int i = 0; i < n; i++) {
            if (i > 0) json += ",";
            json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
        }
        json += "]";
        return json;
    }

private:
    void connectSta() {
        if (_staSsid.length() == 0) return;
        Serial.printf("[WiFi] Connecting to Station SSID: '%s'...\n", _staSsid.c_str());
        WiFi.begin(_staSsid.c_str(), _staPass.c_str());
        _staConnecting = true;
        _staConnectStartTime = millis();
    }

    Preferences _prefs;
    String _staSsid;
    String _staPass;
    bool _staConnecting;
    uint32_t _staConnectStartTime;
    uint32_t _lastReconnectCheck;
};

inline NetworkManager Network;
