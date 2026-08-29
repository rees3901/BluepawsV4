idf_component_register(
    SRCS "guition_jc4880p443c.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_i2c
        esp_driver_ledc
        esp_hw_support
        esp_lcd
)
