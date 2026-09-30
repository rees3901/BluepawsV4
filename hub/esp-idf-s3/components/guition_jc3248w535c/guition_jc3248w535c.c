/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board-level adapter for the GUITION JC3248W535C/EN. Pin assignments follow
 * the vendor JC3248W535 demo and are isolated here so the P4 BSP is untouched.
 */

#include "guition_jc3248w535c.h"

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/sdmmc_host.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include <string.h>

#define LCD_QSPI_HOST SPI2_HOST
#define LCD_SCK_GPIO 47
#define LCD_CS_GPIO 45
#define LCD_DATA0_GPIO 21
#define LCD_DATA1_GPIO 48
#define LCD_DATA2_GPIO 40
#define LCD_DATA3_GPIO 39
#define LCD_BACKLIGHT_GPIO 1
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_DRAW_BUFFER_PIXELS (GUITION_JC3248W535C_LANDSCAPE_WIDTH * 40)

#define TOUCH_I2C_PORT I2C_NUM_0
#define TOUCH_SDA_GPIO 4
#define TOUCH_SCL_GPIO 8
#define TOUCH_I2C_FREQUENCY_HZ 400000

#define SD_CLK_GPIO 12
#define SD_CMD_GPIO 11
#define SD_D0_GPIO 13

#define LCD_BACKLIGHT_TIMER LEDC_TIMER_1
#define LCD_BACKLIGHT_CHANNEL LEDC_CHANNEL_1

static const char *TAG = "jc3248w535c";
static esp_lcd_touch_handle_t touch;
static volatile uint8_t touch_contact_count;
static sdmmc_card_t *sd_card;
/* Exact LCD power, gate and gamma profile shipped for the JC3248W535C.
   The generic AXS15231B defaults target a different panel geometry. */
