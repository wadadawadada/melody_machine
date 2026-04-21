/**
 * @file      melody_machine.ino
 * @brief     MP3 Player + Internet Radio firmware for LilyGO T-LoRa Pager
 *
 * Plays MP3 files from SD card (/MP3 folder) or streams internet radio
 * from M3U playlists (/M3U folder). WiFi via ESP32 built-in.
 * Audio via ES8311 codec (speaker + headphone jack).
 * UI: LVGL 9.3, rotary encoder + keyboard navigation.
 *
 * Board:  LilyGo-T-LoRa-Pager  (esp32:esp32:tlora_pager:Revision=Radio_SX1262)
 * Audio:  ESP8266Audio + EspCodec (Core 0 FreeRTOS task)
 * UI:     LVGL 9.3
 */

#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <esp_heap_caps.h>
#include "audio_player.h"
#include "file_browser.h"
#include "spi_guard.h"
#include "ui_manager.h"
#include "settings_store.h"
#include "theme_manager.h"
#include "wifi_manager.h"

void setup() {
    Serial.setRxBufferSize(4096);
    Serial.begin(115200);

    instance.begin();
    beginLvglHelper(instance);
    instance.setBrightness(DEVICE_MAX_BRIGHTNESS_LEVEL);
    spiGuardInit();

    if (!instance.installSD()) {
        Serial.println("[BOOT] Warning: SD card not detected");
    }

    settingsStoreInit();   // load JSON config, migrate NVS on first boot
    themeManagerInit();    // load active theme from settings

    audioPlayerInit();

    // Init WiFi before heavy playlist scan to reserve system tasks/memory first.
    wifiManagerInit();
    wifiAutoReconnect();   // reconnects if last mode was radio

    fileBrowserInit();     // scans /MP3 or /M3U based on app.mode in settings

    uiManagerInit();

    Serial.println("[BOOT] Melody Machine v2.0 ready");
}

void loop() {
    instance.loop();
    wifiManagerLoop();

    static uint32_t lastUiTick = 0;
    uint32_t now = millis();
    uint32_t uiPeriod = 16;   // ~60 fps idle
    if (uiUsbModeEnabled()) uiPeriod = 150;
    else if (audioPlayerGetState() != PS_STOPPED) uiPeriod = 80;
    if (uiRenderEnabled() && now - lastUiTick >= uiPeriod) {
        lastUiTick = now;
        lv_timer_handler();
    }
    uiManagerLoop();

    static uint32_t hb = 0;
    if (millis() - hb > 2000) {
        hb = millis();
        Serial.printf("[HB] t=%lu screen=%d state=%d radio=%d elapsed=%lu vol=%u heap=%u largest=%u dbg=%d\n",
                      (unsigned long)millis(),
                      (int)uiCurrentScreen(),
                      (int)audioPlayerGetState(),
                      (int)radioPlayerGetStatus(),
                      (unsigned long)audioPlayerGetElapsed(),
                      (unsigned)audioPlayerGetVolume(),
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                      (int)uiDebugMode());
    }
    delay(2);
}
