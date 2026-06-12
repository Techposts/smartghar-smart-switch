// SmartGhar Smart Switch — Matter image entry point.
//
// Endpoint 0: root node. Endpoint 1: On/Off Plug-in Unit (the relay).
// Endpoint 2: Temperature Sensor (board NTC; fixed 25.00°C on the bare bench
// board). Commission via BLE + the setup QR/code printed on the console.
//
// This image shares the device with the TankSync image (two-image product) —
// the SoftAP portal in the TankSync image flashes this one into the inactive
// OTA slot and vice versa. We mark ourselves valid after Matter starts so the
// bootloader's rollback can rescue a broken upload.

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_ota_ops.h>

#include <esp_matter.h>
#include <common_macros.h>

#include <app_priv.h>

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>

static const char *TAG = "smartsw_matter";

uint16_t plug_endpoint_id = 0;
uint16_t temp_endpoint_id = 0;

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

constexpr auto k_timeout_seconds = 300;

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved: {
        ESP_LOGI(TAG, "Fabric removed");
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            chip::CommissioningWindowManager &mgr =
                chip::Server::GetInstance().GetCommissioningWindowManager();
            if (!mgr.IsCommissioningWindowOpen()) {
                mgr.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(k_timeout_seconds),
                                                 chip::CommissioningWindowAdvertisement::kDnssdOnly);
            }
        }
        break;
    }
    default:
        break;
    }
}

static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identify: type %u effect %u", type, effect_id);
    return ESP_OK;
}

// Every attribute write lands here first — the relay follows the OnOff
// attribute, so controller commands, the physical button, and future
// automations all flow through one path.
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type == PRE_UPDATE && endpoint_id == plug_endpoint_id &&
        cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        app_driver_relay_set(val->val.b);
    }
    return ESP_OK;
}

extern "C" void app_main()
{
    nvs_flash_init();

    app_driver_relay_init();   // boot-safe relay OFF before anything else

    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    // Endpoint 1 — the relay as a standard plug (works in every ecosystem).
    on_off_plug_in_unit::config_t plug_config;
    plug_config.on_off.on_off = false;
    endpoint_t *plug = on_off_plug_in_unit::create(node, &plug_config, ENDPOINT_FLAG_NONE, NULL);
    ABORT_APP_ON_FAILURE(plug != nullptr, ESP_LOGE(TAG, "Failed to create plug endpoint"));
    plug_endpoint_id = endpoint::get_id(plug);

    // Endpoint 2 — board temperature (0.01°C units).
    temperature_sensor::config_t temp_config;
    temp_config.temperature_measurement.measured_value = 2500;
    endpoint_t *temp = temperature_sensor::create(node, &temp_config, ENDPOINT_FLAG_NONE, NULL);
    ABORT_APP_ON_FAILURE(temp != nullptr, ESP_LOGE(TAG, "Failed to create temperature endpoint"));
    temp_endpoint_id = endpoint::get_id(temp);

    ESP_LOGI(TAG, "Endpoints: plug=%u temp=%u", plug_endpoint_id, temp_endpoint_id);

    esp_err_t err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

    app_driver_button_start();
    app_driver_telemetry_start();

    // Two-image rollback gate: Matter is up — this image works, accept it.
    // (If we crash before reaching here, the bootloader reverts to the
    // TankSync image in the other slot.)
    esp_ota_mark_app_valid_cancel_rollback();

    ESP_LOGI(TAG, "SmartGhar Smart Switch (Matter image) ready — commission via the QR/code above");
}
