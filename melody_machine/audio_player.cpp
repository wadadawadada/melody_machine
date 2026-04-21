#include "audio_player.h"
#include "spi_guard.h"
#include <LilyGoLib.h>
#include <AudioFileSourceSD.h>
#include <AudioFileSourceICYStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

// ---------------------------------------------------------------------------
// SD source: lock SPI only for open/seek/close, NOT for read.
// The ESP32 SPI driver is internally thread-safe; locking every read()
// caused audio task to hold the mutex for long periods, starving the display.
// ---------------------------------------------------------------------------
static volatile uint8_t _volume       = 70;
static Preferences _prefs;
static volatile bool    _stopRequested = false;

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
            if (_open) instance.codec.setVolume(_volume);
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
        if (_open) instance.codec.setVolume(_volume);
        return _open;
    }

    bool SetBitsPerSample(int bits) override { bps = bits; return true; }
    bool SetChannels(int chan)       override { channels = chan; return true; }

    bool ConsumeSample(int16_t sample[2]) override {
        if (_stopRequested) return false;
        if (!_open) return false;
        // Force periodic scheduler hand-off from the hot audio path.
        if (((++_yieldCounter) & 0x3FFu) == 0) vTaskDelay(1);
        if (instance.codec.write(reinterpret_cast<uint8_t*>(sample), 4) < 0)
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
enum CmdType { CMD_PLAY, CMD_STOP, CMD_TOGGLE_PAUSE, CMD_PLAY_RADIO, CMD_STOP_RADIO };

struct AudioCommand {
    CmdType  type;
    char     path[256];
    uint32_t fileSize;
};

static volatile RadioStatus _radioStatus  = RS_IDLE;
static volatile char        _stationName[128] = {};
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

static QueueHandle_t         _cmdQueue   = nullptr;
static TaskHandle_t          _taskHandle = nullptr;
static EspAudioOutput*       _out        = nullptr;
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

// ---------------------------------------------------------------------------
// Audio task РІР‚вЂќ Core 0, priority 5
// ---------------------------------------------------------------------------
static void audioTask(void*) {
    LockedAudioFileSourceSD* src = nullptr;
    uint32_t startMs     = 0;
    uint32_t lastVolApply = 0;
    uint8_t  appliedVol  = 255;
    uint32_t lastPlayLog = 0;

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
        // Keep I2S open while switching tracks to avoid channel reinit races.
        // Fully close only on explicit stop paths.
        if (closeCodec) _out->hardClose();
        _elapsed = 0;
        _playStartMs = 0;
        _state   = PS_STOPPED;
    };

    while (true) {
        TickType_t wait = (_state == PS_STOPPED) ? pdMS_TO_TICKS(20) : 0;
        AudioCommand cmd;

        if (xQueueReceive(_cmdQueue, &cmd, wait) == pdTRUE) {
            switch (cmd.type) {
            case CMD_PLAY:
                Serial.printf("[AUDIO] CMD_PLAY recv: %s\n", cmd.path);
                _stopRequested = true;
                cleanup(false);
                _stopRequested = false;
                // Do not prefill duration from file size: this rough estimate
                // causes UI progress to run ahead and then jump back when TLEN arrives.
                _duration = 0;
                _done = false;

                _mp3 = new AudioGeneratorMP3();
                src  = new LockedAudioFileSourceSD(cmd.path);
                if (_mp3->begin(src, _out)) {
                    startMs = millis();
                    _playStartMs = startMs;
                    _state  = PS_PLAYING;
                    lastPlayLog = 0;
                    Serial.printf("[AUDIO] Playing: %s\n", cmd.path);
                } else {
                    Serial.printf("[AUDIO] begin() failed: %s\n", cmd.path);
                    cleanup(false);
                }
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
                _duration = 0;
                _done = false;
                memset((char*)_stationName, 0, sizeof(_stationName));
                _radioStatus = RS_CONNECTING;
                Serial.printf("[AUDIO] heap free=%u largest=%u\n",
                              (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

                _icy = new AudioFileSourceICYStream(cmd.path);
                if (!_icy) {
                    Serial.println("[AUDIO] ICY stream alloc failed");
                    _radioStatus = RS_ERROR;
                    cleanup(false);
                    break;
                }
                _icy->RegisterMetadataCB(onIcyMeta, nullptr);

                {
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
                }

                if (!_icyBuf || _icyBufSize == 0) {
                    Serial.println("[AUDIO] Cannot allocate ICY buffer");
                    _radioStatus = RS_ERROR;
                    cleanup(false);
                    break;
                }

                _mp3 = new AudioGeneratorMP3();
                if (!_mp3) {
                    Serial.println("[AUDIO] MP3 decoder alloc failed");
                    _radioStatus = RS_ERROR;
                    cleanup(false);
                    break;
                }
                if (_mp3->begin(_icyBuf, _out)) {
                    startMs = millis();
                    _playStartMs = startMs;
                    _state = PS_PLAYING;
                    _radioStatus = RS_BUFFERING;
                    lastPlayLog = 0;
                    Serial.printf("[AUDIO] Radio stream started: %s (buf=%u)\n",
                                  cmd.path, (unsigned)_icyBufSize);
                } else {
                    Serial.printf("[AUDIO] Radio begin() failed: %s\n", cmd.path);
                    _radioStatus = RS_ERROR;
                    cleanup(false);
                }
                break;

            case CMD_STOP_RADIO:
                _stopRequested = true;
                cleanup(true);
                _stopRequested = false;
                Serial.println("[AUDIO] Radio stopped");
                break;
            }
        }

        // Volume
        if (appliedVol != _volume || millis() - lastVolApply > 200) {
            instance.codec.setVolume(_volume);
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
                 peek.type == CMD_PLAY_RADIO || peek.type == CMD_STOP_RADIO)) {
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
                    _elapsed = (millis() - startMs) / 1000;
                    _playStartMs = startMs;
                    if (_icyBuf && _icyBufSize > 0) {
                        uint32_t fill = _icyBuf->getFillLevel();
                        _bufferFillPct = (int)((uint64_t)fill * 100 / _icyBufSize);
                        if (_bufferFillPct >= 35 && _radioStatus == RS_BUFFERING) {
                            _radioStatus = RS_PLAYING;
                        } else if (_bufferFillPct <= 8 && _radioStatus == RS_PLAYING) {
                            _radioStatus = RS_BUFFERING;
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

    _out      = new EspAudioOutput();
    _mp3      = nullptr;
    _cmdQueue = xQueueCreate(1, sizeof(AudioCommand));

    instance.powerControl(POWER_SPEAK, true);
    instance.codec.setVolume(_volume);

    xTaskCreatePinnedToCore(audioTask, "audio", 12288, nullptr, 2, &_taskHandle, 0);
    Serial.println("[AUDIO] Init OK, task on Core 0");
}

static void sendCmd(const AudioCommand& c) {
    if (!_cmdQueue) return;
    bool isStop = (c.type == CMD_PLAY || c.type == CMD_STOP ||
                   c.type == CMD_PLAY_RADIO || c.type == CMD_STOP_RADIO);
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

bool        audioPlayerIsPaused()    { return _state == PS_PAUSED; }
void        audioPlayerSetVolume(uint8_t vol) {
    _volume = (vol > 100) ? 100 : vol;
    _prefs.putUInt("volume", _volume);
}
uint8_t     audioPlayerGetVolume()   { return _volume; }
PlayerState audioPlayerGetState()    { return _state; }
uint32_t    audioPlayerGetElapsed()  {
    if (_state == PS_PLAYING && _playStartMs != 0) {
        uint32_t live = (millis() - _playStartMs) / 1000;
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
int         radioPlayerGetBufferFillPct() { return _bufferFillPct; }


