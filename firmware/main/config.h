// SmartGhar Smart Switch — board + safety configuration.
// Hardware: ESP32-C6-SuperMini + 5V HiLink (isolated) + 5V-coil 30A relay (via
// PC817 opto + 2N2222) + ZMCT103C 10A current TF + NTC temp.  REV 1.0 schematic.
#pragma once

// ── Pin map (from the REV 1.0 schematic) ─────────────────────────────────────
#define PIN_BUTTON      2     // tactile: short=toggle, 2s=pair, 8s=factory reset
#define PIN_NTC_ADC     1     // GPIO1 = ADC1_CH1 — NTC divider (board temperature)
#define PIN_ZMCT_ADC    0     // GPIO0 = ADC1_CH0 — ZMCT103C burden output (AC current)
#define PIN_RELAY       4     // relay drive (active HIGH → opto → coil). Boot LOW = OFF.
#define PIN_LED         15    // status LED (D1). C6-SuperMini onboard ~GPIO15; adjust to board.
#define LED_ACTIVE_LOW  1     // many SuperMini onboard LEDs are active-low

// ── Mains / metering ─────────────────────────────────────────────────────────
#define MAINS_V_DEFAULT     230    // user-editable (web UI / SET:VOLT). For power = V×I_rms.
#define LOAD_RATED_A        10     // product rating: 10A @ 220V. ZMCT103C is a 10A sensor.

// ── Over-current protection — INRUSH TOLERANT (critical) ─────────────────────
// A pump's start surge is 4-7× run current (a 10A pump → ~40A for tens-to-hundreds
// of ms). The 10A ZMCT SATURATES above ~10A, and the 30A relay laughs at the surge.
// So we MUST NOT trip on startup inrush: (1) ignore current entirely for a grace
// window after the relay turns ON, then (2) trip only on SUSTAINED over-current
// (over the limit continuously for OC_SUSTAIN_MS), never on an instantaneous spike.
#define OC_STARTUP_GRACE_MS  1500   // ignore current this long after relay ON (inrush)
#define OC_LIMIT_MA          11500  // sustained current above this = fault (just over 10A rating)
#define OC_SUSTAIN_MS        3000   // must stay over the limit this long to trip
#define OC_TRIP_HIBERNATE_S  60     // after an over-current trip, hold OFF this long

// ── Over-temperature ─────────────────────────────────────────────────────────
#define OT_LIMIT_C10         750    // board temp ×10 (75.0°C) → trip OFF + fault
#define OT_CLEAR_C10         650    // clears below 65.0°C (hysteresis)

// ── Welded-contact detection ─────────────────────────────────────────────────
// Relay commanded OFF but current still flowing (after a settle) ⇒ contacts welded.
// Software can't open a fused contact — flag it loudly so the hub/HA alerts.
#define WELDED_SETTLE_MS     800
#define WELDED_MA            500

// ── Dry-run (current signature; secondary to the hub's rate-of-rise) ─────────
// A centrifugal pump running dry draws LESS than its wet running current. If
// configured (DRY_MIN_MA > 0), report a dry-run fault when running current sits
// below it after the startup grace. Primary dry-run is the hub (rate-of-rise).
#define DRY_MIN_MA_DEFAULT   0      // 0 = disabled (hub handles dry-run); set per pump

// ── Autonomous max-runtime (true failsafe, independent of the hub) ───────────
#define MAX_RUNTIME_S_DEFAULT 1800  // 30 min; hub also enforces its own

// ── Telemetry / link ─────────────────────────────────────────────────────────
#define SWSTAT_INTERVAL_MS   5000   // SWSTAT cadence (also sent immediately on change)
#define RELAY_ACK_RETRIES    3
#define ESPNOW_PAIR_NETID    6      // shared pairing rendezvous (matches TankSync TX)

// ── NVS keys ─────────────────────────────────────────────────────────────────
#define NVS_NS               "swcfg"
