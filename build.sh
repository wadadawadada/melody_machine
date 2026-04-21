#!/usr/bin/env bash
# Build script for Melody Machine firmware (LilyGO T-LoRa Pager SX1262)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BB_WALLET="$(dirname "$SCRIPT_DIR")/blackbox_wallet"
CLI="$BB_WALLET/arduino-cli-extracted/arduino-cli.exe"
CONFIG="$SCRIPT_DIR/arduino-cli-config.yml"
SKETCH="$SCRIPT_DIR/melody_machine"
BUILD_DIR="$SCRIPT_DIR/build"
OUTPUT="$SCRIPT_DIR/melody_machine.bin"

FQBN="esp32:esp32:tlora_pager:Revision=Radio_SX1262,CDCOnBoot=default,PartitionScheme=app3M_fat9M_16MB"

echo "================================================================"
echo "  Melody Machine Firmware Builder"
echo "  Board: LilyGo T-LoRa Pager (SX1262)"
echo "================================================================"

# Regenerate splash image header
echo "[BUILD] Converting splash image..."
python -c "
from PIL import Image
import os

try:
    img = Image.open('$SCRIPT_DIR/img/1.png').convert('RGB').resize((480, 222), Image.LANCZOS)
    pixels = list(img.getdata())
    lines = ['#pragma once', '#include <lvgl.h>', '', 'static const uint8_t img_splash_data[] = {']
    row = ''
    for i, (r, g, b) in enumerate(pixels):
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        lo = rgb565 & 0xFF
        hi = (rgb565 >> 8) & 0xFF
        row += '0x{:02X},0x{:02X},'.format(lo, hi)
        if (i+1) % 16 == 0:
            lines.append(row)
            row = ''
    if row:
        lines.append(row)
    lines += [
        '};', '',
        'static const lv_image_dsc_t img_splash = {',
        '    .header = { .cf = LV_COLOR_FORMAT_RGB565, .w = 480, .h = 222, .stride = 960 },',
        '    .data_size = {},'.format(480*222*2),
        '    .data = img_splash_data,',
        '};',
    ]
    with open('$SCRIPT_DIR/melody_machine/img_splash.h', 'w') as f:
        f.write('\n'.join(lines))
    print('[BUILD] img_splash.h regenerated')
except Exception as e:
    print('[BUILD] splash regen skipped:', e)
" 2>&1 || true

if [[ "$*" == *"--clean"* ]]; then
    echo "[BUILD] --clean: wiping build cache..."
    rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"

LVGL="$BB_WALLET/local_libs/lvgl-9.3.0"
RADIOLIB="$BB_WALLET/local_libs/RadioLib-7.4.0"

echo "[BUILD] Compiling sketch (live output)..."
"$CLI" compile \
    --config-file "$CONFIG" \
    --fqbn "$FQBN" \
    --build-path "$BUILD_DIR" \
    --warnings default \
    --verbose \
    --library "$RADIOLIB" \
    --library "$LVGL" \
    "$SKETCH"

echo "[BUILD] Locating .bin file..."
BIN_FILE=$(find "$BUILD_DIR" -name "*.bin" | grep -v bootloader | grep -v partitions | head -1)

if [ -z "$BIN_FILE" ]; then
    echo "[ERROR] No .bin file found in $BUILD_DIR"
    ls -la "$BUILD_DIR"
    exit 1
fi

echo "[BUILD] Found: $BIN_FILE"
cp "$BIN_FILE" "$OUTPUT"

echo ""
echo "================================================================"
echo "  SUCCESS: $OUTPUT"
SIZE=$(wc -c < "$OUTPUT")
echo "  Size: $SIZE bytes"
echo "================================================================"
