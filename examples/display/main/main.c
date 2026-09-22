/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/**
 * @file
 * @brief BSP Display Example
 * @details Show an image on the screen with a simple startup animation (LVGL)
 * @example https://espressif.github.io/esp-launchpad/?flashConfigURL=https://espressif.github.io/esp-bsp/config.toml&app=display-
 */

#include <stdio.h>
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_log.h"
#if CONFIG_BSP_HDMI_LT9611
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_lt9611.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

static int sw_sda_gpio = BSP_I2C_SDA;
static int sw_scl_gpio = BSP_I2C_SCL;

static void sw_sda(int level)
{
    gpio_set_level(sw_sda_gpio, level);
    esp_rom_delay_us(10);
}

static void sw_scl(int level)
{
    gpio_set_level(sw_scl_gpio, level);
    esp_rom_delay_us(10);
}

static void sw_start(void)
{
    sw_sda(1);
    sw_scl(1);
    sw_sda(0);
    sw_scl(0);
}

static void sw_stop(void)
{
    sw_sda(0);
    sw_scl(1);
    sw_sda(1);
}

static bool sw_write_byte(uint8_t data)
{
    for (int bit = 7; bit >= 0; --bit) {
        sw_sda((data >> bit) & 1);
        sw_scl(1);
        sw_scl(0);
    }
    sw_sda(1); // Release SDA for the slave ACK.
    sw_scl(1);
    bool ack = gpio_get_level(sw_sda_gpio) == 0;
    sw_scl(0);
    return ack;
}

static bool sw_scan(int sda_gpio, int scl_gpio)
{
    sw_sda_gpio = sda_gpio;
    sw_scl_gpio = scl_gpio;
    const gpio_config_t lines = {
        .pin_bit_mask = (1ULL << sda_gpio) | (1ULL << scl_gpio),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&lines));
    sw_sda(1);
    sw_scl(1);
    ESP_LOGI("lt9611_probe", "SW I2C SDA=GPIO%d SCL=GPIO%d idle: SDA=%d SCL=%d (~25kHz)",
             sda_gpio, scl_gpio, gpio_get_level(sda_gpio), gpio_get_level(scl_gpio));
    sw_scl(0);
    sw_sda(0);
    int sda_low = gpio_get_level(sda_gpio);
    sw_sda(1);
    int sda_high = gpio_get_level(sda_gpio);
    int scl_low = gpio_get_level(scl_gpio);
    sw_scl(1);
    int scl_high = gpio_get_level(scl_gpio);
    ESP_LOGI("lt9611_probe", "GPIO loopback: SDA drive0/read%d release1/read%d; SCL drive0/read%d release1/read%d",
             sda_low, sda_high, scl_low, scl_high);
    int found = 0;
    bool bridge = false;
    for (unsigned addr = 0x08; addr < 0x78; ++addr) {
        sw_start();
        bool ack = sw_write_byte(addr << 1);
        sw_stop();
        if (ack) {
            ESP_LOGI("lt9611_probe", "SW I2C ACK at 0x%02x", addr);
            ++found;
            if (addr == 0x3b) bridge = true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGI("lt9611_probe", "SW scan: %d device(s), 0x3b %s", found, bridge ? "ACK" : "no ACK");
    return bridge;
}

static bool lt9611_i2c_preflight(void)
{
    const int reset_gpio = CONFIG_BSP_LT9611_RESET_GPIO;
    if (reset_gpio >= 0) {
        const gpio_config_t reset_cfg = {
            .pin_bit_mask = 1ULL << reset_gpio,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_ERROR_CHECK(gpio_config(&reset_cfg));
        gpio_set_level(CONFIG_BSP_LT9611_RESET_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(CONFIG_BSP_LT9611_RESET_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    bool sw_bridge_ack = sw_scan(BSP_I2C_SDA, BSP_I2C_SCL);
    if (!sw_bridge_ack) {
        ESP_LOGW("lt9611_probe", "Trying SDA/SCL exchanged to check wiring");
        bool swapped_bridge_ack = sw_scan(BSP_I2C_SCL, BSP_I2C_SDA);
        if (swapped_bridge_ack) {
            ESP_LOGE("lt9611_probe", "Bridge ACK only with SDA/SCL exchanged; verify GPIO wiring before hardware I2C init");
        }
        ESP_LOGE("lt9611_probe", "Display init stopped: no bridge ACK on configured GPIOs after 1s reset-release delay");
        return false;
    }
    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE("lt9611_probe", "I2C bus init failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI("lt9611_probe", "I2C SDA GPIO%d=%d SCL GPIO%d=%d; reset GPIO%d released",
             BSP_I2C_SDA, gpio_get_level(BSP_I2C_SDA),
             BSP_I2C_SCL, gpio_get_level(BSP_I2C_SCL), CONFIG_BSP_LT9611_RESET_GPIO);
    int ack_count = 0;
    bool bridge_ack = false;
    for (unsigned addr = 0x08; addr < 0x78; ++addr) {
        ret = i2c_master_probe(bsp_i2c_get_handle(), addr, 30);
        if (ret == ESP_OK) {
            ESP_LOGI("lt9611_probe", "I2C ACK at 7-bit address 0x%02x", addr);
            ++ack_count;
            if (addr == 0x3b) bridge_ack = true;
        } else if (ret != ESP_ERR_NOT_FOUND) {
            ESP_LOGW("lt9611_probe", "Probe 0x%02x: %s", addr, esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGI("lt9611_probe", "Scan complete: %d device(s), 0x3b %s",
             ack_count, bridge_ack ? "ACK" : "no ACK");
    if (ack_count > 8) {
        ESP_LOGW("lt9611_probe", "Hardware probe result implausible (%d ACKs); use software scan as the check", ack_count);
    }
    if (!sw_bridge_ack) {
        ESP_LOGE("lt9611_probe", "Display init stopped: software I2C got no 0x3b ACK; check bridge power/reset/address/wiring");
    }
    return sw_bridge_ack;
}
#endif

extern void example_lvgl_demo_ui(lv_obj_t *scr);

void app_main(void)
{
#if CONFIG_BSP_HDMI_LT9611
    if (!lt9611_i2c_preflight()) {
        return;
    }
#endif
    if (bsp_display_start() == NULL) {
        ESP_LOGE("example", "Display initialization failed");
        return;
    }

    ESP_LOGI("example", "Display LVGL animation");
    bsp_display_lock(0);
    lv_obj_t *scr = lv_disp_get_scr_act(NULL);
#if (CONFIG_ESP_LCD_TOUCH_MAX_POINTS > 1 && CONFIG_LV_USE_GESTURE_RECOGNITION)
    lv_indev_t *indev = bsp_display_get_input_dev();
    lv_indev_set_rotation_rad_threshold(indev, 0.15f);
#endif
    example_lvgl_demo_ui(scr);

    bsp_display_unlock();
    bsp_display_backlight_on();
#if CONFIG_BSP_HDMI_LT9611
    // Read after LVGL has supplied frames; the init-time counters can still be zero.
    for (int sample = 1; sample <= 3; ++sample) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI("lt9611_probe", "Delayed MIPI RX sample %d/3", sample);
        esp_err_t status = esp_lcd_panel_lt9611_log_rx_status(bsp_display_get_panel_handle());
        if (status != ESP_OK) {
            ESP_LOGE("lt9611_probe", "Delayed RX status failed: %s", esp_err_to_name(status));
            break;
        }
    }
#endif
}
