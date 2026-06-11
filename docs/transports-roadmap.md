# Smart Switch — Multi-Transport Roadmap (Matter / Zigbee / WiFi / ESP-NOW)

> **Status: planning / scope only.** Nothing here is built yet beyond the
> current ESP-NOW hub pairing. This document scopes *how* user-selectable
> transports would work on the ESP32-C6 and in what order to build them. It is
> a milestone plan, not a spec.

## Why the C6 makes this possible

The ESP32-C6 has **two independent 2.4 GHz radios**:

- a **Wi-Fi 6 (802.11)** radio — carries Wi-Fi/MQTT **and** ESP-NOW (ESP-NOW rides on the Wi-Fi PHY), and
- an **802.15.4** radio — carries **Thread (→ Matter)** *or* **Zigbee**.

Plus Bluetooth LE on the shared 2.4 GHz front-end (useful for commissioning).

```mermaid
flowchart LR
  SW[Smart Switch C6] --- WIFI[Wi-Fi radio]
  SW --- R154[802.15.4 radio]
  WIFI --> WM[Wi-Fi / MQTT]
  WIFI --> EN[ESP-NOW / TankSync hub]
  R154 --> TH[Thread → Matter]
  R154 --> ZB[Zigbee]
```

## The hard constraint: pick ONE primary stack

The radios coexist, but the **protocol stacks do not**:

- **Thread/Matter** and **Zigbee** both own the 802.15.4 radio — **mutually exclusive** (one or the other, never both).
- **Matter** (esp-matter) and **Zigbee** (esp-zigbee-sdk) are each **large** (~1–1.5 MB of flash + meaningful RAM). Stacking Matter **and** Zigbee **and** Wi-Fi **and** the switch logic won't fit a 4 MB part — and the XIAO test board is only 2 MB.

So "user can choose" realistically means **one selected transport per unit**, not all-at-once. Two viable shapes:

| Shape | How the user chooses | Trade-off |
|---|---|---|
| **Build-time variants** (recommended first) | A different firmware binary per transport (`-DTRANSPORT=espnow\|wifi\|matter\|zigbee`); user flashes the one they want. | Simplest, smallest, ships fastest. "Choice" = which binary you flash. |
| **Runtime selector** | One firmware, a config (SoftAP / button) picks the stack; an OTA-style partition loads it. | Best UX, but heaviest — needs partitions sized for the largest stack + careful re-init. A later evolution. |

Recommendation: **build-time variants first**, converge toward a runtime selector once the stacks are individually proven.

## Per-transport scope

| Transport | What it gives | Commissioning | Effort |
|---|---|---|---|
| **ESP-NOW** (today) | Pairs to a TankSync hub; hub runs the pump brain. | Button 2 s → channel sweep → hub `PAIR_ACK`. | ✅ done |
| **Wi-Fi / MQTT** (v0.2) | Standalone — reports to any MQTT broker / Home Assistant with **no hub**. | SoftAP portal (see the SoftAP doc) for Wi-Fi creds + broker. | Medium — reuses the hub's MQTT patterns. |
| **Matter-over-Thread** (v0.3) | Native Apple Home / Google / Alexa; local, standards-based. Appears as a Matter on/off plug + power metering. | Matter QR code + a Thread Border Router on the network. | Large — esp-matter SDK, Thread, commissioning, certification questions. |
| **Zigbee** (v0.3+) | Joins an existing Zigbee network (Hue/deCONZ/ZHA/Z2M) as an on/off plug + metering. | Zigbee coordinator "permit join". | Large — esp-zigbee-sdk; separate from Matter. |

## Suggested order

1. **Finish the ESP-NOW basics** — pairing reliability, the SoftAP config portal, current-sensor calibration. (These make *every* transport better; do them first.)
2. **Wi-Fi / MQTT standalone (v0.2)** — biggest reach for least new-stack risk; reuses known MQTT code.
3. **Matter-over-Thread (v0.3)** — the headline "works with Apple/Google/Alexa" feature; build as its own variant.
4. **Zigbee (v0.3+)** — for users already on a Zigbee hub; its own variant.

## Open questions to resolve before building 3/4

- **Flash budget** — confirm the production board's flash size (4 MB min for Matter/Zigbee; the 2 MB XIAO can't host them).
- **Matter certification** — DIY/non-certified Matter devices work on most ecosystems in "uncertified" mode; selling a *certified* Matter product is a separate (paid) process.
- **One metering model** — define the on/off + power/energy attribute mapping once, so Matter, Zigbee, and MQTT all expose the same data shape.

---

*This is a living plan. The near-term work (pairing reliability, SoftAP/config, calibration) is the foundation; Matter and Zigbee are deliberately sequenced after it.*
