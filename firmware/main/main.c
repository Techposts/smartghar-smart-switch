// SmartGhar Smart Switch — ESP32-C6 firmware.
//
// A mains relay (pump controller) that pairs with a TankSync hub over ESP-NOW.
// Per the locked design, the PUMP BRAIN lives on the hub (it has the tank data);
// this firmware is a faithful executor with its OWN autonomous hardware-safety
// that must survive a total hub/WiFi outage:
//   - boot-safe relay OFF
//   - INRUSH-TOLERANT over-current trip (ignore the motor-start surge; trip only
//     on sustained over-current — the 10A ZMCT saturates above ~10A and the pump
//     surges to ~40A briefly, so an instantaneous spike must never trip)
//   - over-temperature trip, welded-contact detection, autonomous max-runtime
// Protocol (matches the hub): PAIR_REQ:<nonce>:<mac>:switch / PAIR_ACK,
//   uplink SWSTAT:<relay>:<load_ma>:<power_w>:<tempCx10>:<fault>:<fw>,
//   downlink RELAY:ON|OFF (→ RELAY_ACK) and SET:VOLT=..:IMAX=..:RUNMAX=..:DRYMIN=..
//
// HARDWARE BRING-UP PENDING: compile-verified only until the REV 1.0 board exists.
// ZMCT/NTC scale constants are first-estimates — calibrate against the real board.

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include "esp_rom_sys.h"   // esp_rom_delay_us
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_netif.h"

#include "config.h"
#include "version_gen.h"   // FIRMWARE_VERSION (generated from VERSION)

static const char *TAG = "smartsw";

// ── Runtime config (NVS-backed, editable via SET / web UI) ───────────────────
static struct {
    uint16_t mains_v;       // for power calc
    uint32_t oc_limit_ma;   // sustained over-current threshold
    uint32_t max_runtime_s; // autonomous failsafe
    uint32_t dry_min_ma;    // 0 = dry-run disabled (hub handles it)
} s_cfg = { MAINS_V_DEFAULT, OC_LIMIT_MA, MAX_RUNTIME_S_DEFAULT, DRY_MIN_MA_DEFAULT };

// ── State ────────────────────────────────────────────────────────────────────
static volatile bool     s_relay_on    = false;
static volatile uint8_t  s_fault       = 0;     // SW_FAULT_* bits (mirror hub's)
static int64_t  s_relay_on_us = 0;              // when relay last turned ON
static int64_t  s_relay_off_us = 0;
static int64_t  s_oc_over_since_us = 0;         // when current first crossed the limit
static int64_t  s_oc_hibernate_until_us = 0;    // lockout after an OC trip
static int      s_load_ma = 0;
static int      s_temp_c10 = 250;
static uint8_t  s_hub_mac[6];
static bool     s_paired = false;
static uint8_t  s_channel = ESPNOW_DEFAULT_CHANNEL;   // hub's WiFi channel (found at pairing)
#if !NO_SENSORS
static adc_oneshot_unit_handle_t s_adc;
#endif

// Fault bits — identical numbering to the hub's SW_FAULT_* so SWSTAT maps 1:1.
#define SW_FAULT_OVERCURRENT  0x01
#define SW_FAULT_OVERTEMP     0x02
#define SW_FAULT_WELDED       0x04
#define SW_FAULT_DRYRUN       0x08
#define SW_FAULT_SENSOR       0x10

// ── Relay ────────────────────────────────────────────────────────────────────
static void relay_apply(bool on) {
    gpio_set_level(PIN_RELAY, on ? 1 : 0);
    if (on && !s_relay_on)  s_relay_on_us = esp_timer_get_time();
    if (!on && s_relay_on)  s_relay_off_us = esp_timer_get_time();
    s_relay_on = on;
    gpio_set_level(PIN_LED, (on ^ LED_ACTIVE_LOW) ? 1 : 0);
}

// ── ADC: ZMCT RMS current + NTC temperature ──────────────────────────────────
#if NO_SENSORS
// Bare-board test profile (XIAO C6, no ZMCT/NTC fitted): report a quiet,
// fault-free reading so the control loop + telemetry work without sensors.
static void adc_setup(void) {}
static int zmct_read_ma(void) { return 0; }
static int ntc_read_c10(void) { return 250; }   // 25.0°C placeholder
#else
static void adc_setup(void) {
    adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&u, &s_adc);
    adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &c);   // GPIO0 ZMCT
    adc_oneshot_config_channel(s_adc, ADC_CHANNEL_1, &c);   // GPIO1 NTC
}

