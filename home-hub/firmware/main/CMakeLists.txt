idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        bluepaws_core
        esp_timer
        guition_jc4880p443c
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(${LVGL_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)
