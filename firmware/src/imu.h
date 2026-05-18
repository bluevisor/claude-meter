#pragma once
#include <stdint.h>

void imu_init(void);
void imu_tick(void);

// Tilt-only gesture API. Each consume returns + clears a one-shot
// latch. A gesture fires when the chip is held past ~42° in that axis
// for ~300 ms, and the axis must return to roughly level before it can
// fire again.
//
// ROLL  (chip X axis) → toggle USAGE↔SPLASH.
// PITCH (chip Y axis) → cycle Claude Code sessions.
bool imu_consume_tilt_left(void);
bool imu_consume_tilt_right(void);
bool imu_consume_tilt_forward(void);
bool imu_consume_tilt_back(void);
