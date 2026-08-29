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
#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

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

static const char *TAG = "guition_board";
static esp_ldo_channel_handle_t dsi_ldo;
static i2c_master_bus_handle_t touch_i2c;
static esp_lcd_touch_handle_t touch;

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
    dpi_config.num_fbs = 1;

    const st7701_vendor_config_t vendor_config = {
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
    return esp_lcd_panel_init(*panel);
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

    const lvgl_port_touch_cfg_t lvgl_touch_config = {
        .disp = display,
        .handle = touch,
    };
    return lvgl_port_add_touch(&lvgl_touch_config) == NULL ? ESP_FAIL : ESP_OK;
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
            .buff_spiram = false,
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

    if (guition_jc4880p443c_backlight_set(80) != ESP_OK) {
        return NULL;
    }
    return display;
}
