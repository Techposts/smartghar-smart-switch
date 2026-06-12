// SmartGhar Smart Switch — Matter image (mode 2 of the two-image product).
// On/Off Plug-in Unit + Temperature Sensor endpoints over the same relay +
// safety hardware as the TankSync image. See docs/matter-image-design.md.
#pragma once

#include <esp_err.h>
#include <esp_matter.h>

// ── Board profile ─────────────────────────────────────────────────────────────
// Default = bare XIAO ESP32-C6 bench unit (relay visualised on the onboard LED,
// BOOT button, no ZMCT/NTC). Build with -DSS_PROD_BOARD=1 for the REV 1.0
// production pin map (mirrors firmware/main/config.h).
#ifdef SS_PROD_BOARD
  #define PIN_RELAY    4
  #define PIN_BUTTON   2
  #define NO_SENSORS   0
#else
  #define PIN_RELAY    15    // XIAO onboard LED — visualises the relay
  #define PIN_BUTTON   9     // XIAO BOOT (active-low)
  #define NO_SENSORS   1
#endif

extern uint16_t plug_endpoint_id;
extern uint16_t temp_endpoint_id;

// Relay driver — boot-safe OFF. (The production-board build re-adds the
// autonomous safety brain ahead of the relay, same as the TankSync image.)
void app_driver_relay_init(void);
void app_driver_relay_set(bool on);
bool app_driver_relay_get(void);

// Button: short press toggles the plug locally (and reports to fabrics).
// 8s hold = Matter factory reset (decommission).
void app_driver_button_start(void);

// Background reporter: board temperature (and later power) every 30s.
void app_driver_telemetry_start(void);
