#include "audio_player.h"
#include "spi_guard.h"
#include <LilyGoLib.h>
#include <AudioFileSourceSD.h>
#include <AudioFileSourceICYStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputFilterBiquad.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include "settings_store.h"

// ---------------------------------------------------------------------------
// SD source: lock SPI only for open/seek/close, NOT for read.
// The ESP32 SPI driver is internally thread-safe; locking every read()
// caused audio task to hold the mutex for long periods, starving the display.
// ---------------------------------------------------------------------------
static volatile uint8_t _volume       = 70;
static Preferences _prefs;
static volatile bool    _stopRequested = false;
static volatile EqPreset _eqPreset = EQ_FLAT;
static volatile EqPreset _eqPresetPending = EQ_FLAT;
static volatile bool _eqDirty = false;

static float hzToNormalizedFc(float hz) {
    // AudioOutputFilterBiquad expects Fc in 0..0.5 (fraction of sample rate).
    float fc = hz / 44100.0f;
    if (fc < 0.0001f) fc = 0.0001f;
    if (fc > 0.45f)   fc = 0.45f;
    return fc;
}

static uint8_t effectiveVolume(uint8_t baseVol, EqPreset preset) {
    int v = (int)baseVol;
    switch (preset) {
    case EQ_BASS:  v += 2; break;
    case EQ_VOCAL: v += 1; break;
    case EQ_BRIGHT: v -= 1; break;
    case EQ_FLAT:
    default: break;
    }
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return (uint8_t)v;
}

class LockedAudioFileSourceSD : public AudioFileSourceSD {
public:
    using AudioFileSourceSD::AudioFileSourceSD;

    bool open(const char* filename) override {
        SpiGuardScope lock(portMAX_DELAY);
        return AudioFileSourceSD::open(filename);
    }
    uint32_t read(void* data, uint32_t len) override {
        if (_stopRequested) {
            vTaskDelay(1);
            return 0;
        }
        uint32_t n = AudioFileSourceSD::read(data, len);
        // MP3 decoder may call read() in long bursts while switching tracks.
        // Periodically yield to let IDLE0 run and keep TWDT satisfied.
        if (((++_readYieldCounter) & 0x1Fu) == 0) vTaskDelay(1);
        return n;
    }
    uint32_t readNonBlock(void* data, uint32_t len) override {
        if (_stopRequested) {
            vTaskDelay(1);
            return 0;
        }
        uint32_t n = AudioFileSourceSD::read(data, len);
        if (((++_readYieldCounter) & 0x1Fu) == 0) vTaskDelay(1);
        return n;
    }
    bool seek(int32_t pos, int dir) override {
        SpiGuardScope lock(portMAX_DELAY);
        return AudioFileSourceSD::seek(pos, dir);
    }
    bool close() override {
        SpiGuardScope lock(portMAX_DELAY);
        return AudioFileSourceSD::close();
    }

private:
    uint32_t _readYieldCounter = 0;
};

// ---------------------------------------------------------------------------
// Audio output: writes PCM to ES8311 via I2S.
// stop() intentionally does NOT close codec РІР‚вЂќ keeps I2S alive between tracks
// so begin() on the next track doesn't need to re-enable the DMA channel.
// ---------------------------------------------------------------------------
class EspAudioOutput : public AudioOutput {
public:
    bool begin() override {
        if (hertz == 0) hertz = 44100;
        channels = 2;
        if (!_open) {
            _open = (instance.codec.open(bps, channels, hertz) != -1);
            if (_open) instance.codec.setVolume(effectiveVolume(_volume, (EqPreset)_eqPresetPending));
        }
        return _open;
    }

    bool SetRate(int hz) override {
        if (hz <= 0) return false;
        if (hertz == hz) return _open;
        hertz = hz;
        if (_open) { instance.codec.close(); _open = false; }
        vTaskDelay(pdMS_TO_TICKS(10));  // let I2S DMA drain before reopen
        _open = (instance.codec.open(bps, channels, hertz) != -1);
        if (_open) instance.codec.setVolume(effectiveVolume(_volume, (EqPreset)_eqPresetPending));
        return _open;
    }

    bool SetBitsPerSample(int bits) override { bps = bits; return true; }
    bool SetChannels(int chan)       override { channels = chan; return true; }

