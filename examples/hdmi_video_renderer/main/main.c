/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_private/esp_cache_private.h"
#include "esp_codec_dev.h"
#include "driver/gpio.h"
#include "hal/axi_icm_ll.h"
#include "bsp/esp-bsp.h"
#include "app_stream_adapter.h"
#include "sdkconfig.h"

static const char *TAG = "main";

#define DISPLAY_WIDTH  1920
#define DISPLAY_HEIGHT 1080

// Calculate buffer size based on RGB format configuration
#ifdef CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
#define DISPLAY_BUFFER_SIZE (DISPLAY_WIDTH * DISPLAY_HEIGHT * 3)  // RGB888: 3 bytes per pixel
#define BYTES_PER_PIXEL 3
#else
#define DISPLAY_BUFFER_SIZE (DISPLAY_WIDTH * DISPLAY_HEIGHT * 2)  // RGB565: 2 bytes per pixel
#define BYTES_PER_PIXEL 2
#endif

#define MP4_FILENAME   BSP_SD_MOUNT_POINT "/" CONFIG_MP4_FILENAME
#define ENABLE_LOOP_PLAYBACK    1    // Set to 0 to disable loop playback
#define NANO_SD_POWER_GPIO GPIO_NUM_45

static esp_lcd_panel_handle_t lcd_panel;
static esp_lcd_panel_io_handle_t lcd_io;
static void *lcd_buffer[CONFIG_BSP_LCD_DPI_BUFFER_NUMS];
static SemaphoreHandle_t trans_sem = NULL;
static app_stream_adapter_handle_t stream_adapter;
static esp_codec_dev_handle_t g_audio_dev = NULL;  // Global audio device handle

static void play_media_file(const char *filename);

static void prioritize_dsi_psram_reads(void)
{
    // 1080p60 RGB565 continuously reads about 249 MB/s from PSRAM. Give the
    // two DW-GDMA memory master ports the highest AXI QoS so JPEG DMA2D and
    // cached CPU accesses yield before the DSI FIFO runs dry.
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(0, 15, 15);
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(1, 15, 15);
    axi_icm_ll_set_dma2d_qos_arbiter_prio(0, 0);
    // JPEG output reaches PSRAM through the DMA2D write master. Smooth that
    // traffic so its long bursts cannot starve the continuous DSI scanout.
    axi_icm_ll_set_qos_burstiness(AXI_ICM_MASTER_DMA2D, 1, AXI_ICM_ACCESS_WRITE);
    axi_icm_ll_set_qos_peak_transaction_rate(AXI_ICM_MASTER_DMA2D, 3, 5,
                                             AXI_ICM_ACCESS_WRITE);
    ESP_LOGI(TAG, "AXI QoS: DW-GDMA priority=15; DMA2D write limited (burst=1, peak=1/16, average=1/64)");
}

static void enable_nano_sd_power(void)
{
    const gpio_config_t power_config = {
        .pin_bit_mask = 1ULL << NANO_SD_POWER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&power_config));
    ESP_ERROR_CHECK(gpio_set_level(NANO_SD_POWER_GPIO, 0));
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "ESP32-P4-Nano TF power enabled on GPIO%d (active low)", NANO_SD_POWER_GPIO);
}

static bool flush_dpi_panel_ready_callback(esp_lcd_panel_handle_t panel_io, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    BaseType_t taskAwake = pdFALSE;

    if (trans_sem) {
        xSemaphoreGiveFromISR(trans_sem, &taskAwake);
    }

    return false;
}

static esp_err_t display_decoded_frame(uint8_t *buffer, uint32_t buffer_size,
                                       uint32_t width, uint32_t height,
                                       uint32_t buffer_index, void *user_data)
{
#if CONFIG_USE_LCD_BUFFER
    // Drop a stale refresh notification before requesting the next framebuffer.
    xSemaphoreTake(trans_sem, 0);
#endif
    // The JPEG image can be 1920x1072. The adapter writes it directly into
    // the middle of this 1920x1080 framebuffer, leaving four black rows above
    // and below, while HDMI timing remains native 1080p60.
    esp_err_t ret = esp_lcd_panel_draw_bitmap(lcd_panel, 0, 0,
                                              DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                              buffer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to present framebuffer: %s", esp_err_to_name(ret));
        return ret;
    }
    xSemaphoreTake(trans_sem, portMAX_DELAY);

    return ESP_OK;
}

void app_main()
{
    ESP_LOGI(TAG, "Starting HDMI MP4 Player application");

    prioritize_dsi_psram_reads();

    // Initialize display
    bsp_display_config_t display_config = {
        .hdmi_resolution = BSP_HDMI_RES_1920x1080,
        .dsi_bus = {
            .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
            .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
        }
    };

    esp_err_t ret = bsp_display_new(&display_config, &lcd_panel, &lcd_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display: %s", esp_err_to_name(ret));
        return;
    }

    trans_sem = xSemaphoreCreateBinary();
    esp_lcd_dpi_panel_event_callbacks_t callbacks = {
#if CONFIG_USE_LCD_BUFFER
        .on_refresh_done = flush_dpi_panel_ready_callback,
#else
        .on_color_trans_done = flush_dpi_panel_ready_callback,
#endif
    };
    esp_lcd_dpi_panel_register_event_callbacks(lcd_panel, &callbacks, NULL);

#if CONFIG_USE_LCD_BUFFER
#if CONFIG_BSP_LCD_DPI_BUFFER_NUMS == 3
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(lcd_panel, 3,
                    &lcd_buffer[0], &lcd_buffer[1], &lcd_buffer[2]));
#else
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(lcd_panel, 2,
                    &lcd_buffer[0], &lcd_buffer[1]));
