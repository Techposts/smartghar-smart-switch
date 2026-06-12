idf_component_register(
    SRCS "main.c" "softap_portal.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        driver
        esp_adc
        esp_timer
        esp_hw_support
        esp_http_server
        app_update
)

# Bare XIAO ESP32-C6 test build (no ZMCT/NTC): idf.py -DXIAO_C6_TEST=1 build
if(XIAO_C6_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE XIAO_C6_TEST=1)
endif()
