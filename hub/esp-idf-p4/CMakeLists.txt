cmake_minimum_required(VERSION 3.16)

# The pinned esp_wifi_remote/esp_hosted components select their ESP-IDF
# compatibility Kconfig using this macro. ESP-IDF 5.5.4 does not export it,
# while the components intentionally ship a shared 5.5 compatibility set.
set(ENV{ESP_IDF_VERSION} "5.5")
set_property(GLOBAL PROPERTY RULE_LAUNCH_CUSTOM
             "${CMAKE_COMMAND} -E env ESP_IDF_VERSION=5.5")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(bluepaws_home_hub)

# Reuse the proven, dependency-free Off-Grid dashboard assets. Only this public
# directory is packaged; credentials and journal data are never part of it.
spiffs_create_partition_image(storage ../platformio/data FLASH_IN_PROJECT)