// Sample the ZMCT over a few mains cycles, remove DC bias, return RMS current mA.
// NOTE: the ZMCT103C burden + scale constant ZMCT_MA_PER_COUNT must be calibrated
// on the real board with a known load — this is a first estimate.
#define ZMCT_SAMPLES        400      // ~5 mains cycles @ 50Hz at ~50us/sample
#define ZMCT_MA_PER_COUNT   12.0f    // PLACEHOLDER — calibrate with the board
static int zmct_read_ma(void) {
    long sum = 0; int raw;
    int buf[ZMCT_SAMPLES];
    for (int i = 0; i < ZMCT_SAMPLES; i++) {
        adc_oneshot_read(s_adc, ADC_CHANNEL_0, &raw);
        buf[i] = raw; sum += raw;
        esp_rom_delay_us(50);
    }
    float mean = (float)sum / ZMCT_SAMPLES;
    double sq = 0;
    for (int i = 0; i < ZMCT_SAMPLES; i++) { float d = buf[i] - mean; sq += (double)d * d; }
    float rms_counts = sqrtf((float)(sq / ZMCT_SAMPLES));
    return (int)(rms_counts * ZMCT_MA_PER_COUNT);
}

// NTC via divider on GPIO1 → board temperature ×10. Beta-model first estimate;
// calibrate NTC_BETA / NTC_R25 / series R to the fitted thermistor.
#define NTC_BETA   3950.0f
static int ntc_read_c10(void) {
    int raw = 0;
    adc_oneshot_read(s_adc, ADC_CHANNEL_1, &raw);
    if (raw <= 0 || raw >= 4095) { s_fault |= SW_FAULT_SENSOR; return 250; }
    // ratio → resistance → temp (beta). Constants approximate until board cal.
    float ratio = raw / 4095.0f;
    float r = (10000.0f * ratio) / (1.0f - ratio);      // 10k series, NTC to GND (assumed)
    float tK = 1.0f / (1.0f/298.15f + (1.0f/NTC_BETA) * logf(r / 10000.0f));
    return (int)((tK - 273.15f) * 10.0f);
}
#endif  // NO_SENSORS

// ── ESP-NOW link ─────────────────────────────────────────────────────────────
static void send_to_hub(const char *s) {
    if (s_paired) esp_now_send(s_hub_mac, (const uint8_t *)s, strlen(s));
}
static void send_swstat(void) {
    char m[96];
    int pw = (int)((long)s_load_ma * s_cfg.mains_v / 1000);
    snprintf(m, sizeof(m), "SWSTAT:%d:%d:%d:%d:%d:%s",
             s_relay_on ? 1 : 0, s_load_ma, pw, s_temp_c10, s_fault, FIRMWARE_VERSION);
    send_to_hub(m);
}

static void cfg_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint16_t v16; uint32_t v32; size_t l = 6;
        if (nvs_get_u16(h, "volt", &v16) == ESP_OK) s_cfg.mains_v = v16;
        if (nvs_get_u32(h, "imax", &v32) == ESP_OK) s_cfg.oc_limit_ma = v32;
        if (nvs_get_u32(h, "runmax", &v32) == ESP_OK) s_cfg.max_runtime_s = v32;
        if (nvs_get_u32(h, "drymin", &v32) == ESP_OK) s_cfg.dry_min_ma = v32;
        if (nvs_get_blob(h, "hubmac", s_hub_mac, &l) == ESP_OK && l == 6) s_paired = true;
        uint8_t ch;
        if (nvs_get_u8(h, "chan", &ch) == ESP_OK && ch >= ESPNOW_CHAN_MIN && ch <= ESPNOW_CHAN_MAX) s_channel = ch;
        nvs_close(h);
    }
}
static void cfg_save_u32(const char *k, uint32_t v) {
    nvs_handle_t h; if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, k, v); nvs_commit(h); nvs_close(h);
    }
}

// Parse SET:VOLT=..:IMAX=..:RUNMAX=..:DRYMIN=..
static void handle_set(const char *p) {
    char *v;
    if ((v = strstr(p, "VOLT=")))   { s_cfg.mains_v = atoi(v+5); cfg_save_u32("volt", s_cfg.mains_v); }
    if ((v = strstr(p, "IMAX=")))   { s_cfg.oc_limit_ma = atoi(v+5); cfg_save_u32("imax", s_cfg.oc_limit_ma); }
    if ((v = strstr(p, "RUNMAX="))) { s_cfg.max_runtime_s = atoi(v+7); cfg_save_u32("runmax", s_cfg.max_runtime_s); }
    if ((v = strstr(p, "DRYMIN="))) { s_cfg.dry_min_ma = atoi(v+7); cfg_save_u32("drymin", s_cfg.dry_min_ma); }
}

