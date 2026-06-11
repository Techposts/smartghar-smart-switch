# Changelog

All notable changes to the SmartGhar Smart Switch firmware. Versions follow [SemVer](https://semver.org); tags are `ss-vX.Y.Z`.

## v0.1.0 — first firmware (early bring-up)

First public version of the SmartGhar Smart Switch (ESP32-C6). **Compile-verified only — REV 1.0 hardware is in fab; not for production loads yet.** Current/temperature scale constants are first estimates pending calibration on the real board.

- Pairs with a TankSync hub over ESP-NOW (`PAIR_REQ:…:switch`); the pump brain runs on the hub, this firmware executes `RELAY:ON/OFF` and reports `SWSTAT` telemetry.
- Autonomous hardware-safety that survives a hub/WiFi outage: boot-safe relay OFF, **inrush-tolerant over-current** (ignores the motor start-surge, trips only on sustained over-current), over-temperature, welded-contact detection, autonomous max-runtime.
- Button: short = manual toggle · 2s = pair · 8s = factory reset.
- Runtime config via `SET:` (mains voltage, current limit, max-runtime, dry-run threshold), persisted to NVS.
