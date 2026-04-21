#include "file_browser.h"
#include "settings_store.h"
#include "spi_guard.h"
#include <SD.h>
#include <algorithm>

static std::vector<FileEntry>      _entries;
static std::vector<FileEntry>      _tracks;
static std::vector<String>         _pathStack;
static String                      _currentPath = "/MP3";
static std::vector<RadioStation>   _radioStations;
static const int                   MAX_RADIO_STATIONS = 600;
static const int                   MAX_RADIO_NAME_LEN = 80;
static const int                   MAX_RADIO_URL_LEN  = 220;

static bool isMP3(const String& name) {
    if (name.length() < 4) return false;
    String lower = name;
    lower.toLowerCase();
    return lower.endsWith(".mp3");
}

static bool cmpEntries(const FileEntry& a, const FileEntry& b) {
    if (a.isDir != b.isDir) return (int)a.isDir > (int)b.isDir;
    String na = a.name; na.toLowerCase();
    String nb = b.name; nb.toLowerCase();
    return na < nb;
}

void fileBrowserInit() {
    String mode = settingsGetString("app.mode", "mp3");
    if (mode == "radio") {
        if (!fileBrowserScanRadio("/M3U")) {
            Serial.println("[FB] /M3U not found, radio playlist empty");
        }
    } else {
        if (!fileBrowserScan("/MP3")) {
            Serial.println("[FB] /MP3 not found, using /");
            fileBrowserScan("/");
        }
    }
}

bool fileBrowserScan(const char* path) {
    _entries.clear();
    _tracks.clear();

    SpiGuardScope spiLock(pdMS_TO_TICKS(800));
    if (!spiLock.locked()) {
        Serial.println("[FB] SPI lock timeout");
        return false;
    }

    File root = SD.open(path);
    if (!root) {
        Serial.printf("[FB] Cannot open: %s\n", path);
        return false;
    }
    if (!root.isDirectory()) {
        root.close();
        return false;
    }

    File f = root.openNextFile();
    while (f) {
        String fname = f.name();

        // Skip hidden files and system files
        if (fname.startsWith(".") || fname.startsWith("_")) {
            f.close();
            f = root.openNextFile();
            continue;
        }

        // Build full path
        String fpath = String(path);
        if (!fpath.endsWith("/")) fpath += "/";
        fpath += fname;

        if (f.isDirectory()) {
            FileEntry e;
            e.name     = fname;
            e.fullPath = fpath;
            e.isDir    = true;
            e.size     = 0;
            _entries.push_back(e);
        } else if (isMP3(fname)) {
            FileEntry e;
            e.name     = fname;
            e.fullPath = fpath;
            e.isDir    = false;
            e.size     = f.size();
            _entries.push_back(e);
            _tracks.push_back(e);
        }

        f.close();
        f = root.openNextFile();
    }
    root.close();

    // Sort: directories first, then alphabetically
    std::sort(_entries.begin(), _entries.end(), cmpEntries);
    std::sort(_tracks.begin(),  _tracks.end(),  cmpEntries);

    _currentPath = path;
    Serial.printf("[FB] %s: %d entries (%d tracks)\n",
                  path, _entries.size(), _tracks.size());
    return true;
}

int               fileBrowserCount()               { return (int)_entries.size(); }
const FileEntry&  fileBrowserGet(int i)             { return _entries[i]; }
const String&     fileBrowserCurrentPath()          { return _currentPath; }
int               fileBrowserTrackCount()           { return (int)_tracks.size(); }
const FileEntry&  fileBrowserTrack(int i)           { return _tracks[i]; }

bool fileBrowserEnter(int index) {
    if (index < 0 || index >= (int)_entries.size()) return false;
    if (!_entries[index].isDir) return false;
    String path = _entries[index].fullPath;
    _pathStack.push_back(_currentPath);
    if (!fileBrowserScan(path.c_str())) {
        _pathStack.pop_back();
        return false;
    }
    return true;
}

bool fileBrowserGoUp() {
    if (_pathStack.empty()) return false;
    String parent = _pathStack.back();
    _pathStack.pop_back();
    return fileBrowserScan(parent.c_str());
}

bool fileBrowserCanGoUp() { return !_pathStack.empty(); }

int fileBrowserFindTrack(const String& fullPath) {
    for (int i = 0; i < (int)_tracks.size(); i++) {
        if (_tracks[i].fullPath == fullPath) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// M3U radio station parser
// ---------------------------------------------------------------------------

bool fileBrowserScanRadio(const char* folder) {
    _radioStations.clear();

    SpiGuardScope spiLock(pdMS_TO_TICKS(800));
    if (!spiLock.locked()) {
        Serial.println("[FB] SPI lock timeout (radio scan)");
        return false;
    }

    File root = SD.open(folder);
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        Serial.printf("[FB] Radio folder not found: %s\n", folder);
        return false;
    }

    File f = root.openNextFile();
    while (f) {
        String fname = f.name();
        String lower = fname;
        lower.toLowerCase();
        if (!f.isDirectory() && lower.endsWith(".m3u")) {
            String fpath = String(folder);
            if (!fpath.endsWith("/")) fpath += "/";
            fpath += fname;

            File m3u = SD.open(fpath.c_str(), FILE_READ);
            if (m3u) {
                String pendingName;
                while (m3u.available()) {
                    String line = m3u.readStringUntil('\n');
                    line.trim();
                    if (line.startsWith("#EXTINF")) {
                        int comma = line.lastIndexOf(',');
                        if (comma >= 0) pendingName = line.substring(comma + 1);
                        else pendingName = "";
                    } else if (line.startsWith("http://") || line.startsWith("https://")) {
                        if ((int)_radioStations.size() >= MAX_RADIO_STATIONS) {
                            break;
                        }
                        RadioStation st;
                        if ((int)line.length() > MAX_RADIO_URL_LEN) line = line.substring(0, MAX_RADIO_URL_LEN);
                        st.url = line;
                        String name = (pendingName.length() > 0) ? pendingName : line;
                        if ((int)name.length() > MAX_RADIO_NAME_LEN) name = name.substring(0, MAX_RADIO_NAME_LEN);
                        st.name = name;
                        _radioStations.push_back(st);
                        pendingName = "";
                    }
                }
                m3u.close();
            }
            if ((int)_radioStations.size() >= MAX_RADIO_STATIONS) {
                break;
            }
        }
        f.close();
        f = root.openNextFile();
    }
    root.close();

    Serial.printf("[FB] Radio scan %s: %d stations\n", folder, (int)_radioStations.size());
    return !_radioStations.empty();
}

int fileBrowserRadioCount() {
    return (int)_radioStations.size();
}

const RadioStation& fileBrowserRadioGet(int index) {
    static RadioStation empty = {"", ""};
    if (index < 0 || index >= (int)_radioStations.size()) return empty;
    return _radioStations[index];
}
