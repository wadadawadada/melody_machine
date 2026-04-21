#pragma once
#include <Arduino.h>
#include <vector>

struct FileEntry {
    String   name;      // filename only (for display)
    String   fullPath;  // absolute path on SD
    bool     isDir;
    uint32_t size;      // bytes (for duration estimate)
};

void              fileBrowserInit();
bool              fileBrowserScan(const char* path);
int               fileBrowserCount();
const FileEntry&  fileBrowserGet(int index);
const String&     fileBrowserCurrentPath();
bool              fileBrowserEnter(int index);   // enter sub-directory
bool              fileBrowserGoUp();             // go to parent directory
bool              fileBrowserCanGoUp();
// Returns list of all MP3 tracks in current directory (for playlist)
int               fileBrowserTrackCount();
const FileEntry&  fileBrowserTrack(int index);
int               fileBrowserFindTrack(const String& fullPath);

struct RadioStation { String name; String url; };

bool                 fileBrowserScanRadio(const char* folder = "/M3U");
int                  fileBrowserRadioCount();
const RadioStation&  fileBrowserRadioGet(int index);