    bool ConsumeSample(int16_t sample[2]) override {
        if (_stopRequested) return false;
        if (!_open) return false;
        // Force periodic scheduler hand-off from the hot audio path.
        if (((++_yieldCounter) & 0x3FFu) == 0) vTaskDelay(1);
        int16_t out[2] = { sample[0], sample[1] };
        // AudioOutputFilterBiquad halves sample amplitude internally.
        // Compensate digitally for non-flat presets instead of over-driving codec volume.
        if (_eqPresetPending != EQ_FLAT) {
            int32_t l = (int32_t)out[0] * 2;
            int32_t r = (int32_t)out[1] * 2;
            if (l > 32767) l = 32767;
            if (l < -32768) l = -32768;
            if (r > 32767) r = 32767;
            if (r < -32768) r = -32768;
            out[0] = (int16_t)l;
            out[1] = (int16_t)r;
        }
        if (instance.codec.write(reinterpret_cast<uint8_t*>(out), 4) < 0)
            return false;
        return true;
    }

    // Deliberately empty: keeps codec/I2S open between tracks.
    bool stop() override { return true; }

    void hardClose() {
        if (_open) { instance.codec.close(); _open = false; }
    }

private:
    bool _open = false;
    uint32_t _yieldCounter = 0;
};

// ---------------------------------------------------------------------------
// Player state
// ---------------------------------------------------------------------------
enum CmdType { CMD_PLAY, CMD_STOP, CMD_TOGGLE_PAUSE, CMD_PLAY_RADIO, CMD_STOP_RADIO, CMD_SEEK };

struct AudioCommand {
    CmdType  type;
    char     path[256];
    uint32_t fileSize;
    uint32_t seekSec;   // target position in seconds (CMD_SEEK)
};

static volatile RadioStatus _radioStatus  = RS_IDLE;
static volatile char        _stationName[128] = {};
static volatile char        _stationUrl[512]  = {};
static volatile int         _bufferFillPct = 0;

static void onIcyMeta(void*, const char* type, bool, const char* str) {
    if (!type || !str) return;
    if (strcmp(type, "StreamTitle") == 0) {
        strncpy((char*)_stationName, str, sizeof(_stationName) - 1);
        _stationName[sizeof(_stationName) - 1] = '\0';
    }
}

static volatile PlayerState _state    = PS_STOPPED;
static volatile uint32_t    _elapsed  = 0;
static volatile uint32_t    _duration = 0;
static volatile bool        _done     = false;
static volatile uint32_t    _playStartMs = 0;
static uint32_t              _seekOffsetSec = 0;  // seconds base added after seek/restart

// Last played local file — needed for seek (restart at offset)
static char     _lastLocalPath[256] = {};
static uint32_t _lastLocalSize      = 0;

static QueueHandle_t         _cmdQueue   = nullptr;
static TaskHandle_t          _taskHandle = nullptr;
static EspAudioOutput*       _out        = nullptr;
static AudioOutput*          _pipelineOut = nullptr;
static AudioOutputFilterBiquad* _eq1 = nullptr;
static AudioOutputFilterBiquad* _eq2 = nullptr;
static AudioOutputFilterBiquad* _eq3 = nullptr;
static AudioGeneratorMP3*    _mp3        = nullptr;
static AudioFileSourceICYStream* _icy    = nullptr;
static AudioFileSourceBuffer*    _icyBuf = nullptr;
static uint32_t                  _icyBufSize = 0;

static uint32_t pickRadioBufferSize() {
    const uint32_t free8   = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (free8 > 150000 && largest > 100000) return 65536;
    if (free8 > 100000 && largest > 70000)  return 49152;
    if (free8 > 70000  && largest > 45000)  return 32768;
    if (free8 > 45000  && largest > 30000)  return 24576;
    return 16384;
}

static EqPreset eqPresetFromString(const String& s) {
    String v = s;
    v.toLowerCase();
    if (v == "bright") return EQ_BRIGHT;
    if (v == "bass")   return EQ_BASS;
    if (v == "vocal")  return EQ_VOCAL;
    return EQ_FLAT;
}

static const char* eqPresetLabel(EqPreset p) {
    switch (p) {
    case EQ_BRIGHT: return "Bright";
    case EQ_BASS:   return "Bass";
    case EQ_VOCAL:  return "Vocal";
    case EQ_FLAT:
    default:        return "Flat";
    }
}

static void clearEqPipeline() {
    if (_eq3) { delete _eq3; _eq3 = nullptr; }
    if (_eq2) { delete _eq2; _eq2 = nullptr; }
    if (_eq1) { delete _eq1; _eq1 = nullptr; }
    _pipelineOut = _out;
}

