#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "app_main";

#define APP_LCD_BRIGHTNESS_PERCENT 80

static void create_status_screen(void)
{
    ESP_ERROR_CHECK(bsp_display_lock(0));

    lv_obj_t *screen =
#if LVGL_VERSION_MAJOR >= 9
        lv_screen_active();
#else
        lv_scr_act();
#endif

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);

    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "ESP32-S3 Touch LCD 1.85B\nBSP ready");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);

    bsp_display_unlock();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32-S3-Touch-LCD-1.85B BSP framework");

    ESP_ERROR_CHECK(bsp_i2c_init());

    lv_display_t *display = bsp_display_start();
    if (display == NULL) {
        ESP_LOGE(TAG, "Failed to initialize display");
        return;
    }

    ESP_ERROR_CHECK(bsp_display_brightness_set(APP_LCD_BRIGHTNESS_PERCENT));
    create_status_screen();

    button_handle_t buttons[BSP_BUTTON_NUM] = {0};
    int button_count = 0;
    esp_err_t ret = bsp_iot_button_create(buttons, &button_count, BSP_BUTTON_NUM);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Initialized %d button(s)", button_count);
    } else {
        ESP_LOGW(TAG, "BOOT button initialization failed: %s", esp_err_to_name(ret));
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
