#include "settings_store.h"
#include "spi_guard.h"
#include <SD.h>
#include <Preferences.h>
#include <ArduinoJson.h>

#define SETTINGS_PATH     "/config/settings.json"
#define SETTINGS_TMP_PATH "/config/settings.tmp"

static DynamicJsonDocument _doc(8192);
static bool _sdAvailable = false;
static bool _loaded = false;
static bool _dirty = false;

// ---------------------------------------------------------------------------
// Dot-path helpers: "wifi.networks" → nested JsonObject access
// ---------------------------------------------------------------------------

static JsonVariant getNode(const char* key) {
    JsonVariant cur = _doc.as<JsonVariant>();
    char buf[64];
    strncpy(buf, key, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char* tok = strtok(buf, ".");
    while (tok) {
        char* next = strtok(nullptr, ".");
        if (next) {
            if (!cur[tok].is<JsonObject>()) cur[tok].to<JsonObject>();
            cur = cur[tok];
        } else {
            cur = cur[tok];
        }
        tok = next;
    }
    return cur;
}

static JsonVariant ensureNode(const char* key) {
    JsonVariant cur = _doc.as<JsonVariant>();
    char buf[64];
    strncpy(buf, key, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char* tok = strtok(buf, ".");
    while (tok) {
        char* next = strtok(nullptr, ".");
        if (next) {
            if (!cur.as<JsonObject>()[tok].is<JsonObject>()) {
                cur.as<JsonObject>()[tok].to<JsonObject>();
            }
            cur = cur.as<JsonObject>()[tok];
        } else {
            cur = cur.as<JsonObject>()[tok];
        }
        tok = next;
    }
    return cur;
}

// ---------------------------------------------------------------------------
// Public getters
// ---------------------------------------------------------------------------

String settingsGetString(const char* key, const char* def) {
    if (!_loaded) return String(def);
    JsonVariant v = getNode(key);
    if (v.isNull() || !v.is<const char*>()) return String(def);
    return String(v.as<const char*>());
}

int settingsGetInt(const char* key, int def) {
    if (!_loaded) return def;
    JsonVariant v = getNode(key);
    if (v.isNull()) return def;
    return v.as<int>();
}

bool settingsGetBool(const char* key, bool def) {
    if (!_loaded) return def;
    JsonVariant v = getNode(key);
    if (v.isNull()) return def;
    return v.as<bool>();
}

// ---------------------------------------------------------------------------
// Public setters
// ---------------------------------------------------------------------------

void settingsPutString(const char* key, const char* value) {
    ensureNode(key).set(value);
}

void settingsPutInt(const char* key, int value) {
    ensureNode(key).set(value);
}

void settingsPutBool(const char* key, bool value) {
    ensureNode(key).set(value);
}

// ---------------------------------------------------------------------------
// Save: atomic write via temp file
// ---------------------------------------------------------------------------

bool settingsSave() {
    if (!_sdAvailable) return false;
    SpiGuardScope lock(pdMS_TO_TICKS(500));
    if (!lock.locked()) {
        _dirty = true; // retry later via settingsFlushIfDirty
        return false;
    }

    SD.mkdir("/config");
    File f = SD.open(SETTINGS_TMP_PATH, FILE_WRITE);
    if (!f) { _dirty = true; return false; }
    serializeJson(_doc, f);
    f.close();

    SD.remove(SETTINGS_PATH);
    SD.rename(SETTINGS_TMP_PATH, SETTINGS_PATH);
    _dirty = false;
    return true;
}

void settingsMarkDirty() {
    _dirty = true;
}

bool settingsFlushIfDirty() {
    if (!_dirty) return true;
    return settingsSave();
}

// ---------------------------------------------------------------------------
// WiFi network list helpers
// ---------------------------------------------------------------------------

static JsonArray networksArray() {
    if (!_doc["wifi"].is<JsonObject>()) _doc["wifi"].to<JsonObject>();
    if (!_doc["wifi"]["networks"].is<JsonArray>()) _doc["wifi"]["networks"].to<JsonArray>();
    return _doc["wifi"]["networks"].as<JsonArray>();
}

int wifiNetworkCount() {
    return (int)networksArray().size();
}

WifiNetwork wifiNetworkGet(int index) {
    JsonArray arr = networksArray();
    if (index < 0 || index >= (int)arr.size()) return {"", ""};
    JsonObject o = arr[index].as<JsonObject>();
    return {o["ssid"].as<String>(), o["pass"].as<String>()};
}

void wifiNetworkAdd(const String& ssid, const String& pass) {
    JsonArray arr = networksArray();
    for (JsonObject o : arr) {
        if (o["ssid"].as<String>() == ssid) {
            o["pass"] = pass;
            return;
        }
    }
    JsonObject entry = arr.createNestedObject();
    entry["ssid"] = ssid;
    entry["pass"] = pass;
}

void wifiNetworkRemove(int index) {
    JsonArray arr = networksArray();
    if (index < 0 || index >= (int)arr.size()) return;
    arr.remove(index);
}

// ---------------------------------------------------------------------------
// Init: load from SD, migrate NVS on first boot
// ---------------------------------------------------------------------------

static void migrateFromNvs() {
    Preferences uiPrefs, playerPrefs;

    uiPrefs.begin("mm_ui", true);
    _doc["brightness"]  = (int)uiPrefs.getUChar("brightness", 16);
    _doc["timeout"]     = (int)uiPrefs.getUInt("timeout", 60);
    _doc["kb_mode"]     = (int)uiPrefs.getChar("kb_mode", 4);
    _doc["dbg_mode"]    = (int)uiPrefs.getChar("dbg_mode", 0);
    _doc["repeat"]      = (int)uiPrefs.getChar("repeat", 0);
    _doc["shuffle"]     = uiPrefs.getBool("shuffle", false);
    _doc["last_track"]  = uiPrefs.getString("last_track", "").c_str();
    uiPrefs.end();

    playerPrefs.begin("mm_player", true);
    _doc["volume"] = (int)playerPrefs.getUInt("volume", 70);
    playerPrefs.end();

    if (!_doc["theme"].is<JsonObject>()) _doc["theme"].to<JsonObject>();
    _doc["theme"]["active"] = 0;

    if (!_doc["app"].is<JsonObject>()) _doc["app"].to<JsonObject>();
    _doc["app"]["mode"] = "mp3";

    if (!_doc["wifi"].is<JsonObject>()) _doc["wifi"].to<JsonObject>();
    _doc["wifi"]["enabled"]  = false;
    _doc["wifi"]["last_ssid"] = "";
    _doc["wifi"]["networks"].to<JsonArray>();

    _doc["migrated"] = true;
    Serial.println("[CFG] Migrated settings from NVS to SD JSON");
}

void settingsStoreInit() {
    _sdAvailable = (SD.cardType() != CARD_NONE);

    if (!_sdAvailable) {
        Serial.println("[CFG] SD not available — using defaults");
        migrateFromNvs();
        _loaded = true;
        return;
    }

    SD.mkdir("/config");

    bool parseOk = false;
    {
        SpiGuardScope lock(pdMS_TO_TICKS(1000));
        if (lock.locked()) {
            File f = SD.open(SETTINGS_PATH, FILE_READ);
            if (f) {
                DeserializationError err = deserializeJson(_doc, f);
                f.close();
                parseOk = (err == DeserializationError::Ok);
                if (!parseOk) {
                    Serial.printf("[CFG] JSON parse error: %s\n", err.c_str());
                }
            }
        }
    }

    if (!parseOk) {
        _doc.clear();
        migrateFromNvs();
        _loaded = true;
        settingsSave();
    } else {
        _loaded = true;
        Serial.println("[CFG] Settings loaded from SD");
    }
}
