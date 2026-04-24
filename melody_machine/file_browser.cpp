#include "file_browser.h"
#include "settings_store.h"
#include "spi_guard.h"
#include <SD.h>
#include <algorithm>

static std::vector<FileEntry>      _entries;
static std::vector<FileEntry>      _tracks;
static std::vector<String>         _pathStack;
static String                      _currentPath = "/melody_machine/mp3";
static std::vector<RadioStation>   _radioStations;
static const int                   MAX_RADIO_STATIONS = 600;
static const int                   MAX_RADIO_NAME_LEN = 80;
static const int                   MAX_RADIO_URL_LEN  = 220;

// M3U playlist list (two-level radio browser)
static std::vector<String>  _m3uNames;   // display name (filename without .m3u)
static std::vector<String>  _m3uPaths;   // full SD path
static int                  _m3uSelected = -1;  // -1 = showing playlist list

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

static bool cmpStrings(const String& a, const String& b) {
    String la = a; la.toLowerCase();
    String lb = b; lb.toLowerCase();
    return la < lb;
}

void fileBrowserInit() {
    String mode = settingsGetString("app.mode", "mp3");
    if (mode == "radio") {
        if (!fileBrowserScanRadio("/melody_machine/m3u")) {
            Serial.println("[FB] /melody_machine/m3u not found, radio playlist empty");
        }
    } else {
        if (!fileBrowserScan("/melody_machine/mp3")) {
            Serial.println("[FB] /melody_machine/mp3 not found, trying legacy /MP3");
            if (!fileBrowserScan("/MP3")) {
                Serial.println("[FB] /MP3 also not found, using /");
                fileBrowserScan("/");
            }
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
        String fullName = f.name();  // ESP32 SD lib may return full path
        int lastSlash = fullName.lastIndexOf('/');
        String fname = (lastSlash >= 0) ? fullName.substring(lastSlash + 1) : fullName;

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
// M3U radio station parser (single file)
// ---------------------------------------------------------------------------

// Internal parser — caller must already hold the SPI lock.
static bool parseM3uFileLocked(const char* fpath) {
    _radioStations.clear();

    File m3u = SD.open(fpath, FILE_READ);
    if (!m3u) {
        Serial.printf("[FB] parseM3u: cannot open %s\n", fpath);
        return false;
    }

    String pendingName;
    while (m3u.available()) {
        String line = m3u.readStringUntil('\n');
        line.trim();
        if (line.startsWith("#EXTINF")) {
            int comma = line.lastIndexOf(',');
            if (comma >= 0) pendingName = line.substring(comma + 1);
            else pendingName = "";
        } else if (line.startsWith("http://") || line.startsWith("https://")) {
            if ((int)_radioStations.size() >= MAX_RADIO_STATIONS) break;
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
    Serial.printf("[FB] parseM3u: %s -> %d stations\n", fpath, (int)_radioStations.size());
    return !_radioStations.empty();
}

static bool parseM3uFile(const char* fpath) {
    SpiGuardScope spiLock(pdMS_TO_TICKS(800));
    if (!spiLock.locked()) {
        Serial.println("[FB] parseM3u: SPI lock timeout");
        return false;
    }
    return parseM3uFileLocked(fpath);
}

// ---------------------------------------------------------------------------
// Radio browser: scan folder for .m3u files (builds playlist list)
// ---------------------------------------------------------------------------

bool fileBrowserScanRadio(const char* folder) {
    _m3uNames.clear();
    _m3uPaths.clear();
    _radioStations.clear();
    _m3uSelected = -1;

    SpiGuardScope spiLock(pdMS_TO_TICKS(800));
    if (!spiLock.locked()) {
        Serial.println("[FB] SPI lock timeout (radio scan)");
        return false;
    }

    File root = SD.open(folder);
    // Fallback to legacy path if new path is missing
    if ((!root || !root.isDirectory()) && String(folder) == "/melody_machine/m3u") {
        if (root) root.close();
        Serial.printf("[FB] %s not found, trying legacy /M3U\n", folder);
        root = SD.open("/M3U");
        if (!root || !root.isDirectory()) {
            if (root) root.close();
            Serial.println("[FB] /M3U also not found, radio playlist empty");
            return false;
        }
        folder = "/M3U";
    }
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        Serial.printf("[FB] Radio folder not found: %s\n", folder);
        return false;
    }

    File f = root.openNextFile();
    while (f) {
        String fullName = f.name();  // may be full path on ESP32 SD lib
        // Extract just the filename component
        int lastSlash = fullName.lastIndexOf('/');
        String fname = (lastSlash >= 0) ? fullName.substring(lastSlash + 1) : fullName;
        String lower = fname;
        lower.toLowerCase();
        Serial.printf("[FB] radio scan entry: '%s' isDir=%d\n", fname.c_str(), f.isDirectory());
        if (!f.isDirectory() && lower.endsWith(".m3u")) {
            String fpath = String(folder);
            if (!fpath.endsWith("/")) fpath += "/";
            fpath += fname;

            // Display name: strip .m3u extension
            String dname = fname;
            if (dname.length() > 4) dname = dname.substring(0, dname.length() - 4);

            Serial.printf("[FB] found m3u: '%s' -> '%s'\n", dname.c_str(), fpath.c_str());
            _m3uNames.push_back(dname);
            _m3uPaths.push_back(fpath);
        }
        f.close();
        f = root.openNextFile();
    }
    root.close();

    // Sort alphabetically
    // (simple bubble sort on small lists is fine here)
    int n = (int)_m3uNames.size();
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - 1 - i; j++) {
            String la = _m3uNames[j]; la.toLowerCase();
            String lb = _m3uNames[j+1]; lb.toLowerCase();
            if (la > lb) {
                std::swap(_m3uNames[j], _m3uNames[j+1]);
                std::swap(_m3uPaths[j], _m3uPaths[j+1]);
            }
        }
    }

    Serial.printf("[FB] Radio scan %s: %d playlists\n", folder, n);

    return n > 0;
}

int fileBrowserRadioCount() {
    return (int)_radioStations.size();
}

const RadioStation& fileBrowserRadioGet(int index) {
    static RadioStation empty = {"", ""};
    if (index < 0 || index >= (int)_radioStations.size()) return empty;
    return _radioStations[index];
}

int fileBrowserM3uCount() {
    return (int)_m3uNames.size();
}

String fileBrowserM3uName(int index) {
    if (index < 0 || index >= (int)_m3uNames.size()) return "";
    return _m3uNames[index];
}

bool fileBrowserM3uLoad(int index) {
    if (index < 0 || index >= (int)_m3uPaths.size()) return false;
    bool ok = parseM3uFile(_m3uPaths[index].c_str());
    if (ok) _m3uSelected = index;
    Serial.printf("[FB] Loaded playlist %d (%s): %d stations\n",
                  index, _m3uNames[index].c_str(), (int)_radioStations.size());
    return ok;
}

int fileBrowserM3uSelected() {
    return _m3uSelected;
}

void fileBrowserM3uGoBack() {
    _radioStations.clear();
    _m3uSelected = -1;
}
