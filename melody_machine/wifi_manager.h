#pragma once
#include <Arduino.h>

enum WifiState {
    WS_DISABLED = 0,
    WS_SCANNING,
    WS_CONNECTING,
    WS_CONNECTED,
    WS_FAILED
};

void       wifiManagerInit();
void       wifiManagerLoop();

WifiState  wifiGetState();
bool       wifiIsConnected();
String     wifiGetSSID();
int8_t     wifiGetRSSI();

bool       wifiConnect(const String& ssid);
void       wifiDisconnect();
void       wifiAutoReconnect();

bool       wifiStartScan();
int        wifiScanResultCount();
String     wifiScanSSID(int i);
int8_t     wifiScanRSSI(int i);
