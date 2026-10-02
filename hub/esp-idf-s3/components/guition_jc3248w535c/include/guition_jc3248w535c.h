/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GUITION_JC3248W535C_NATIVE_WIDTH 320
#define GUITION_JC3248W535C_NATIVE_HEIGHT 480
#define GUITION_JC3248W535C_LANDSCAPE_WIDTH 480
#define GUITION_JC3248W535C_LANDSCAPE_HEIGHT 320
#define GUITION_JC3248W535C_SD_MOUNT_POINT "/sdcard"

typedef struct {
    bool mounted;
    uint64_t card_capacity_bytes;
    uint64_t volume_total_bytes;
    uint64_t volume_free_bytes;
    uint32_t sector_size_bytes;
    uint32_t frequency_khz;
    char product_name[9];
} guition_jc3248w535c_sd_info_t;

/** Start AXS15231B display, capacitive touch and LVGL in landscape. */
lv_display_t *guition_jc3248w535c_display_start(void);

/** Set the PWM backlight from 0 to 100 percent. */
esp_err_t guition_jc3248w535c_backlight_set(int brightness_percent);

/** Most recently sampled touch-contact count. */
uint8_t guition_jc3248w535c_touch_count(void);

/** Consume one or more two-finger map zoom steps reported by the touch panel. */
int8_t guition_jc3248w535c_take_pinch_steps(void);

/** Mount the board's one-bit SDMMC slot without formatting on failure. */
esp_err_t guition_jc3248w535c_sd_mount(guition_jc3248w535c_sd_info_t *info);

#ifdef __cplusplus
}
#endif
