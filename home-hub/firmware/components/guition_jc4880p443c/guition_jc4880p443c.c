/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 BluePaws contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * This focused adapter is derived from the ESP32-P4 Function EV Board BSP as
 * modified and distributed by GUITION for the JC4880P443C_I_W. See NOTICE.md.
 */

#include "guition_jc4880p443c.h"

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"

#include <string.h>

#define LCD_H_RES 480
#define LCD_V_RES 800
#define LCD_DRAW_BUFFER_PIXELS (LCD_H_RES * 50)
#define LCD_DSI_LANES 2
#define LCD_DSI_BIT_RATE_MBPS 750
#define LCD_DSI_LDO_CHANNEL 3
#define LCD_DSI_LDO_MILLIVOLTS 2500
#define LCD_BACKLIGHT_TIMER LEDC_TIMER_1
#define LCD_BACKLIGHT_CHANNEL LEDC_CHANNEL_1
#define TOUCH_I2C_PORT I2C_NUM_1
#define TOUCH_I2C_FREQUENCY_HZ 400000
#define SD_POWER_LDO_CHANNEL 4

static const char *TAG = "guition_board";
static esp_ldo_channel_handle_t dsi_ldo;
static i2c_master_bus_handle_t touch_i2c;
static esp_lcd_touch_handle_t touch;
static volatile uint8_t touch_contact_count;
static volatile int8_t pending_pinch_steps;
static volatile bool pending_quick_settings_swipe;
static uint64_t pinch_reference_squared;
static bool pinch_tracking;
static bool quick_settings_tracking;
static bool quick_settings_latched;
static int32_t quick_settings_start_x;
static int32_t quick_settings_start_y;
static sd_pwr_ctrl_handle_t sd_power;
static sdmmc_card_t *sd_card;

static uint8_t logged_contact_count = UINT8_MAX;

/* Exact panel sequence recovered from the factory image's matching GUITION tree. */
static const st7701_lcd_init_cmd_t factory_panel_init[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t[]){0x08}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x10, 0x08}, 2, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},
    {0xB0, (uint8_t[]){0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09,
                       0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71}, 16, 0},
    {0xB1, (uint8_t[]){0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08,
                       0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x5D}, 1, 0},
    {0xB1, (uint8_t[]){0x58}, 1, 0},
    {0xB2, (uint8_t[]){0x87}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x4E}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xB9, (uint8_t[]){0x10, 0x1F}, 2, 0},
    {0xBB, (uint8_t[]){0x03}, 1, 0},
    {0xBC, (uint8_t[]){0x00}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xD0, (uint8_t[]){0x88}, 1, 0},
    {0xE0, (uint8_t[]){0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0,
                       0x00, 0xA0, 0x00, 0x40, 0x40}, 11, 0},
    {0xE2, (uint8_t[]){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00,
                       0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0,
                       0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0,
                       0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},
    {0xEB, (uint8_t[]){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, (uint8_t[]){0x08, 0x01}, 2, 0},
    {0xED, (uint8_t[]){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF,
                       0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},
    {0xEF, (uint8_t[]){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 20},
};

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LCD_BACKLIGHT_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = GUITION_JC4880P443C_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_BACKLIGHT_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_BACKLIGHT_TIMER,
        .duty = 0,
        .hpoint = 0,
    };

    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    return ledc_channel_config(&channel);
}

esp_err_t guition_jc4880p443c_backlight_set(int brightness_percent)
{
    if (brightness_percent < 0) {
        brightness_percent = 0;
    } else if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    const uint32_t duty = (1023U * (uint32_t)brightness_percent) / 100U;
    ESP_RETURN_ON_ERROR(
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BACKLIGHT_CHANNEL, duty), TAG, "backlight duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BACKLIGHT_CHANNEL);
}

esp_err_t guition_jc4880p443c_sd_mount(guition_jc4880p443c_sd_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));

    if (sd_card == NULL) {
        const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 12,
            .allocation_unit_size = 32 * 1024,
        };

        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.slot = SDMMC_HOST_SLOT_0;
        host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

        const sd_pwr_ctrl_ldo_config_t power_config = {
            .ldo_chan_id = SD_POWER_LDO_CHANNEL,
        };
        esp_err_t result = sd_pwr_ctrl_new_on_chip_ldo(&power_config, &sd_power);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "SD power LDO failed: %s", esp_err_to_name(result));
            return result;
        }
        host.pwr_ctrl_handle = sd_power;

        const sdmmc_slot_config_t slot_config = {
            .cd = SDMMC_SLOT_NO_CD,
            .wp = SDMMC_SLOT_NO_WP,
            .width = 4,
            .flags = 0,
        };
        result = esp_vfs_fat_sdmmc_mount(
            GUITION_JC4880P443C_SD_MOUNT_POINT,
            &host,
            &slot_config,
            &mount_config,
            &sd_card);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "SD FAT mount failed: %s", esp_err_to_name(result));
            sd_pwr_ctrl_del_on_chip_ldo(sd_power);
            sd_power = NULL;
            return result;
        }
    }

    info->mounted = true;
    info->sector_size_bytes = (uint32_t)sd_card->csd.sector_size;
    info->card_capacity_bytes =
        (uint64_t)sd_card->csd.capacity * (uint64_t)sd_card->csd.sector_size;
    info->frequency_khz = (uint32_t)sd_card->real_freq_khz;
    memcpy(info->product_name, sd_card->cid.name, sizeof(sd_card->cid.name));
    info->product_name[sizeof(info->product_name) - 1] = '\0';

    const esp_err_t info_result = esp_vfs_fat_info(
        GUITION_JC4880P443C_SD_MOUNT_POINT,
        &info->volume_total_bytes,
        &info->volume_free_bytes);
    if (info_result != ESP_OK) {
        ESP_LOGW(TAG, "Could not read FAT capacity: %s", esp_err_to_name(info_result));
    }

    sdmmc_card_print_info(stdout, sd_card);
    ESP_LOGI(TAG,
             "SD mounted: product=%s card=%llu bytes FAT=%llu bytes free=%llu bytes",
             info->product_name,
             info->card_capacity_bytes,
             info->volume_total_bytes,
             info->volume_free_bytes);
    return ESP_OK;
}

