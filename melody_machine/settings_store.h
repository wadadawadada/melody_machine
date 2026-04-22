#pragma once
#include <Arduino.h>

void   settingsStoreInit();

String settingsGetString(const char* key, const char* def = "");
int    settingsGetInt   (const char* key, int def = 0);
bool   settingsGetBool  (const char* key, bool def = false);
void   settingsPutString(const char* key, const char* value);
void   settingsPutInt   (const char* key, int value);
void   settingsPutBool  (const char* key, bool value);
bool   settingsSave();
void   settingsMarkDirty();   // schedule save without holding SPI
bool   settingsFlushIfDirty(); // call from UI loop when SD is free

struct WifiNetwork { String ssid; String pass; };
int         wifiNetworkCount();
WifiNetwork wifiNetworkGet(int index);
void        wifiNetworkAdd(const String& ssid, const String& pass);
void        wifiNetworkRemove(int index);
