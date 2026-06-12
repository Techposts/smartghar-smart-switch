// SmartGhar Smart Switch — Matter image drivers: relay, button, telemetry.
// Plain GPIO (no device-hal LED driver) — the relay is just an output, and the
// button is a polled task identical in spirit to the TankSync image's.

#include <esp_log.h>
#include <esp_matter.h>
#include <driver/gpio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include <app_priv.h>

using namespace esp_matter;
using namespace chip::app::Clusters;

static const char *TAG = "app_driver";
static bool s_relay_on = false;

void app_driver_relay_init(void)
{
    gpio_config_t rc = {};
    rc.pin_bit_mask = 1ULL << PIN_RELAY;
    rc.mode = GPIO_MODE_OUTPUT;
    gpio_config(&rc);
    gpio_set_level((gpio_num_t)PIN_RELAY, 0);   // boot-safe OFF

    gpio_config_t bc = {};
    bc.pin_bit_mask = 1ULL << PIN_BUTTON;
    bc.mode = GPIO_MODE_INPUT;
    bc.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&bc);
}

void app_driver_relay_set(bool on)
{
    gpio_set_level((gpio_num_t)PIN_RELAY, on ? 1 : 0);
    s_relay_on = on;
    ESP_LOGI(TAG, "relay -> %s", on ? "ON" : "OFF");
}

bool app_driver_relay_get(void) { return s_relay_on; }

// Report a device-side state change INTO the Matter data model so every
// fabric (Apple/Google/Alexa/HA) sees the physical toggle.
static void report_onoff(bool on)
{
    esp_matter_attr_val_t val = esp_matter_bool(on);
    attribute::update(plug_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
}

static void button_task(void *arg)
{
    (void)arg;
    int64_t down = 0;
    for (;;) {
        bool pressed = gpio_get_level((gpio_num_t)PIN_BUTTON) == 0;
        int64_t now = esp_timer_get_time();
        if (pressed && down == 0) down = now;
        if (!pressed && down) {
            int64_t held_ms = (now - down) / 1000;
            if (held_ms >= 8000) {
                ESP_LOGW(TAG, "8s hold — Matter factory reset (decommission)");
                esp_matter::factory_reset();
            } else if (held_ms >= 40) {
                // The attribute update round-trips through the PRE_UPDATE
                // callback, which drives the relay — one source of truth.
                report_onoff(!s_relay_on);
            }
            down = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void app_driver_button_start(void)
{
    xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);
}

// ── Telemetry: board temperature every 30s ───────────────────────────────────
#if NO_SENSORS
static int read_temp_c100(void) { return 2500; }   // bare bench board: 25.00°C
#else
// Production board: NTC divider on GPIO1 (same beta model as the TankSync
// image — unified when the shared driver component lands).
#include "esp_adc/adc_oneshot.h"
#include <math.h>
static adc_oneshot_unit_handle_t s_adc;
static bool s_adc_ready = false;
static int read_temp_c100(void)
{
    if (!s_adc_ready) {
        adc_oneshot_unit_init_cfg_t u = {};
        u.unit_id = ADC_UNIT_1;
        adc_oneshot_new_unit(&u, &s_adc);
        adc_oneshot_chan_cfg_t c = {};
        c.atten = ADC_ATTEN_DB_12;
        c.bitwidth = ADC_BITWIDTH_DEFAULT;
        adc_oneshot_config_channel(s_adc, ADC_CHANNEL_1, &c);
        s_adc_ready = true;
    }
    int raw = 0;
    adc_oneshot_read(s_adc, ADC_CHANNEL_1, &raw);
    if (raw <= 0 || raw >= 4095) return 2500;
    float ratio = raw / 4095.0f;
    float r = (10000.0f * ratio) / (1.0f - ratio);
    float tK = 1.0f / (1.0f / 298.15f + (1.0f / 3950.0f) * logf(r / 10000.0f));
    return (int)((tK - 273.15f) * 100.0f);
}
#endif

static void telemetry_task(void *arg)
{
    (void)arg;
    for (;;) {
        // Matter TemperatureMeasurement is int16 in 0.01°C units.
        esp_matter_attr_val_t val = esp_matter_nullable_int16((int16_t)read_temp_c100());
        attribute::update(temp_endpoint_id, TemperatureMeasurement::Id,
                          TemperatureMeasurement::Attributes::MeasuredValue::Id, &val);
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void app_driver_telemetry_start(void)
{
    xTaskCreate(telemetry_task, "telem", 3072, NULL, 4, NULL);
}
