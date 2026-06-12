# Matter Image — design & build plan

> Status: esp-matter SDK installing; architecture locked 2026-06-12 (see
> transports-roadmap.md "DECIDED" section). This is the concrete plan for the
> second firmware image.

## Shape

A separate ESP-IDF + esp-matter project at `firmware-matter/`, sharing source
with the TankSync image where it matters:

- **Shared (symlink or copy-by-build):** the safety brain (relay control,
  ZMCT RMS + calibration scale, NTC, over-current/welded/dry-run/max-runtime
  logic), the SoftAP portal (minus pairing UI), `config.h` pin map.
- **Matter-only:** esp-matter node with three endpoints:
  1. `on_off_plugin_unit` — the relay. Universal (Apple/Alexa/Google/HA).
  2. `temperature_sensor` — NTC board temp. Universal.
  3. Electrical Power Measurement cluster (Matter 1.3) on the plug endpoint —
     active power + RMS current from the ZMCT. HA full support; big-3 partial.
- **Commissioning:** BLE + QR (esp-matter default). Test DAC/CD credentials
  for development (`fctry` partition); CSA certification is a later,
  pre-mass-production step.
- The Matter data model only *requests* the relay; the safety brain still has
  final say (same rule as hub mode: never close into a hard fault).

## The 1.9 MB gate (first thing to prove)

The image must fit `ota_0` = 0x1E0000 in `partitions_two_image.csv`. Plan:
`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`, WiFi transport only (no Thread/OpenThread),
BLE only until commissioned (esp-matter supports releasing BLE memory post-
commissioning), no chip-shell/tracing, single fabric history trimmed. Espressif's
own C6 examples land ~1.6–1.7 MB with -Os — tight but inside the slot.
**If it cannot fit: stop, escalate to the 8 MB N8 module decision.**

## Mode-swap (portal feature, both images)

- Portal gains a "Mode" card: current mode + "Switch to Matter / TankSync".
- Swap = download the OTHER image (bundled checksummed at release; served from
  GitHub releases over the portal's future STA link, or uploaded from the
  phone in the portal page — **v1: upload-from-phone**, no internet needed),
  write to the inactive OTA slot, set boot, reboot.
- Rollback safety: `esp_ota_mark_app_valid()` only after the new mode boots
  and its stack comes up; a crash-looping image auto-falls-back to the other
  slot — i.e. a failed Matter experiment lands the user back in TankSync mode.
- Both images MUST be built against `partitions_two_image.csv`. Moving the
  bench XIAOs (currently single-app table) needs one serial reflash.

## Build order

1. esp-matter env + `examples/light` compile for C6 (sanity).
2. `firmware-matter/` skeleton: plug-unit endpoint driving PIN_RELAY through
   the shared safety brain; SWSTAT-equivalent attribute updates (power/temp)
   on the 5s cadence.
3. Size check vs 0x1E0000 → go/no-go gate.
4. Commission into HA (fastest Matter controller to test), then Alexa/Apple.
5. TankSync image 0.3.0: adopt the two-image partition table + portal Mode
   card with upload-swap.
6. Bench: serial-reflash one XIAO to the new table, walk the full swap loop
   TankSync → Matter → TankSync.

## Out of scope (this slice)

- Thread / Matter-over-Thread (later: same data model, 802.15.4 radio).
- CSA certification & production DACs (pre-mass-production work item).
- Energy (kWh) accumulation — add once power measurement is proven.
