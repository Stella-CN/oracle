#pragma once

#include "esp_err.h"
#include "lvgl.h"

typedef struct {
    lv_obj_t *gif_layer;
    lv_obj_t *name_label;
} app_ui_t;

esp_err_t app_ui_create(app_ui_t *ui);
lv_obj_t *app_ui_gif_layer(const app_ui_t *ui);
void app_ui_set_status_locked(app_ui_t *ui, const char *text);
void app_ui_set_status(app_ui_t *ui, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void app_ui_set_gif_name_locked(app_ui_t *ui, int index, int total, const char *name);
void app_ui_set_load_failed_locked(app_ui_t *ui, const char *name);
