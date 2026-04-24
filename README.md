# Melody Machine v0.2

MP3 player and internet radio firmware for the **LilyGO T-LoRa Pager** (ESP32-S3).

![Melody Machine splash screen](img/1.png)

![MP3 Player](img/mp3_payer.bmp)

![Internet Radio](img/internet_radio.bmp)

---

## Features

- **MP3 Player** — plays MP3 files from SD card with folder browser, shuffle, repeat modes, and seek
- **Internet Radio** — streams internet radio via M3U playlists over WiFi (ICY metadata support)
- **Folder browser** — navigate into subdirectories; `..` row returns to parent
- **Radio playlist browser** — two-level navigation: playlist file list → stations inside
- **Seek** — rewind / fast-forward MP3 tracks with the rotary encoder (±5 s per step)
- **Auto power-off** — configurable idle power-off timer (15 min – 2 h), activates when playback is stopped
- **LVGL UI** — graphical interface on the 480×222 TFT display with 4 switchable themes
- **WiFi Manager** — non-blocking WiFi with network list, password entry via on-screen keyboard, and auto-reconnect
- **Settings** — all settings persisted as JSON on SD card (`/melody_machine/settings.json`); survives reboots and reflashes
- **Dual-core audio** — MP3 decoding runs on Core 0 via FreeRTOS, keeping the UI responsive on Core 1

### Themes

4 switchable themes are available in Settings → Theme.

---

## Hardware

| Component | Details |
|-----------|---------|
| MCU | ESP32-S3, dual-core Xtensa 240 MHz |
| Display | 480×222 ST7796U TFT, RGB565, SPI |
| Audio codec | ES8311 via I2S — speaker + headphone jack |
| Storage | MicroSD via SPI |
| Input | Physical QWERTY keyboard (TCA8418 I2C matrix) + rotary encoder |
| Connectivity | WiFi 802.11 b/g/n (built-in) |
| Board | LilyGO T-LoRa Pager |

---

## Controls

### Player Screen

| Input | Action |
|-------|--------|
| Rotate encoder | Scroll browser list |
| Click encoder | Enter folder / play track / confirm |
| `Q` / `A` | Volume +5 / -5 |
| `W` / `D` | Previous / next track |
| `Space` | Play / pause |
| `B` / Backspace | Stop · go up folder · exit seek mode |
| `R` | Cycle repeat: off → one → all |
| `H` | Toggle shuffle |
| `N` | Toggle **seek mode** (MP3 only, while playing/paused) |
| `S` | Open settings |
| `S` + `H` | Screenshot to SD |
| `i` | Open controls help |

#### Seek Mode (`N`)

Press `N` to enter seek mode. The status chip shows **SEEK** in yellow.

| Input | Action |
|-------|--------|
| Rotate encoder | Jump ±5 seconds |
| `Enter` or `B` | Exit seek mode |

### Settings Screen

| Row | Description |
|-----|-------------|
| Mode | Switch MP3 ↔ Radio |
| WiFi network | Connect / manage networks |
| Equalizer | Flat / Bright / Bass / Vocal |
| Brightness | Display brightness |
| Screen timeout | Auto-dim timer |
| KB backlight | Keyboard backlight timeout |
| Theme | Cycle through 4 themes |
| Auto power-off | Power off after idle: Off / 15m / 30m / 45m / 1h / 90m / 2h |
| Debug mode | Serial debug output |
| USB mode | USB MSC (SD card sharing) |
| Restart device | Reboot |
| Power off | Deep sleep |

---

## SD Card Layout

```
SD:/
└── melody_machine/
    ├── settings.json        ← auto-created on first boot
    ├── mp3/
    │   ├── song1.mp3
    │   └── subfolder/
    │       └── song2.mp3
    └── m3u/
        ├── rock.m3u
        └── jazz.m3u
```

> **Migration note:** previous versions used `/MP3/`, `/M3U/`, `/config/settings.json`.  
> Move your files to the new paths above before first boot with v2.1 firmware.

### Example M3U playlist (`/melody_machine/m3u/stations.m3u`)

```
#EXTM3U
#EXTINF:-1,My Radio Station
http://stream.example.com/radio

#EXTINF:-1,Another Station
http://other.example.com/stream.mp3
```

### Adding WiFi networks

Edit `/melody_machine/settings.json` on the SD card (or use the WiFi screen in settings):

```json
{
  "wifi": {
    "networks": [
      { "ssid": "MyNetwork", "pass": "mypassword" }
    ]
  }
}
```

---

## Flashing Pre-built Firmware

Download `melody_machine.bin` from the [Releases](../../releases/tag/v0.1) page and flash with `esptool.py`:

```bash
esptool.py --chip esp32s3 --port COM3 --baud 921600 \
  write_flash 0x10000 melody_machine.bin
```

Or use [ESP Flash Download Tool](https://www.espressif.com/en/support/download/other-tools) (Windows) — flash at address `0x10000`.

---

## GUI Flasher (One File)

Use `melody_flasher.py` from the project root for a graphical flashing flow.

### What it does

- Auto-detects serial devices (COM ports)
- Auto-selects firmware from root (`melody_machine.bin` by default)
- Lets you browse and choose any `.bin` manually
- Shows live flashing logs and a progress bar

### Run

```bash
python melody_flasher.py
```

### Notes

- The script is a single file and can stay in the root next to `melody_machine.bin`.
- It flashes the selected firmware to address `0x10000` for ESP32-S3.
- `pyserial` is optional (recommended for better port detection): `pip install pyserial`

### Screenshot

![GUI Flasher](img/flasher_ui.png)

---

## Building from Source

### Requirements

- [arduino-cli](https://arduino.github.io/arduino-cli/)
- ESP32 Arduino core (`esp32:esp32`) with LilyGO board support
- Python 3 with Pillow (`pip install pillow`) — for splash image generation
- Libraries (see `about.md` for the full list):
  - LilyGoLib, ESP8266Audio (patched), lvgl 9.3.0, RadioLib, XPowersLib, SensorLib, Adafruit TCA8418

### Build

```bash
bash build.sh
```

Output: `melody_machine.bin` in the project root.

### Flash

```bash
arduino-cli upload \
  --fqbn "esp32:esp32:tlora_pager:Revision=Radio_SX1262,CDCOnBoot=default,PartitionScheme=app3M_fat9M_16MB" \
  --port COM3 \
  --input-dir build/
```

---

## Architecture

```
Core 1 (Arduino loop)          Core 0 (FreeRTOS)
─────────────────────          ─────────────────
LVGL timer handler             audioTask (priority 5)
uiManagerLoop()                  │
wifiManagerLoop()                ├─ MP3:   SD → ID3 → Helix → EspAudioOutput → ES8311
                                 └─ Radio: ICY stream → 32KB buffer → Helix → ES8311
```

Audio commands flow from UI → command queue → `audioTask`. Playback state (`state`, `elapsed`, `duration`) flows back via volatile globals.

See [about.md](about.md) for full architecture details including the ESP-IDF 5.x I2S driver conflict fix.

---

## License

MIT — see [LICENSE](LICENSE)
