/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * dbg_console - 通用串口调试助手组件实现
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_console.h"

#include "dbg_console.h"

static const char *TAG = "dbg_console";

static esp_console_repl_t *s_repl = NULL;

/* ---------------------------------------------------------------------------
 * 内置系统命令
 * ------------------------------------------------------------------------- */

static int cmd_free(int argc, char **argv)
{
    dbg_console_printf("internal: free %u KB, min %u KB, largest block %u KB\n",
                       (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                       (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                       (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    dbg_console_printf("psram   : free %u KB, min %u KB, largest block %u KB\n",
                       (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                       (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024),
                       (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    return 0;
}

static int cmd_restart(int argc, char **argv)
{
    ESP_LOGW(TAG, "Restarting...");
    esp_restart();
    return 0;
}

static int cmd_version(int argc, char **argv)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    dbg_console_printf("app     : %s %s (compiled %s %s)\n",
                       app->project_name, app->version, app->date, app->time);
    dbg_console_printf("idf     : %s\n", esp_get_idf_version());
    dbg_console_printf("chip    : %s, %d core(s), rev v%d.%d\n",
                       CONFIG_IDF_TARGET, chip.cores,
                       chip.revision / 100, chip.revision % 100);
    return 0;
}

static int cmd_loglevel(int argc, char **argv)
{
    if (argc != 3) {
        dbg_console_printf("usage: loglevel <tag|*> <none|error|warn|info|debug|verbose>\n");
        return 1;
    }

    static const struct {
        const char *name;
        esp_log_level_t level;
    } level_map[] = {
        { "none", ESP_LOG_NONE },   { "error", ESP_LOG_ERROR },
        { "warn", ESP_LOG_WARN },   { "info", ESP_LOG_INFO },
        { "debug", ESP_LOG_DEBUG }, { "verbose", ESP_LOG_VERBOSE },
    };

    for (size_t i = 0; i < sizeof(level_map) / sizeof(level_map[0]); i++) {
        if (strcmp(argv[2], level_map[i].name) == 0) {
            esp_log_level_set(argv[1], level_map[i].level);
            dbg_console_printf("log level of '%s' set to %s\n", argv[1], argv[2]);
            return 0;
        }
    }

    dbg_console_printf("unknown level: %s\n", argv[2]);
    return 1;
}

static void register_system_cmds(void)
{
    dbg_console_register_cmd("free", "Show free heap (internal & PSRAM)", NULL, cmd_free);
    dbg_console_register_cmd("restart", "Software reset of the chip", NULL, cmd_restart);
    dbg_console_register_cmd("version", "Show app/IDF/chip version", NULL, cmd_version);
    dbg_console_register_cmd("loglevel", "Set log level of a tag",
                             "<tag|*> <none|error|warn|info|debug|verbose>", cmd_loglevel);
}

/* ---------------------------------------------------------------------------
 * 公共 API
 * ------------------------------------------------------------------------- */

esp_err_t dbg_console_register_cmd(const char *name, const char *help,
                                   const char *hint, dbg_console_cmd_func_t func)
{
    const esp_console_cmd_t cmd = {
        .command = name,
        .help = help,
        .hint = hint,
        .func = func,
    };
    esp_err_t err = esp_console_cmd_register(&cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register command '%s': %s", name, esp_err_to_name(err));
    }
    return err;
}

int dbg_console_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int ret = vprintf(fmt, args);
    va_end(args);
    return ret;
}

esp_err_t dbg_console_start(const dbg_console_config_t *config)
{
    if (s_repl != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    dbg_console_config_t default_cfg = DBG_CONSOLE_DEFAULT_CONFIG();
    if (config == NULL) {
        config = &default_cfg;
    }

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = (config->prompt != NULL) ? config->prompt : "dbg> ";
    repl_config.max_cmdline_length =
        (config->max_cmdline_length > 0) ? config->max_cmdline_length : 256;

    esp_err_t err;
#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    err = esp_console_new_repl_uart(&hw_config, &repl_config, &s_repl);
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &s_repl);
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_USB_CDC_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &s_repl);
#else
#error "No console device enabled in sdkconfig (ESP_CONSOLE_UART / USB_SERIAL_JTAG / USB_CDC)"
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create REPL: %s", esp_err_to_name(err));
        s_repl = NULL;
        return err;
    }

    if (config->register_system_cmds) {
        register_system_cmds();
    }

    if (config->register_user_cmds != NULL) {
        err = config->register_user_cmds(config->user_ctx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register user commands: %s", esp_err_to_name(err));
            s_repl->del(s_repl);
            s_repl = NULL;
            return err;
        }
    }

    err = esp_console_start_repl(s_repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start REPL: %s", esp_err_to_name(err));
        s_repl->del(s_repl);
        s_repl = NULL;
        return err;
    }

    ESP_LOGI(TAG, "Debug console started, type 'help' to list commands");
    return ESP_OK;
}
