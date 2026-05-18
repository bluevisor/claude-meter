#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_OVERVIEW,
    SCREEN_BLUETOOTH,
    SCREEN_COUNT,
};

// Routed from main.cpp on a double-shake gesture: cycles screens, or on
// the overview screen advances the focused session via BLE.
void ui_handle_shake(void);

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_cycle_screen(void);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
