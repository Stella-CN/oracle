#pragma once

#include <stdbool.h>

typedef enum {
    APP_EVT_NEXT,
    APP_EVT_PREV,
    APP_EVT_GOTO,
} app_evt_type_t;

typedef struct {
    app_evt_type_t type;
    int index;
} app_evt_t;

typedef bool (*app_post_event_cb_t)(app_evt_type_t type, int index, void *ctx);