static const axs15231b_lcd_init_cmd_t jc3248w535c_init_cmds[] = {
    {0xBB, (uint8_t []){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5}, 8, 0},
    {0xA0, (uint8_t []){0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00}, 17, 0},
    {0xA2, (uint8_t []){0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80, 0x80, 0x80, 0x20, 0xf9, 0x10, 0x02, 0xff, 0xff, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A}, 31, 0},
    {0xD0, (uint8_t []){0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15, 0x42, 0xC2, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x60, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12}, 30, 0},
    {0xA3, (uint8_t []){0xA0, 0x06, 0xAa, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55}, 22, 0},
    {0xC1, (uint8_t []){0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0d, 0x00, 0xFF, 0x40}, 30, 0},
    {0xC3, (uint8_t []){0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01}, 11, 0},
    {0xC4, (uint8_t []){0x00, 0x24, 0x33, 0x80, 0x00, 0xea, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50}, 29, 0},
    {0xC5, (uint8_t []){0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00}, 23, 0},
    {0xC6, (uint8_t []){0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22}, 20, 0},
    {0xC7, (uint8_t []){0x50, 0x32, 0x28, 0x00, 0xa2, 0x80, 0x8f, 0x00, 0x80, 0xff, 0x07, 0x11, 0x9c, 0x67, 0xff, 0x24, 0x0c, 0x0d, 0x0e, 0x0f}, 20, 0},
    {0xC9, (uint8_t []){0x33, 0x44, 0x44, 0x01}, 4, 0},
    {0xCF, (uint8_t []){0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0x88, 0x00, 0x65, 0x09, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08}, 27, 0},
    {0xD5, (uint8_t []){0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92, 0x74, 0x04, 0x08, 0x6A, 0x04, 0x46, 0x03, 0x03, 0x03, 0x03, 0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00}, 30, 0},
    {0xD6, (uint8_t []){0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00, 0x01, 0x83, 0x07, 0x07, 0x00, 0x07, 0x07, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00}, 30, 0},
    {0xD7, (uint8_t []){0x03, 0x01, 0x0b, 0x09, 0x0f, 0x0d, 0x1E, 0x1F, 0x18, 0x1d, 0x1f, 0x19, 0x40, 0x8E, 0x04, 0x00, 0x20, 0xA0, 0x1F}, 19, 0},
    {0xD8, (uint8_t []){0x02, 0x00, 0x0a, 0x08, 0x0e, 0x0c, 0x1E, 0x1F, 0x18, 0x1d, 0x1f, 0x19}, 12, 0},
    {0xD9, (uint8_t []){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDD, (uint8_t []){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDF, (uint8_t []){0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90}, 8,  0},
    {0xE0, (uint8_t []){0x3B, 0x28, 0x10, 0x16, 0x0c, 0x06, 0x11, 0x28, 0x5c, 0x21, 0x0D, 0x35, 0x13, 0x2C, 0x33, 0x28, 0x0D}, 17, 0},
    {0xE1, (uint8_t []){0x37, 0x28, 0x10, 0x16, 0x0b, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14, 0x2C, 0x33, 0x28, 0x0F}, 17, 0},
    {0xE2, (uint8_t []){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE3, (uint8_t []){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F}, 17, 0},
    {0xE4, (uint8_t []){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE5, (uint8_t []){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0F}, 17, 0},
    {0xA4, (uint8_t []){0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30}, 16, 0},
    {0xA4, (uint8_t []){0x85, 0x85, 0x95, 0x85}, 4, 0},
    {0xBB, (uint8_t []){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 0},
    {0x13, (uint8_t []){0x00}, 0, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x2C, (uint8_t []){0x00, 0x00, 0x00, 0x00}, 4, 0},
};
static esp_lcd_panel_io_handle_t lcd_panel_io;
static esp_lcd_panel_handle_t lcd_panel;
static uint16_t *lcd_rotation_buffer;
static esp_err_t (*lcd_draw_bitmap_original)(esp_lcd_panel_t *panel,
                                             int x_start, int y_start,
                                             int x_end, int y_end,
                                             const void *color_data);

/* esp_lcd_axs15231b 2.1.0 skips RASET for QSPI partial updates. LVGL does not
   guarantee sequential top-to-bottom dirty rectangles, so the controller
   otherwise writes strips into stale rows and produces repeated columns/noise.
   Set the row window explicitly before delegating to the component driver. */
static esp_err_t lcd_draw_bitmap_with_row_window(esp_lcd_panel_t *panel,
                                                  int x_start, int y_start,
                                                  int x_end, int y_end,
                                                  const void *color_data)
{
    const uint8_t row_window[] = {
        (uint8_t)((y_start >> 8) & 0xff),
        (uint8_t)(y_start & 0xff),
        (uint8_t)(((y_end - 1) >> 8) & 0xff),
        (uint8_t)((y_end - 1) & 0xff),
    };
    const int qspi_raset_command = (0x02 << 24) | (0x2b << 8);
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(lcd_panel_io, qspi_raset_command,
                                  row_window, sizeof(row_window)),
        TAG, "AXS15231B row window");
    return lcd_draw_bitmap_original(panel, x_start, y_start, x_end, y_end,
                                    color_data);
}

/* GUITION's working BSP does not use MADCTL rotation on this panel. Convert
   each LVGL landscape dirty rectangle into the controller's native portrait
   memory layout, using a bounded DMA buffer. LVGL submits one flush at a time,
   and esp_lvgl_port signals completion from the panel IO callback before this
   buffer can be reused. */
static void lcd_landscape_flush(lv_display_t *display, const lv_area_t *area,
                                uint8_t *color_map)
{
    const int width = lv_area_get_width(area);
    const int height = lv_area_get_height(area);
    const uint16_t *source = (const uint16_t *)color_map;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint16_t pixel = source[y * width + x];
            /* The panel expects big-endian RGB565 on the wire. */
            lcd_rotation_buffer[x * height + (height - y - 1)] =
                (uint16_t)((pixel << 8) | (pixel >> 8));
        }
    }

    const int physical_x_start =
        GUITION_JC3248W535C_NATIVE_WIDTH - area->y2 - 1;
    const int physical_x_end =
        GUITION_JC3248W535C_NATIVE_WIDTH - area->y1;
    const int physical_y_start = area->x1;
    const int physical_y_end = area->x2 + 1;

    esp_err_t result = esp_lcd_panel_draw_bitmap(
        lcd_panel, physical_x_start, physical_y_start,
        physical_x_end, physical_y_end, lcd_rotation_buffer);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "LCD flush failed: %s", esp_err_to_name(result));
        lv_display_flush_ready(display);
    }
}

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
        .gpio_num = LCD_BACKLIGHT_GPIO,
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