static void on_espnow_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len <= 0 || len > 120) return;
    char p[128]; int n = len < 127 ? len : 127; memcpy(p, data, n); p[n] = 0;

    if (strncmp(p, "RELAY:", 6) == 0) {
        bool on = strstr(p, "ON") != NULL;
        // Honour the hub's command UNLESS a hardware fault forces OFF.
        if (on && (s_fault & (SW_FAULT_OVERCURRENT | SW_FAULT_OVERTEMP))) {
            send_to_hub("RELAY_ACK:OFF");   // refuse to close into a fault
        } else {
            relay_apply(on);
            send_to_hub(on ? "RELAY_ACK:ON" : "RELAY_ACK:OFF");
            send_swstat();
        }
    } else if (strncmp(p, "SET:", 4) == 0) {
        handle_set(p);
        send_to_hub("SET_ACK");
    } else if (strncmp(p, "PAIR_ACK", 8) == 0) {
        // PAIR_ACK:<addr>:0:<nonce>:<chan> — we just need the hub MAC (= sender).
        memcpy(s_hub_mac, info->src_addr, 6);
        // We received the PAIR_ACK on the channel we're currently swept to — that
        // IS the hub's channel. Persist both so we come back paired on reboot.
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "hubmac", s_hub_mac, 6);
            nvs_set_u8(h, "chan", s_channel);
            nvs_commit(h); nvs_close(h);
        }
        esp_now_peer_info_t peer = { .channel = 0, .ifidx = WIFI_IF_STA };
        memcpy(peer.peer_addr, s_hub_mac, 6);
        esp_now_add_peer(&peer);
        s_paired = true;
        ESP_LOGI(TAG, "Paired to hub %02x%02x%02x%02x%02x%02x",
                 s_hub_mac[0],s_hub_mac[1],s_hub_mac[2],s_hub_mac[3],s_hub_mac[4],s_hub_mac[5]);
    }
}

