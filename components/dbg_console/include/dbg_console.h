/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * dbg_console - 通用串口调试助手组件
 *
 * 基于 esp_console REPL，与 esp_log 共用同一个控制台串口，
 * 提供“设备运行时发送指令 + 接收 log”的双向调试通道。
 *
 * 特性:
 *  - 一行代码启动交互式命令行 (REPL)，自动适配 UART / USB-Serial-JTAG / USB-CDC
 *  - 提供简洁的自定义命令注册 API，业务层可随时扩展指令
 *  - 内置常用系统调试命令: free / restart / version / loglevel
 *  - 命令回调在独立 REPL 任务上下文执行，不阻塞业务任务
 */

#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_console.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 命令处理函数原型 (与 esp_console 保持一致)
 *
 * @param argc 参数个数 (含命令名本身)
 * @param argv 参数列表
 * @return 0 表示成功，非 0 表示失败 (会在控制台提示)
 */
typedef esp_console_cmd_func_t dbg_console_cmd_func_t;

/**
 * @brief REPL 启动前的命令注册回调
 *
 * 回调在 esp_console REPL 创建之后、esp_console_start_repl() 之前执行。
 * 适合注册业务命令，避免 REPL 已启动但 help 暂时看不到业务命令的窗口期。
 */
typedef esp_err_t (*dbg_console_register_hook_t)(void *user_ctx);

/**
 * @brief 调试控制台配置
 */
typedef struct {
    const char *prompt;          /*!< 提示符，NULL 则使用 "dbg> " */
    size_t max_cmdline_length;   /*!< 命令行最大长度，0 则使用 256 */
    bool register_system_cmds;   /*!< 是否注册内置系统命令 (free/restart/version/loglevel) */
    dbg_console_register_hook_t register_user_cmds; /*!< 可选业务命令注册回调 */
    void *user_ctx;              /*!< 传给 register_user_cmds 的上下文 */
} dbg_console_config_t;

/** @brief 默认配置 */
#define DBG_CONSOLE_DEFAULT_CONFIG()        \
    {                                       \
        .prompt = "dbg> ",                  \
        .max_cmdline_length = 256,          \
        .register_system_cmds = true,       \
        .register_user_cmds = NULL,         \
        .user_ctx = NULL,                   \
    }

/**
 * @brief 启动调试控制台 (REPL)
 *
 * 根据 sdkconfig 中的控制台设备配置自动选择 UART / USB-Serial-JTAG / USB-CDC。
 * 启动后即可在串口终端中输入命令，输入 help 查看全部命令。
 *
 * @param config 配置，传 NULL 使用默认配置
 * @return
 *  - ESP_OK              成功
 *  - ESP_ERR_INVALID_STATE 已经启动
 *  - 其他                底层 esp_console 错误
 */
esp_err_t dbg_console_start(const dbg_console_config_t *config);

/**
 * @brief 注册一条自定义调试命令
 *
 * 可以在 dbg_console_start() 的 register_user_cmds 回调中调用，也可以在启动后动态调用。
 *
 * @param name 命令名 (如 "next")
 * @param help help 中显示的说明文字
 * @param hint 参数提示 (如 "<index>")，无参数可传 NULL
 * @param func 命令处理函数
 * @return ESP_OK 成功；其他为 esp_console 错误码
 */
esp_err_t dbg_console_register_cmd(const char *name, const char *help,
                                   const char *hint, dbg_console_cmd_func_t func);

/**
 * @brief 打印一行到控制台 (printf 风格)
 *
 * 命令回调中输出查询结果时使用，输出不带 log 前缀。
 */
int dbg_console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
