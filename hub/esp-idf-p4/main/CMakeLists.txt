idf_component_register(
    SRCS "main.cpp" "app_shell.cpp" "ui_icons.cpp" "home_hub_cloud.cpp"
         "home_hub_settings_store.cpp" "home_hub_camera.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        bluepaws_core
        esp_driver_jpeg
        esp_event
        esp_http_client
        esp_netif
        esp_timer
        esp_wifi
        guition_jc4880p443c
        json
        mbedtls
        nvs_flash
        espressif__esp_video
        espressif__quirc
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=vApplicationGetTimerTaskMemory")

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(${LVGL_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)
