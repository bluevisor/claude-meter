#pragma once

// Hardcoded WiFi provisioning. The radio is brought up at boot in
// non-blocking fashion and then left idle — nothing else in the firmware
// depends on WiFi being connected. It's wired up so the credentials are
// in NVS / available for future use (NTP, OTA, direct API polling, ...).

void wifi_cfg_begin(void);
bool wifi_cfg_connected(void);