#endif
#else
    size_t data_cache_line_size = 0;
    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size));
    for (int i = 0; i < CONFIG_BSP_LCD_DPI_BUFFER_NUMS; i++) {
        lcd_buffer[i] = heap_caps_aligned_calloc(data_cache_line_size, 1, DISPLAY_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        if (lcd_buffer[i] == NULL) {
            ESP_LOGE(TAG, "Failed to allocate buffer %d", i);
            return;
        }
    }
#endif

    for (int i = 0; i < CONFIG_BSP_LCD_DPI_BUFFER_NUMS; i++) {
        // Keep the eight rows outside a 1920x1072 MJPEG frame black. The
        // decoder overwrites only the centered video area on every frame.
        memset(lcd_buffer[i], 0, DISPLAY_BUFFER_SIZE);
    }

    enable_nano_sd_power();
    ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card");
        return;
    }

    // Initialize audio codec
    ESP_LOGI(TAG, "Initializing audio codec...");
    esp_codec_dev_handle_t audio_dev = bsp_audio_codec_speaker_init();
    if (audio_dev == NULL) {
        ESP_LOGW(TAG, "Failed to initialize audio codec, continuing without audio");
        g_audio_dev = NULL;
    } else {
        ESP_LOGI(TAG, "Audio codec initialized successfully");
        esp_codec_dev_set_out_vol(audio_dev, 60);
        g_audio_dev = audio_dev;
    }

    // Initialize stream adapter with unified configuration
    ESP_LOGI(TAG, "Initializing stream adapter...");

    // User data can be any context information needed by the callback
    void *custom_user_data = NULL;  // Can be any struct pointer or data

    app_stream_adapter_config_t adapter_config = {
        .frame_cb = display_decoded_frame,
        .user_data = custom_user_data,          // Pass user data to callback
        .decode_buffers = lcd_buffer,
        .buffer_count = CONFIG_BSP_LCD_DPI_BUFFER_NUMS,
        .buffer_size = DISPLAY_BUFFER_SIZE,
        .output_width = DISPLAY_WIDTH,
        .output_height = DISPLAY_HEIGHT,
        .audio_dev = audio_dev,
#ifdef CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
        .jpeg_config = APP_STREAM_JPEG_CONFIG_DEFAULT_RGB888()
#else
        .jpeg_config = APP_STREAM_JPEG_CONFIG_DEFAULT_RGB565()
#endif
    };

    ret = app_stream_adapter_init(&adapter_config, &stream_adapter);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize stream adapter: %d", ret);
        if (audio_dev) {
            esp_codec_dev_close(audio_dev);
        }
        return;
    }

    const char *format_str = (adapter_config.jpeg_config.output_format == APP_STREAM_JPEG_OUTPUT_RGB888) ? "RGB888" : "RGB565";
    ESP_LOGI(TAG, "Stream adapter initialized with %s format%s", format_str,
             audio_dev ? " and audio support" : "");

    FILE *fp = fopen(MP4_FILENAME, "rb");
    if (fp) {
        fclose(fp);

        app_stream_adapter_set_file(stream_adapter, MP4_FILENAME, g_audio_dev != NULL);
        play_media_file(MP4_FILENAME);
    } else {
        ESP_LOGW(TAG, "MP4 file not found: %s", MP4_FILENAME);
    }
}

static void play_media_file(const char *filename)
{
#if ENABLE_LOOP_PLAYBACK
    ESP_LOGI(TAG, "Starting loop playback of %s", filename);
#else
    ESP_LOGI(TAG, "Playing %s", filename);
#endif

    // Get stream info once
    uint32_t width, height, fps, duration;
    esp_err_t ret = app_stream_adapter_get_info(stream_adapter, &width, &height, &fps, &duration);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Media info: %"PRIu32"x%"PRIu32", %"PRIu32" fps, duration: %"PRIu32" ms",
                 width, height, fps, duration);
    }

#if ENABLE_LOOP_PLAYBACK
    uint32_t loop_count = 0;

    // Continuous playback loop
    while (1) {
        ESP_LOGI(TAG, "Starting playback loop #%u", ++loop_count);

        // Start playback
        ret = app_stream_adapter_start(stream_adapter);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start playback: %d", ret);
            break;
        }

        // Monitor playback with improved detection
        uint32_t stable_count = 0;
        uint32_t last_frames = 0;

        while (1) {
            app_stream_stats_t stats;
            ret = app_stream_adapter_get_stats(stream_adapter, &stats);

            if (ret == ESP_OK) {
                if (stats.frames_processed == last_frames) {
                    stable_count++;
                    // A 1080p first frame can take longer than steady-state
                    // decoding. Wait through the decoder timeout before
                    // declaring startup failure; after frames have arrived,
                    // three unchanged samples still indicate end-of-stream.
                    const uint32_t stable_limit = (last_frames == 0) ? 14 : 3;
                    if (stable_count >= stable_limit) {
                        if (last_frames == 0) {
                            ESP_LOGE(TAG, "No video frame decoded within 7 seconds");
                        } else {
                            ESP_LOGI(TAG, "Playback loop #%u finished (%u frames), restarting...",
                                     loop_count, stats.frames_processed);
                        }
                        break;
                    }
                } else {
                    stable_count = 0;
                    last_frames = stats.frames_processed;
                }
            } else {
                ESP_LOGW(TAG, "Error getting stats, assuming playback ended");
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(500)); // Check every 500ms
        }

        // Stop and reset for next loop
        ret = app_stream_adapter_stop(stream_adapter);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop playback safely: %s", esp_err_to_name(ret));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200)); // Brief pause
        app_stream_adapter_set_file(stream_adapter, filename, g_audio_dev != NULL);
    }
#else
    // Single playback mode
    ret = app_stream_adapter_start(stream_adapter);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start playback: %d", ret);
    }
#endif
}
