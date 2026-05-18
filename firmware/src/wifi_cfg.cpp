#include "wifi_cfg.h"
#include <Arduino.h>
#include <WiFi.h>

#if __has_include("secrets.h")
  #include "secrets.h"
#endif

#ifndef WIFI_SSID
  // Build-time WiFi credentials. Real values belong in
  // firmware/src/secrets.h (gitignored — see secrets.h.example). If
  // secrets.h is absent the radio just spins on these placeholders and
  // fails to associate, which is fine because the daemon talks BLE.
  #define WIFI_SSID "your-ssid"
#endif
#ifndef WIFI_PASSWORD
  #define WIFI_PASSWORD "your-password"
#endif

static bool s_begun = false;

void wifi_cfg_begin(void) {
    if (s_begun) return;
    s_begun = true;

    // Station mode, autoreconnect on, sleep enabled so the BLE coexist
    // arbiter stays happy. We do NOT block waiting for join — the daemon
    // talks to us over BLE, so radio readiness isn't on the critical path.
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("WiFi: begin SSID=\"%s\" (non-blocking)\n", WIFI_SSID);
}

bool wifi_cfg_connected(void) {
    return WiFi.status() == WL_CONNECTED;
}
