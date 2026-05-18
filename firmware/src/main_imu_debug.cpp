// Standalone firmware that displays raw IMU readings on the LCD.
// Built as a separate platformio env (`pio run -e imu_debug -t upload`)
// — replaces the main app entirely so you can sit the device on its
// wedge and read what the accelerometer actually reports.
//
// Lines on screen:
//   ax / ay / az      raw g, three decimals
//   roll / pitch      asin() of the raw axis in degrees
//   dR  / dP          same minus the hardcoded REST_*_DEG baseline;
//                     turns green + appends '*' once past 30°.
//
// The prism cube rotates the image 180° from the viewer's perspective,
// so we draw into a rotated offscreen Canvas and push that to the LCD
// in one shot per frame — fixes both the flip and the fillScreen
// flash that the naive direct-write approach caused.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <SensorQMI8658.hpp>
#include <math.h>
#include <esp_heap_caps.h>
#include "display_cfg.h"

#define REST_ROLL_DEG    0.0f
#define REST_PITCH_DEG   23.0f
#define TILT_THRESH_DEG  30.0f

Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, GFX_NOT_DEFINED);
Arduino_ST7789  *panel = new Arduino_ST7789(bus, LCD_RST, 0 /* rotation */,
                                            true /* IPS */, LCD_WIDTH, LCD_HEIGHT,
                                            0, 0, 0, 0);
// Offscreen 240×240 canvas; pixels are pushed to the panel with a
// manual Y-flip in flush_flipped() — same approach as the main
// firmware's my_flush_cb. Canvas's own rotation modes (1/2/3) all
// over-flip (180° applies *both* X and Y), so we keep the canvas at
// rotation=0 and do the single-axis flip ourselves.
Arduino_Canvas  *screen = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel, 0, 0, 0);
SensorQMI8658    imu;

static void flush_flipped(void) {
    uint16_t* fb = screen->getFramebuffer();
    if (!fb) return;
    for (int y = 0; y < LCD_HEIGHT; y++) {
        int src_y = LCD_HEIGHT - 1 - y;
        panel->draw16bitRGBBitmap(0, y, &fb[src_y * LCD_WIDTH], LCD_WIDTH, 1);
    }
}

static inline float g_to_deg(float v) {
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    return asinf(v) * (180.0f / (float)M_PI);
}

void setup(void) {
    Serial.begin(115200);
    delay(200);

    Wire.begin(IIC_SDA, IIC_SCL);

    panel->begin();
    panel->fillScreen(0x0000);
    panel->invertDisplay(false);
    screen->begin();
    screen->setTextWrap(false);

    if (!imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        screen->fillScreen(0x0000);
        screen->setCursor(8, 8);
        screen->setTextColor(0xF800);
        screen->setTextSize(2);
        screen->print("QMI8658 init FAIL");
        screen->flush();
        Serial.println("QMI8658 init failed");
        while (true) delay(1000);
    }
    imu.configAccelerometer(
        SensorQMI8658::ACC_RANGE_4G,
        SensorQMI8658::ACC_ODR_LOWPOWER_128Hz,
        SensorQMI8658::LPF_MODE_3);
    imu.enableAccelerometer();
    Serial.println("imu_debug ready");
}

static uint32_t last_draw_ms = 0;

void loop(void) {
    uint32_t now = millis();
    if (now - last_draw_ms < 100) {
        delay(5);
        return;
    }
    last_draw_ms = now;

    float ax, ay, az;
    if (!imu.getAccelerometer(ax, ay, az)) return;

    float roll_deg  = g_to_deg(ax);
    float pitch_deg = g_to_deg(ay);
    float rel_roll  = roll_deg  - REST_ROLL_DEG;
    float rel_pitch = pitch_deg - REST_PITCH_DEG;

    screen->fillScreen(0x0000);
    screen->setTextSize(2);

    screen->setTextColor(0xFFFF);
    screen->setCursor(8, 10);
    screen->printf("ax %+0.3f", ax);
    screen->setCursor(8, 32);
    screen->printf("ay %+0.3f", ay);
    screen->setCursor(8, 54);
    screen->printf("az %+0.3f", az);

    screen->setTextColor(0xFFE0);   // yellow
    screen->setCursor(8, 88);
    screen->printf("roll  %+6.1f", roll_deg);
    screen->setCursor(8, 110);
    screen->printf("pitch %+6.1f", pitch_deg);

    bool past_roll  = fabsf(rel_roll)  > TILT_THRESH_DEG;
    bool past_pitch = fabsf(rel_pitch) > TILT_THRESH_DEG;
    screen->setTextColor(past_roll  ? 0x07E0 : 0x07FF);
    screen->setCursor(8, 144);
    screen->printf("dR %+6.1f%c", rel_roll,  past_roll  ? '*' : ' ');
    screen->setTextColor(past_pitch ? 0x07E0 : 0x07FF);
    screen->setCursor(8, 166);
    screen->printf("dP %+6.1f%c", rel_pitch, past_pitch ? '*' : ' ');

    screen->setTextSize(1);
    screen->setTextColor(0x8410);
    screen->setCursor(8, 200);
    screen->printf("rest %.1f/%.1f  thresh %.0f deg",
                REST_ROLL_DEG, REST_PITCH_DEG, TILT_THRESH_DEG);
    screen->setCursor(8, 212);
    screen->print("on-wedge view (prism-corrected)");

    // Single push to the panel via the Y-flipped writer — no
    // fillScreen flash, no flicker.
    flush_flipped();
}
