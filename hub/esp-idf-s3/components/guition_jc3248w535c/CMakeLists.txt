idf_component_register(
    SRCS "guition_jc3248w535c.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_sdmmc
        esp_lcd
        fatfs
        sdmmc
)
