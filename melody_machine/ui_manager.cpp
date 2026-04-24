#include "ui_manager.h"
#include "audio_player.h"
#include "file_browser.h"
#include "img_splash.h"
#include "spi_guard.h"
#include "theme_manager.h"
#include "wifi_manager.h"
#include "settings_store.h"
#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <draw/lv_draw_private.h>
#include <core/lv_obj_draw_private.h>
#include <core/lv_refr_private.h>
#include <display/lv_display_private.h>
#include <SD.h>
#include <esp_system.h>
#include <algorithm>
#include <vector>
#include <ctype.h>
#include <string.h>

#if defined(SOC_USB_OTG_SUPPORTED) && SOC_USB_OTG_SUPPORTED && defined(CONFIG_TINYUSB_MSC_ENABLED) && CONFIG_TINYUSB_MSC_ENABLED
#include <USB.h>
#include <USBMSC.h>
#define MM_USB_RUNTIME_MSC 1
#else
#define MM_USB_RUNTIME_MSC 0
#endif

#define SW 480
#define SH 222
#define TOP_H 22
#define CONTENT_Y 23
#define CONTENT_H 181
#define BOT_LINE_Y 204
#define BOT_H 17
#define BOT_Y 205
#define PROGRESS_H 5
#define DIVIDER_X 300
#define LEFT_W 300
#define RIGHT_X 301
#define RIGHT_W 179
#define PAD 14
#define PLAYLIST_ROWS 8
#define BAT_FILL_MAX_W 24

// Shorthand for active theme
#define TH (themeGet())

static UiScreen _screen = UI_SPLASH;
static UiScreen _returnScreen = UI_PLAYER;
static uint32_t _splashAt = 0;

static std::vector<int> _playlist;
static int _plIdx = 0;
static int _listCursor = 0;
static int _listScroll = 0;
static RepeatMode _repeat = RM_NONE;
static bool _shuffle = false;

// MP3 folder browser: true = browsing dirs/files, false = playlist-only view
// In folder browser mode the right panel shows fileBrowserCount() items (dirs+files).
// Row 0 is ".." (back) when we're inside a subdirectory.
static bool _mp3BrowserMode = true;

static uint8_t _displayBrightness = DEVICE_MAX_BRIGHTNESS_LEVEL;
static uint32_t _screenTimeoutSec = 60;
static bool _screenDimmed = false;
static uint32_t _lastInputMs = 0;
static uint8_t _batteryPct = 75;
static bool _batteryCharging = false;
static uint32_t _batteryPollAt = 0;
static uint32_t _uiElapsedBaseSec = 0;
static uint32_t _uiElapsedBaseMs = 0;
static uint32_t _uiElapsedRawLast = 0;
static PlayerState _uiElapsedStateLast = PS_STOPPED;
static int _uiElapsedTrackLast = -1;
static bool _uiDurationUnreliable = false;
static uint32_t _uiDurationShownSec = 0;

static lv_obj_t* _scrSplash   = nullptr;
static lv_obj_t* _scrPlayer   = nullptr;
static lv_obj_t* _scrSettings = nullptr;
static lv_obj_t* _scrInfo     = nullptr;
static lv_obj_t* _scrWifi     = nullptr;

// Player widgets
static lv_obj_t* _nowClip = nullptr;   // clipping container for marquee
static lv_obj_t* _lblNow = nullptr;
static lv_obj_t* _lblState = nullptr;
static lv_obj_t* _barVolume = nullptr;
static lv_obj_t* _lblVol = nullptr;
static lv_obj_t* _lblInfo = nullptr;
static lv_obj_t* _lblInfoRight = nullptr;
static lv_obj_t* _lblInfoIndicator = nullptr;
static lv_obj_t* _lblPlCount = nullptr;
static lv_obj_t* _lblShuffle = nullptr;
static lv_obj_t* _lblRepeat = nullptr;
static lv_obj_t* _barProgress = nullptr;
static lv_obj_t* _lblBatPct = nullptr;
static lv_obj_t* _batFill = nullptr;
static lv_obj_t* _lblBatCharge = nullptr;
static lv_obj_t* _lblWifi = nullptr;
static lv_obj_t* _lblMode = nullptr;
static lv_obj_t* _lblListTitle = nullptr;
static lv_obj_t* _lblInfoSettings = nullptr;
static lv_obj_t* _plRow[PLAYLIST_ROWS];
static lv_obj_t* _plLabel[PLAYLIST_ROWS];

// Settings
#define SETTINGS_COUNT 13
static const int SETTINGS_ROW_H = 22;
static const int SETTINGS_ROW_GAP = 2;
static const int SETTINGS_VISIBLE = 7;
static int _settingsCursor = 0;
static int _settingsScroll = 0;
static lv_obj_t* _settingsRow[SETTINGS_COUNT];
static lv_obj_t* _settingsName[SETTINGS_COUNT];
static lv_obj_t* _settingsValue[SETTINGS_COUNT];

static bool _usbModeEnabled = false;
static UiDebugMode _debugMode = UI_DBG_NORMAL;
static bool _kbBacklight = true;
static uint32_t _kbTimeoutSec = 0;
static bool _kbDimmed = false;
static uint32_t _lastPauseToggleMs = 0;

// Seek mode (MP3 only): encoder adjusts target position, seek applied on exit
static bool     _seekMode       = false;
static uint32_t _seekTargetSec  = 0;
static const int32_t SEEK_STEP_SEC = 5;

// Auto power-off
static uint32_t _powerOffTimeoutSec = 0;  // 0 = disabled
static uint32_t _lastActivityForPowerOff = 0;

// Marquee (scrolling title) state
static enum { MQ_PAUSE_START, MQ_SCROLLING, MQ_PAUSE_END } _mqPhase = MQ_PAUSE_START;
static uint32_t _mqPhaseMs  = 0;   // when current phase started
static int32_t  _mqOffsetX  = 0;   // current pixel offset (0 = home)
static int32_t  _mqTextW    = 0;   // cached text width
static String   _mqLastText = "";  // detect text change
static const int32_t  MQ_CLIP_W       = 274;  // visible width (matches _nowClip)
static const uint32_t MQ_PAUSE_START_MS = 3000; // wait before scrolling
static const uint32_t MQ_PAUSE_END_MS  = 1500; // wait at end before reset
static const int32_t  MQ_SPEED_PX_PER_SEC = 40; // scroll speed
static int _pendingRadioPlayIdx = -1;
static uint32_t _pendingRadioPlayAtMs = 0;
static uint32_t _pendingSComboAtMs = 0;
static constexpr uint32_t SHOT_COMBO_WINDOW_MS = 550;

// WiFi screen
#define WIFI_ROWS 7
static lv_obj_t* _wifiRow[WIFI_ROWS];
static lv_obj_t* _wifiLabel[WIFI_ROWS];
static lv_obj_t* _wifiStatus = nullptr;
static lv_obj_t* _wifiScroll = nullptr;
static bool _wifiScanPending = false;
static int _wifiCursor = 0;
static int _wifiScroll_ = 0;
static int _wifiCount = 0;
static uint32_t _lastPlayReqMs = 0;

// Password entry overlay
static lv_obj_t* _pwOverlay = nullptr;
static lv_obj_t* _pwPrompt  = nullptr;
static lv_obj_t* _pwLabel   = nullptr;
static char      _pwBuf[64] = {};
static int       _pwLen     = 0;
static String    _pwTargetSSID;
static uint32_t  _pwLastCharMs = 0;
static bool      _pwEntryActive = false;

#if MM_USB_RUNTIME_MSC
static USBMSC _usbMsc;
static bool _usbMscStarted = false;
#endif

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void buildSplash();
static void buildPlayer();
static void buildSettings();
static void buildInfo();
static void buildWifi();
static void showScreen(UiScreen s);
static void teardownScreens();
static void rebuildScreens();
static void refreshPlayer();
static void tickMarquee();
static void refreshSettings();
static void refreshWifi();
static void loadUiSettings();
static void saveUiSettings();
static void buildPlaylist(int startTrackIndex);
static void startPlaying(int trackIdx);
static void playNext();
static void playPrev();
static void switchMode(bool toRadio);
static bool isRadioMode();
static void showPwOverlay(const String& ssid);
static void hidePwOverlay();
static void pwHandleKey(char k, char raw);
static void requestRadioPlay(int idx, const char* why);
static void applyEqPresetFromUi(EqPreset preset);
static bool saveScreenshotToSd(char* outPath, size_t outPathLen);
static lv_draw_buf_t* takeSnapshotCompat(lv_obj_t* obj, lv_color_format_t cf);

// ---------------------------------------------------------------------------
// USB MSC (unchanged from original)
// ---------------------------------------------------------------------------
#if MM_USB_RUNTIME_MSC
static int32_t usbMscRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    if (!spiGuardLock(pdMS_TO_TICKS(100))) return -1;
    size_t secSize = SD.sectorSize();
    if (secSize == 0 || buffer == nullptr || bufsize == 0) { spiGuardUnlock(); return -1; }
    uint8_t* out = static_cast<uint8_t*>(buffer);
    uint32_t done = 0;
    while (done < bufsize) {
        uint32_t curLba = lba + ((offset + done) / secSize);
        uint32_t curOff = (offset + done) % secSize;
        uint32_t remain = bufsize - done;
        uint32_t avail = (uint32_t)(secSize - curOff);
        uint32_t chunk = (remain < avail) ? remain : avail;
        uint8_t secBuf[512];
        if (secSize > sizeof(secBuf)) { spiGuardUnlock(); return -1; }
        if (!SD.readRAW(secBuf, curLba)) { spiGuardUnlock(); return -1; }
        memcpy(out + done, secBuf + curOff, chunk);
        done += chunk;
    }
    spiGuardUnlock();
    return (int32_t)bufsize;
}
static int32_t usbMscWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    if (!spiGuardLock(pdMS_TO_TICKS(100))) return -1;
    size_t secSize = SD.sectorSize();
    if (secSize == 0 || buffer == nullptr || bufsize == 0) { spiGuardUnlock(); return -1; }
    uint32_t done = 0;
    while (done < bufsize) {
        uint32_t curLba = lba + ((offset + done) / secSize);
        uint32_t curOff = (offset + done) % secSize;
        uint32_t remain = bufsize - done;
        uint32_t avail = (uint32_t)(secSize - curOff);
        uint32_t chunk = (remain < avail) ? remain : avail;
        uint8_t secBuf[512];
        if (secSize > sizeof(secBuf)) { spiGuardUnlock(); return -1; }
        if (!SD.readRAW(secBuf, curLba)) { spiGuardUnlock(); return -1; }
        memcpy(secBuf + curOff, buffer + done, chunk);
        if (!SD.writeRAW(secBuf, curLba)) { spiGuardUnlock(); return -1; }
        done += chunk;
    }
    spiGuardUnlock();
    return (int32_t)bufsize;
}
static bool usbMscStartStop(uint8_t, bool, bool) { return true; }
static bool enableUsbMsc() {
    if (_usbMscStarted) return true;
    if (!spiGuardLock(pdMS_TO_TICKS(200))) return false;
    if (SD.cardType() == CARD_NONE || SD.numSectors() == 0 || SD.sectorSize() == 0) { spiGuardUnlock(); return false; }
    _usbMsc.vendorID("LILYGO"); _usbMsc.productID("MELODY_SD"); _usbMsc.productRevision("1.0");
    _usbMsc.onRead(usbMscRead); _usbMsc.onWrite(usbMscWrite); _usbMsc.onStartStop(usbMscStartStop);
    _usbMsc.isWritable(true); _usbMsc.mediaPresent(true);
    if (!_usbMsc.begin((uint32_t)SD.numSectors(), (uint16_t)SD.sectorSize())) { spiGuardUnlock(); return false; }
    spiGuardUnlock();
    USB.begin();
    _usbMscStarted = true;
    return true;
}
static void disableUsbMsc() {
    if (!_usbMscStarted) return;
    _usbMsc.mediaPresent(false); _usbMsc.end(); _usbMscStarted = false;
}
#else
static bool enableUsbMsc() { return false; }
static void disableUsbMsc() {}
#endif

// ---------------------------------------------------------------------------
// Style helpers (use TH for colours)
// ---------------------------------------------------------------------------
static void stylePanel(lv_obj_t* o, lv_color_t bg) {
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
}

static void styleLabel(lv_obj_t* o, lv_color_t col, const lv_font_t* font) {
    lv_obj_set_style_text_color(o, col, 0);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
}