static void rebuildEqPipeline(EqPreset preset) {
    clearEqPipeline();
    if (!_out || preset == EQ_FLAT) {
        _pipelineOut = _out;
        return;
    }
    AudioOutput* sink = _out;
    auto addBiquad = [&](AudioOutputFilterBiquad*& slot, int type, float hz, float q, float gain) -> bool {
        slot = new AudioOutputFilterBiquad(type, hzToNormalizedFc(hz), q, gain, sink);
        if (!slot) return false;
        sink = slot;
        return true;
    };

    bool ok = true;
    switch (preset) {
    case EQ_BRIGHT:
        ok = addBiquad(_eq1, bq_type_highshelf, 3000.0f, 0.707f, 2.2f) &&
             addBiquad(_eq2, bq_type_peak,      1800.0f, 1.000f, 1.2f);
        break;
    case EQ_BASS:
        ok = addBiquad(_eq1, bq_type_lowshelf,  220.0f, 0.707f, 6.0f) &&
             addBiquad(_eq2, bq_type_highshelf, 3200.0f, 0.707f, -1.2f);
        break;
    case EQ_VOCAL:
        ok = addBiquad(_eq1, bq_type_peak,      1700.0f, 1.000f, 3.5f) &&
             addBiquad(_eq2, bq_type_lowshelf,   220.0f, 0.707f, -1.8f);
        break;
    case EQ_FLAT:
    default:
        ok = true;
        break;
    }

    if (!ok) {
        clearEqPipeline();
        _pipelineOut = _out;
        _eqPreset = EQ_FLAT;
        _eqPresetPending = EQ_FLAT;
        _eqDirty = false;
        Serial.println("[AUDIO] EQ alloc failed, fallback to Flat");
        return;
    }
    _pipelineOut = sink;
}