static esp_err_t display_panel_new(esp_lcd_panel_handle_t *panel,
                                   esp_lcd_panel_io_handle_t *panel_io)
{
    const esp_ldo_channel_config_t ldo_config = {
        .chan_id = LCD_DSI_LDO_CHANNEL,
        .voltage_mv = LCD_DSI_LDO_MILLIVOLTS,
    };
    ESP_RETURN_ON_ERROR(
        esp_ldo_acquire_channel(&ldo_config, &dsi_ldo), TAG, "MIPI DSI PHY power");

    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    const esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = LCD_DSI_LANES,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = LCD_DSI_BIT_RATE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus), TAG, "MIPI DSI bus");

    const esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, panel_io), TAG, "MIPI DBI command IO");

    esp_lcd_dpi_panel_config_t dpi_config =
        ST7701_480_360_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    /*
     * GUITION ships a locally patched ST7701 component whose unfortunately
     * named 480x360 preset contains the timings for this board's 480x800
     * glass. Keep the registry component pristine and apply those
     * board-specific timings here instead.
     */
    dpi_config.dpi_clock_freq_mhz = 34;
    dpi_config.video_timing.h_size = LCD_H_RES;
    dpi_config.video_timing.v_size = LCD_V_RES;
    dpi_config.video_timing.hsync_back_porch = 42;
    dpi_config.video_timing.hsync_pulse_width = 12;
    dpi_config.video_timing.hsync_front_porch = 42;
    dpi_config.video_timing.vsync_back_porch = 8;
    dpi_config.video_timing.vsync_pulse_width = 2;
    dpi_config.video_timing.vsync_front_porch = 166;
    dpi_config.num_fbs = 1;
    /* Use synchronous CPU copying until pre-v3 DMA2D is validated on hardware. */
    dpi_config.flags.use_dma2d = false;

    const st7701_vendor_config_t vendor_config = {
        .init_cmds = factory_panel_init,
        .init_cmds_size = sizeof(factory_panel_init) / sizeof(factory_panel_init[0]),
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_config,
        },
        .flags = {
            .use_mipi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GUITION_JC4880P443C_LCD_RESET_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_st7701(*panel_io, &panel_config, panel), TAG, "ST7701 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), TAG, "ST7701 reset");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), TAG, "ST7701 init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(*panel, true), TAG, "ST7701 display on");
    return ESP_OK;
}