static void styleChip(lv_obj_t* o, lv_color_t bg, lv_color_t border, lv_color_t text, const lv_font_t* font) {
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(o, border, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_left(o, 4, 0);
    lv_obj_set_style_pad_right(o, 4, 0);
    lv_obj_set_style_pad_top(o, 6, 0);
    lv_obj_set_style_pad_bottom(o, 6, 0);
    lv_obj_set_style_text_color(o, text, 0);
    lv_obj_set_style_text_font(o, font, 0);
}

// ---------------------------------------------------------------------------
// Misc helpers
// ---------------------------------------------------------------------------
static void applyBrightness() {
    instance.setBrightness(_screenDimmed ? 0 : _displayBrightness);
}

static void noteActivity() {
    _lastInputMs = millis();
    _lastActivityForPowerOff = millis();
    if (_screenDimmed) { _screenDimmed = false; applyBrightness(); }
    if (_kbDimmed && _kbBacklight) { _kbDimmed = false; instance.kb.setBrightness(255); }
}

static String stripMp3(const String& src) {
    if (src.length() > 4) {
        String lower = src; lower.toLowerCase();
        if (lower.endsWith(".mp3")) return src.substring(0, src.length() - 4);
    }
    return src;
}

static String trimName(const String& src, int maxLen) {
    if ((int)src.length() <= maxLen) return src;
    if (maxLen < 4) return src.substring(0, maxLen);
    return src.substring(0, maxLen - 3) + "...";
}

static void formatTime(uint32_t sec, char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    if (sec == UINT32_MAX) { snprintf(out, outLen, "--:--"); return; }
    snprintf(out, outLen, "%lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
}

static lv_draw_buf_t* takeSnapshotCompat(lv_obj_t* obj, lv_color_format_t cf) {
    if (!obj) return nullptr;

    // Snapshot implementation compatible with LVGL build where LV_USE_SNAPSHOT=0.
    lv_obj_update_layout(obj);
    int32_t w = lv_obj_get_width(obj);
    int32_t h = lv_obj_get_height(obj);
    int32_t ext = lv_obj_get_ext_draw_size(obj);
    w += ext * 2;
    h += ext * 2;
    if (w <= 0 || h <= 0) return nullptr;

    lv_draw_buf_t* drawBuf = lv_draw_buf_create(w, h, cf, LV_STRIDE_AUTO);
    if (!drawBuf) return nullptr;
    lv_draw_buf_clear(drawBuf, NULL);

    lv_area_t snapshotArea;
    lv_obj_get_coords(obj, &snapshotArea);
    lv_area_increase(&snapshotArea, ext, ext);

    lv_layer_t layer;
    lv_layer_init(&layer);
    layer.draw_buf = drawBuf;
    layer.buf_area.x1 = snapshotArea.x1;
    layer.buf_area.y1 = snapshotArea.y1;
    layer.buf_area.x2 = snapshotArea.x1 + w - 1;
    layer.buf_area.y2 = snapshotArea.y1 + h - 1;
    layer.color_format = cf;
    layer._clip_area = snapshotArea;
    layer.phy_clip_area = snapshotArea;

    lv_display_t* oldDisp = lv_refr_get_disp_refreshing();
    lv_display_t* newDisp = lv_obj_get_display(obj);
    lv_layer_t* oldLayer = newDisp->layer_head;
    newDisp->layer_head = &layer;
    lv_refr_set_disp_refreshing(newDisp);
    lv_obj_redraw(&layer, obj);

    while (layer.draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        lv_draw_dispatch();
    }

    newDisp->layer_head = oldLayer;
    lv_refr_set_disp_refreshing(oldDisp);
    return drawBuf;
}

static bool saveScreenshotToSd(char* outPath, size_t outPathLen) {
    lv_obj_t* scr = lv_screen_active();
    if (!scr) return false;

    lv_draw_buf_t* snap = takeSnapshotCompat(scr, LV_COLOR_FORMAT_RGB565);
    if (!snap || !snap->data) return false;

    const uint32_t w = snap->header.w;
    const uint32_t h = snap->header.h;
    const uint32_t stride = snap->header.stride;
    const uint32_t rowBytes = w * 3U;
    const uint32_t rowPad = (4U - (rowBytes & 3U)) & 3U;
    const uint32_t pixelDataSize = (rowBytes + rowPad) * h;
    const uint32_t fileSize = 14U + 40U + pixelDataSize;

    if (!SD.exists("/SCREENSHOTS")) SD.mkdir("/SCREENSHOTS");

    char path[64];
    snprintf(path, sizeof(path), "/SCREENSHOTS/shot_%lu.bmp", (unsigned long)millis());
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        lv_draw_buf_destroy(snap);
        return false;
    }

    uint8_t fh[14] = {
        'B', 'M',
        (uint8_t)(fileSize & 0xFF), (uint8_t)((fileSize >> 8) & 0xFF),
        (uint8_t)((fileSize >> 16) & 0xFF), (uint8_t)((fileSize >> 24) & 0xFF),
        0, 0, 0, 0,
        54, 0, 0, 0
    };
    f.write(fh, sizeof(fh));

    uint8_t ih[40] = {};
    ih[0] = 40;
    ih[4] = (uint8_t)(w & 0xFF); ih[5] = (uint8_t)((w >> 8) & 0xFF);
    ih[6] = (uint8_t)((w >> 16) & 0xFF); ih[7] = (uint8_t)((w >> 24) & 0xFF);
    ih[8] = (uint8_t)(h & 0xFF); ih[9] = (uint8_t)((h >> 8) & 0xFF);
    ih[10] = (uint8_t)((h >> 16) & 0xFF); ih[11] = (uint8_t)((h >> 24) & 0xFF);
    ih[12] = 1;
    ih[14] = 24;
    ih[20] = (uint8_t)(pixelDataSize & 0xFF); ih[21] = (uint8_t)((pixelDataSize >> 8) & 0xFF);
    ih[22] = (uint8_t)((pixelDataSize >> 16) & 0xFF); ih[23] = (uint8_t)((pixelDataSize >> 24) & 0xFF);
    f.write(ih, sizeof(ih));

    uint8_t line[SW * 3 + 4];
    for (int y = (int)h - 1; y >= 0; --y) {
        const uint16_t* src = reinterpret_cast<const uint16_t*>(snap->data + (uint32_t)y * stride);
        uint8_t* dst = line;
        for (uint32_t x = 0; x < w; ++x) {
            uint16_t p = src[x];
            uint8_t r = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
            uint8_t g = (uint8_t)(((p >> 5)  & 0x3F) * 255 / 63);
            uint8_t b = (uint8_t)(( p        & 0x1F) * 255 / 31);
            *dst++ = b; *dst++ = g; *dst++ = r;
        }
        for (uint32_t i = 0; i < rowPad; ++i) *dst++ = 0;
        f.write(line, rowBytes + rowPad);
    }

    f.close();
    lv_draw_buf_destroy(snap);
    if (outPath && outPathLen > 0) {
        strncpy(outPath, path, outPathLen - 1);
        outPath[outPathLen - 1] = '\0';
    }
    return true;
}

static void refreshBatteryState() {
    uint32_t now = millis();
    if (_batteryPollAt != 0 && (now - _batteryPollAt) < 1500) return;
    _batteryPollAt = now;
    uint32_t probe = instance.getDeviceProbe();
    if (probe & HW_GAUGE_ONLINE) {
        if (instance.gauge.refresh()) {
            int soc = constrain((int)instance.gauge.getStateOfCharge(), 0, 100);
            _batteryPct = (uint8_t)soc;
            BatteryStatus bs = instance.gauge.getBatteryStatus();
            _batteryCharging = !bs.isInDischargeMode();
            return;
        }
    }
    if (probe & HW_PMU_ONLINE) {
        int mv = instance.ppm.getBattVoltage();
        if (mv > 0) {
            int est = (mv - 3300) * 100 / (4200 - 3300);
            _batteryPct = (uint8_t)constrain(est, 0, 100);
        }
        const char* st = instance.ppm.getChargeStatusString();
        if (st) {
            String s = st; s.toLowerCase();
            _batteryCharging = (s.indexOf("charge") >= 0) && (s.indexOf("no charge") < 0);
        }
    }
}

static int currentTrackIndex() {
    if (_playlist.empty() || _plIdx < 0 || _plIdx >= (int)_playlist.size()) return -1;
    return _playlist[_plIdx];
}

// Convert a track-list index to the corresponding browser row index.
// Browser rows: row 0 = ".." if canGoUp, then fileBrowserGet(0..n-1).
// Returns the track's row, or -1 if not found in current dir view.
static int trackIdxToBrowserRow(int trackIdx) {
    if (trackIdx < 0 || trackIdx >= fileBrowserTrackCount()) return -1;
    const String& tpath = fileBrowserTrack(trackIdx).fullPath;
    bool hasBack = fileBrowserCanGoUp();
    int n = fileBrowserCount();
    for (int i = 0; i < n; i++) {
        if (fileBrowserGet(i).fullPath == tpath) {
            return hasBack ? (i + 1) : i;
        }
    }
    return -1;
}

static void clampListWindow(int total) {
    if (total <= 0) { _listCursor = 0; _listScroll = 0; return; }
    _listCursor = constrain(_listCursor, 0, total - 1);
    if (_listCursor < _listScroll) _listScroll = _listCursor;
    if (_listCursor >= _listScroll + PLAYLIST_ROWS) _listScroll = _listCursor - PLAYLIST_ROWS + 1;
    if (_listScroll < 0) _listScroll = 0;
}

static const uint32_t TIMEOUT_OPTS[] = {15, 30, 60, 120, 300, 0};
static const int TIMEOUT_OPTS_N = sizeof(TIMEOUT_OPTS) / sizeof(TIMEOUT_OPTS[0]);

// Power-off timer options (seconds): Off, 15m, 30m, 45m, 1h, 90m, 2h
static const uint32_t POWEROFF_OPTS[] = {0, 900, 1800, 2700, 3600, 5400, 7200};
static const int POWEROFF_OPTS_N = sizeof(POWEROFF_OPTS) / sizeof(POWEROFF_OPTS[0]);

static int powerOffOptionIndex(uint32_t sec) {
    for (int i = 0; i < POWEROFF_OPTS_N; i++) if (POWEROFF_OPTS[i] == sec) return i;
    return 0;
}

static String powerOffLabel(uint32_t sec) {
    if (sec == 0) return "Off";
    if (sec < 3600) { char b[12]; snprintf(b, sizeof(b), "%lum", (unsigned long)(sec / 60)); return String(b); }
    if (sec % 3600 == 0) { char b[12]; snprintf(b, sizeof(b), "%luh", (unsigned long)(sec / 3600)); return String(b); }
    char b[16]; snprintf(b, sizeof(b), "%luh%lum", (unsigned long)(sec/3600), (unsigned long)((sec%3600)/60)); return String(b);
}

static const int KB_MODE_OPTS_N = 5;
static const char* KB_MODE_LABELS[KB_MODE_OPTS_N] = {"Off","15s","30s","1m","Never"};
static const uint32_t KB_MODE_TIMEOUT[KB_MODE_OPTS_N] = {0, 15, 30, 60, 0};
static int _kbMode = 4;

static void applyKbMode() {
    if (_kbMode == 0) {
        _kbBacklight = false; _kbTimeoutSec = 0; instance.kb.setBrightness(0);
    } else {
        _kbBacklight = true; _kbTimeoutSec = KB_MODE_TIMEOUT[_kbMode];
        instance.kb.setBrightness(255); _kbDimmed = false;
    }
}

static int timeoutOptionIndex(uint32_t sec) {
    for (int i = 0; i < TIMEOUT_OPTS_N; i++) if (TIMEOUT_OPTS[i] == sec) return i;
    return 2;
}

static String timeoutLabel(uint32_t sec) {
    if (sec == 0) return "Never";
    if (sec < 60) { char b[12]; snprintf(b, sizeof(b), "%lus", (unsigned long)sec); return String(b); }
    char b[12]; snprintf(b, sizeof(b), "%lum", (unsigned long)(sec / 60)); return String(b);
}

static const char* debugModeLabel(UiDebugMode m) {
    switch (m) {
    case UI_DBG_DISPLAY_ONLY:         return "Display only";
    case UI_DBG_AUDIO_ONLY:           return "Audio only";
    case UI_DBG_AUDIO_SD_NO_UI:       return "Audio+SD no UI";
    case UI_DBG_DISPLAY_SD_NO_DECODE: return "Display+SD no dec";
    default: return "Normal";
    }
}

static bool audioPlaybackAllowed() {
    return _debugMode != UI_DBG_DISPLAY_ONLY && _debugMode != UI_DBG_DISPLAY_SD_NO_DECODE;
}

static bool allowPlayRequest() {
    uint32_t now = millis();
    if (_lastPlayReqMs != 0 && (now - _lastPlayReqMs) < 120) return false;
    _lastPlayReqMs = now;
    return true;
}

static bool isRadioMode() {
    return settingsGetString("app.mode", "mp3") == "radio";
}

static EqPreset eqPresetFromStorage(const String& s) {
    String v = s;
    v.toLowerCase();
    if (v == "bright") return EQ_BRIGHT;
    if (v == "bass")   return EQ_BASS;
    if (v == "vocal")  return EQ_VOCAL;
    return EQ_FLAT;
}

static const char* eqPresetStorageName(EqPreset p) {
    switch (p) {
    case EQ_BRIGHT: return "bright";
    case EQ_BASS:   return "bass";
    case EQ_VOCAL:  return "vocal";
    case EQ_FLAT:
    default:        return "flat";
    }
}

// ---------------------------------------------------------------------------
// Settings persistence (now via settings_store)
// ---------------------------------------------------------------------------
static void loadUiSettings() {
    _displayBrightness = (uint8_t)settingsGetInt("brightness", DEVICE_MAX_BRIGHTNESS_LEVEL);
    if (_displayBrightness < 1 || _displayBrightness > DEVICE_MAX_BRIGHTNESS_LEVEL)
        _displayBrightness = DEVICE_MAX_BRIGHTNESS_LEVEL;

    _screenTimeoutSec = (uint32_t)settingsGetInt("timeout", 60);
    bool valid = false;
    for (int i = 0; i < TIMEOUT_OPTS_N; i++) if (_screenTimeoutSec == TIMEOUT_OPTS[i]) { valid = true; break; }
    if (!valid) _screenTimeoutSec = 60;

    _kbMode = constrain(settingsGetInt("kb_mode", 4), 0, KB_MODE_OPTS_N - 1);

    // Safety: never restore debug mode from persisted settings.
    // A non-UI debug mode can make the device appear "dead" after reboot.
    _debugMode = UI_DBG_NORMAL;

    EqPreset eq = eqPresetFromStorage(settingsGetString("audio.eq", "flat"));
    audioPlayerSetEqPreset(eq);

    int rpt = constrain(settingsGetInt("repeat", 0), (int)RM_NONE, (int)RM_ALL);
    _repeat = (RepeatMode)rpt;

    _shuffle = settingsGetBool("shuffle", false);

    _powerOffTimeoutSec = (uint32_t)settingsGetInt("poweroff_timeout", 0);
    bool poValid = false;
    for (int i = 0; i < POWEROFF_OPTS_N; i++) if (POWEROFF_OPTS[i] == _powerOffTimeoutSec) { poValid = true; break; }
    if (!poValid) _powerOffTimeoutSec = 0;
}

static void saveUiSettings() {
    settingsPutInt("brightness", _displayBrightness);
    settingsPutInt("timeout", (int)_screenTimeoutSec);
    settingsPutInt("kb_mode", _kbMode);
    settingsPutString("audio.eq", eqPresetStorageName(audioPlayerGetEqPreset()));
    settingsPutInt("repeat", (int)_repeat);
    settingsPutBool("shuffle", _shuffle);
    settingsPutInt("poweroff_timeout", (int)_powerOffTimeoutSec);
    settingsSave();
}

// ---------------------------------------------------------------------------
// Screen builders
// ---------------------------------------------------------------------------
static void buildSplash() {
    _scrSplash = lv_obj_create(nullptr);
    stylePanel(_scrSplash, TH->bg);
    lv_obj_t* img = lv_img_create(_scrSplash);
    lv_img_set_src(img, &img_splash);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
}

static void buildPlayer() {
    _scrPlayer = lv_obj_create(nullptr);
    stylePanel(_scrPlayer, TH->bg);

    lv_obj_t* top = lv_obj_create(_scrPlayer);
    lv_obj_set_size(top, SW, TOP_H);
    lv_obj_set_pos(top, 0, 0);
    stylePanel(top, TH->topbar);

    lv_obj_t* topLine = lv_obj_create(_scrPlayer);
    lv_obj_set_size(topLine, SW, 1);
    lv_obj_set_pos(topLine, 0, TOP_H);
    stylePanel(topLine, TH->topbar);

    _lblBatPct = lv_label_create(top);
    styleLabel(_lblBatPct, TH->muted, TH->fontSmall);
    lv_obj_set_pos(_lblBatPct, 408, 6);
    lv_obj_set_width(_lblBatPct, 34);
    lv_obj_set_style_text_align(_lblBatPct, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(_lblBatPct, "75%");

    lv_obj_t* batBox = lv_obj_create(top);
    lv_obj_set_size(batBox, 28, 12);
    lv_obj_set_pos(batBox, 444, 5);
    lv_obj_set_style_bg_opa(batBox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(batBox, TH->muted, 0);
    lv_obj_set_style_border_width(batBox, 1, 0);
    lv_obj_set_style_radius(batBox, 1, 0);
    lv_obj_set_style_pad_all(batBox, 0, 0);

    lv_obj_t* batTip = lv_obj_create(top);
    lv_obj_set_size(batTip, 3, 6);
    lv_obj_set_pos(batTip, 472, 8);
    stylePanel(batTip, TH->muted);

    _batFill = lv_obj_create(top);
    lv_obj_set_size(_batFill, BAT_FILL_MAX_W, 8);
    lv_obj_set_pos(_batFill, 446, 7);
    stylePanel(_batFill, TH->accent);

    // Charging lightning bolt — inside battery box, yellow, scaled down + rotated
    _lblBatCharge = lv_label_create(top);
    styleLabel(_lblBatCharge, lv_color_hex(0xFFCC00), TH->fontSmall);
    lv_obj_set_pos(_lblBatCharge, 456, 7);
    lv_label_set_text(_lblBatCharge, LV_SYMBOL_CHARGE);
    lv_obj_set_style_transform_scale(_lblBatCharge, 190, 0);   // ~74% of original
    lv_obj_set_style_transform_rotation(_lblBatCharge, 200, 0); // 20° clockwise
    lv_obj_set_style_transform_pivot_x(_lblBatCharge, 0, 0);
    lv_obj_set_style_transform_pivot_y(_lblBatCharge, 0, 0);
    lv_obj_add_flag(_lblBatCharge, LV_OBJ_FLAG_HIDDEN);

    // WiFi icon
    _lblWifi = lv_label_create(top);
    styleLabel(_lblWifi, TH->accent, TH->fontSmall);
    lv_obj_set_pos(_lblWifi, 380, 6);
    lv_label_set_text(_lblWifi, "");

    _barVolume = lv_bar_create(_scrPlayer);
    lv_obj_set_size(_barVolume, 5, CONTENT_H);
    lv_obj_set_pos(_barVolume, 0, CONTENT_Y);
    lv_obj_set_style_bg_color(_barVolume, TH->dim, 0);
    lv_obj_set_style_bg_color(_barVolume, TH->vol, LV_PART_INDICATOR);
    lv_obj_set_style_radius(_barVolume, 0, 0);
    lv_obj_set_style_radius(_barVolume, 0, LV_PART_INDICATOR);
    lv_bar_set_start_value(_barVolume, 0, LV_ANIM_OFF);
    lv_bar_set_range(_barVolume, 0, 100);
    lv_bar_set_value(_barVolume, 70, LV_ANIM_OFF);

    lv_obj_t* split = lv_obj_create(_scrPlayer);
    lv_obj_set_size(split, 1, CONTENT_H);
    lv_obj_set_pos(split, DIVIDER_X, CONTENT_Y);
    stylePanel(split, TH->divider);

    _nowClip = lv_obj_create(_scrPlayer);
    lv_obj_set_size(_nowClip, MQ_CLIP_W, 26);
    lv_obj_set_pos(_nowClip, 18, 24);
    stylePanel(_nowClip, TH->bg);
    lv_obj_add_flag(_nowClip, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(_nowClip, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_clip_corner(_nowClip, false, 0);
    lv_obj_set_scrollbar_mode(_nowClip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(_nowClip, LV_OBJ_FLAG_SCROLLABLE);

    _lblNow = lv_label_create(_nowClip);
    styleLabel(_lblNow, TH->warn, TH->fontTrack);
    lv_obj_set_pos(_lblNow, 0, 0);
    lv_obj_set_width(_lblNow, LV_SIZE_CONTENT);
    lv_obj_set_style_text_letter_space(_lblNow, 1, 0);
    lv_obj_set_style_text_align(_lblNow, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(_lblNow, LV_LABEL_LONG_CLIP);
    lv_label_set_text(_lblNow, "No track selected");

    _lblState = lv_label_create(_scrPlayer);
    styleLabel(_lblState, TH->muted, TH->fontBody);
    lv_obj_set_pos(_lblState, 18, 54);
    lv_obj_set_width(_lblState, 282);
    lv_obj_set_style_text_align(_lblState, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(_lblState, "track 00 of 00");

    const int chipW = 134;
    const int chipH = 26;

    _lblVol = lv_label_create(_scrPlayer);
    styleChip(_lblVol, TH->bg, TH->vol, TH->vol, TH->fontBody);
    lv_obj_set_size(_lblVol, chipW, chipH);
    lv_obj_set_pos(_lblVol, 18, 94);
    lv_obj_set_style_text_align(_lblVol, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(_lblVol, "VOL 70%");

    _lblShuffle = lv_label_create(_scrPlayer);
    styleChip(_lblShuffle, TH->bg, TH->border, TH->accent, TH->fontBody);
    lv_obj_set_size(_lblShuffle, chipW, chipH);
    lv_obj_set_pos(_lblShuffle, 160, 94);
    lv_obj_set_style_text_align(_lblShuffle, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(_lblShuffle, "PLAYING");

    _lblRepeat = lv_label_create(_scrPlayer);
    styleChip(_lblRepeat, TH->bg, TH->border, TH->muted, TH->fontBody);
    lv_obj_set_size(_lblRepeat, chipW, chipH);
    lv_obj_set_pos(_lblRepeat, 18, 128);
    lv_obj_set_style_text_align(_lblRepeat, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(_lblRepeat, "RPT OFF");

    _lblPlCount = lv_label_create(_scrPlayer);
    styleChip(_lblPlCount, TH->bg, TH->border, TH->muted, TH->fontBody);
    lv_obj_set_size(_lblPlCount, chipW, chipH);
    lv_obj_set_pos(_lblPlCount, 160, 128);
    lv_obj_set_style_text_align(_lblPlCount, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(_lblPlCount, "SHF OFF");

    _lblInfoIndicator = lv_label_create(_scrPlayer);
    styleChip(_lblInfoIndicator, TH->bg, TH->border, TH->text, TH->fontBody);
    lv_obj_set_size(_lblInfoIndicator, chipW, chipH);
    lv_obj_set_pos(_lblInfoIndicator, 18, 162);
    lv_obj_set_style_text_align(_lblInfoIndicator, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(_lblInfoIndicator, "HELP [ i ]");

    _lblMode = lv_label_create(_scrPlayer);
    styleChip(_lblMode, TH->bg, TH->border, TH->text, TH->fontBody);
    lv_obj_set_size(_lblMode, chipW, chipH);
    lv_obj_set_pos(_lblMode, 160, 162);
    lv_obj_set_style_text_align(_lblMode, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(_lblMode, isRadioMode() ? "RADIO" : "MP3");

    lv_obj_t* rightPanel = lv_obj_create(_scrPlayer);
    lv_obj_set_size(rightPanel, RIGHT_W, CONTENT_H);
    lv_obj_set_pos(rightPanel, RIGHT_X, CONTENT_Y);
    stylePanel(rightPanel, TH->panel);

    lv_obj_t* rightHeadBg = lv_obj_create(_scrPlayer);
    lv_obj_set_size(rightHeadBg, RIGHT_W, 16);
    lv_obj_set_pos(rightHeadBg, RIGHT_X, CONTENT_Y);
    stylePanel(rightHeadBg, TH->hdr);

    lv_obj_t* rightHeadSep = lv_obj_create(_scrPlayer);
    lv_obj_set_size(rightHeadSep, RIGHT_W, 1);
    lv_obj_set_pos(rightHeadSep, RIGHT_X, 38);
    stylePanel(rightHeadSep, TH->divider);

    _lblListTitle = lv_label_create(_scrPlayer);
    styleLabel(_lblListTitle, TH->accent, TH->fontSmall);
    lv_obj_set_pos(_lblListTitle, 310, 25);
    lv_label_set_text(_lblListTitle, isRadioMode() ? "STATIONS" : "BROWSER");

    for (int i = 0; i < PLAYLIST_ROWS; i++) {
        int ry = 38 + i * 20;
        _plRow[i] = lv_obj_create(_scrPlayer);
        lv_obj_set_size(_plRow[i], RIGHT_W, 20);
        lv_obj_set_pos(_plRow[i], RIGHT_X, ry);
        stylePanel(_plRow[i], TH->panel);

        _plLabel[i] = lv_label_create(_plRow[i]);
        styleLabel(_plLabel[i], TH->dim, TH->fontBody);
        lv_obj_set_width(_plLabel[i], RIGHT_W - 12);
        lv_obj_set_height(_plLabel[i], LV_SIZE_CONTENT);
        lv_obj_align(_plLabel[i], LV_ALIGN_LEFT_MID, 6, 0);
        lv_label_set_long_mode(_plLabel[i], LV_LABEL_LONG_CLIP);
        lv_label_set_text(_plLabel[i], "");
    }

    lv_obj_t* footBg = lv_obj_create(_scrPlayer);
    lv_obj_set_size(footBg, SW, PROGRESS_H);
    lv_obj_set_pos(footBg, 0, SH - PROGRESS_H);
    stylePanel(footBg, TH->dim);

    _barProgress = lv_obj_create(_scrPlayer);
    lv_obj_set_size(_barProgress, 1, PROGRESS_H);
    lv_obj_set_pos(_barProgress, 0, SH - PROGRESS_H);
    stylePanel(_barProgress, TH->accent);
}

static void buildInfo() {
    _scrInfo = lv_obj_create(nullptr);
    stylePanel(_scrInfo, TH->bg);

    lv_obj_t* hdr = lv_obj_create(_scrInfo);
    lv_obj_set_size(hdr, SW, TOP_H);
    lv_obj_set_pos(hdr, 0, 0);
    stylePanel(hdr, TH->topbar);

    lv_obj_t* title = lv_label_create(hdr);
    styleLabel(title, TH->accent, TH->fontBody);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, PAD, 0);
    lv_label_set_text(title, "Controls");

    lv_obj_t* topLine = lv_obj_create(_scrInfo);
    lv_obj_set_size(topLine, SW, 1);
    lv_obj_set_pos(topLine, 0, TOP_H);
    stylePanel(topLine, TH->topbar);

    const int colGap = 12;
    const int colW = (SW - PAD * 2 - colGap) / 2;

    _lblInfo = lv_label_create(_scrInfo);
    styleLabel(_lblInfo, TH->text, TH->fontBody);
    lv_obj_set_pos(_lblInfo, PAD, TOP_H + 8);
    lv_obj_set_width(_lblInfo, colW);
    lv_obj_set_style_text_line_space(_lblInfo, 6, 0);
    lv_label_set_long_mode(_lblInfo, LV_LABEL_LONG_WRAP);
    lv_label_set_text(_lblInfo,
        "[Q][A]       volume +/-\n"
        "[W][D]       prev / next\n"
        "[SPC]        play / pause\n"
        "[B]          stop / back\n"
        "[R]          repeat mode\n"
        "[H]          shuffle\n"
        "[N]          seek mode");

    _lblInfoRight = lv_label_create(_scrInfo);
    styleLabel(_lblInfoRight, TH->text, TH->fontBody);
    lv_obj_set_pos(_lblInfoRight, PAD + colW + colGap, TOP_H + 8);
    lv_obj_set_width(_lblInfoRight, colW);
    lv_obj_set_style_text_line_space(_lblInfoRight, 6, 0);
    lv_label_set_long_mode(_lblInfoRight, LV_LABEL_LONG_WRAP);
    lv_label_set_text(_lblInfoRight,
        "[S]          settings\n"
        "[S+H]        screenshot\n"
        "[rot]        list scroll\n"
        "[rot click]  play select\n"
        "[ENTER]      play track\n"
        "seek: [rot] +/-5s\n"
        "      [ENTER/B] exit");

    lv_obj_t* hint = lv_label_create(_scrInfo);
    styleLabel(hint, TH->muted, TH->fontBody);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, PAD, -3);
    lv_label_set_text(hint, "B: BACK");
}

static void buildSettings() {
    _scrSettings = lv_obj_create(nullptr);
    stylePanel(_scrSettings, TH->bg);

    lv_obj_t* hdr = lv_obj_create(_scrSettings);
    lv_obj_set_size(hdr, SW, TOP_H);
    lv_obj_set_pos(hdr, 0, 0);
    stylePanel(hdr, TH->topbar);

    lv_obj_t* title = lv_label_create(hdr);
    styleLabel(title, TH->accent, TH->fontBody);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, PAD, 0);
    lv_label_set_text(title, "Settings");

    const char* names[SETTINGS_COUNT] = {
        "Back",
        "Mode",
        "WiFi network",
        "Equalizer",
        "Brightness",
        "Screen timeout",
        "KB backlight",
        "Theme",
        "Auto power-off",
        "Debug mode",
        "USB mode",
        "Restart device",
        "Power off"
    };

    int y0 = TOP_H + 2;
    for (int i = 0; i < SETTINGS_COUNT; i++) {
        _settingsRow[i] = lv_obj_create(_scrSettings);
        lv_obj_set_size(_settingsRow[i], SW - 16, SETTINGS_ROW_H);
        lv_obj_set_pos(_settingsRow[i], 8, y0 + i * (SETTINGS_ROW_H + SETTINGS_ROW_GAP));
        lv_obj_set_style_bg_color(_settingsRow[i], TH->bg, 0);
        lv_obj_set_style_bg_opa(_settingsRow[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(_settingsRow[i], TH->border, 0);
        lv_obj_set_style_border_width(_settingsRow[i], 1, 0);
        lv_obj_set_style_radius(_settingsRow[i], 0, 0);
        lv_obj_set_style_pad_all(_settingsRow[i], 0, 0);

        _settingsName[i] = lv_label_create(_settingsRow[i]);
        styleLabel(_settingsName[i], TH->text, TH->fontBody);
        lv_obj_align(_settingsName[i], LV_ALIGN_LEFT_MID, PAD, 0);
        lv_label_set_text(_settingsName[i], names[i]);

        _settingsValue[i] = lv_label_create(_settingsRow[i]);
        styleLabel(_settingsValue[i], TH->accent, TH->fontSmall);
        lv_obj_align(_settingsValue[i], LV_ALIGN_RIGHT_MID, -PAD, 0);
        lv_label_set_text(_settingsValue[i], "");
    }

    lv_obj_t* hint = lv_label_create(_scrSettings);
    styleLabel(hint, TH->muted, TH->fontBody);
    lv_obj_set_width(hint, SW - PAD * 2);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -3);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_CLIP);
    lv_label_set_text(hint, "ROT: SELECT   A/D: ADJUST   ENTER: OK   B: BACK");
}

static void buildWifi() {
    _scrWifi = lv_obj_create(nullptr);
    stylePanel(_scrWifi, TH->bg);

    lv_obj_t* hdr = lv_obj_create(_scrWifi);
    lv_obj_set_size(hdr, SW, TOP_H);
    lv_obj_set_pos(hdr, 0, 0);
    stylePanel(hdr, TH->topbar);

    lv_obj_t* title = lv_label_create(hdr);
    styleLabel(title, TH->accent, TH->fontBody);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, PAD, 0);
    lv_label_set_text(title, "WiFi Networks");

    _wifiStatus = lv_label_create(_scrWifi);
    styleLabel(_wifiStatus, TH->text, TH->fontBody);
    lv_obj_set_pos(_wifiStatus, PAD, TOP_H + 4);
    lv_obj_set_width(_wifiStatus, SW - PAD * 2);
    lv_label_set_text(_wifiStatus, "Scanning...");

    for (int i = 0; i < WIFI_ROWS; i++) {
        int ry = TOP_H + 22 + i * 22;
        _wifiRow[i] = lv_obj_create(_scrWifi);
        lv_obj_set_size(_wifiRow[i], SW - 16, 20);
        lv_obj_set_pos(_wifiRow[i], 8, ry);
        lv_obj_set_style_bg_color(_wifiRow[i], TH->bg, 0);
        lv_obj_set_style_bg_opa(_wifiRow[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(_wifiRow[i], TH->border, 0);
        lv_obj_set_style_border_width(_wifiRow[i], 1, 0);
        lv_obj_set_style_radius(_wifiRow[i], 0, 0);
        lv_obj_set_style_pad_all(_wifiRow[i], 0, 0);

        _wifiLabel[i] = lv_label_create(_wifiRow[i]);
        styleLabel(_wifiLabel[i], TH->dim, TH->fontBody);
        lv_obj_align(_wifiLabel[i], LV_ALIGN_LEFT_MID, 6, 0);
        lv_label_set_text(_wifiLabel[i], "");
    }

    lv_obj_t* hint = lv_label_create(_scrWifi);
    styleLabel(hint, TH->muted, TH->fontBody);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, PAD, -3);
    lv_label_set_text(hint, "ROT: select  ENTER: connect  B: back");
}

// ---------------------------------------------------------------------------
// Teardown / rebuild for theme switching
// ---------------------------------------------------------------------------
static void teardownScreens() {
    if (_scrSplash)   { lv_obj_delete(_scrSplash);   _scrSplash   = nullptr; }
    if (_scrPlayer)   { lv_obj_delete(_scrPlayer);   _scrPlayer   = nullptr; }
    if (_scrSettings) { lv_obj_delete(_scrSettings); _scrSettings = nullptr; }
    if (_scrInfo)     { lv_obj_delete(_scrInfo);     _scrInfo     = nullptr; }
    if (_scrWifi)     { lv_obj_delete(_scrWifi);     _scrWifi     = nullptr; }
    _pwOverlay = nullptr; _pwPrompt = nullptr; _pwLabel = nullptr;
}

static void rebuildScreens() {
    buildSplash();
    buildPlayer();
    buildSettings();
    buildInfo();
    buildWifi();
}

// ---------------------------------------------------------------------------
// Show screen
// ---------------------------------------------------------------------------
static void showScreen(UiScreen s) {
    lv_obj_t* target = nullptr;
    if (s == UI_SPLASH) {
        target = _scrSplash;
    } else if (s == UI_SETTINGS) {
        target = _scrSettings;
        refreshSettings();
    } else if (s == UI_INFO) {
        target = _scrInfo;
    } else if (s == UI_WIFI) {
        target = _scrWifi;
        _wifiCursor = 0; _wifiScroll_ = 0; _wifiCount = 0;
        if (wifiStartScan()) {
            _wifiScanPending = true;
            lv_label_set_text(_wifiStatus, "Scanning...");
        } else {
            _wifiScanPending = false;
            lv_label_set_text(_wifiStatus, "Scan failed, press R");
        }
    } else {
        target = _scrPlayer;
        refreshPlayer();
        s = UI_PLAYER;
    }
    if (target) lv_scr_load(target);
    _screen = s;
}

// ---------------------------------------------------------------------------
// Playlist / play helpers
// ---------------------------------------------------------------------------
static void buildPlaylist(int startTrackIndex) {
    int n = fileBrowserTrackCount();
    _playlist.clear();
    for (int i = 0; i < n; i++) _playlist.push_back(i);

    if (_shuffle && n > 1) {
        for (int i = n - 1; i > 0; i--) {
            int j = esp_random() % (i + 1);
            std::swap(_playlist[i], _playlist[j]);
        }
        for (int i = 0; i < n; i++) {
            if (_playlist[i] == startTrackIndex) { std::swap(_playlist[0], _playlist[i]); break; }
        }
        _plIdx = 0;
    } else {
        _plIdx = startTrackIndex;
    }
}

static void startPlaying(int trackIdx) {
    if (!audioPlaybackAllowed()) return;
    if (!allowPlayRequest()) return;
    if (trackIdx < 0 || trackIdx >= fileBrowserTrackCount()) return;
    buildPlaylist(trackIdx);
    const FileEntry& e = fileBrowserTrack(_playlist[_plIdx]);
    audioPlayerPlay(e.fullPath.c_str(), e.size);
    // Keep _listCursor where the user had it (browser cursor ≠ track index).
    // Only clamp to the browser total so it stays valid.
    int browserTotal = fileBrowserCount() + (fileBrowserCanGoUp() ? 1 : 0);
    clampListWindow(browserTotal > 0 ? browserTotal : 1);
    if (_screen != UI_PLAYER) showScreen(UI_PLAYER);
    else refreshPlayer();
}

static void playNext() {
    if (!audioPlaybackAllowed()) return;
    if (!allowPlayRequest()) return;
    int n = (int)_playlist.size();
    if (n == 0) return;
    if (_repeat == RM_ONE) {
        const FileEntry& e = fileBrowserTrack(_playlist[_plIdx]);
        audioPlayerPlay(e.fullPath.c_str(), e.size);
        return;
    }
    _plIdx++;
    if (_plIdx >= n) {
        if (_repeat == RM_ALL) {
            _plIdx = 0;
            if (_shuffle && n > 1) {
                for (int i = n - 1; i > 0; i--) { int j = esp_random()%(i+1); std::swap(_playlist[i],_playlist[j]); }
                _plIdx = 0;
            }
        } else {
            _plIdx = n - 1;
            audioPlayerStop();
            refreshPlayer();
            return;
        }
    }
    const FileEntry& e = fileBrowserTrack(_playlist[_plIdx]);
    audioPlayerPlay(e.fullPath.c_str(), e.size);
    { int row = trackIdxToBrowserRow(_playlist[_plIdx]); if (row >= 0) _listCursor = row; }
    int bt = fileBrowserCount() + (fileBrowserCanGoUp() ? 1 : 0);
    clampListWindow(bt > 0 ? bt : 1);
    refreshPlayer();
}

static void playPrev() {
    if (!audioPlaybackAllowed()) return;
    if (!allowPlayRequest()) return;
    int n = (int)_playlist.size();
    if (n == 0) return;
    if (audioPlayerGetElapsed() > 3) {
        const FileEntry& e = fileBrowserTrack(_playlist[_plIdx]);
        audioPlayerPlay(e.fullPath.c_str(), e.size);
        refreshPlayer();
        return;
    }
    _plIdx--;
    if (_plIdx < 0) _plIdx = (_repeat == RM_ALL) ? n - 1 : 0;
    const FileEntry& e = fileBrowserTrack(_playlist[_plIdx]);
    audioPlayerPlay(e.fullPath.c_str(), e.size);
    { int row = trackIdxToBrowserRow(_playlist[_plIdx]); if (row >= 0) _listCursor = row; }
    int bt = fileBrowserCount() + (fileBrowserCanGoUp() ? 1 : 0);
    clampListWindow(bt > 0 ? bt : 1);
    refreshPlayer();
}

static void switchMode(bool toRadio) {
    audioPlayerStop();
    radioPlayerStop();
    settingsPutString("app.mode", toRadio ? "radio" : "mp3");
    settingsSave();
    _playlist.clear();
    _plIdx = 0;
    _listCursor = 0;
    _listScroll = 0;
    _mp3BrowserMode = true;
    if (toRadio) {
        fileBrowserScanRadio("/melody_machine/m3u");
        wifiAutoReconnect();
    } else {
        wifiDisconnect(); // MP3 mode keeps WiFi disabled
        if (!fileBrowserScan("/melody_machine/mp3"))
            if (!fileBrowserScan("/MP3")) fileBrowserScan("/");
    }
    refreshPlayer();
}

static void requestRadioPlay(int idx, const char* why) {
    int n = fileBrowserRadioCount();
    if (n <= 0) return;
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    Serial.printf("[UI-RPLAY] why=%s idx=%d cur=%d pl=%d rs=%d\n",
                  (why ? why : "?"), idx, _listCursor, _plIdx, (int)radioPlayerGetStatus());
    // Row 0 is always "..", so station idx maps to browser row idx+1
    _listCursor = idx + 1;
    RadioStatus rs = radioPlayerGetStatus();
    bool radioActive = (rs == RS_CONNECTING || rs == RS_BUFFERING || rs == RS_PLAYING);
    // Avoid re-sending PLAY to the same active station.
    if (radioActive && idx == _plIdx) return;
    _pendingRadioPlayIdx = idx;
    _pendingRadioPlayAtMs = millis() + (radioActive ? 120 : 80);
}

static void applyEqPresetFromUi(EqPreset preset) {
    if (preset < EQ_FLAT || preset >= EQ_PRESET_COUNT) preset = EQ_FLAT;
    audioPlayerSetEqPreset(preset);
    saveUiSettings();

    if (isRadioMode()) {
        int n = fileBrowserRadioCount();
        if (n > 0 && _plIdx >= 0 && _plIdx < n) {
            RadioStatus rs = radioPlayerGetStatus();
            if (rs == RS_CONNECTING || rs == RS_BUFFERING || rs == RS_PLAYING) {
                // Restart current stream so new EQ is heard immediately.
                radioPlayerStop();
                _pendingRadioPlayIdx = _plIdx;
                _pendingRadioPlayAtMs = millis() + 140;
            }
        }
        return;
    }

    if (_playlist.empty() || _plIdx < 0 || _plIdx >= (int)_playlist.size()) return;
    if (audioPlayerGetState() == PS_STOPPED) return;

    int curTrack = _playlist[_plIdx];
    if (curTrack < 0 || curTrack >= fileBrowserTrackCount()) return;
    const FileEntry& e = fileBrowserTrack(curTrack);
    // Restart current track so new EQ is heard immediately.
    audioPlayerPlay(e.fullPath.c_str(), e.size);
}

// ---------------------------------------------------------------------------
// Refresh: player screen
// ---------------------------------------------------------------------------
static void refreshPlayer() {
    bool radio = isRadioMode();

    // Allow S+H chord in player. If H is not pressed shortly after S,
    // fall back to normal Settings action.
    if (_screen == UI_PLAYER && _pendingSComboAtMs != 0 &&
        (millis() - _pendingSComboAtMs > SHOT_COMBO_WINDOW_MS)) {
        _pendingSComboAtMs = 0;
        _settingsCursor = 0;
        _settingsScroll = 0;
        _returnScreen = UI_PLAYER;
        showScreen(UI_SETTINGS);
        return;
    }
    // In radio mode: if showing playlist list, total = m3u count; if showing stations, total = station count
    bool radioShowingPlaylists = radio && (fileBrowserM3uSelected() < 0) && (fileBrowserM3uCount() > 0);
    int total;
    if (radio) {
        total = radioShowingPlaylists ? fileBrowserM3uCount() : fileBrowserRadioCount();
    } else {
        // MP3 browser: show fileBrowserCount() (dirs+files), plus ".." row if can go up
        int browserTotal = fileBrowserCount() + (fileBrowserCanGoUp() ? 1 : 0);
        total = browserTotal;
    }
    if (!radio) {
        // clamp against browser total
        int browserTotal = fileBrowserCount() + (fileBrowserCanGoUp() ? 1 : 0);
        if (browserTotal <= 0) { _listCursor = 0; _listScroll = 0; }
        else {
            _listCursor = constrain(_listCursor, 0, browserTotal - 1);
            if (_listCursor < _listScroll) _listScroll = _listCursor;
            if (_listCursor >= _listScroll + PLAYLIST_ROWS) _listScroll = _listCursor - PLAYLIST_ROWS + 1;
            if (_listScroll < 0) _listScroll = 0;
        }
    }

    refreshBatteryState();
    char batBuf[12];
    snprintf(batBuf, sizeof(batBuf), "%u%%", (unsigned)_batteryPct);
    lv_label_set_text(_lblBatPct, batBuf);
    int batW = (BAT_FILL_MAX_W * (int)_batteryPct) / 100;
    if (_batteryPct > 0 && batW < 1) batW = 1;
    if (batW > BAT_FILL_MAX_W) batW = BAT_FILL_MAX_W;
    lv_obj_set_size(_batFill, batW, 8);
    // Win96: topbar is navy, accent is also navy — use grey so fill is visible
    lv_color_t batColor;
    if (_batteryPct <= 20)
        batColor = TH->vol;
    else if (themeGetActiveId() == TH_WIN95)
        batColor = lv_color_hex(0x808080);
    else
        batColor = TH->accent;
    lv_obj_set_style_bg_color(_batFill, batColor, 0);

    // Charging icon
    if (_lblBatCharge) {
        if (_batteryCharging)
            lv_obj_clear_flag(_lblBatCharge, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(_lblBatCharge, LV_OBJ_FLAG_HIDDEN);
    }

    // WiFi icon
    if (wifiIsConnected()) {
#ifdef LV_SYMBOL_WIFI
        lv_label_set_text(_lblWifi, LV_SYMBOL_WIFI);
#else
        lv_label_set_text(_lblWifi, "WiFi");
#endif
        lv_obj_set_style_text_color(_lblWifi, TH->accent, 0);
    } else {
        lv_label_set_text(_lblWifi, "");
    }

    lv_label_set_text(_lblMode, radio ? "RADIO" : "MP3");
    lv_obj_set_style_text_color(_lblMode, radio ? TH->warn : TH->text, 0);

    PlayerState st = audioPlayerGetState();

    // Volume indicators must be refreshed in both MP3 and Radio modes.
    uint8_t vol = audioPlayerGetVolume();
    lv_bar_set_value(_barVolume, vol, LV_ANIM_OFF);
    char volBuf[20];
    snprintf(volBuf, sizeof(volBuf), "VOL %d%%", (int)vol);
    lv_label_set_text(_lblVol, volBuf);

    if (radio) {
        // Radio mode display
        RadioStatus rs = radioPlayerGetStatus();

        String station = radioPlayerGetStationName();
        if (station.length() == 0 && total > 0 && _plIdx >= 0 && _plIdx < total) {
            station = fileBrowserRadioGet(_plIdx).name;
        }
        if (station.length() == 0) station = "Internet Radio";
        lv_label_set_text(_lblNow, station.c_str());

        char subBuf[48];
        const char* rsText = "Stopped";
        if (rs == RS_CONNECTING) rsText = "Connecting";
        else if (rs == RS_BUFFERING) rsText = "Buffering";
        else if (rs == RS_PLAYING) rsText = "Playing";
        else if (rs == RS_ERROR) rsText = "Error";
        snprintf(subBuf, sizeof(subBuf), "%s  [%02d/%02d]", rsText, _plIdx + 1, total);
        lv_label_set_text(_lblState, subBuf);

        // Keep status chip simple in radio mode; buffering is shown by bottom progress bar.
        lv_label_set_text(_lblShuffle, (rs == RS_IDLE || rs == RS_ERROR) ? "STOPPED" : "PLAYING");
        lv_obj_set_style_text_color(_lblShuffle, (rs == RS_PLAYING) ? TH->accent : TH->text, 0);
        lv_obj_set_style_border_color(_lblShuffle, (rs == RS_PLAYING) ? TH->accent : TH->border, 0);
        lv_label_set_text(_lblRepeat, "RPT --");
        lv_obj_set_style_text_color(_lblRepeat, TH->muted, 0);
        lv_obj_set_style_border_color(_lblRepeat, TH->border, 0);
        lv_label_set_text(_lblPlCount, "SHF --");
        lv_obj_set_style_text_color(_lblPlCount, TH->muted, 0);
        lv_obj_set_style_border_color(_lblPlCount, TH->border, 0);

        // Progress bar in radio mode:
        // - CONNECTING/BUFFERING: show buffer fill.
        //   If decoder reports 0 for a while, use a capped fallback ramp (not infinite).
        // - PLAYING: full green.
        static RadioStatus prevRs = RS_IDLE;
        static uint32_t rsPhaseStartMs = 0;
        if (rs != prevRs) {
            prevRs = rs;
            rsPhaseStartMs = millis();
        }

        int fillPct = radioPlayerGetBufferFillPct();
        if (fillPct < 0) fillPct = 0;
        if (fillPct > 100) fillPct = 100;

        int fillW;
        lv_color_t barCol;
        if (rs == RS_PLAYING) {
            fillW = SW;
            barCol = lv_color_hex(0x18ff78); // solid green when playing
        } else if (rs == RS_BUFFERING) {
            fillW = SW; // buffer is already full at this point (sync fill in begin())
            barCol = TH->warn;
        } else if (rs == RS_CONNECTING) {
            fillW = SW / 4; // fixed yellow stub while TCP connects
            barCol = TH->warn;
        } else {
            fillW = 0;
            barCol = TH->warn;
        }
        lv_obj_set_width(_barProgress, fillW > 0 ? fillW : 1);
        lv_obj_set_style_bg_color(_barProgress, barCol, 0);
        static uint32_t lastUiRadLog = 0;
        if (millis() - lastUiRadLog > 1000) {
            lastUiRadLog = millis();
            Serial.printf("[UI-RAD] rs=%d rawPct=%d fillW=%d sw=%d\n",
                          (int)rs, fillPct, fillW, SW);
        }

        // Playlist rows: either list of .m3u files, or stations inside selected playlist
        lv_label_set_text(_lblListTitle, radioShowingPlaylists ? "PLAYLISTS" : "STATIONS");
        for (int i = 0; i < PLAYLIST_ROWS; i++) {
            int idx = _listScroll + i;
            if (idx >= total) {
                lv_obj_set_style_bg_color(_plRow[i], TH->panel, 0);
                lv_obj_set_style_border_width(_plRow[i], 0, 0);
                lv_label_set_text(_plLabel[i], "");
                continue;
            }
            bool isCur;
            bool isBack = false;
            String itemText;
            if (radioShowingPlaylists) {
                isCur = (idx == _listCursor);
                char nbuf[8]; snprintf(nbuf, sizeof(nbuf), "%02d", idx + 1);
                itemText = String(nbuf) + "  " + trimName(fileBrowserM3uName(idx), 17);
            } else {
                // Stations: row 0 is always ".." (back to playlist list)
                bool hasBack = (fileBrowserM3uCount() > 0);
                if (hasBack && idx == 0) {
                    isBack = true;
                    isCur  = (_listCursor == 0);
                    itemText = "..";
                } else {
                    int stIdx = hasBack ? (idx - 1) : idx;
                    isCur = (idx == _listCursor);
                    bool playing = (stIdx == _plIdx);
                    char nbuf[8]; snprintf(nbuf, sizeof(nbuf), "%02d", stIdx + 1);
                    itemText = String(nbuf) + "  ";
                    if (playing) itemText += "> ";
                    itemText += trimName(fileBrowserRadioGet(stIdx).name, playing ? 17 : 19);
                }
            }
            if (isCur) {
                lv_obj_set_style_bg_color(_plRow[i], TH->hilight, 0);
                lv_obj_set_style_border_color(_plRow[i], TH->warn, 0);
                lv_obj_set_style_border_width(_plRow[i], 1, 0);
                lv_obj_set_style_text_color(_plLabel[i], isBack ? TH->muted : TH->text, 0);
            } else {
                // Highlight currently playing station row
                bool stPlaying = false;
                if (!radioShowingPlaylists && !isBack) {
                    bool hasBack = (fileBrowserM3uCount() > 0);
                    int stIdx = hasBack ? (idx - 1) : idx;
                    stPlaying = (stIdx == _plIdx) && (radioPlayerGetStatus() == RS_PLAYING || radioPlayerGetStatus() == RS_BUFFERING || radioPlayerGetStatus() == RS_CONNECTING);
                }
                if (stPlaying) {
                    lv_obj_set_style_bg_color(_plRow[i], TH->playrow, 0);
                    lv_obj_set_style_border_width(_plRow[i], 0, 0);
                    lv_obj_set_style_text_color(_plLabel[i], TH->accent, 0);
                } else {
                    lv_obj_set_style_bg_color(_plRow[i], TH->panel, 0);
                    lv_obj_set_style_border_width(_plRow[i], 0, 0);
                    lv_obj_set_style_text_color(_plLabel[i], isBack ? TH->muted : TH->dim, 0);
                }
            }
            lv_label_set_text(_plLabel[i], itemText.c_str());
        }
    } else {
        // MP3 mode display
        int curTrack = currentTrackIndex();
        if (curTrack != _uiElapsedTrackLast) {
            _uiElapsedTrackLast = curTrack;
            _uiElapsedRawLast = 0;
            _uiElapsedBaseSec = 0;
            _uiElapsedBaseMs = millis();
            _uiDurationUnreliable = false;
            _uiDurationShownSec = 0;
        }
        int trackNum = (curTrack >= 0) ? (curTrack + 1) : 0;

        if (curTrack >= 0 && curTrack < total) {
            lv_label_set_text(_lblNow, stripMp3(fileBrowserTrack(curTrack).name).c_str());
        } else {
            lv_label_set_text(_lblNow, "No track selected");
        }

        if (_usbModeEnabled) lv_label_set_text(_lblShuffle, "USB MODE");
        else if (_seekMode)        lv_label_set_text(_lblShuffle, "SEEK");
        else if (st == PS_PLAYING) lv_label_set_text(_lblShuffle, "PLAYING");
        else if (st == PS_PAUSED)  lv_label_set_text(_lblShuffle, "PAUSED");
        else                       lv_label_set_text(_lblShuffle, "STOPPED");
        lv_obj_set_style_text_color(_lblShuffle, _seekMode ? TH->warn : (st == PS_PLAYING) ? TH->accent : TH->text, 0);
        lv_obj_set_style_border_color(_lblShuffle, _seekMode ? TH->warn : (st == PS_PLAYING) ? TH->accent : TH->border, 0);

        // Seek mode hint in info indicator chip
        if (_lblInfoIndicator) {
            if (_seekMode) {
                lv_label_set_text(_lblInfoIndicator, "SEEK  ROT+ENTER");
                lv_obj_set_style_text_color(_lblInfoIndicator, TH->warn, 0);
                lv_obj_set_style_border_color(_lblInfoIndicator, TH->warn, 0);
            } else {
                lv_label_set_text(_lblInfoIndicator, "HELP [ i ]");
                lv_obj_set_style_text_color(_lblInfoIndicator, TH->text, 0);
                lv_obj_set_style_border_color(_lblInfoIndicator, TH->border, 0);
            }
        }

        if (_repeat == RM_NONE) {
            lv_obj_set_style_text_color(_lblRepeat, TH->muted, 0);
            lv_obj_set_style_border_color(_lblRepeat, TH->border, 0);
            lv_label_set_text(_lblRepeat, "RPT OFF");
        } else if (_repeat == RM_ONE) {
            lv_obj_set_style_text_color(_lblRepeat, TH->warn, 0);
            lv_obj_set_style_border_color(_lblRepeat, TH->warn, 0);
            lv_label_set_text(_lblRepeat, "RPT 1");
        } else {
            lv_obj_set_style_text_color(_lblRepeat, TH->warn, 0);
            lv_obj_set_style_border_color(_lblRepeat, TH->warn, 0);
            lv_label_set_text(_lblRepeat, "RPT ALL");
        }
        lv_obj_set_style_text_color(_lblPlCount, _shuffle ? TH->warn : TH->muted, 0);
        lv_obj_set_style_border_color(_lblPlCount, _shuffle ? TH->warn : TH->border, 0);
        lv_label_set_text(_lblPlCount, _shuffle ? "SHF ON" : "SHF OFF");

        uint32_t elapsedRaw = audioPlayerGetElapsed();
        uint32_t duration   = audioPlayerGetDuration();
        uint32_t durationGuess = duration;
        if (durationGuess == 0 && curTrack >= 0 && curTrack < total) {
            durationGuess = (uint32_t)(fileBrowserTrack(curTrack).size / 12000UL);
        }
        uint32_t nowMs = millis();
        if (st != _uiElapsedStateLast) {
            _uiElapsedStateLast = st;
            _uiElapsedRawLast   = elapsedRaw;
            _uiElapsedBaseSec   = elapsedRaw;
            _uiElapsedBaseMs    = nowMs;
        }
        if (st == PS_PLAYING) {
            if (elapsedRaw != _uiElapsedRawLast) {
                _uiElapsedRawLast = elapsedRaw;
                _uiElapsedBaseSec = elapsedRaw;
                _uiElapsedBaseMs  = nowMs;
            }
        } else {
            _uiElapsedRawLast = elapsedRaw;
            _uiElapsedBaseSec = elapsedRaw;
            _uiElapsedBaseMs  = nowMs;
        }
        uint32_t msSinceUpdate = nowMs - _uiElapsedBaseMs;
        if (msSinceUpdate > 1000) msSinceUpdate = 1000;
        uint32_t elapsedMs = _uiElapsedBaseSec * 1000UL + (st == PS_PLAYING ? msSinceUpdate : 0);

        if (_uiDurationShownSec == 0) _uiDurationShownSec = durationGuess;
        if (durationGuess > _uiDurationShownSec) _uiDurationShownSec = durationGuess;
        uint32_t elapsedSecLive = elapsedMs / 1000UL;
        if (st == PS_PLAYING && elapsedSecLive + 8 > _uiDurationShownSec) _uiDurationShownSec = elapsedSecLive + 8;
        uint32_t durMs = _uiDurationShownSec * 1000UL;
        if (st == PS_PLAYING && durMs > 0 && elapsedMs + 250 >= durMs) _uiDurationUnreliable = true;
        bool durationKnown = durMs > 0;
        if (!durationKnown) durMs = elapsedMs + 12000;
        if (elapsedMs > durMs) elapsedMs = durMs;

        uint32_t elapsed       = _seekMode ? _seekTargetSec : (elapsedMs / 1000);
        uint32_t shownDuration = durationKnown ? _uiDurationShownSec : 0;
        if (shownDuration > 0 && elapsed > shownDuration) elapsed = shownDuration;

        char eb[12], db[12];
        formatTime(elapsed, eb, sizeof(eb));
        formatTime(shownDuration == 0 ? UINT32_MAX : shownDuration, db, sizeof(db));

        char subBuf[48];
        if (_seekMode) {
            snprintf(subBuf, sizeof(subBuf), ">> SEEK  %s / %s", eb,
                     shownDuration > 0 ? db : "--:--");
        } else if (shownDuration > 0) {
            snprintf(subBuf, sizeof(subBuf), "track %02d of %02d  %s / %s", trackNum, total, eb, db);
        } else {
            snprintf(subBuf, sizeof(subBuf), "track %02d of %02d  %s / --:--", trackNum, total, eb);
        }
        lv_label_set_text(_lblState, subBuf);

        int fillW = 1;
        if (durMs > 0) {
            fillW = (int)((uint64_t)elapsedMs * (uint64_t)SW / durMs);
            if (fillW < 1 && elapsedMs > 0) fillW = 1;
            if (fillW > SW) fillW = SW;
        }
        lv_obj_set_width(_barProgress, fillW);
        lv_obj_set_style_bg_color(_barProgress, TH->accent, 0);

        // MP3 folder browser: show ".." + dirs + files
        bool hasBack = fileBrowserCanGoUp();
        int browserCount = fileBrowserCount();
        // Show current folder name as title (last path component)
        {
            const String& cp = fileBrowserCurrentPath();
            int lastSlash = cp.lastIndexOf('/');
            String folderName = (lastSlash >= 0 && lastSlash < (int)cp.length() - 1)
                                ? cp.substring(lastSlash + 1)
                                : cp;
            folderName.toUpperCase();
            lv_label_set_text(_lblListTitle, folderName.c_str());
        }
        for (int i = 0; i < PLAYLIST_ROWS; i++) {
            int idx = _listScroll + i;
            int browserTotal = browserCount + (hasBack ? 1 : 0);
            if (idx >= browserTotal) {
                lv_obj_set_style_bg_color(_plRow[i], TH->panel, 0);
                lv_obj_set_style_border_width(_plRow[i], 0, 0);
                lv_label_set_text(_plLabel[i], "");
                continue;
            }

            bool selected = (idx == _listCursor);
            bool isBackRow = hasBack && (idx == 0);
            String itemText;
            bool isDir = false;
            bool isPlaying = false;

            if (isBackRow) {
                itemText = "..";
            } else {
                int entryIdx = hasBack ? (idx - 1) : idx;
                const FileEntry& fe = fileBrowserGet(entryIdx);
                isDir = fe.isDir;
                if (isDir) {
                    itemText = "[" + trimName(fe.name, 15) + "/]";
                } else {
                    // Find track index in track list to detect playing
                    int trackIdx = fileBrowserFindTrack(fe.fullPath);
                    isPlaying = (trackIdx >= 0) && (trackIdx == curTrack) && (st != PS_STOPPED);
                    char nbuf[8]; snprintf(nbuf, sizeof(nbuf), "%02d", entryIdx + 1);
                    itemText = String(nbuf) + "  ";
                    if (isPlaying) itemText += "> ";
                    itemText += trimName(stripMp3(fe.name), isPlaying ? 17 : 19);
                }
            }

            if (isPlaying) {
                lv_obj_set_style_bg_color(_plRow[i], TH->playrow, 0);
                lv_obj_set_style_border_width(_plRow[i], 0, 0);
                lv_obj_set_style_text_color(_plLabel[i], TH->accent, 0);
            } else if (selected) {
                lv_obj_set_style_bg_color(_plRow[i], TH->hilight, 0);
                lv_obj_set_style_border_color(_plRow[i], TH->warn, 0);
                lv_obj_set_style_border_width(_plRow[i], 1, 0);
                lv_obj_set_style_text_color(_plLabel[i], isBackRow ? TH->muted : (isDir ? TH->warn : TH->text), 0);
            } else {
                lv_obj_set_style_bg_color(_plRow[i], TH->panel, 0);
                lv_obj_set_style_border_width(_plRow[i], 0, 0);
                lv_obj_set_style_text_color(_plLabel[i], isBackRow ? TH->muted : (isDir ? TH->warn : TH->dim), 0);
            }
            lv_label_set_text(_plLabel[i], itemText.c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// Refresh: settings screen
// ---------------------------------------------------------------------------
static void refreshSettings() {
    bool radio = isRadioMode();
    // 0=Back, 1=Mode, 2=WiFi network, 3=EQ, 4=Brightness, 5=Screen timeout,
    // 6=KB backlight, 7=Theme, 8=Auto power-off, 9=Debug, 10=USB, 11=Restart, 12=Power off
    lv_label_set_text(_settingsValue[0], "");
    lv_label_set_text(_settingsValue[1], radio ? "Radio" : "MP3");
    String wssid = wifiGetSSID();
    lv_label_set_text(_settingsValue[2], wssid.length() > 0 ? wssid.c_str() : "-- none --");
    lv_label_set_text(_settingsValue[3], audioPlayerGetEqPresetName(audioPlayerGetEqPreset()));
    String b = String((int)_displayBrightness) + "/" + String((int)DEVICE_MAX_BRIGHTNESS_LEVEL);
    lv_label_set_text(_settingsValue[4], b.c_str());
    lv_label_set_text(_settingsValue[5], timeoutLabel(_screenTimeoutSec).c_str());
    lv_label_set_text(_settingsValue[6], KB_MODE_LABELS[_kbMode]);
    lv_label_set_text(_settingsValue[7], themeName(themeGetActiveId()));
    lv_label_set_text(_settingsValue[8], powerOffLabel(_powerOffTimeoutSec).c_str());
    lv_label_set_text(_settingsValue[9], debugModeLabel(_debugMode));
#if MM_USB_RUNTIME_MSC
    lv_label_set_text(_settingsValue[10], _usbModeEnabled ? "SD shared" : "Off");
#else
    lv_label_set_text(_settingsValue[10], "Unavailable");
#endif
    lv_label_set_text(_settingsValue[11], "");
    lv_label_set_text(_settingsValue[12], "");

    if (_settingsCursor < _settingsScroll) _settingsScroll = _settingsCursor;
    if (_settingsCursor >= _settingsScroll + SETTINGS_VISIBLE) _settingsScroll = _settingsCursor - SETTINGS_VISIBLE + 1;
    if (_settingsScroll < 0) _settingsScroll = 0;

    int y0 = TOP_H + 2;
    for (int i = 0; i < SETTINGS_COUNT; i++) {
        int visIdx = i - _settingsScroll;
        if (visIdx < 0 || visIdx >= SETTINGS_VISIBLE) {
            lv_obj_add_flag(_settingsRow[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(_settingsRow[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(_settingsRow[i], 8, y0 + visIdx * (SETTINGS_ROW_H + SETTINGS_ROW_GAP));
        bool sel = (i == _settingsCursor);
        lv_obj_set_style_bg_color(_settingsRow[i], sel ? TH->hilight : TH->bg, 0);
        lv_obj_set_style_border_color(_settingsRow[i], sel ? TH->accent : TH->border, 0);
    }
}

// ---------------------------------------------------------------------------
// Refresh: WiFi screen
// ---------------------------------------------------------------------------
static void refreshWifi() {
    int n = wifiScanResultCount();
    if (n < 0) { lv_label_set_text(_wifiStatus, "Scanning..."); return; }
    _wifiScanPending = false;
    _wifiCount = n;

    WifiState ws = wifiGetState();
    if (ws == WS_CONNECTING)     lv_label_set_text(_wifiStatus, "Connecting...");
    else if (ws == WS_CONNECTED) lv_label_set_text(_wifiStatus, ("Connected: " + wifiGetSSID()).c_str());
    else if (ws == WS_FAILED)    lv_label_set_text(_wifiStatus, "Connection failed");
    else if (n == 0)             lv_label_set_text(_wifiStatus, "No networks found");
    else                         lv_label_set_text(_wifiStatus, "Select a network:");

    if (_wifiCursor < _wifiScroll_) _wifiScroll_ = _wifiCursor;
    if (_wifiCursor >= _wifiScroll_ + WIFI_ROWS) _wifiScroll_ = _wifiCursor - WIFI_ROWS + 1;
    if (_wifiScroll_ < 0) _wifiScroll_ = 0;

    for (int i = 0; i < WIFI_ROWS; i++) {
        int idx = _wifiScroll_ + i;
        if (idx >= n) {
            lv_obj_set_style_bg_color(_wifiRow[i], TH->bg, 0);
            lv_obj_set_style_border_width(_wifiRow[i], 0, 0);
            lv_label_set_text(_wifiLabel[i], "");
            continue;
        }
        bool sel = (idx == _wifiCursor);
        lv_obj_set_style_bg_color(_wifiRow[i], sel ? TH->hilight : TH->bg, 0);
        lv_obj_set_style_border_color(_wifiRow[i], sel ? TH->accent : TH->border, 0);
        lv_obj_set_style_border_width(_wifiRow[i], 1, 0);
        lv_obj_set_style_text_color(_wifiLabel[i], sel ? TH->text : TH->dim, 0);

        String ssid = wifiScanSSID(idx);
        int8_t rssi = wifiScanRSSI(idx);
        char buf[48];
        snprintf(buf, sizeof(buf), "%s  [%ddBm]", ssid.c_str(), (int)rssi);
        lv_label_set_text(_wifiLabel[i], buf);
    }
}

// ---------------------------------------------------------------------------
// Password overlay
// ---------------------------------------------------------------------------
static void showPwOverlay(const String& ssid) {
    _pwTargetSSID = ssid;
    _pwLen = 0;
    memset(_pwBuf, 0, sizeof(_pwBuf));
    _pwEntryActive = true;

    _pwOverlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_pwOverlay, SW - 60, 80);
    lv_obj_align(_pwOverlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_pwOverlay, TH->panel, 0);
    lv_obj_set_style_bg_opa(_pwOverlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_pwOverlay, TH->accent, 0);
    lv_obj_set_style_border_width(_pwOverlay, 2, 0);
    lv_obj_set_style_radius(_pwOverlay, 4, 0);
    lv_obj_set_style_pad_all(_pwOverlay, 8, 0);

    _pwPrompt = lv_label_create(_pwOverlay);
    styleLabel(_pwPrompt, TH->text, TH->fontBody);
    lv_obj_align(_pwPrompt, LV_ALIGN_TOP_LEFT, 0, 0);
    String prompt = "Password for: " + ssid;
    lv_label_set_text(_pwPrompt, prompt.c_str());

    _pwLabel = lv_label_create(_pwOverlay);
    styleLabel(_pwLabel, TH->warn, TH->fontBody);
    lv_obj_align(_pwLabel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_label_set_text(_pwLabel, "_");
}

static void hidePwOverlay() {
    _pwEntryActive = false;
    if (_pwOverlay) { lv_obj_delete(_pwOverlay); _pwOverlay = nullptr; }
    _pwPrompt = nullptr; _pwLabel = nullptr;
}

static void pwHandleKey(char k, char raw) {
    if (!_pwEntryActive) return;

    if (raw == 8 || raw == 127) {  // backspace
        if (_pwLen > 0) _pwLen--;
    } else if (raw == '\n' || raw == '\r') {  // confirm
        _pwBuf[_pwLen] = '\0';
        String pass = String(_pwBuf);
        hidePwOverlay();
        wifiNetworkAdd(_pwTargetSSID, pass);
        settingsSave();
        wifiConnect(_pwTargetSSID);
        lv_label_set_text(_wifiStatus, "Connecting...");
        return;
    } else if (k == 'b' && raw == 'B') {
        hidePwOverlay();
        return;
    } else if (raw >= 0x20 && raw < 0x7F && _pwLen < (int)sizeof(_pwBuf) - 1) {
        _pwBuf[_pwLen++] = raw;
    }

    _pwBuf[_pwLen] = '\0';

    // Display password as plain text while typing (no masking).
    char disp[66];
    memcpy(disp, _pwBuf, _pwLen);
    disp[_pwLen] = '_';
    disp[_pwLen + 1] = '\0';
    _pwLastCharMs = millis();
    if (_pwLabel) lv_label_set_text(_pwLabel, disp);
}

// ---------------------------------------------------------------------------
// Marquee (scrolling title)
// ---------------------------------------------------------------------------
static void tickMarquee() {
    if (!_lblNow || !_nowClip || _screen != UI_PLAYER) return;

    // Detect text change — reset scroll
    const char* txt = lv_label_get_text(_lblNow);
    String cur = txt ? String(txt) : "";
    if (cur != _mqLastText) {
        _mqLastText  = cur;
        _mqOffsetX   = 0;
        _mqPhase     = MQ_PAUSE_START;
        _mqPhaseMs   = millis();
        _mqTextW     = lv_obj_get_width(_lblNow);
        lv_obj_set_x(_lblNow, 0);
        return;
    }

    // No scroll needed if text fits
    _mqTextW = lv_obj_get_width(_lblNow);
    int32_t overflow = _mqTextW - MQ_CLIP_W;
    if (overflow <= 0) {
        if (_mqOffsetX != 0) { _mqOffsetX = 0; lv_obj_set_x(_lblNow, 0); }
        return;
    }

    uint32_t now = millis();
    switch (_mqPhase) {
    case MQ_PAUSE_START:
        if (now - _mqPhaseMs >= MQ_PAUSE_START_MS) {
            _mqPhase   = MQ_SCROLLING;
            _mqPhaseMs = now;
        }
        break;
    case MQ_SCROLLING: {
        uint32_t elapsed = now - _mqPhaseMs;
        _mqOffsetX = (int32_t)((uint64_t)elapsed * MQ_SPEED_PX_PER_SEC / 1000);
        if (_mqOffsetX >= overflow) {
            _mqOffsetX = overflow;
            lv_obj_set_x(_lblNow, -_mqOffsetX);
            _mqPhase   = MQ_PAUSE_END;
            _mqPhaseMs = now;
        } else {
            lv_obj_set_x(_lblNow, -_mqOffsetX);
        }
        break;
    }
    case MQ_PAUSE_END:
        if (now - _mqPhaseMs >= MQ_PAUSE_END_MS) {
            _mqOffsetX = 0;
            lv_obj_set_x(_lblNow, 0);
            _mqPhase   = MQ_PAUSE_START;
            _mqPhaseMs = now;
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Public init / loop
// ---------------------------------------------------------------------------
void uiManagerInit() {
    loadUiSettings();

    buildSplash();
    buildPlayer();
    buildSettings();
    buildInfo();
    buildWifi();

    _screenDimmed = false;
    _lastInputMs  = millis();
    _lastActivityForPowerOff = millis();
    applyBrightness();
    applyKbMode();
    saveUiSettings();


    lv_scr_load(_scrSplash);
    _screen   = UI_SPLASH;
    _splashAt = millis();
    Serial.println("[UI] Init OK");
}

void uiManagerLoop() {
    // Theme change: rebuild all screens
    if (themeManagerNeedsRedraw()) {
        UiScreen cur = _screen;
        lv_obj_t* blank = lv_obj_create(nullptr);
        lv_scr_load(blank);
        teardownScreens();
        rebuildScreens();
        themeManagerClearRedraw();
        showScreen(cur);
    }

    if (!_screenDimmed && _screenTimeoutSec > 0 && (millis() - _lastInputMs > _screenTimeoutSec * 1000UL)) {
        _screenDimmed = true;
        applyBrightness();
    }

    // Auto power-off: only when stopped/idle and no input for the configured duration
    if (_powerOffTimeoutSec > 0) {
        PlayerState pst = audioPlayerGetState();
        RadioStatus rst = radioPlayerGetStatus();
        bool isIdle = (pst == PS_STOPPED) && (rst == RS_IDLE || rst == RS_ERROR);
        if (!isIdle) _lastActivityForPowerOff = millis();
        if (isIdle && (millis() - _lastActivityForPowerOff > _powerOffTimeoutSec * 1000UL)) {
            Serial.println("[UI] Auto power-off triggered");
            instance.sleep(WAKEUP_SRC_BOOT_BUTTON);
        }
    }
    if (!_kbDimmed && _kbBacklight && _kbTimeoutSec > 0 && (millis() - _lastInputMs > _kbTimeoutSec * 1000UL)) {
        _kbDimmed = true;
        instance.kb.setBrightness(0);
    }

    if (_screen == UI_SPLASH && (millis() - _splashAt > 1800)) {
        showScreen(UI_PLAYER);
        return;
    }

    bool radio = isRadioMode();

    // Mode policy:
    // Radio -> WiFi ON (auto reconnect + start stream when link appears)
    // MP3   -> WiFi OFF
    if (!radio) {
        if (wifiGetState() != WS_DISABLED) wifiDisconnect();
    } else {
        // Keep scan screen isolated from auto-reconnect to avoid scan races.
        if (_screen != UI_WIFI && !_pwEntryActive) {
            if (wifiGetState() == WS_DISABLED || wifiGetState() == WS_FAILED) wifiAutoReconnect();
        }
    }

    // MP3 auto-advance
    if (!radio && _screen == UI_PLAYER && !_usbModeEnabled && audioPlaybackAllowed() && audioPlayerTrackDone()) {
        audioPlayerClearDone();
        playNext();
        return;
    }

    // Radio deferred play: send a single PLAY command (queue overwrite drops stale requests).
    if (radio && _pendingRadioPlayIdx >= 0 && millis() >= _pendingRadioPlayAtMs) {
        int n = fileBrowserRadioCount();
        if (n > 0) {
            int idx = _pendingRadioPlayIdx;
            if (idx < 0) idx = 0;
            if (idx >= n) idx = n - 1;
            _plIdx = idx;
            radioPlayerPlayURL(fileBrowserRadioGet(idx).url.c_str());
        }
        _pendingRadioPlayIdx = -1;
    }

    // WiFi scan poll
    if (_screen == UI_WIFI && _wifiScanPending) {
        if (wifiScanResultCount() >= 0) {
            _wifiScanPending = false;
            refreshWifi();
        }
    }

    static uint32_t lastMarquee = 0;
    if (_screen == UI_PLAYER && !_screenDimmed && millis() - lastMarquee >= 50) {
        lastMarquee = millis();
        tickMarquee();
    }

    static uint32_t lastRefresh = 0;
    if (!_screenDimmed && uiRenderEnabled()) {
        uint32_t playerPeriod = 220;
        if (audioPlayerGetState() == PS_PLAYING || radioPlayerGetStatus() == RS_PLAYING) playerPeriod = 70;
        else if (audioPlayerGetState() == PS_PAUSED) playerPeriod = 140;
        if ((_screen == UI_PLAYER   && millis() - lastRefresh > playerPeriod) ||
            (_screen == UI_SETTINGS && millis() - lastRefresh > 500) ||
            (_screen == UI_WIFI     && millis() - lastRefresh > 500)) {
            lastRefresh = millis();
            if (_screen == UI_PLAYER)   refreshPlayer();
            else if (_screen == UI_SETTINGS) refreshSettings();
            else if (_screen == UI_WIFI) refreshWifi();
        }
    }

    RotaryMsg_t rot = instance.getRotary();
    char key = 0;
    int keyState = instance.getKeyChar(&key);

    bool hasInput = (rot.dir != ROTARY_DIR_NONE) || rot.centerBtnPressed || (keyState == KB_PRESSED);
    if (hasInput) {
        bool wasDimmed = _screenDimmed;
        noteActivity();
        if (wasDimmed) { instance.clearRotaryMsg(); return; }
    }

    if (rot.dir == ROTARY_DIR_NONE && !rot.centerBtnPressed && keyState != KB_PRESSED) return;

    // Password overlay intercepts all input
    if (_pwEntryActive) {
        if (keyState == KB_PRESSED && key != 0) {
            pwHandleKey(key, key);
        }
        if (rot.dir != ROTARY_DIR_NONE || rot.centerBtnPressed) instance.clearRotaryMsg();
        return;
    }

    static bool rotBtnWasPressed = false;
    static uint32_t lastRotClickMs = 0;
    bool rotBtnNow = rot.centerBtnPressed;
    if (rotBtnNow && !rotBtnWasPressed && (millis() - lastRotClickMs > 80)) {
        lastRotClickMs = millis();
        if (_screen == UI_PLAYER) {
            key = '\n'; keyState = KB_PRESSED;  // delegate to key handler below
        } else if (_screen == UI_SETTINGS) {
            key = '\n'; keyState = KB_PRESSED;
        } else if (_screen == UI_WIFI) {
            key = '\n'; keyState = KB_PRESSED;
        }
    }
    rotBtnWasPressed = rotBtnNow;

    if (rot.dir != ROTARY_DIR_NONE) {
        int dir = (rot.dir == ROTARY_DIR_UP) ? 1 : -1;
        if (_screen == UI_PLAYER) {
            if (_seekMode && !radio) {
                // Seek mode: encoder adjusts target, seek applied on exit
                int32_t target = (int32_t)_seekTargetSec + dir * SEEK_STEP_SEC;
                if (target < 0) target = 0;
                _seekTargetSec = (uint32_t)target;
                refreshPlayer();
            } else if (instance.kb.symbol_key_pressed) {
                uint8_t v = audioPlayerGetVolume();
                audioPlayerSetVolume((uint8_t)constrain((int)v + dir * 5, 0, 100));
                refreshPlayer();
            } else {
                _listCursor += dir;
                int listTotal;
                if (radio) {
                    bool showPl = (fileBrowserM3uSelected() < 0) && (fileBrowserM3uCount() > 0);
                    listTotal = showPl ? fileBrowserM3uCount() : fileBrowserRadioCount() + (fileBrowserM3uCount() > 0 ? 1 : 0);
                } else {
                    listTotal = fileBrowserCount() + (fileBrowserCanGoUp() ? 1 : 0);
                }
                _listCursor = constrain(_listCursor, 0, listTotal > 0 ? listTotal - 1 : 0);
                if (_listCursor < _listScroll) _listScroll = _listCursor;
                if (_listCursor >= _listScroll + PLAYLIST_ROWS) _listScroll = _listCursor - PLAYLIST_ROWS + 1;
                if (_listScroll < 0) _listScroll = 0;
                refreshPlayer();
            }
        } else if (_screen == UI_SETTINGS) {
            _settingsCursor = constrain(_settingsCursor + dir, 0, SETTINGS_COUNT - 1);
            refreshSettings();
        } else if (_screen == UI_WIFI) {
            _wifiCursor = constrain(_wifiCursor + dir, 0, _wifiCount > 0 ? _wifiCount - 1 : 0);
            refreshWifi();
        }
    }

    if (rot.dir != ROTARY_DIR_NONE || rot.centerBtnPressed) instance.clearRotaryMsg();
    if (keyState != KB_PRESSED) return;

    if (key == 0) return;

    char k = (char)tolower((unsigned char)key);

    if (_screen == UI_PLAYER) {
        if (k == 'h' && _pendingSComboAtMs != 0 &&
            (millis() - _pendingSComboAtMs <= SHOT_COMBO_WINDOW_MS)) {
            _pendingSComboAtMs = 0;
            char shotPath[64] = {};
            if (saveScreenshotToSd(shotPath, sizeof(shotPath))) {
                Serial.printf("[UI] Screenshot saved: %s\n", shotPath);
            } else {
                Serial.println("[UI] Screenshot failed");
            }
            refreshPlayer();
            return;
        } else if (_pendingSComboAtMs != 0 && k != 's') {
            _pendingSComboAtMs = 0;
        }

        if (k == ' ' || k == 'p') {
            if (_usbModeEnabled || !audioPlaybackAllowed()) return;
            if (radio) {
                // Space in radio mode: stop/start
                if (radioPlayerGetStatus() == RS_IDLE || radioPlayerGetStatus() == RS_ERROR) {
                    requestRadioPlay(_plIdx, "space");
                } else {
                    _pendingRadioPlayIdx = -1;
                    radioPlayerStop();
                }
            } else {
                if (audioPlayerGetState() == PS_STOPPED) {
                    // Find the track under cursor in the folder browser
                    bool hasBack = fileBrowserCanGoUp();
                    int entryIdx = hasBack ? (_listCursor - 1) : _listCursor;
                    if (entryIdx >= 0 && entryIdx < fileBrowserCount()) {
                        const FileEntry& fe = fileBrowserGet(entryIdx);
                        if (!fe.isDir) {
                            int trackIdx = fileBrowserFindTrack(fe.fullPath);
                            if (trackIdx >= 0) startPlaying(trackIdx);
                        }
                    }
                } else {
                    audioPlayerTogglePause();
                }
            }
            _lastPauseToggleMs = millis();
            refreshPlayer();
        } else if (key == '\n' || key == '\r') {
            if (_seekMode) {
                _seekMode = false;
                audioPlayerSeek(_seekTargetSec);
                refreshPlayer();
                return;
            }
            if (_usbModeEnabled) return;
            if (radio) {
                // Radio two-level browser
                bool showPl = (fileBrowserM3uSelected() < 0) && (fileBrowserM3uCount() > 0);
                if (showPl) {
                    // Select a playlist file
                    if (_listCursor >= 0 && _listCursor < fileBrowserM3uCount()) {
                        fileBrowserM3uLoad(_listCursor);
                        _listCursor = 0; _listScroll = 0; _plIdx = 0;
                        refreshPlayer();
                    }
                } else {
                    // Stations list: row 0 is always ".."
                    if (_listCursor == 0) {
                        fileBrowserM3uGoBack();
                        _listCursor = 0; _listScroll = 0;
                        refreshPlayer();
                    } else {
                        int stIdx = _listCursor - 1;
                        if (audioPlaybackAllowed() && stIdx >= 0 && stIdx < fileBrowserRadioCount()) {
                            requestRadioPlay(stIdx, "enter");
                        }
                    }
                }
            } else {
                // MP3 folder browser
                bool hasBack = fileBrowserCanGoUp();
                if (hasBack && _listCursor == 0) {
                    // ".." go up
                    fileBrowserGoUp();
                    _listCursor = 0; _listScroll = 0;
                    refreshPlayer();
                } else {
                    int entryIdx = hasBack ? (_listCursor - 1) : _listCursor;
                    if (entryIdx >= 0 && entryIdx < fileBrowserCount()) {
                        const FileEntry& fe = fileBrowserGet(entryIdx);
                        if (fe.isDir) {
                            fileBrowserEnter(entryIdx);
                            _listCursor = 0; _listScroll = 0;
                            refreshPlayer();
                        } else if (audioPlaybackAllowed()) {
                            int trackIdx = fileBrowserFindTrack(fe.fullPath);
                            if (trackIdx >= 0) startPlaying(trackIdx);
                        }
                    }
                }
            }
        } else if (k == 'q') {
            uint8_t v = audioPlayerGetVolume();
            audioPlayerSetVolume(v < 95 ? v + 5 : 100);
            refreshPlayer();
        } else if (k == 'a') {
            uint8_t v = audioPlayerGetVolume();
            audioPlayerSetVolume(v > 5 ? v - 5 : 0);
            refreshPlayer();
        } else if (k == 'w') {
            if (_usbModeEnabled || !audioPlaybackAllowed()) return;
            if (radio && _plIdx > 0) {
                requestRadioPlay(_plIdx - 1, "prev");
            } else if (!radio) playPrev();
        } else if (k == 'd') {
            if (_usbModeEnabled || !audioPlaybackAllowed()) return;
            if (radio) {
                int n = fileBrowserRadioCount();
                if (n > 0) requestRadioPlay((_plIdx + 1) % n, "next");
            } else playNext();
        } else if (k == 'r') {
            if (!radio) { _repeat = (RepeatMode)(((int)_repeat + 1) % 3); saveUiSettings(); refreshPlayer(); }
        } else if (k == 'h') {
            if (!radio) {
                _shuffle = !_shuffle;
                int ct = currentTrackIndex();
                if (ct >= 0) buildPlaylist(ct);
                saveUiSettings();
                refreshPlayer();
            }
        } else if (k == 's') {
            _pendingSComboAtMs = millis();
        } else if (k == 'b' || key == 8) {
            if (_seekMode) {
                _seekMode = false;
                audioPlayerSeek(_seekTargetSec);
                refreshPlayer();
                return;
            }
            if (radio) {
                bool showPl = (fileBrowserM3uSelected() < 0) && (fileBrowserM3uCount() > 0);
                if (!showPl) {
                    // In stations view: go back to playlist list
                    radioPlayerStop();
                    fileBrowserM3uGoBack();
                    _listCursor = 0; _listScroll = 0;
                } else {
                    radioPlayerStop();
                }
            } else {
                if (fileBrowserCanGoUp()) {
                    fileBrowserGoUp();
                    _listCursor = 0; _listScroll = 0;
                } else {
                    audioPlayerStop();
                }
            }
            refreshPlayer();
        } else if (k == 'n') {
            // Toggle seek mode (MP3 only, while playing or paused)
            PlayerState pst = audioPlayerGetState();
            if (!radio && !_usbModeEnabled && (pst == PS_PLAYING || pst == PS_PAUSED)) {
                _seekMode = !_seekMode;
                if (_seekMode) {
                    _seekTargetSec = audioPlayerGetElapsed();
                } else {
                    // Exiting via N — apply seek
                    audioPlayerSeek(_seekTargetSec);
                }
                refreshPlayer();
            }
        } else if (k == 'i') {
            _returnScreen = UI_PLAYER;
            showScreen(UI_INFO);
        }
    } else if (_screen == UI_INFO) {
        if (k == 'b' || key == 8) showScreen(_returnScreen);
    } else if (_screen == UI_WIFI) {
        if (k == 'b' || key == 8) {
            showScreen(_returnScreen);
            return;
        }
        if (k == 'r') {
            if (wifiStartScan()) {
                _wifiScanPending = true;
                lv_label_set_text(_wifiStatus, "Scanning...");
            } else {
                _wifiScanPending = false;
                lv_label_set_text(_wifiStatus, "Scan failed, press R");
            }
            return;
        }
        if (key == '\n' || key == '\r') {
            if (_wifiCount > 0 && _wifiCursor < _wifiCount) {
                String ssid = wifiScanSSID(_wifiCursor);
                // Check if already known
                bool known = false;
                for (int i = 0; i < wifiNetworkCount(); i++) {
                    if (wifiNetworkGet(i).ssid == ssid) { known = true; break; }
                }
                if (known) {
                    wifiConnect(ssid);
                    lv_label_set_text(_wifiStatus, "Connecting...");
                } else {
                    showPwOverlay(ssid);
                }
            }
        }
    } else if (_screen == UI_SETTINGS) {
        bool adjustMinus = (k == 'a' || k == 'q');
        bool adjustPlus  = (k == 'd' || k == 'w');

        // 0=Back, 1=Mode, 2=WiFi network, 3=EQ, 4=Brightness, 5=Screen timeout,
        // 6=KB backlight, 7=Theme, 8=Auto power-off, 9=Debug, 10=USB, 11=Restart, 12=Power off
        if (adjustMinus || adjustPlus) {
            int d = adjustPlus ? 1 : -1;
            if (_settingsCursor == 3) {
                int eq = constrain((int)audioPlayerGetEqPreset() + d, 0, (int)EQ_PRESET_COUNT - 1);
                applyEqPresetFromUi((EqPreset)eq);
                refreshSettings(); return;
            }
            if (_settingsCursor == 4) {
                _displayBrightness = (uint8_t)constrain((int)_displayBrightness + d, 1, (int)DEVICE_MAX_BRIGHTNESS_LEVEL);
                _screenDimmed = false; applyBrightness(); saveUiSettings(); refreshSettings(); return;
            }
            if (_settingsCursor == 5) {
                int idx = timeoutOptionIndex(_screenTimeoutSec);
                idx = constrain(idx + d, 0, TIMEOUT_OPTS_N - 1);
                _screenTimeoutSec = TIMEOUT_OPTS[idx];
                saveUiSettings(); refreshSettings(); return;
            }
            if (_settingsCursor == 6) {
                _kbMode = constrain(_kbMode + d, 0, KB_MODE_OPTS_N - 1);
                applyKbMode(); saveUiSettings(); refreshSettings(); return;
            }
            if (_settingsCursor == 7) {
                int id = (int)themeGetActiveId() + d;
                if (id < 0) id = themeCount() - 1;
                if (id >= themeCount()) id = 0;
                themeSetActive((ThemeId)id);
                refreshSettings(); return;
            }
            if (_settingsCursor == 8) {
                int idx = powerOffOptionIndex(_powerOffTimeoutSec);
                idx = constrain(idx + d, 0, POWEROFF_OPTS_N - 1);
                _powerOffTimeoutSec = POWEROFF_OPTS[idx];
                _lastActivityForPowerOff = millis();
                saveUiSettings(); refreshSettings(); return;
            }
            if (_settingsCursor == 9) {
                int mode = constrain((int)_debugMode + d, (int)UI_DBG_NORMAL, (int)UI_DBG_DISPLAY_SD_NO_DECODE);
                _debugMode = (UiDebugMode)mode;
                if (!audioPlaybackAllowed() && audioPlayerGetState() != PS_STOPPED) audioPlayerStop();
                saveUiSettings(); refreshSettings(); return;
            }
        }

        if (k == 'b' || key == 8) { showScreen(_returnScreen); return; }

        if (k == ' ' || key == '\n' || key == '\r') {
            if (_settingsCursor == 0) {
                showScreen(_returnScreen);
            } else if (_settingsCursor == 1) {
                switchMode(!isRadioMode());
                _returnScreen = UI_PLAYER;
                showScreen(UI_PLAYER);
                return;
            } else if (_settingsCursor == 2) {
                _returnScreen = UI_SETTINGS;
                showScreen(UI_WIFI);
            } else if (_settingsCursor == 3) {
                int eq = ((int)audioPlayerGetEqPreset() + 1) % (int)EQ_PRESET_COUNT;
                applyEqPresetFromUi((EqPreset)eq);
                refreshSettings();
            } else if (_settingsCursor == 4) {
                _displayBrightness = (uint8_t)((_displayBrightness % DEVICE_MAX_BRIGHTNESS_LEVEL) + 1);
                _screenDimmed = false; applyBrightness(); saveUiSettings(); refreshSettings();
            } else if (_settingsCursor == 5) {
                int idx = (timeoutOptionIndex(_screenTimeoutSec) + 1) % TIMEOUT_OPTS_N;
                _screenTimeoutSec = TIMEOUT_OPTS[idx];
                saveUiSettings(); refreshSettings();
            } else if (_settingsCursor == 6) {
                _kbMode = (_kbMode + 1) % KB_MODE_OPTS_N;
                applyKbMode(); saveUiSettings(); refreshSettings();
            } else if (_settingsCursor == 7) {
                int id = ((int)themeGetActiveId() + 1) % themeCount();
                themeSetActive((ThemeId)id);
                refreshSettings();
            } else if (_settingsCursor == 8) {
                int idx = (powerOffOptionIndex(_powerOffTimeoutSec) + 1) % POWEROFF_OPTS_N;
                _powerOffTimeoutSec = POWEROFF_OPTS[idx];
                _lastActivityForPowerOff = millis();
                saveUiSettings(); refreshSettings();
            } else if (_settingsCursor == 9) {
                _debugMode = (UiDebugMode)(((int)_debugMode + 1) % ((int)UI_DBG_DISPLAY_SD_NO_DECODE + 1));
                if (!audioPlaybackAllowed() && audioPlayerGetState() != PS_STOPPED) audioPlayerStop();
                saveUiSettings(); refreshSettings();
            } else if (_settingsCursor == 10) {
#if MM_USB_RUNTIME_MSC
                if (!_usbModeEnabled) {
                    audioPlayerStop(); delay(30);
                    _usbModeEnabled = enableUsbMsc();
                } else {
                    disableUsbMsc(); _usbModeEnabled = false;
                }
#endif
                refreshSettings();
            } else if (_settingsCursor == 11) {
                esp_restart();
            } else if (_settingsCursor == 12) {
                instance.sleep(WAKEUP_SRC_BOOT_BUTTON);
            }
        }
    }
}

UiScreen    uiCurrentScreen()  { return _screen; }
bool        uiUsbModeEnabled() { return _usbModeEnabled; }
UiDebugMode uiDebugMode()      { return _debugMode; }
bool        uiRenderEnabled()  {
    return _debugMode != UI_DBG_AUDIO_ONLY && _debugMode != UI_DBG_AUDIO_SD_NO_UI;
}