// ---------------------------------------------------------------------------
// Audio task РІР‚вЂќ Core 0, priority 5
// ---------------------------------------------------------------------------
static void audioTask(void*) {
    LockedAudioFileSourceSD* src = nullptr;
    uint32_t startMs     = 0;
    uint32_t lastVolApply = 0;
    uint8_t  appliedVol  = 255;
    uint32_t lastPlayLog = 0;
    uint32_t lastRadioLog = 0;
    bool radioHadPositiveFill = false;

    static int16_t silenceBuf[256];
    memset(silenceBuf, 0, sizeof(silenceBuf));

    auto cleanup = [&](bool closeCodec) {
        if (_mp3) {
            if (_mp3->isRunning()) _mp3->stop();
            delete _mp3; _mp3 = nullptr;
        }
        if (src) { delete src; src = nullptr; }
        // _mp3->stop() already closes the active source chain.
        // Avoid duplicate close() calls on radio sources during station switch.
        if (_icyBuf) { delete _icyBuf; _icyBuf = nullptr; }
        if (_icy)    { delete _icy;    _icy    = nullptr; }
        _icyBufSize   = 0;
        _bufferFillPct = 0;
        _radioStatus  = RS_IDLE;
        radioHadPositiveFill = false;
        // Keep I2S open while switching tracks to avoid channel reinit races.
        // Fully close only on explicit stop paths.
        if (closeCodec) _out->hardClose();
        _elapsed = 0;
        _playStartMs = 0;
        _seekOffsetSec = 0;
        _state   = PS_STOPPED;
    };

    auto applyPendingEq = [&]() {
        if (!_eqDirty) return;
        rebuildEqPipeline((EqPreset)_eqPresetPending);
        _eqPreset = _eqPresetPending;
        _eqDirty = false;
        Serial.printf("[AUDIO] EQ preset: %s\n", eqPresetLabel((EqPreset)_eqPreset));
    };

    auto startLocal = [&](const char* path, uint32_t fileSize, uint32_t offsetSec = 0) {
        _duration = 0;
        _done = false;
        strncpy(_lastLocalPath, path, sizeof(_lastLocalPath) - 1);
        _lastLocalPath[sizeof(_lastLocalPath) - 1] = '\0';
        _lastLocalSize = fileSize;
        _seekOffsetSec = offsetSec;
        _mp3 = new AudioGeneratorMP3();
        src  = new LockedAudioFileSourceSD(path);
        if (_mp3 && src) {
            if (offsetSec > 0 && fileSize > 0) {
                // Estimate byte offset assuming 128 kbps CBR = 16000 bytes/sec
                uint32_t byteOffset = offsetSec * 16000;
                if (byteOffset >= fileSize) byteOffset = 0;
                src->seek((int32_t)byteOffset, SEEK_SET);
                Serial.printf("[AUDIO] Seek to %us (~%u bytes)\n", offsetSec, byteOffset);
            }
            if (_mp3->begin(src, _pipelineOut)) {
                startMs = millis();
                _playStartMs = startMs;
                _elapsed = offsetSec;
                _state  = PS_PLAYING;
                lastPlayLog = 0;
                Serial.printf("[AUDIO] Playing: %s offset=%us\n", path, offsetSec);
                return;
            }
        }
        Serial.printf("[AUDIO] begin() failed: %s\n", path);
        cleanup(false);
    };

    auto startRadio = [&](const char* url) {
        _duration = 0;
        _done = false;
        memset((char*)_stationName, 0, sizeof(_stationName));
        strncpy((char*)_stationUrl, url ? url : "", sizeof(_stationUrl) - 1);
        _stationUrl[sizeof(_stationUrl) - 1] = '\0';
        _radioStatus = RS_CONNECTING;
        Serial.printf("[AUDIO] heap free=%u largest=%u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

        _icy = new AudioFileSourceICYStream(url);
        if (!_icy) {
            Serial.println("[AUDIO] ICY stream alloc failed");
            _radioStatus = RS_ERROR;
            cleanup(false);
            return;
        }
        _icy->RegisterMetadataCB(onIcyMeta, nullptr);

        const uint32_t preferred = pickRadioBufferSize();
        const uint32_t sizes[] = {preferred, 65536, 49152, 32768, 24576, 16384, 12288};
        _icyBuf = nullptr;
        _icyBufSize = 0;
        for (uint32_t s : sizes) {
            if (s > preferred) continue;
            AudioFileSourceBuffer* candidate = new AudioFileSourceBuffer(_icy, s);
            if (candidate) {
                _icyBuf = candidate;
                _icyBufSize = s;
                break;
            }
        }
        if (!_icyBuf || _icyBufSize == 0) {
            Serial.println("[AUDIO] Cannot allocate ICY buffer");
            _radioStatus = RS_ERROR;
            cleanup(false);
            return;
        }

        _mp3 = new AudioGeneratorMP3();
        if (!_mp3) {
            Serial.println("[AUDIO] MP3 decoder alloc failed");
            _radioStatus = RS_ERROR;
            cleanup(false);
            return;
        }
        if (_mp3->begin(_icyBuf, _pipelineOut)) {
            startMs = millis();
            _playStartMs = startMs;
            _state = PS_PLAYING;
            _radioStatus = RS_BUFFERING;
            lastPlayLog = 0;
            Serial.printf("[AUDIO] Radio stream started: %s (buf=%u)\n",
                          url, (unsigned)_icyBufSize);
            return;
        }
        Serial.printf("[AUDIO] Radio begin() failed: %s\n", url);
        _radioStatus = RS_ERROR;
        cleanup(false);
    };

    while (true) {
        if (_eqDirty && _state == PS_STOPPED) applyPendingEq();

        TickType_t wait = (_state == PS_STOPPED) ? pdMS_TO_TICKS(20) : 0;
        AudioCommand cmd;

        if (xQueueReceive(_cmdQueue, &cmd, wait) == pdTRUE) {
            switch (cmd.type) {
            case CMD_PLAY:
                Serial.printf("[AUDIO] CMD_PLAY recv: %s\n", cmd.path);
                _stopRequested = true;
                cleanup(false);
                _stopRequested = false;
                applyPendingEq();
                startLocal(cmd.path, cmd.fileSize);
                break;

            case CMD_STOP:
                _stopRequested = true;
                cleanup(true);
                _stopRequested = false;
                Serial.println("[AUDIO] Stopped");
                break;

            case CMD_TOGGLE_PAUSE:
                if (_state == PS_PLAYING) {
                    if (_playStartMs != 0) _elapsed = (millis() - _playStartMs) / 1000;
                    _state = PS_PAUSED;
                    Serial.println("[AUDIO] Paused");
                } else if (_state == PS_PAUSED) {
                    startMs = millis() - _elapsed * 1000;
                    _playStartMs = startMs;
                    _state  = PS_PLAYING;
                    Serial.println("[AUDIO] Resumed");
                }
                break;

            case CMD_PLAY_RADIO:
                Serial.printf("[AUDIO] CMD_PLAY_RADIO: %s\n", cmd.path);
                _stopRequested = true;
                cleanup(false);
                _stopRequested = false;
                applyPendingEq();
                startRadio(cmd.path);
                break;

            case CMD_STOP_RADIO:
                _stopRequested = true;
                cleanup(true);
                _stopRequested = false;
                Serial.println("[AUDIO] Radio stopped");
                break;

            case CMD_SEEK:
                if (_lastLocalPath[0] != '\0') {
                    _stopRequested = true;
                    cleanup(false);
                    _stopRequested = false;
                    applyPendingEq();
                    startLocal(_lastLocalPath, _lastLocalSize, cmd.seekSec);
                }
                break;
            }
        }

        // Volume
        if (appliedVol != _volume || millis() - lastVolApply > 200) {
            instance.codec.setVolume(effectiveVolume(_volume, (EqPreset)_eqPresetPending));
            appliedVol   = _volume;
            lastVolApply = millis();
        }

        if (_state == PS_PLAYING) {
            // Fast preemption for station switch: do not enter decoder loop
            // when a stop/play command has already requested pipeline teardown.
            if (_stopRequested) {
                cleanup(false);
                _done = false;
                vTaskDelay(1);
                continue;
            }

            // Peek queue: if CMD_PLAY/STOP is waiting, don't start another loop() call
            AudioCommand peek;
            if (xQueuePeek(_cmdQueue, &peek, 0) == pdTRUE &&
                (peek.type == CMD_PLAY || peek.type == CMD_STOP ||
                 peek.type == CMD_PLAY_RADIO || peek.type == CMD_STOP_RADIO ||
                 peek.type == CMD_SEEK)) {
                vTaskDelay(1);
                continue;  // receive on next iteration
            }

            if (_mp3 && _mp3->isRunning()) {
                if (!_mp3->loop()) {
                    if (_icy) {
                        _radioStatus = RS_ERROR;
                        Serial.println("[AUDIO] Radio stream ended/error");
                    }
                    cleanup(false);
                    _done = true;
                    Serial.println("[AUDIO] Track finished");
                } else {
                    _elapsed = _seekOffsetSec + (millis() - startMs) / 1000;
                    _playStartMs = startMs;
                    if (_icyBuf && _icyBufSize > 0) {
                        uint32_t fill = _icyBuf->getFillLevel();
                        _bufferFillPct = (int)((uint64_t)fill * 100 / _icyBufSize);
                        if (fill > 0) radioHadPositiveFill = true;
                        // Promote to PLAYING once decoder is actively running
                        // (fill > 0 means at least one successful read from buffer).
                        if (_radioStatus == RS_BUFFERING && (fill > 0 || _elapsed >= 1)) {
                            _radioStatus = RS_PLAYING;
                        }
                        if (millis() - lastRadioLog > 1000) {
                            lastRadioLog = millis();
                            Serial.printf("[AUDIO-RAD] st=%d fill=%u/%u pct=%d\n",
                                          (int)_radioStatus,
                                          (unsigned)fill,
                                          (unsigned)_icyBufSize,
                                          (int)_bufferFillPct);
                        }
                    }
                    if (millis() - lastPlayLog > 2000) {
                        lastPlayLog = millis();
                        Serial.printf("[AUDIO] elapsed=%lus dur=%lus\n",
                                      (unsigned long)_elapsed,
                                      (unsigned long)_duration);
                    }
                }
            } else {
                if (_icy) _radioStatus = RS_ERROR;
                cleanup(false);
                _done = true;
            }
            // Avoid starving IDLE task on the same core.
            vTaskDelay(1);

        } else if (_state == PS_PAUSED) {
            instance.codec.write(
                reinterpret_cast<uint8_t*>(silenceBuf), sizeof(silenceBuf));
            // Keep yielding while paused to prevent WDT on CPU0.
            vTaskDelay(1);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void audioPlayerInit() {
    _prefs.begin("mm_player", false);
    _volume = (uint8_t)_prefs.getUInt("volume", 70);
    if (_volume > 100) _volume = 70;
    _eqPreset = eqPresetFromString(settingsGetString("audio.eq", "flat"));
    _eqPresetPending = _eqPreset;
    _eqDirty = false;

    _out      = new EspAudioOutput();
    _pipelineOut = _out;
    rebuildEqPipeline((EqPreset)_eqPreset);
    _mp3      = nullptr;
    _cmdQueue = xQueueCreate(1, sizeof(AudioCommand));

    instance.powerControl(POWER_SPEAK, true);
    instance.codec.setVolume(effectiveVolume(_volume, (EqPreset)_eqPresetPending));

    xTaskCreatePinnedToCore(audioTask, "audio", 12288, nullptr, 2, &_taskHandle, 0);
    Serial.printf("[AUDIO] Init OK, task on Core 0, EQ=%s\n", eqPresetLabel((EqPreset)_eqPreset));
}

static void sendCmd(const AudioCommand& c) {
    if (!_cmdQueue) return;
    bool isStop = (c.type == CMD_PLAY || c.type == CMD_STOP ||
                   c.type == CMD_PLAY_RADIO || c.type == CMD_STOP_RADIO ||
                   c.type == CMD_SEEK);
    if (isStop) _stopRequested = true;
    if (c.type == CMD_PLAY) Serial.printf("[AUDIO] CMD_PLAY send: %s\n", c.path);
    if (c.type == CMD_PLAY_RADIO) Serial.printf("[AUDIO] CMD_PLAY_RADIO send: %s\n", c.path);
    xQueueOverwrite(_cmdQueue, &c);
}

void audioPlayerPlay(const char* path, uint32_t fileSizeBytes) {
    AudioCommand c = {};
    c.type     = CMD_PLAY;
    c.fileSize = fileSizeBytes;
    strncpy(c.path, path, sizeof(c.path) - 1);
    sendCmd(c);
}

void audioPlayerStop()        { AudioCommand c = {}; c.type = CMD_STOP;         sendCmd(c); }
void audioPlayerTogglePause() { AudioCommand c = {}; c.type = CMD_TOGGLE_PAUSE; sendCmd(c); }
void audioPlayerSeek(uint32_t targetSec) {
    AudioCommand c = {};
    c.type    = CMD_SEEK;
    c.seekSec = targetSec;
    sendCmd(c);
}

bool        audioPlayerIsPaused()    { return _state == PS_PAUSED; }
void        audioPlayerSetVolume(uint8_t vol) {
    _volume = (vol > 100) ? 100 : vol;
    _prefs.putUInt("volume", _volume);
    // Apply immediately so UI controls remain responsive even if decoder loop blocks.
    instance.codec.setVolume(effectiveVolume(_volume, (EqPreset)_eqPresetPending));
}
uint8_t     audioPlayerGetVolume()   { return _volume; }
void        audioPlayerSetEqPreset(EqPreset preset) {
    if (preset < EQ_FLAT || preset >= EQ_PRESET_COUNT) preset = EQ_FLAT;
    _eqPresetPending = preset;
    _eqDirty = true;
}
EqPreset    audioPlayerGetEqPreset() { return (EqPreset)_eqPresetPending; }
const char* audioPlayerGetEqPresetName(EqPreset preset) {
    if (preset < EQ_FLAT || preset >= EQ_PRESET_COUNT) preset = EQ_FLAT;
    return eqPresetLabel(preset);
}
PlayerState audioPlayerGetState()    { return _state; }
uint32_t    audioPlayerGetElapsed()  {
    if (_state == PS_PLAYING && _playStartMs != 0) {
        uint32_t live = _seekOffsetSec + (millis() - _playStartMs) / 1000;
        return (live > _elapsed) ? live : _elapsed;
    }
    return _elapsed;
}
uint32_t    audioPlayerGetDuration() { return _duration; }
bool        audioPlayerTrackDone()   { return _done; }
void        audioPlayerClearDone()   { _done = false; }

void radioPlayerPlayURL(const char* url) {
    AudioCommand c = {};
    c.type = CMD_PLAY_RADIO;
    strncpy(c.path, url, sizeof(c.path) - 1);
    sendCmd(c);
}

void radioPlayerStop() {
    AudioCommand c = {};
    c.type = CMD_STOP_RADIO;
    sendCmd(c);
}

RadioStatus radioPlayerGetStatus()      { return _radioStatus; }
String      radioPlayerGetStationName() { return String((const char*)_stationName); }
String      radioPlayerGetStationUrl()  { return String((const char*)_stationUrl); }
int         radioPlayerGetBufferFillPct() { return _bufferFillPct; }
