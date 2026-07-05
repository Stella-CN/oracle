#include "app_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "app_config.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "app_ui";

static void align_status_label(app_ui_t *ui)
{
    if (ui == NULL || ui->name_label == NULL) {
        return;
    }

    lv_obj_align(ui->name_label, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_move_to_index(ui->name_label, -1);
}

esp_err_t app_ui_create(app_ui_t *ui)
{
    if (ui == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(ui, 0, sizeof(*ui));

    esp_err_t err = bsp_display_lock(APP_LVGL_LOCK_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to lock display for UI create: %s", esp_err_to_name(err));
        return err;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    ui->gif_layer = lv_obj_create(screen);
    lv_obj_remove_style_all(ui->gif_layer);
    lv_obj_set_size(ui->gif_layer, LV_PCT(100), LV_PCT(100));
    lv_obj_align(ui->gif_layer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ui->gif_layer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui->gif_layer, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ui->gif_layer, LV_OBJ_FLAG_SCROLLABLE);

    ui->name_label = lv_label_create(screen);
    lv_obj_set_style_text_color(ui->name_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(ui->name_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui->name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ui->name_label, LV_PCT(95));
    app_ui_set_status_locked(ui, "starting...");

    bsp_display_unlock();
    return ESP_OK;
}

lv_obj_t *app_ui_gif_layer(const app_ui_t *ui)
{
    return (ui != NULL) ? ui->gif_layer : NULL;
}

void app_ui_set_status_locked(app_ui_t *ui, const char *text)
{
    if (ui == NULL || ui->name_label == NULL) {
        return;
    }

    lv_label_set_text(ui->name_label, (text != NULL) ? text : "");
    align_status_label(ui);
}

void app_ui_set_status(app_ui_t *ui, const char *fmt, ...)
{
    if (ui == NULL || fmt == NULL) {
        return;
    }

    char text[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    if (bsp_display_lock(APP_LVGL_LOCK_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to lock display, skip status update");
        return;
    }
    app_ui_set_status_locked(ui, text);
    bsp_display_unlock();
}

void app_ui_set_gif_name_locked(app_ui_t *ui, int index, int total, const char *name)
{
    if (ui == NULL || ui->name_label == NULL) {
        return;
    }

    lv_label_set_text_fmt(ui->name_label, "%d/%d %s", index + 1, total,
                          (name != NULL) ? name : "");
    align_status_label(ui);
}

void app_ui_set_load_failed_locked(app_ui_t *ui, const char *name)
{
    if (ui == NULL || ui->name_label == NULL) {
        return;
    }

    lv_label_set_text_fmt(ui->name_label, "load failed: %s",
                          (name != NULL) ? name : "unknown");
    align_status_label(ui);
}
