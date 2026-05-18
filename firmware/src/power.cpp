#include "power.h"
#include <Arduino.h>

// ESP32-S3-LCD-1.3 has no PMU and no battery gauge — the board runs from
// USB only. These stubs preserve the upstream API surface so the rest of
// the firmware compiles untouched.
//
// power_pwr_pressed() is the legacy "middle button" hook from the AMOLED
// board; on this board, screen cycling is handled directly in main.cpp
// against the BOOT button (GPIO 0), so this always returns false.

void power_init(void)        {}
void power_tick(void)        {}
int  power_battery_pct(void) { return -1; }
bool power_is_charging(void) { return false; }
bool power_pwr_pressed(void) { return false; }
