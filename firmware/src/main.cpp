#include <Arduino.h>
#include <lvgl.h>
#include <ArduinoJson.h>

#include "display_cfg.h"
#include "data.h"
#include "ui.h"
#include "ble.h"
#include "power.h"
#include "wifi_cfg.h"
#include "imu.h"

// ---- Hardware objects ----
// Arduino_GFX provides setRotation() for ST7789 via MADCTL, so we don't
// need the CPU strip-rotation that the AMOLED port had to do for CO5300.
Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, GFX_NOT_DEFINED);
Arduino_ST7789  *gfx = new Arduino_ST7789(bus, LCD_RST, 0 /* rotation */,
                                          true /* IPS */, LCD_WIDTH, LCD_HEIGHT,
                                          0, 0, 0, 0);
SensorQMI8658    imu;

static UsageData usage = {};

// ---- LVGL draw buffers ----
// Two PSRAM-backed partial render buffers. 40-line strips for 240-wide
// rows = 240*40*2 = 19200 bytes each — fits comfortably in PSRAM.
#define BUF_LINES 40
static uint16_t *buf1 = nullptr;
static uint16_t *buf2 = nullptr;

static uint32_t my_tick(void) {
    return millis();
}

// Prism-cube optics: the cube on top reflects the image with a vertical
// flip from the viewer's perspective on this build, so we pre-mirror each
// strip on the Y axis before pushing it. (Waveshare's demo flips X instead;
// that's correct for *their* prism orientation — this enclosure is rotated
// 90°.)
//
// Per strip we swap row N with row (h-1-N) and then re-anchor the strip
// on the Y axis so its panel position flips too. Cheap: 40 rows × 240
// uint16_t per swap → memcpy in the ESP-S3 cache, well under a millisecond.
static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    uint16_t* src = (uint16_t*)px_map;
    static uint16_t scratch_row[LCD_WIDTH];
    for (int32_t a = 0, b = h - 1; a < b; a++, b--) {
        uint16_t* ra = &src[a * w];
        uint16_t* rb = &src[b * w];
        memcpy(scratch_row, ra, w * 2);
        memcpy(ra,          rb, w * 2);
        memcpy(rb, scratch_row, w * 2);
    }
    int32_t dst_y = LCD_HEIGHT - area->y2 - 1;
    gfx->draw16bitRGBBitmap(area->x1, dst_y, src, w, h);
    lv_display_flush_ready(disp);
}

static bool parse_usage_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    const char* mode = doc["mode"] | "subscription";
    if (strcmp(mode, "api") == 0) {
        out->mode = MODE_API;
        out->tokens_used         = (uint64_t)(doc["tu"]  | (uint64_t)0);
        out->tokens_quota        = (uint64_t)(doc["tq"]  | (uint64_t)0);
        out->dollars_spent_cents = (int)(doc["ds"]  | 0);
        out->dollars_budget_cents= (int)(doc["db"]  | 0);
        out->burn_per_min        = (int)(doc["bm"]  | 0);
        out->burn_pct_change     = (int)(doc["bc"]  | 0);
        out->api_reset_mins      = (int)(doc["ar"]  | -1);
        strlcpy(out->period_label, doc["pl"] | "", sizeof(out->period_label));
        out->session_pct = 0.0f;
        out->session_reset_mins = -1;
        out->weekly_pct  = 0.0f;
        out->weekly_reset_mins  = -1;
    } else {
        out->mode = MODE_SUBSCRIPTION;
        out->session_pct        = doc["s"]  | 0.0f;
        out->session_reset_mins = doc["sr"] | -1;
        out->weekly_pct         = doc["w"]  | 0.0f;
        out->weekly_reset_mins  = doc["wr"] | -1;
        out->tokens_used = 0;
        out->tokens_quota = 0;
        out->dollars_spent_cents = 0;
        out->dollars_budget_cents = 0;
        out->burn_per_min = 0;
        out->burn_pct_change = 0;
        out->api_reset_mins = -1;
        out->period_label[0] = '\0';
    }
    out->ctx_used = (uint64_t)(doc["cu"] | (uint64_t)0);
    out->ctx_max  = (uint64_t)(doc["cm"] | (uint64_t)0);
    out->session_active = ((int)(doc["act"] | 0)) != 0;
    out->task_tokens  = (uint32_t)(doc["tt"] | (uint32_t)0);
    out->task_seconds = (uint32_t)(doc["ts"] | (uint32_t)0);
    strlcpy(out->model_label, doc["ml"] | "", sizeof(out->model_label));
    strlcpy(out->phase, doc["ph"] | "working", sizeof(out->phase));
    out->session_count = (uint8_t)(doc["sn"] | 1);
    out->session_index = (uint8_t)(doc["si"] | 0);
    out->sessions_listed = 0;
    JsonArrayConst sl = doc["sl"].as<JsonArrayConst>();
    if (!sl.isNull()) {
        for (JsonObjectConst row : sl) {
            if (out->sessions_listed >= 6) break;
            auto& dst = out->sessions[out->sessions_listed++];
            strlcpy(dst.model_label, row["ml"] | "", sizeof(dst.model_label));
            dst.ctx_pct     = (uint8_t)(row["cp"] | 0);
            dst.active      = ((int)(row["ac"] | 0)) != 0;
            dst.task_tokens = (uint32_t)(row["tt"] | (uint32_t)0);
        }
    }
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->ok = doc["ok"] | false;
    out->valid = true;
    return true;
}

#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int  cmd_pos = 0;

static void send_screenshot(void) {
    const uint32_t w = LCD_WIDTH, h = LCD_HEIGHT;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size  = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) { Serial.println("SCREENSHOT_ERR"); return; }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);
    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(),
                                                   LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) { heap_caps_free(sbuf); Serial.println("SCREENSHOT_ERR"); return; }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
                  (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
}

static void check_serial_cmd(void) {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

void setup(void) {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    // I2C for the QMI8658 IMU
    Wire.begin(IIC_SDA, IIC_SCL);

    // Display init (ST7789 240x240 via SPI)
    gfx->begin();
    gfx->fillScreen(0x0000);
    // Arduino_GFX already sends INVON during begin() because we pass ips=true.
    // Explicitly inverting again here flipped the colors a second time, which
    // is what produced the negative-image look on this Waveshare panel.
    gfx->invertDisplay(false);

    // IMU left uninitialized on purpose. The board is mounted under a
    // fixed prism cube — accelerometer-driven auto-rotation would just
    // scramble the image as you nudge the desk. Pin 47/48 stay free for
    // future direct use of the QMI8658 if needed.

    // LVGL
    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    buf2 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);

    lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, LCD_WIDTH * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    // BLE peripheral (daemon link)
    ble_init();

    // IMU — used here for shake-to-cycle / shake-to-switch gestures.
    imu_init();

    // WiFi — connect with hardcoded creds (see secrets.h.example) and then
    // sit idle. Not used for data on this build; provisioned for future use
    // and so the radio is up for NTP / OTA if you want them later.
    wifi_cfg_begin();

    // No physical buttons on this enclosure — screen cycling happens via
    // touch only. BLE advertises as a Generic Display, no HID profile.

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_show_screen(SCREEN_USAGE);

    Serial.println("Dashboard ready, waiting for data on BLE...");
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

void loop(void) {
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    imu_tick();
    if (imu_consume_double_shake()) ui_handle_shake();

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    check_serial_cmd();

    if (ble_has_data()) {
        if (parse_usage_json(ble_get_data(), &usage)) {
            ui_payload_received();
            ui_update(&usage);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