static void enter_pairing(void) {
    ESP_LOGI(TAG, "Pairing — sweeping channels for the hub…");
    uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    esp_now_peer_info_t peer = { .channel = 0, .ifidx = WIFI_IF_STA };
    memcpy(peer.peer_addr, bcast, 6);
    if (!esp_now_is_peer_exist(bcast)) esp_now_add_peer(&peer);
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint16_t nonce = (uint16_t)(esp_random() & 0xffff);
    char req[64];
    snprintf(req, sizeof(req), "PAIR_REQ:%u:%02x%02x%02x%02x%02x%02x:switch",
             nonce, mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    // No WiFi creds (like the battery TX) → sweep channels 1-13, broadcasting on
    // each, until the hub answers PAIR_ACK (the recv cb sets s_paired + persists
    // the channel). The hub must be in its 60s pairing window.
    for (int pass = 0; pass < 4 && !s_paired; pass++) {
        for (int ch = ESPNOW_CHAN_MIN; ch <= ESPNOW_CHAN_MAX && !s_paired; ch++) {
            s_channel = (uint8_t)ch;
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            esp_now_send(bcast, (const uint8_t *)req, strlen(req));
            // Dwell ~900ms listening for the hub's PAIR_ACK before hopping — the
            // hub needs time to gate, register, add the peer, and reply 3×. Too
            // short and we move off-channel before the ACK lands (the hub registers
            // us but we never learn we're paired). Check s_paired every 100ms so
            // we break out promptly once the ACK arrives.
            for (int w = 0; w < 9 && !s_paired; w++) vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);  // settle on the paired/last channel
    ESP_LOGI(TAG, "Pairing done (paired=%d, chan=%u)", (int)s_paired, s_channel);
}

// ── Safety task — the autonomous floor (runs even with no hub) ───────────────
static void safety_task(void *arg) {
    (void)arg;
    for (;;) {
        int64_t now = esp_timer_get_time();
        s_temp_c10 = ntc_read_c10();
        s_load_ma  = zmct_read_ma();

        // Over-temp (with hysteresis)
        if (s_temp_c10 >= OT_LIMIT_C10) { s_fault |= SW_FAULT_OVERTEMP; if (s_relay_on) relay_apply(false); }
        else if (s_temp_c10 < OT_CLEAR_C10) s_fault &= ~SW_FAULT_OVERTEMP;

        if (s_relay_on) {
            int64_t on_for = now - s_relay_on_us;
            // INRUSH GRACE: ignore current entirely right after turn-on (the pump
            // surge to ~40A is normal and the 10A ZMCT saturates anyway).
            if (on_for > (int64_t)OC_STARTUP_GRACE_MS * 1000) {
                // Sustained over-current only — never trip on an instantaneous spike.
                if (s_load_ma > (int)s_cfg.oc_limit_ma) {
                    if (s_oc_over_since_us == 0) s_oc_over_since_us = now;
                    else if (now - s_oc_over_since_us > (int64_t)OC_SUSTAIN_MS * 1000) {
                        ESP_LOGW(TAG, "OVER-CURRENT %dmA sustained — trip", s_load_ma);
                        s_fault |= SW_FAULT_OVERCURRENT;
                        s_oc_hibernate_until_us = now + (int64_t)OC_TRIP_HIBERNATE_S * 1000000;
                        relay_apply(false); send_swstat();
                    }
                } else s_oc_over_since_us = 0;
                // Dry-run by current signature (secondary; hub's rate-of-rise is primary)
                if (s_cfg.dry_min_ma > 0 && s_load_ma < (int)s_cfg.dry_min_ma)
                    s_fault |= SW_FAULT_DRYRUN;
                else s_fault &= ~SW_FAULT_DRYRUN;
            }
            // Autonomous max-runtime failsafe (independent of the hub)
            if (now - s_relay_on_us > (int64_t)s_cfg.max_runtime_s * 1000000) {
                ESP_LOGW(TAG, "max-runtime — autonomous OFF");
                relay_apply(false); send_swstat();
            }
        } else {
            s_oc_over_since_us = 0;
            // Welded-contact: relay OFF but current still flowing after a settle.
            if (s_relay_off_us && now - s_relay_off_us > (int64_t)WELDED_SETTLE_MS * 1000
                && s_load_ma > WELDED_MA) {
                s_fault |= SW_FAULT_WELDED;
            } else if (s_load_ma < WELDED_MA) s_fault &= ~SW_FAULT_WELDED;
            // Clear an over-current trip after the hibernate window.
            if ((s_fault & SW_FAULT_OVERCURRENT) && now > s_oc_hibernate_until_us)
                s_fault &= ~SW_FAULT_OVERCURRENT;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

// ── Telemetry task ───────────────────────────────────────────────────────────
static void telem_task(void *arg) {
    (void)arg;
    for (;;) { send_swstat(); vTaskDelay(pdMS_TO_TICKS(SWSTAT_INTERVAL_MS)); }
}

// ── Button task — short=toggle, 2s=pair, 8s=factory reset ────────────────────
static void button_task(void *arg) {
    (void)arg;
    int64_t down = 0;
    for (;;) {
        bool pressed = gpio_get_level(PIN_BUTTON) == 0;   // active-low w/ pullup
        int64_t now = esp_timer_get_time();
        if (pressed && down == 0) down = now;
        if (!pressed && down) {
            int64_t held = (now - down) / 1000;
            if (held >= 8000) {                            // factory reset
                nvs_handle_t h; if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) { nvs_erase_all(h); nvs_commit(h); nvs_close(h); }
                esp_restart();
            } else if (held >= 2000) {
                enter_pairing();
            } else if (held >= 40) {                       // short → manual toggle
                relay_apply(!s_relay_on); send_swstat();
            }
            down = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void app_main(void) {
    // 1. Relay SAFE-OFF before anything else.
    gpio_config_t rc = { .pin_bit_mask = 1ULL << PIN_RELAY, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rc);
    gpio_set_level(PIN_RELAY, 0);
    gpio_config_t lc = { .pin_bit_mask = 1ULL << PIN_LED, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&lc); gpio_set_level(PIN_LED, LED_ACTIVE_LOW ? 1 : 0);
    gpio_config_t bc = { .pin_bit_mask = 1ULL << PIN_BUTTON, .mode = GPIO_MODE_INPUT, .pull_up_en = 1 };
    gpio_config(&bc);

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
    cfg_load();
    adc_setup();

    // 2. WiFi STA + ESP-NOW. Power-save OFF so ESP-NOW RX is never missed (mains
    //    powered, so always-on radio is free). STA gives us the home channel that
    //    ESP-NOW must share with the hub. (WiFi creds / SoftAP provisioning +
    //    web UI land in the next slice — for bring-up, creds via NVS/menuconfig.)
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wic);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
    // Unassociated STA → we own the channel. Park on the paired hub's channel
    // (from NVS) so steady-state SWSTAT/RELAY reach the hub. Pairing re-sweeps.
    esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);

    esp_now_init();
    esp_now_register_recv_cb(on_espnow_recv);
    if (s_paired) {
        esp_now_peer_info_t peer = { .channel = 0, .ifidx = WIFI_IF_STA };
        memcpy(peer.peer_addr, s_hub_mac, 6);
        esp_now_add_peer(&peer);
    }

    ESP_LOGI(TAG, "SmartGhar Smart Switch v%s — relay safe-OFF, paired=%d", FIRMWARE_VERSION, s_paired);

    xTaskCreate(safety_task, "safety", 4096, NULL, 6, NULL);
    xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);
    xTaskCreate(telem_task,  "telem",  3072, NULL, 4, NULL);

    // Auto-pair on boot when we have no hub yet — sweep channels for a hub that's
    // in its pairing window. (The button still triggers pairing on demand later.)
    if (!s_paired) {
        vTaskDelay(pdMS_TO_TICKS(1500));   // let WiFi/ESP-NOW settle
        enter_pairing();
    }
}
