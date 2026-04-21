#pragma once
#include <Arduino.h>

enum UiScreen {
    UI_SPLASH = 0,
    UI_BROWSER,
    UI_PLAYER,
    UI_SETTINGS,
    UI_INFO,
    UI_WIFI
};

enum UiDebugMode {
    UI_DBG_NORMAL = 0,
    UI_DBG_DISPLAY_ONLY,
    UI_DBG_AUDIO_ONLY,
    UI_DBG_AUDIO_SD_NO_UI,
    UI_DBG_DISPLAY_SD_NO_DECODE
};

void     uiManagerInit();
void     uiManagerLoop();
UiScreen uiCurrentScreen();
bool     uiUsbModeEnabled();
UiDebugMode uiDebugMode();
bool     uiRenderEnabled();
