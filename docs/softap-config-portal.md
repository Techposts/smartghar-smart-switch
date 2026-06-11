# Smart Switch — SoftAP + Config Portal (design / build plan)

> **Status: designed, not built.** This is the concrete plan for the switch's own
> WiFi access point + web config page (roadmap v0.1.x). It's written so the build
> is a clean, focused pass — it needs hardware testing (joining the AP, walking
> the page) that a config-from-the-hub flow doesn't.

## Why the switch needs its own portal

Today the switch is **hub-paired over ESP-NOW only** — no WiFi creds, no web UI.
Three things that doesn't cover:

1. **Standalone use** — running the switch with **no TankSync hub** (WiFi/MQTT to Home Assistant directly).
2. **Current-sensor calibration** — the ZMCT103C scale constant must be tuned per board against a known load; that's an interactive, on-device task.
3. **First-run config** — mains voltage, over-current limit, manual relay test — without needing a hub.

## Radio strategy — the one real gotcha

The switch uses **WiFi STA (unassociated)** for ESP-NOW, parked on the hub's
channel. SoftAP needs an AP interface. The clean approach:

- **Default runtime:** STA-only (ESP-NOW), exactly as today.
- **Config mode (on demand):** switch to **APSTA** — bring up a SoftAP
  `SmartGhar-Switch-XXXX` (XXXX = MAC suffix) while keeping STA alive. ESP-NOW
  keeps working on the STA channel; the AP shares that channel (single-radio
  constraint — the AP **must** use the STA channel, can't pick its own).
- **Exit:** drop back to STA-only after save / timeout (10 min idle).

**Trigger:** a button gesture that isn't already taken. Current map is short=toggle,
2s=pair, 8s=reset. Add **double-tap = enter/exit config AP** (or 4s hold). Also
auto-enter config AP on first boot of an **unpaired, never-configured** unit.

## HTTP surface (esp_http_server, port 80 on the AP, `192.168.4.1`)

| Method/Path | Purpose |
|---|---|
| `GET /` | Single-page config UI (one embedded HTML C-string, like the hub) |
| `GET /api/info` | `{fw, mac, paired, relay, load_ma, temp_c, mains_v, oc_limit_ma, dry_min_ma, zmct_scale}` |
| `POST /api/relay?on=0\|1` | Manual relay test from the page |
| `POST /api/config` | Save `{volt, imax, drymin}` → NVS (same keys the SET frame writes) |
| `POST /api/calibrate?amps=<known>` | Set `zmct_scale` so the present RMS reads `<known>` A (calibration) |
| `POST /api/wifi` | Standalone mode: `{ssid, pass, broker, port}` → NVS, for the future WiFi/MQTT transport |
| `POST /api/exit` | Leave config AP, return to STA/ESP-NOW |

Keep the page tiny + dependency-free (match the hub's editorial single-file style).

## Calibration flow (the page's headline feature)

1. User wires a **known resistive load** (e.g. a 1000 W heater ≈ 4.3 A @ 230 V) through the switch.
2. Page: "Turn the load on, enter its current (A), press Calibrate."
3. `POST /api/calibrate?amps=4.3` → firmware computes `zmct_scale = 4300 / present_rms_counts`, persists it.
4. Live `load_ma` on the page now reads true. (Replaces the placeholder `ZMCT_MA_PER_COUNT`.)

## NVS keys (extend the existing `swcfg` namespace)

Reuse `volt`/`imax`/`runmax`/`drymin` (already written by SET). Add:
`zmct_scale` (float ×1000), `wifi_ssid`, `wifi_pass`, `mqtt_host`, `mqtt_port`.

## Security

- The config AP is **open** (no password) but **only reachable while in config mode**
  (deliberately entered) and **auto-closes** on idle/save — same trade-off as the
  hub's setup AP. WPA2 with a printed PIN is a later hardening step.
- Calibration + relay endpoints are local-AP only (not exposed in STA mode).

## Build order (one focused session)

1. `softap_config` component: bring-up/teardown of APSTA + the HTTP server.
2. The embedded HTML page + the 6 endpoints.
3. Wire the button gesture + first-boot auto-enter.
4. Calibration math + `zmct_scale` persistence; apply it in `zmct_read_ma()`.
5. **Hardware test:** join `SmartGhar-Switch-XXXX`, walk every endpoint, calibrate against a known load, confirm ESP-NOW still works in APSTA.
6. Bump to v0.2.0 (this is a meaningful feature step) + release.

## Explicitly out of scope here

- The **standalone WiFi/MQTT transport** itself (this portal only *captures* the
  creds; running MQTT without a hub is the separate v0.2 transport work).
- Matter/Zigbee (see `transports-roadmap.md`).
