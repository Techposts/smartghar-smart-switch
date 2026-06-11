// SmartGhar Smart Switch — SoftAP config portal (design: docs/softap-config-portal.md).
//
// On demand (4s button hold, or auto on first boot of an unpaired unit) the
// switch raises an OPEN access point `SmartGhar-Switch-XXXX` (XXXX = last two
// STA-MAC bytes — the same suffix the hub uses for the default device name) and
// serves a single-page config UI on http://192.168.4.1/.
//
// Radio: APSTA mode. The AP MUST share the STA channel (single radio), so
// ESP-NOW to the hub keeps working while the portal is up. The portal closes
// on /api/exit or after 10 minutes idle, returning to STA-only.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void portal_start(void);
void portal_stop(void);
bool portal_active(void);

// ── Accessors implemented by main.c (the portal owns no switch state) ────────
typedef struct {
    const char *fw;          // FIRMWARE_VERSION
    uint8_t  mac[6];         // STA MAC
    bool     paired;
    bool     relay_on;
    int      load_ma;
    int      temp_c10;
    uint8_t  fault;
    uint16_t mains_v;
    uint32_t oc_limit_ma;
    uint32_t dry_min_ma;
    uint32_t max_runtime_s;
    float    zmct_scale;     // mA per RMS ADC count (calibratable)
} sw_status_t;

void  sw_portal_get_status(sw_status_t *out);
void  sw_portal_set_relay(bool on);                    // behaves like a physical press (hub adopts manual hold)
void  sw_portal_apply_config(uint16_t volt, uint32_t imax_ma, uint32_t drymin_ma, uint32_t runmax_s);
// Calibrate the ZMCT scale so the present RMS reading equals known_ma.
// Returns the new scale, or a negative value if it can't (no current flowing /
// sensors not fitted on this board profile).
float sw_portal_calibrate(int known_ma);
// Persist standalone-mode WiFi/MQTT credentials (future transport — captured now).
void  sw_portal_save_wifi(const char *ssid, const char *pass, const char *host, uint16_t port);
