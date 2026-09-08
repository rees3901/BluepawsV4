cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(bluepaws_home_hub)

# Reuse the proven, dependency-free Off-Grid dashboard assets. Only this public
# directory is packaged; credentials and journal data are never part of it.
spiffs_create_partition_image(storage ../../hub/data FLASH_IN_PROJECT)