uint8_t guition_jc4880p443c_touch_count(void)
{
    return touch_contact_count;
}

int8_t guition_jc4880p443c_take_pinch_steps(void)
{
    const int8_t steps = pending_pinch_steps;
    pending_pinch_steps = 0;
    return steps;
}

bool guition_jc4880p443c_take_quick_settings_swipe(void)
{
    const bool pending = pending_quick_settings_swipe;
    pending_quick_settings_swipe = false;
    return pending;
}

static bool update_quick_settings_swipe(lv_display_t *display,
                                        const esp_lcd_touch_point_data_t *contacts,
                                        uint8_t contact_count)
{
    if (contact_count < 2) {
        quick_settings_tracking = false;
        quick_settings_latched = false;
        return false;
    }

    lv_point_t first = {.x = contacts[0].x, .y = contacts[0].y};
    lv_point_t second = {.x = contacts[1].x, .y = contacts[1].y};
    lv_display_rotate_point(display, &first);
    lv_display_rotate_point(display, &second);
    const int32_t centre_x = (first.x + second.x) / 2;
    const int32_t centre_y = (first.y + second.y) / 2;

    if (quick_settings_latched) {
        return true;
    }
    if (!quick_settings_tracking) {
        /* Both fingers must begin inside the top 64 displayed pixels. */
        if (first.y <= 64 && second.y <= 64) {
            quick_settings_tracking = true;
            quick_settings_start_x = centre_x;
            quick_settings_start_y = centre_y;
        }
        return quick_settings_tracking;
    }

    const int32_t travel_y = centre_y - quick_settings_start_y;
    const int32_t travel_x = centre_x - quick_settings_start_x;
    if (travel_y >= 72 && travel_x >= -120 && travel_x <= 120) {
        pending_quick_settings_swipe = true;
        quick_settings_tracking = false;
        quick_settings_latched = true;
        pending_pinch_steps = 0;
        ESP_LOGI(TAG, "GT911 two-finger quick-settings swipe");
    } else if (travel_y < -24 || travel_x < -140 || travel_x > 140) {
        quick_settings_tracking = false;
    }
    return quick_settings_tracking || quick_settings_latched;
}

static void update_pinch(const esp_lcd_touch_point_data_t *contacts, uint8_t contact_count)
{
    if (contact_count < 2) {
        pinch_tracking = false;
        pinch_reference_squared = 0;
        return;
    }

    const int32_t delta_x = (int32_t)contacts[0].x - (int32_t)contacts[1].x;
    const int32_t delta_y = (int32_t)contacts[0].y - (int32_t)contacts[1].y;
    const uint64_t distance_squared =
        (uint64_t)((int64_t)delta_x * delta_x + (int64_t)delta_y * delta_y);
    if (!pinch_tracking) {
        /* Ignore an unrealistically close initial pair, which is usually palm noise. */
        if (distance_squared >= 400) {
            pinch_reference_squared = distance_squared;
            pinch_tracking = true;
        }
        return;
    }

    int8_t step = 0;
    /* 1.20x separation zooms in; its reciprocal (0.833x) zooms out. */
    if (distance_squared * 100 >= pinch_reference_squared * 144) {
        step = 1;
    } else if (distance_squared * 144 <= pinch_reference_squared * 100) {
        step = -1;
    }
    if (step == 0) {
        return;
    }

    int next_steps = (int)pending_pinch_steps + step;
    if (next_steps > 4) {
        next_steps = 4;
    } else if (next_steps < -4) {
        next_steps = -4;
    }
    pending_pinch_steps = (int8_t)next_steps;
    pinch_reference_squared = distance_squared;
    ESP_LOGI(TAG, "GT911 pinch step: %s", step > 0 ? "zoom in" : "zoom out");
}

