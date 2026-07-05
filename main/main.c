#include "esp_err.h"
#include "esp_log.h"

#include "gif_viewer_app.h"

static const char *TAG = "oracle_main";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting GIF viewer app");
    ESP_ERROR_CHECK(gif_viewer_app_start(NULL));
}
