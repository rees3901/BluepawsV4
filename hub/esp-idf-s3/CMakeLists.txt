cmake_minimum_required(VERSION 3.16)

# The portable state, simulator and map maths stay single-sourced with the P4
# application. Hardware-specific code lives in this project and its board BSP.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../esp-idf-p4/components/bluepaws_core")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bluepaws_home_hub_s3)