static void touchpad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    esp_lcd_touch_point_data_t contacts[CONFIG_ESP_LCD_TOUCH_MAX_POINTS] = {0};
    uint8_t contact_count = 0;
    esp_err_t result = esp_lcd_touch_read_data(touch);
    if (result == ESP_OK) {
        result = esp_lcd_touch_get_data(
            touch, contacts, &contact_count, CONFIG_ESP_LCD_TOUCH_MAX_POINTS);
    }
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "GT911 sample failed: %s", esp_err_to_name(result));
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    touch_contact_count = contact_count;
    const bool quick_settings_gesture =
        update_quick_settings_swipe(lv_indev_get_display(indev), contacts, contact_count);
    if (quick_settings_gesture) {
        pinch_tracking = false;
        pinch_reference_squared = 0;
    } else {
        update_pinch(contacts, contact_count);
    }
    if (contact_count != logged_contact_count) {
        if (contact_count >= 2) {
            ESP_LOGI(TAG,
                     "GT911 contacts: %u (track IDs %u, %u)",
                     contact_count,
                     contacts[0].track_id,
                     contacts[1].track_id);
        } else if (contact_count == 1) {
            ESP_LOGI(TAG, "GT911 contacts: 1 (track ID %u)", contacts[0].track_id);
        } else {
            ESP_LOGI(TAG, "GT911 contacts: 0");
        }
        logged_contact_count = contact_count;
    }

    if (contact_count > 0) {
        data->point.x = contacts[0].x;
        data->point.y = contacts[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static esp_err_t touch_new(lv_display_t *display)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = GUITION_JC4880P443C_TOUCH_SDA_GPIO,
        .scl_io_num = GUITION_JC4880P443C_TOUCH_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &touch_i2c), TAG, "touch I2C");

    esp_lcd_panel_io_handle_t touch_io = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_config.scl_speed_hz = TOUCH_I2C_FREQUENCY_HZ;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(touch_i2c, &io_config, &touch_io), TAG, "GT911 IO");

    const esp_lcd_touch_config_t touch_config = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_gt911(touch_io, &touch_config, &touch), TAG, "GT911 touch");

    if (!lvgl_port_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }
    lv_indev_t *indev = lv_indev_create();
    if (indev != NULL) {
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touchpad_read);
        lv_indev_set_display(indev, display);
    }
    lvgl_port_unlock();
    return indev == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

lv_display_t *guition_jc4880p443c_display_start(void)
{
    ESP_LOGI(TAG, "Starting JC4880P443C_I_W 480x800 ST7701/GT911 display");
    if (backlight_init() != ESP_OK) {
        return NULL;
    }

    const lvgl_port_cfg_t lvgl_config = {
        .task_priority = 4,
        .task_stack = 16384,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    if (lvgl_port_init(&lvgl_config) != ESP_OK) {
        return NULL;
    }

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t panel_io = NULL;
    if (display_panel_new(&panel, &panel_io) != ESP_OK) {
        return NULL;
    }

    const lvgl_port_display_cfg_t display_config = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .buffer_size = LCD_DRAW_BUFFER_PIXELS,
        .double_buffer = false,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = true,
            .mirror_x = false,
            .mirror_y = false,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = false,
            /* DMA2D requires cacheable memory; use the board's 32 MB PSRAM. */
            .buff_spiram = true,
            .swap_bytes = false,
            .sw_rotate = true,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_config = {
        .flags = {
            .avoid_tearing = false,
        },
    };
    lv_display_t *display = lvgl_port_add_disp_dsi(&display_config, &dsi_config);
    if (display == NULL || touch_new(display) != ESP_OK) {
        return NULL;
    }

    if (!lvgl_port_lock(0)) {
        return NULL;
    }
    lv_display_set_rotation(display, LV_DISPLAY_ROTATION_90);
    ESP_LOGI(TAG, "LVGL display resolution: %" LV_PRId32 "x%" LV_PRId32,
             lv_display_get_horizontal_resolution(display),
             lv_display_get_vertical_resolution(display));
    lvgl_port_unlock();

    if (guition_jc4880p443c_backlight_set(80) != ESP_OK) {
        return NULL;
    }
    return display;
}
