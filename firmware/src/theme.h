#pragma once
#include <lvgl.h>

// Palette imported from the Claude Design handoff (Clawd Meter.html).
// AMOLED background is pure black so unlit pixels stay off; accents are
// pushed toward higher saturation so the warm Claude hues read vividly
// against the void.

#define THEME_BG           lv_color_hex(0x000000)  // pure black (AMOLED off)
#define THEME_TEXT         lv_color_hex(0xF5F4F0)  // primary text
#define THEME_TEXT_MUTED   lv_color_hex(0xA39B8C)  // sub-labels / "Resets in"
#define THEME_TEXT_VDIM    lv_color_hex(0x6E6A64)  // unit tags ("k", "/200k")
#define THEME_HAIRLINE     lv_color_hex(0x2A2622)  // row divider
#define THEME_BAR_BG       lv_color_hex(0x1A1815)  // unfilled bar track

#define THEME_ACCENT       lv_color_hex(0xF54A10)  // brand terra-cotta (max sat)
#define THEME_ACCENT_DEEP  lv_color_hex(0xB03808)  // "%" sigil shadow / segment hot
#define THEME_AMBER        lv_color_hex(0xFFB000)  // weekly / warn (pure golden)
#define THEME_AMBER_DEEP   lv_color_hex(0xB07000)
#define THEME_GREEN        lv_color_hex(0x00B070)  // context bar (vivid teal)

// Aliases used by the old (pre-redesign) UI code so legacy call sites still
// compile. The redesign uses the THEME_* names directly.
#define THEME_PANEL        THEME_BG
#define THEME_DIM          THEME_TEXT_MUTED
#define THEME_RED          lv_color_hex(0xE61500)
