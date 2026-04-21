#pragma once
#include <Arduino.h>
#include <lvgl.h>

enum ThemeId { TH_DEFAULT = 0, TH_WIN95 = 1, TH_FANTASY = 2, TH_LINUX = 3 };

struct Theme {
    const char*      name;
    lv_color_t       bg;
    lv_color_t       topbar;
    lv_color_t       panel;
    lv_color_t       hdr;
    lv_color_t       border;
    lv_color_t       divider;
    lv_color_t       accent;
    lv_color_t       text;
    lv_color_t       muted;
    lv_color_t       dim;
    lv_color_t       hilight;
    lv_color_t       warn;
    lv_color_t       vol;
    lv_color_t       playrow;
    const lv_font_t* fontLarge;
    const lv_font_t* fontTrack;
    const lv_font_t* fontTitle;
    const lv_font_t* fontBody;
    const lv_font_t* fontSmall;
};

void           themeManagerInit();
const Theme*   themeGet();
ThemeId        themeGetActiveId();
void           themeSetActive(ThemeId id);
bool           themeManagerNeedsRedraw();
void           themeManagerClearRedraw();
int            themeCount();
const char*    themeName(ThemeId id);