esp_err_t guition_jc3248w535c_backlight_set(int brightness_percent)
{
    if (brightness_percent < 0) brightness_percent = 0;
    if (brightness_percent > 100) brightness_percent = 100;
    const uint32_t duty = (1023U * (uint32_t)brightness_percent) / 100U;
    ESP_RETURN_ON_ERROR(
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BACKLIGHT_CHANNEL, duty),
        TAG, "backlight duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BACKLIGHT_CHANNEL);
}

uint8_t guition_jc3248w535c_touch_count(void)
{
    return touch_contact_count;
}

static esp_err_t panel_new(esp_lcd_panel_handle_t *panel,
                           esp_lcd_panel_io_handle_t *panel_io)
{
    const spi_bus_config_t bus_config = AXS15231B_PANEL_BUS_QSPI_CONFIG(
        LCD_SCK_GPIO, LCD_DATA0_GPIO, LCD_DATA1_GPIO, LCD_DATA2_GPIO,
        LCD_DATA3_GPIO,
        GUITION_JC3248W535C_NATIVE_WIDTH * GUITION_JC3248W535C_NATIVE_HEIGHT *
            sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(LCD_QSPI_HOST, &bus_config, SPI_DMA_CH_AUTO),
        TAG, "QSPI bus");

    esp_lcd_panel_io_spi_config_t io_config =
        AXS15231B_PANEL_IO_QSPI_CONFIG(LCD_CS_GPIO, NULL, NULL);
    io_config.pclk_hz = LCD_PIXEL_CLOCK_HZ;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_QSPI_HOST,
                                 &io_config, panel_io),
        TAG, "AXS15231B QSPI IO");

    const axs15231b_vendor_config_t vendor_config = {
        .init_cmds = jc3248w535c_init_cmds,
        .init_cmds_size =
            sizeof(jc3248w535c_init_cmds) / sizeof(jc3248w535c_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_axs15231b(*panel_io, &panel_config, panel),
        TAG, "AXS15231B panel");
    lcd_panel_io = *panel_io;
    lcd_panel = *panel;
    lcd_draw_bitmap_original = (*panel)->draw_bitmap;
    (*panel)->draw_bitmap = lcd_draw_bitmap_with_row_window;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), TAG, "panel init");
    /* esp_lcd_axs15231b 2.1.0 retains the driver's legacy `off` parameter
       semantics behind esp_lcd_panel_disp_on_off(): false sends DISPON and
       true sends DISPOFF. Match the exact-board vendor BSP until the component
       normalises this API. */
    return esp_lcd_panel_disp_on_off(*panel, false);
}

static void touchpad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    esp_lcd_touch_point_data_t contacts[CONFIG_ESP_LCD_TOUCH_MAX_POINTS] = {0};
    uint8_t count = 0;

    esp_err_t result = esp_lcd_touch_read_data(touch);
    if (result == ESP_OK &&
        esp_lcd_touch_get_data(touch, contacts, &count,
                               CONFIG_ESP_LCD_TOUCH_MAX_POINTS) == ESP_OK) {
        touch_contact_count = count;
        if (count > 0) {
            /* LVGL is rotated 90 degrees clockwise in software. Convert the
               controller's native portrait coordinates into landscape. */
            data->point.x =
                GUITION_JC3248W535C_NATIVE_HEIGHT - 1 - contacts[0].y;
            data->point.y = contacts[0].x;
            data->state = LV_INDEV_STATE_PRESSED;
            return;
        }
    }
    touch_contact_count = 0;
    data->state = LV_INDEV_STATE_RELEASED;
}

