#include "theme_manager.h"
#include "settings_store.h"

static const Theme _themes[6] = {
    {
        // TH_DEFAULT — dark green (original)
        "Default",
        lv_color_hex(0x000000),  // bg
        lv_color_hex(0x000000),  // topbar
        lv_color_hex(0x000000),  // panel
        lv_color_hex(0x000000),  // hdr
        lv_color_hex(0x141414),  // border
        lv_color_hex(0x1c1c1c),  // divider
        lv_color_hex(0x18ff78),  // accent
        lv_color_hex(0x888888),  // text
        lv_color_hex(0x2e2e2e),  // muted
        lv_color_hex(0x252525),  // dim
        lv_color_hex(0x0b0b00),  // hilight
        lv_color_hex(0xf5ff62),  // warn
        lv_color_hex(0xff8000),  // vol
        lv_color_hex(0x000000),  // playrow
        &lv_font_montserrat_16,
        &lv_font_montserrat_20,
        &lv_font_montserrat_14,
        &lv_font_montserrat_12,
        &lv_font_montserrat_10,
    },
    {
        // TH_WIN95 — Windows 96
        "Windows 96",
        lv_color_hex(0xC0C0C0),  // bg
        lv_color_hex(0x000080),  // topbar (navy)
        lv_color_hex(0xC0C0C0),  // panel
        lv_color_hex(0x808080),  // hdr
        lv_color_hex(0x808080),  // border
        lv_color_hex(0x808080),  // divider
        lv_color_hex(0x000080),  // accent (navy)
        lv_color_hex(0x000000),  // text
        lv_color_hex(0x808080),  // muted
        lv_color_hex(0xA0A0A0),  // dim
        lv_color_hex(0x8080FF),  // hilight — light blue so black text is readable
        lv_color_hex(0xFFFF00),  // warn
        lv_color_hex(0xFF0000),  // vol
        lv_color_hex(0xC0C0C0),  // playrow
        &lv_font_montserrat_16,
        &lv_font_montserrat_20,
        &lv_font_montserrat_14,
        &lv_font_montserrat_12,
        &lv_font_montserrat_10,
    },
    {
        // TH_FANTASY — dark purple / pink
        "Fantasy",
        lv_color_hex(0x1a0a2e),  // bg
        lv_color_hex(0x2d1b5e),  // topbar
        lv_color_hex(0x1a0a2e),  // panel
        lv_color_hex(0x2d1b5e),  // hdr
        lv_color_hex(0x3d2a6e),  // border
        lv_color_hex(0x3d2a6e),  // divider
        lv_color_hex(0xff79c6),  // accent (pink)
        lv_color_hex(0xbd93f9),  // text (lavender)
        lv_color_hex(0x6272a4),  // muted
        lv_color_hex(0x44475a),  // dim
        lv_color_hex(0x44475a),  // hilight
        lv_color_hex(0xffb86c),  // warn (orange)
        lv_color_hex(0xff5555),  // vol (red)
        lv_color_hex(0x1a0a2e),  // playrow
        &lv_font_montserrat_16,
        &lv_font_montserrat_20,
        &lv_font_montserrat_14,
        &lv_font_montserrat_12,
        &lv_font_montserrat_10,
    },
    {
        // TH_LINUX — CRT green terminal
        "Linux Console",
        lv_color_hex(0x000000),  // bg
        lv_color_hex(0x000000),  // topbar
        lv_color_hex(0x000000),  // panel
        lv_color_hex(0x000000),  // hdr
        lv_color_hex(0x003300),  // border
        lv_color_hex(0x003300),  // divider
        lv_color_hex(0x00ff00),  // accent (CRT green)
        lv_color_hex(0x00cc00),  // text
        lv_color_hex(0x007700),  // muted
        lv_color_hex(0x003300),  // dim
        lv_color_hex(0x001a00),  // hilight
        lv_color_hex(0xffff00),  // warn
        lv_color_hex(0xff8800),  // vol
        lv_color_hex(0x000000),  // playrow
        &lv_font_montserrat_16,
        &lv_font_montserrat_20,
        &lv_font_montserrat_14,
        &lv_font_montserrat_12,
        &lv_font_montserrat_10,
    },
    {
        // TH_FONY — dark navy, teal + white
        "Fony",
        lv_color_hex(0x171C2B),  // bg
        lv_color_hex(0x0F1320),  // topbar (darker navy)
        lv_color_hex(0x1E2438),  // panel (slightly lighter)
        lv_color_hex(0x252D42),  // hdr
        lv_color_hex(0x2A3350),  // border
        lv_color_hex(0x2A3350),  // divider
        lv_color_hex(0x00F2B8),  // accent (teal)
        lv_color_hex(0xFFFFFF),  // text (white)
        lv_color_hex(0x8899BB),  // muted (blue-grey)
        lv_color_hex(0xC0CCDD),  // dim (light blue-grey — readable on dark panel)
        lv_color_hex(0x00C49A),  // hilight (darker teal, white text readable)
        lv_color_hex(0x00F2B8),  // warn (teal — only two colors allowed)
        lv_color_hex(0x00F2B8),  // vol
        lv_color_hex(0x1E2438),  // playrow
        &lv_font_montserrat_16,
        &lv_font_montserrat_20,
        &lv_font_montserrat_14,
        &lv_font_montserrat_12,
        &lv_font_montserrat_10,
    },
    {
        // TH_MONO — black background, white everything
        "Mono",
        lv_color_hex(0x000000),  // bg
        lv_color_hex(0x000000),  // topbar
        lv_color_hex(0x000000),  // panel
        lv_color_hex(0x111111),  // hdr
        lv_color_hex(0x333333),  // border
        lv_color_hex(0x222222),  // divider
        lv_color_hex(0xFFFFFF),  // accent (white)
        lv_color_hex(0xFFFFFF),  // text (white)
        lv_color_hex(0x888888),  // muted (mid-grey)
        lv_color_hex(0xCCCCCC),  // dim (light grey — readable on black)
        lv_color_hex(0x444444),  // hilight (dark grey, white text readable)
        lv_color_hex(0xFFFFFF),  // warn (white)
        lv_color_hex(0xFFFFFF),  // vol (white)
        lv_color_hex(0x000000),  // playrow
        &lv_font_montserrat_16,
        &lv_font_montserrat_20,
        &lv_font_montserrat_14,
        &lv_font_montserrat_12,
        &lv_font_montserrat_10,
    },
};

static ThemeId _activeId = TH_DEFAULT;
static bool    _needsRedraw = false;

void themeManagerInit() {
    int id = settingsGetInt("theme.active", 0);
    if (id < 0 || id >= 6) id = 0;
    _activeId = (ThemeId)id;
    _needsRedraw = false;
}

const Theme* themeGet() {
    return &_themes[_activeId];
}

ThemeId themeGetActiveId() {
    return _activeId;
}

void themeSetActive(ThemeId id) {
    if (id < 0 || id >= 6) id = TH_DEFAULT;
    if (id == _activeId) return;
    _activeId = id;
    settingsPutInt("theme.active", (int)id);
    settingsSave();
    _needsRedraw = true;
}

bool themeManagerNeedsRedraw() {
    return _needsRedraw;
}

void themeManagerClearRedraw() {
    _needsRedraw = false;
}

int themeCount() {
    return 6;
}

const char* themeName(ThemeId id) {
    if (id < 0 || id >= 6) return "Unknown";
    return _themes[id].name;
}
