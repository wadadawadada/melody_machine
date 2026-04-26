#include "favorites_store.h"
#include "spi_guard.h"
#include <SD.h>
#include <ArduinoJson.h>

#define FAVORITES_PATH "/melody_machine/favorites.json"
#define FAVORITES_TMP  "/melody_machine/favorites.tmp"
#define MAX_FAVORITES  100

static std::vector<FavoriteStation> _favs;
static bool _loaded = false;

void favoritesInit() {
    _favs.clear();
    _loaded = false;

    SpiGuardScope lock(pdMS_TO_TICKS(1000));
    if (!lock.locked()) return;

    File f = SD.open(FAVORITES_PATH, FILE_READ);
    if (!f) { _loaded = true; return; }

    DynamicJsonDocument doc(16384);
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err != DeserializationError::Ok) { _loaded = true; return; }

    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
        if (_favs.size() >= MAX_FAVORITES) break;
        const char* name = obj["name"] | "";
        const char* url  = obj["url"]  | "";
        if (strlen(url) > 0) {
            FavoriteStation fs;
            fs.name = name;
            fs.url  = url;
            _favs.push_back(fs);
        }
    }
    _loaded = true;
}

int favoritesCount() { return (int)_favs.size(); }

const FavoriteStation& favoritesGet(int index) {
    static FavoriteStation empty;
    if (index < 0 || index >= (int)_favs.size()) return empty;
    return _favs[index];
}

bool favoritesContains(const String& url) {
    for (const auto& f : _favs)
        if (f.url == url) return true;
    return false;
}

bool favoritesAdd(const String& name, const String& url) {
    if (favoritesContains(url)) return false;
    if ((int)_favs.size() >= MAX_FAVORITES) return false;
    FavoriteStation fs;
    fs.name = name;
    fs.url  = url;
    _favs.push_back(fs);
    favoritesSave();
    return true;
}

void favoritesRemove(int index) {
    if (index < 0 || index >= (int)_favs.size()) return;
    _favs.erase(_favs.begin() + index);
    favoritesSave();
}

bool favoritesSave() {
    SpiGuardScope lock(pdMS_TO_TICKS(500));
    if (!lock.locked()) return false;

    SD.mkdir("/melody_machine");

    DynamicJsonDocument doc(16384);
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& fs : _favs) {
        JsonObject obj = arr.createNestedObject();
        obj["name"] = fs.name;
        obj["url"]  = fs.url;
    }

    File f = SD.open(FAVORITES_TMP, FILE_WRITE);
    if (!f) return false;
    serializeJson(doc, f);
    f.close();

    SD.remove(FAVORITES_PATH);
    SD.rename(FAVORITES_TMP, FAVORITES_PATH);
    return true;
}