static esp_err_t touch_new(lv_display_t *display)
{
    i2c_master_bus_handle_t bus = NULL;
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_SDA_GPIO,
        .scl_io_num = TOUCH_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bus), TAG, "touch I2C");

    esp_lcd_panel_io_handle_t touch_io = NULL;
    esp_lcd_panel_io_i2c_config_t io_config =
        ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG();
    io_config.scl_speed_hz = TOUCH_I2C_FREQUENCY_HZ;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(bus, &io_config, &touch_io), TAG, "touch IO");

    const esp_lcd_touch_config_t touch_config = {
        .x_max = GUITION_JC3248W535C_NATIVE_WIDTH,
        .y_max = GUITION_JC3248W535C_NATIVE_HEIGHT,
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
        esp_lcd_touch_new_i2c_axs15231b(touch_io, &touch_config, &touch),
        TAG, "AXS15231B touch");

    if (!lvgl_port_lock(0)) return ESP_ERR_TIMEOUT;
    lv_indev_t *indev = lv_indev_create();
    if (indev != NULL) {
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touchpad_read);
        lv_indev_set_display(indev, display);
    }
    lvgl_port_unlock();
    return indev == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

lv_display_t *guition_jc3248w535c_display_start(void)
{
    ESP_LOGI(TAG, "Starting GUITION JC3248W535C AXS15231B display/touch");
    if (backlight_init() != ESP_OK) return NULL;

    const lvgl_port_cfg_t lvgl_config = {
        .task_priority = 4,
        .task_stack = 8192,
        .task_affinity = 1,
        .task_max_sleep_ms = 100,
        .timer_period_ms = 5,
    };
    if (lvgl_port_init(&lvgl_config) != ESP_OK) return NULL;

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t panel_io = NULL;
    if (panel_new(&panel, &panel_io) != ESP_OK) return NULL;

    const lvgl_port_display_cfg_t display_config = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .buffer_size = LCD_DRAW_BUFFER_PIXELS,
        .double_buffer = true,
        .hres = GUITION_JC3248W535C_LANDSCAPE_WIDTH,
        .vres = GUITION_JC3248W535C_LANDSCAPE_HEIGHT,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            /* Rotation is performed by lcd_landscape_flush so the panel stays
               in its vendor-tested native portrait addressing mode. */
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
            .swap_bytes = false,
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    lv_display_t *display = lvgl_port_add_disp(&display_config);
    if (display == NULL) return NULL;

    lcd_rotation_buffer = heap_caps_malloc(
        LCD_DRAW_BUFFER_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (lcd_rotation_buffer == NULL) {
        ESP_LOGE(TAG, "Unable to allocate LCD rotation DMA buffer");
        return NULL;
    }
    lv_display_set_flush_cb(display, lcd_landscape_flush);

    if (touch_new(display) != ESP_OK) return NULL;
    if (guition_jc3248w535c_backlight_set(80) != ESP_OK) return NULL;

    ESP_LOGI(TAG, "LVGL ready at %" LV_PRId32 "x%" LV_PRId32,
             lv_display_get_horizontal_resolution(display),
             lv_display_get_vertical_resolution(display));
    return display;
}

esp_err_t guition_jc3248w535c_sd_mount(guition_jc3248w535c_sd_info_t *info)
{
    if (info == NULL) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));

    if (sd_card == NULL) {
        const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 10,
            .allocation_unit_size = 32 * 1024,
        };
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_config.width = 1;
        slot_config.clk = SD_CLK_GPIO;
        slot_config.cmd = SD_CMD_GPIO;
        slot_config.d0 = SD_D0_GPIO;
        slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        const esp_err_t result = esp_vfs_fat_sdmmc_mount(
            GUITION_JC3248W535C_SD_MOUNT_POINT, &host, &slot_config,
            &mount_config, &sd_card);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "SD mount unavailable: %s", esp_err_to_name(result));
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
    (void)esp_vfs_fat_info(GUITION_JC3248W535C_SD_MOUNT_POINT,
                           &info->volume_total_bytes,
                           &info->volume_free_bytes);
    ESP_LOGI(TAG, "SD mounted: %s, %llu MiB free",
             info->product_name,
             info->volume_free_bytes / (1024ULL * 1024ULL));
    return ESP_OK;
}
