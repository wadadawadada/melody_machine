#include "wifi_manager.h"
#include "settings_store.h"
#include <WiFi.h>

#define CONNECT_TIMEOUT_MS 10000

static WifiState _state = WS_DISABLED;
static uint32_t  _connectStartMs = 0;
static String    _connectingSSID;

void wifiManagerInit() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false); // Keep link stable for internet radio.
    WiFi.disconnect(true);
    _state = WS_DISABLED;
    Serial.println("[WIFI] Manager init");
}

void wifiManagerLoop() {
    if (_state == WS_CONNECTING) {
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED) {
            _state = WS_CONNECTED;
            settingsPutString("wifi.last_ssid", _connectingSSID.c_str());
            settingsSave();
            Serial.printf("[WIFI] Connected to %s, IP=%s\n",
                          _connectingSSID.c_str(),
                          WiFi.localIP().toString().c_str());
        } else if (millis() - _connectStartMs > CONNECT_TIMEOUT_MS) {
            _state = WS_FAILED;
            Serial.printf("[WIFI] Connection timeout for %s\n", _connectingSSID.c_str());
        }
    } else if (_state == WS_CONNECTED) {
        if (WiFi.status() != WL_CONNECTED) {
            _state = WS_FAILED;
            Serial.println("[WIFI] Connection lost");
        }
    }
}

WifiState wifiGetState() { return _state; }

bool wifiIsConnected() { return _state == WS_CONNECTED; }

String wifiGetSSID() {
    return (_state == WS_CONNECTED) ? WiFi.SSID() : String("");
}

int8_t wifiGetRSSI() {
    return (_state == WS_CONNECTED) ? WiFi.RSSI() : 0;
}

bool wifiConnect(const String& ssid) {
    int n = wifiNetworkCount();
    String pass;
    bool found = false;
    for (int i = 0; i < n; i++) {
        WifiNetwork net = wifiNetworkGet(i);
        if (net.ssid == ssid) {
            pass = net.pass;
            found = true;
            break;
        }
    }
    if (!found) {
        Serial.printf("[WIFI] SSID '%s' not in known networks\n", ssid.c_str());
        _state = WS_FAILED;
        return false;
    }

    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    WiFi.disconnect(false, true);
    delay(20);
    WiFi.begin(ssid.c_str(), pass.c_str());
    _connectingSSID = ssid;
    _connectStartMs = millis();
    _state = WS_CONNECTING;
    Serial.printf("[WIFI] Connecting to %s ...\n", ssid.c_str());
    return true;
}

void wifiDisconnect() {
    WiFi.disconnect(false, true);
    WiFi.scanDelete();
    _state = WS_DISABLED;
    Serial.println("[WIFI] Disconnected");
}

void wifiAutoReconnect() {
    if (_state == WS_CONNECTING || _state == WS_CONNECTED || _state == WS_SCANNING) return;
    String mode = settingsGetString("app.mode", "mp3");
    if (mode != "radio") return;
    String lastSsid = settingsGetString("wifi.last_ssid", "");
    if (lastSsid.length() == 0 && wifiNetworkCount() > 0) {
        lastSsid = wifiNetworkGet(0).ssid;
    }
    if (lastSsid.length() == 0) return;
    Serial.printf("[WIFI] Auto-reconnect to %s\n", lastSsid.c_str());
    wifiConnect(lastSsid);
}

bool wifiStartScan() {
    if (_state == WS_SCANNING) return true;
    WiFi.mode(WIFI_STA);
    if (_state == WS_CONNECTING) {
        WiFi.disconnect(false, true);
        delay(20);
    }
    WiFi.scanDelete();
    int rc = WiFi.scanNetworks(true, false); // async, visible SSIDs only
    if (rc == WIFI_SCAN_FAILED) {
        _state = WS_FAILED;
        Serial.println("[WIFI] Scan start failed");
        return false;
    }
    _state = WS_SCANNING;
    Serial.println("[WIFI] Scan started");
    return true;
}

int wifiScanResultCount() {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return -1;
    if (n == WIFI_SCAN_FAILED) {
        _state = WS_FAILED;
        Serial.println("[WIFI] Scan failed");
        return 0;
    }
    if (n < 0) {
        _state = WS_FAILED;
        return 0;
    }
    if (_state == WS_SCANNING) _state = WS_DISABLED;
    return n;
}

String wifiScanSSID(int i) {
    return WiFi.SSID(i);
}

int8_t wifiScanRSSI(int i) {
    return (int8_t)WiFi.RSSI(i);
}
