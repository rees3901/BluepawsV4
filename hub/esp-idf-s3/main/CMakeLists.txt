idf_component_register(
    SRCS "main.cpp"
         "../../esp-idf-p4/main/home_hub_cloud.cpp"
         "../../esp-idf-p4/main/home_hub_web.cpp"
         "../../esp-idf-p4/main/home_hub_bluetooth.cpp"
         "../../esp-idf-p4/main/home_hub_mdns.cpp"
         "../../esp-idf-p4/main/home_hub_settings_store.cpp"
         "../../esp-idf-p4/main/home_hub_testbed_telemetry.cpp"
    INCLUDE_DIRS "." "../../esp-idf-p4/main"
    REQUIRES
        bluepaws_core
        bt
        esp_event
        esp_http_client
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        guition_jc3248w535c
        json
        lwip
        mbedtls
        nvs_flash
        spiffs
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(${LVGL_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)
