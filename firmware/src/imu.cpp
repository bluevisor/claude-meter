#include "imu.h"
#include "display_cfg.h"
#include <Arduino.h>
#include <math.h>

// Tilt-only gesture detector. Two independent state machines, one per
// axis. Fires the instant the chip crosses the threshold — no hold
// time. The axis must return to "roughly resting" before it can fire
// again.
//
// We threshold in *degrees*, not raw g. The chip is mounted at a
// fixed ~23° pitch (the desk wedge), so its resting `ay` reads ~0.38
// instead of 0. A flat g-threshold then needs ~40° of physical tilt
// on pitch to reach (sin is non-linear) while roll triggers at 30°.
// Converting via asin → degrees and subtracting a hardcoded rest
// offset normalises both axes so 30° feels like 30° in either.
//
// Adjust REST_*_DEG if the mount geometry ever changes. To inspect
// the live raw values, send the serial command "imu" — the firmware
// will stream `ax/ay/az g, roll/pitch deg` until you send "stop".
//
// ROLL  (chip's X axis) → toggle USAGE↔SPLASH
// PITCH (Y axis)        → cycle Claude Code sessions

#define IMU_POLL_MS             10
#define REST_ROLL_DEG           0.0f
#define REST_PITCH_DEG          23.0f     // wedge tips the chip forward
#define TILT_THRESH_DEG         30.0f     // gesture fires past this angle
#define TILT_NEUTRAL_DEG        12.0f     // re-arm below this angle

static bool      imu_ok = false;
static uint32_t  last_poll_ms = 0;

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
    Serial.println("QMI8658 init OK (degree-tilt, 30° threshold)");
}

static inline float g_to_deg(float v) {
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    return asinf(v) * (180.0f / (float)M_PI);
}

static void update_axis(tilt_axis_t& s, float rel_deg, const char* label) {
    if (fabsf(rel_deg) < TILT_NEUTRAL_DEG) s.ready = true;
    if (!s.ready) return;

    if (rel_deg >  TILT_THRESH_DEG) {
        s.pos_latched = true;
        s.ready       = false;
        Serial.printf("[imu] tilt %s + (%.1f deg)\n", label, rel_deg);
    } else if (rel_deg < -TILT_THRESH_DEG) {
        s.neg_latched = true;
        s.ready       = false;
        Serial.printf("[imu] tilt %s - (%.1f deg)\n", label, rel_deg);
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

    float roll_rel  = g_to_deg(ax) - REST_ROLL_DEG;
    float pitch_rel = g_to_deg(ay) - REST_PITCH_DEG;

    update_axis(roll,  roll_rel,  "ROLL");
    update_axis(pitch, pitch_rel, "PITCH");
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
