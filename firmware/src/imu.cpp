#include "imu.h"
#include "display_cfg.h"
#include <Arduino.h>
#include <math.h>

// Tilt-only gesture detector. Two independent state machines, one per
// axis. A gesture fires the instant the chip crosses the threshold —
// no hold time. The axis must return to "roughly resting" before it
// can fire again.
//
// Key trick: the chip is mounted at a fixed angle inside its housing
// (the desk wedge tips the whole thing forward ~30-45°), so the raw
// `ax` / `ay` values at rest are *not* zero. We track a slow IIR
// baseline that follows the rest orientation, and measure tilts
// relative to *that* baseline. The baseline only adapts while the
// chip is near-rest, so the user can hold a tilt indefinitely without
// it being absorbed into the baseline.
//
// ROLL  (chip's X axis) → toggle USAGE↔SPLASH
// PITCH (Y axis)        → cycle Claude Code sessions
//
// Threshold = sin(30°) = 0.50 g. Re-arm below ~sin(12°) ≈ 0.20 g.

#define IMU_POLL_MS             10
#define TILT_AXIS_THRESH        0.50f     // sin(30°)
#define TILT_AXIS_NEUTRAL       0.20f     // ~sin(12°)
// Slow IIR that follows the rest orientation. α=0.002 at 100 Hz →
// ~5 s time constant. Plenty of room for the user to hold a tilt
// without it being pulled into the baseline.
#define BASELINE_ALPHA          0.002f
// Only update the baseline while the chip is near-rest, so a held
// tilt doesn't drag the baseline along with it.
#define BASELINE_UPDATE_BAND    0.30f

static bool      imu_ok = false;
static uint32_t  last_poll_ms = 0;

// Resting-orientation baseline. Initialised on the first sample,
// updated slowly thereafter.
static bool   baseline_init = false;
static float  bx = 0.0f, by = 0.0f;

struct tilt_axis_t {
    bool      ready;           // false until axis returns to neutral
    bool      pos_latched;
    bool      neg_latched;
};

static tilt_axis_t roll  = { true, false, false };
static tilt_axis_t pitch = { true, false, false };

void imu_init(void) {
    if (!imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        Serial.println("QMI8658 init failed");
        return;
    }
    imu.configAccelerometer(
        SensorQMI8658::ACC_RANGE_4G,
        SensorQMI8658::ACC_ODR_LOWPOWER_128Hz,
        SensorQMI8658::LPF_MODE_3);
    imu.enableAccelerometer();
    imu_ok = true;
    Serial.println("QMI8658 init OK (relative-tilt, 42°)");
}

static void update_axis(tilt_axis_t& s, float v, const char* label) {
    if (fabsf(v) < TILT_AXIS_NEUTRAL) s.ready = true;
    if (!s.ready) return;

    if (v >  TILT_AXIS_THRESH) {
        s.pos_latched = true;
        s.ready       = false;
        Serial.printf("[imu] tilt %s + (rel=%.2f)\n", label, v);
    } else if (v < -TILT_AXIS_THRESH) {
        s.neg_latched = true;
        s.ready       = false;
        Serial.printf("[imu] tilt %s - (rel=%.2f)\n", label, v);
    }
}

void imu_tick(void) {
    if (!imu_ok) return;

    uint32_t now = millis();
    if (now - last_poll_ms < IMU_POLL_MS) return;
    last_poll_ms = now;

    float ax, ay, az;
    if (!imu.getAccelerometer(ax, ay, az)) return;
    (void)az;

    if (!baseline_init) {
        bx = ax; by = ay;
        baseline_init = true;
        return;
    }

    // Tilt relative to the rest baseline.
    float drx = ax - bx;
    float dry = ay - by;

    // Only let the baseline adapt while we're close to its current
    // value. Holding a tilt freezes the baseline, so the tilt signal
    // stays large and the gesture stays "armed" or "fired" cleanly.
    if (fabsf(drx) < BASELINE_UPDATE_BAND)
        bx = (1.0f - BASELINE_ALPHA) * bx + BASELINE_ALPHA * ax;
    if (fabsf(dry) < BASELINE_UPDATE_BAND)
        by = (1.0f - BASELINE_ALPHA) * by + BASELINE_ALPHA * ay;

    update_axis(roll,  drx, "ROLL");
    update_axis(pitch, dry, "PITCH");
}

bool imu_consume_tilt_left(void) {
    if (!roll.neg_latched) return false;
    roll.neg_latched = false;
    return true;
}
bool imu_consume_tilt_right(void) {
    if (!roll.pos_latched) return false;
    roll.pos_latched = false;
    return true;
}
bool imu_consume_tilt_forward(void) {
    if (!pitch.pos_latched) return false;
    pitch.pos_latched = false;
    return true;
}
bool imu_consume_tilt_back(void) {
    if (!pitch.neg_latched) return false;
    pitch.neg_latched = false;
    return true;
}
