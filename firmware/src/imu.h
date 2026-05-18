#pragma once
#include <stdint.h>

void imu_init(void);
void imu_tick(void);

// Returns and clears a one-shot "double-shake" event. The user has to
// jostle the device twice in quick succession (~200-900 ms apart) for
// this to fire. Polling clears the latch so each event is consumed once.
bool imu_consume_double_shake(void);
