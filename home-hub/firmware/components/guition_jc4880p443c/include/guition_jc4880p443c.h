/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware constants are derived from GUITION's JC4880P443C_I_W schematic
 * and its ESP-IDF 5.5.4 vendor example.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GUITION_JC4880P443C_NATIVE_WIDTH  480
#define GUITION_JC4880P443C_NATIVE_HEIGHT 800
#define GUITION_JC4880P443C_LANDSCAPE_WIDTH  800
#define GUITION_JC4880P443C_LANDSCAPE_HEIGHT 480

#define GUITION_JC4880P443C_LCD_RESET_GPIO      5
#define GUITION_JC4880P443C_BACKLIGHT_GPIO     23
#define GUITION_JC4880P443C_TOUCH_SDA_GPIO      7
#define GUITION_JC4880P443C_TOUCH_SCL_GPIO      8
#define GUITION_JC4880P443C_SD_MOUNT_POINT "/sdcard"

typedef struct {
    bool mounted;
    uint64_t card_capacity_bytes;
    uint64_t volume_total_bytes;
    uint64_t volume_free_bytes;
    uint32_t sector_size_bytes;
    uint32_t frequency_khz;
    char product_name[9];
} guition_jc4880p443c_sd_info_t;

/** Start the MIPI-DSI panel, GT911 touch controller and LVGL in landscape. */
lv_display_t *guition_jc4880p443c_display_start(void);

/** Set the PWM backlight from 0 to 100 percent. */
esp_err_t guition_jc4880p443c_backlight_set(int brightness_percent);

/** Return the number of contacts reported by the most recent GT911 sample. */
uint8_t guition_jc4880p443c_touch_count(void);

/** Consume accumulated pinch steps: positive zooms in, negative zooms out. */
int8_t guition_jc4880p443c_take_pinch_steps(void);

/**
 * Mount the first FAT partition through the board's four-bit SDMMC slot.
 * The function never formats a card when mounting fails.
 */
esp_err_t guition_jc4880p443c_sd_mount(guition_jc4880p443c_sd_info_t *info);

#ifdef __cplusplus
}
#endif
