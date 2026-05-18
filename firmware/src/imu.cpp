#include "imu.h"
#include "display_cfg.h"
#include <Arduino.h>
#include <math.h>

// Sample the accelerometer ~50 Hz and look for "shake" spikes. A single
// shake = magnitude jumps above SHAKE_HI then settles below SHAKE_LO.
// A double-shake = two such spikes within DOUBLE_WIN. The latch is
// consumed by imu_consume_double_shake().

#define IMU_POLL_MS        20      // 50 Hz
#define SHAKE_HI           1.45f   // |a| > 1.45 g triggers a spike
#define SHAKE_LO           1.10f   // need to drop below this between spikes
#define SPIKE_COOLDOWN_MS  120     // ignore further hi crossings for this long
#define DOUBLE_WIN_MIN_MS  200
#define DOUBLE_WIN_MAX_MS  900

static bool      imu_ok = false;
static uint32_t  last_poll_ms = 0;
static bool      armed = true;             // ready for next hi-crossing
static uint32_t  last_spike_ms = 0;
static uint32_t  prev_spike_ms = 0;
static bool      double_shake_latched = false;

void imu_init(void) {
    if (!imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        Serial.println("QMI8658 init failed");
        return;
    }
    imu.configAccelerometer(
        SensorQMI8658::ACC_RANGE_4G,
        SensorQMI8658::ACC_ODR_LOWPOWER_21Hz,
        SensorQMI8658::LPF_MODE_3);
    imu.enableAccelerometer();
    imu_ok = true;
    Serial.println("QMI8658 init OK (shake detector)");
}

void imu_tick(void) {
    if (!imu_ok) return;

    uint32_t now = millis();
    if (now - last_poll_ms < IMU_POLL_MS) return;
    last_poll_ms = now;

    float ax, ay, az;
    if (!imu.getAccelerometer(ax, ay, az)) return;

    float mag = sqrtf(ax * ax + ay * ay + az * az);

    if (armed && mag > SHAKE_HI && (now - last_spike_ms) > SPIKE_COOLDOWN_MS) {
        // Spike. Check whether it pairs with the prior one within the
        // double-shake window before recording.
        uint32_t gap = (last_spike_ms != 0) ? (now - last_spike_ms) : UINT32_MAX;
        prev_spike_ms = last_spike_ms;
        last_spike_ms = now;
        armed = false;
        if (gap >= DOUBLE_WIN_MIN_MS && gap <= DOUBLE_WIN_MAX_MS) {
            double_shake_latched = true;
            // Consume both spikes so a triple-shake doesn't double-fire.
            prev_spike_ms = 0;
            last_spike_ms = 0;
        }
    } else if (!armed && mag < SHAKE_LO) {
        armed = true;
    }
}

bool imu_consume_double_shake(void) {
    if (!double_shake_latched) return false;
    double_shake_latched = false;
    return true;
}
