# SmartGhar Smart Switch

An open, local-first **mains relay with current + temperature sensing** built on the ESP32-C6. It pairs with a [TankSync](https://github.com/Techposts/TankSync) hub to control a water pump automatically by tank level — and works standalone as a general 30A smart switch + power meter.

> **Status: v0.1.0 — early bring-up.** Firmware is compile-verified; the REV 1.0 hardware is in fab. Current/temperature scale constants are first estimates pending calibration on the real board. Not yet for production loads.

## What it is

- **ESP32-C6** — WiFi 6, Thread, Zigbee, BLE, ESP-NOW. One board, your choice of how it talks.
- **30A relay** switching a **10A @ 220V** rated load (lots of contact headroom = reliability and easy handling of motor start-surge).
- **ZMCT103C** current transformer + **NTC** temperature — so the switch *knows* whether the pump is actually drawing current, not just whether it was told to turn on.
- **Isolated HiLink PSU** — the low-voltage/MCU side is galvanically isolated from mains.

## How it works with TankSync

The **pump brain runs on the TankSync hub** (that's where the tank data is). The switch is a faithful executor with its own autonomous hardware-safety:

```
TANK level ──▶ TankSync HUB ──ESP-NOW (RELAY cmd)──▶ SMART SWITCH ──▶ pump
                  │  ▲                                    │
                  │  └──────── SWSTAT telemetry ──────────┘
```

The hub decides *when* to run the pump (level thresholds, source-tank guard, rate-of-rise dry-run, max-runtime). The switch decides *when to protect itself*, even if the hub or WiFi drops.

### Safety (autonomous — survives a hub/WiFi outage)

- **Boot-safe** — relay is OFF on every power-up.
- **Inrush-tolerant over-current** — a pump surges to ~40A at start (4–7× run current) for a fraction of a second. The firmware ignores current during a startup grace window and trips only on *sustained* over-current — never on the start surge. (The 10A ZMCT saturates above ~10A; the 30A relay handles the surge easily.)
- **Over-temperature** trip with hysteresis.
- **Welded-contact detection** — relay commanded OFF but current still flowing ⇒ flagged loudly (software can't open a fused contact, but it can alert).
- **Autonomous max-runtime** — a hard runtime cap even if the hub never sends OFF. The hub sets this failsafe *longer* than its own intended run (`SET:RUNMAX = hub_max + buffer`) so the switch is a true backstop for a silent hub, never the part that cuts a valid run short.

## Pairing with a TankSync hub

The switch has no WiFi credentials of its own — it finds the hub the same way the battery tank sensors do, by sweeping WiFi channels for the hub's pairing window.

1. **Open the hub's pairing window** — on the TankSync hub web UI, go to **Devices → Pair new**. This opens a ~60-second window (shared by LoRa and ESP-NOW).
2. **Put the switch into pairing** — hold the button for **2 seconds** (a fresh, unpaired switch also enters pairing automatically on boot). It sweeps channels 1–13 broadcasting `PAIR_REQ:<nonce>:<mac>:switch`.
3. **The hub recognises the device type** — the `:switch` token tells the hub this is a Smart Switch, not a tank, so it registers it as a **switch** (it appears under the **Switches** tab, not Tanks) and replies `PAIR_ACK` with the assigned address + its WiFi channel.
4. **The switch remembers the hub** — it persists the hub MAC + channel to flash, so it re-connects instantly after a power cut. No re-pairing needed.

**Re-pairing / moving to another hub:** hold the button **8 seconds** to factory-reset (clears the stored hub), then pair again. Removing the switch from a hub (Switches → **Unpair**) frees its address; pairing it again — even to the same hub — assigns a fresh slot.

## Pump control (configured on the hub)

The pump rule lives on the hub (**Switches** tab → *Pump control*). Per switch you set: the **tank to keep filled**, **start %** (pump on) and **stop %** (pump off), an optional **source-tank guard** (don't run if the source/sump is below X %), **max run time**, and **min rest between runs**.

Anti-cycling / motor-protection rules the hub enforces so the pump never operates abruptly:

- **Hysteresis band** — stop % must be at least 5 % above start % (the UI enforces it); a wide band + the 5-minute tank-report cadence means the pump can't chatter on sensor noise.
- **Min rest between runs** — after the pump stops it won't restart until this elapses (anti short-cycle / inrush).
- **Dry-run lockout** — if rate-of-rise (or the switch's own current signature) says the pump is running dry, the hub stops it **and latches it off for 30 minutes** instead of retrying every few minutes, so a dry pump isn't run repeatedly.
- **Failsafe coordination** — the switch's autonomous max-runtime is always set longer than the hub's run, so the two timers never fight and restart-flap.

## Protocol (ESP-NOW, matches the TankSync hub)

| Direction | Frame |
|---|---|
| pair | `PAIR_REQ:<nonce>:<mac>:switch` → `PAIR_ACK:<addr>:0:<nonce>:<chan>` |
| switch → hub | `SWSTAT:<relay>:<load_ma>:<power_w>:<tempC×10>:<faultbits>:<fw>` |
| hub → switch | `RELAY:ON\|OFF` → `RELAY_ACK:ON\|OFF` |
| hub → switch | `SET:VOLT=..:IMAX=..:RUNMAX=..:DRYMIN=..` → `SET_ACK` |

Fault bits: `1` over-current · `2` over-temp · `4` welded · `8` dry-run · `16` sensor.

## Pin map (REV 1.0)

| GPIO | Function |
|------|----------|
| 4 | Relay drive (via PC817 opto → 2N2222 → coil; boot LOW = OFF) |
| 0 | ZMCT103C current (ADC1_CH0) |
| 1 | NTC temperature (ADC1_CH1) |
| 2 | Button — short = manual toggle · 2s = pair · 8s = factory reset |

## Roadmap

- **v0.1.x** — hardware bring-up + calibration on the REV 1.0 board; ZMCT/NTC fitting; SoftAP WiFi provisioning + minimal editorial config web UI (mains voltage, calibration, manual control).
- **v0.2** — standalone mode (WiFi/MQTT, Home Assistant) without a TankSync hub; energy (kWh) accumulation.
- **v0.3** — **Matter over Thread** (native Apple Home / Alexa / Google) and **Zigbee** as alternate transports. The C6 hardware supports them; firmware exposes a transport selector. *Universal board, phased firmware.*

## Build

```sh
. $IDF_PATH/export.sh
cd firmware
idf.py set-target esp32c6
idf.py build
```

## License

Firmware: AGPL-3.0-or-later. © Ravi Singh / SmartGharLabs.
