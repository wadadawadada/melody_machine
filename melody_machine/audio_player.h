#pragma once
#include <Arduino.h>

enum PlayerState { PS_STOPPED, PS_PLAYING, PS_PAUSED };
enum RepeatMode  { RM_NONE = 0, RM_ONE, RM_ALL };
enum RadioStatus { RS_IDLE = 0, RS_CONNECTING, RS_BUFFERING, RS_PLAYING, RS_ERROR };
enum EqPreset    { EQ_FLAT = 0, EQ_BRIGHT, EQ_BASS, EQ_VOCAL, EQ_PRESET_COUNT };

void        audioPlayerInit();
void        audioPlayerPlay(const char* path, uint32_t fileSizeBytes = 0);
void        audioPlayerStop();
void        audioPlayerTogglePause();
bool        audioPlayerIsPaused();
void        audioPlayerSetVolume(uint8_t vol);
uint8_t     audioPlayerGetVolume();
void        audioPlayerSetEqPreset(EqPreset preset);
EqPreset    audioPlayerGetEqPreset();
const char* audioPlayerGetEqPresetName(EqPreset preset);
PlayerState audioPlayerGetState();
uint32_t    audioPlayerGetElapsed();
uint32_t    audioPlayerGetDuration();
bool        audioPlayerTrackDone();
void        audioPlayerClearDone();
void        audioPlayerSeek(uint32_t targetSec);

void        radioPlayerPlayURL(const char* url);
void        radioPlayerStop();
RadioStatus radioPlayerGetStatus();
String      radioPlayerGetStationName();
String      radioPlayerGetStationUrl();
int         radioPlayerGetBufferFillPct();
