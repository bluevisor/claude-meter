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

// Roll (left/right tilt) toggles USAGE↔SPLASH inside a single
// Claude Code session.
void ui_handle_tilt_left(void);
void ui_handle_tilt_right(void);

// Pitch (forward/back tilt) cycles between active Claude Code
// sessions. No-op when only one session is active.
void ui_handle_tilt_forward(void);
void ui_handle_tilt_back(void);

// Called from main.cpp the moment a payload is received from the
// daemon. Drives the stale-data watchdog.
void ui_payload_received(void);

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_cycle_screen(void);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
