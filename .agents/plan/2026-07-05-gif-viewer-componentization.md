# GIF Viewer App 组件化改造计划

## Summary
- 将当前 `main` 中的 GIF 读取、SD 扫描、播放、UI、按键、串口控制台和事件循环整体迁移为 `components/gif_viewer_app`。
- 保留入口文件为 `main/main.c`，只负责调用新组件启动接口。
- 新组件采用非阻塞启动：`gif_viewer_app_start()` 创建内部 FreeRTOS task 后返回，其他程序可继续运行并调用控制接口。
- 参考依据：ESP-IDF 官方建议大量项目代码拆入 components；组件 manifest 应放在组件根目录；如未来改 `.cpp`，`app_main` 需 C linkage。

## Key Changes
- 新建 `components/gif_viewer_app`：
  - 迁移现有 `app_console/app_ui/gif_player/gif_playlist/sd_media/app_events/app_config` 相关文件。
  - 新增公开头文件 `include/gif_viewer_app.h`，内部实现文件 `gif_viewer_app.c`。
  - 新增 `CMakeLists.txt`，声明 `REQUIRES waveshare__esp32_s3_touch_lcd_1_85B dbg_console heap`。
  - 将现有 `main/idf_component.yml` 的 Waveshare BSP override 迁移到新组件 manifest，路径改为 `../waveshare__esp32_s3_touch_lcd_1_85B`。
- 简化 `main`：
  - `main/CMakeLists.txt` 只编译 `main.c`，依赖 `gif_viewer_app`。
  - `main/main.c` 只包含日志和 `ESP_ERROR_CHECK(gif_viewer_app_start(NULL));`。
  - 不改为 `main.cpp`，因为已确认本次保留 `main.c`。
- 公开 API：
  - `gif_viewer_app_config_t`，支持 `enable_boot_button`、`enable_console`、`event_queue_length`、`task_stack_size`、`task_priority`、`task_core_id`、`initial_index`。
  - `GIF_VIEWER_APP_DEFAULT_CONFIG()` 提供默认配置：按键/控制台开启、队列长度 8、任务栈 6144、优先级低于 LVGL task、无核心绑定、初始索引 0。
  - `gif_viewer_app_start()`、`gif_viewer_app_next()`、`gif_viewer_app_prev()`、`gif_viewer_app_show_index()`、`gif_viewer_app_get_status()`。

## Implementation Notes
- `gif_viewer_app.c` 持有当前 `main.c` 中的静态状态：事件队列、状态锁、播放列表、SD 状态、UI、播放器、当前索引、task handle。
- 原 `app_main` 初始化流程迁入内部 task：初始化 I2C/display/UI/player/button，挂载 SD 并扫描，播放首张 GIF，启动控制台，进入事件循环。
- 原 `post_event()` 改为返回 `esp_err_t` 的内部函数；给 `app_console` 适配现有 bool callback，避免扩大控制台模块改动。
- `gif_viewer_app_status_t` 不暴露内部结构体，复制必要状态：生命周期、媒体状态、GIF 数量、当前索引、当前文件名、画布尺寸、帧号、最后错误、堆余量。
- 保持现有 LVGL lock 规则：所有 LVGL 对象创建、播放切换和 UI 更新仍在 `bsp_display_lock()` 保护下执行。
- 不顺手修复 GIF disposal/空帧等 review 中发现的问题；本次只做组件化，避免混入行为改动。

## Test Plan
- 构建：`IDF_PATH=/Users/lvjiaqing/.espressif/v6.0.1/esp-idf ninja -C build`。
- 检查 CMake 重新配置后 `main` 只依赖 `gif_viewer_app`，新组件被正常编译链接。
- 检查 `main/main.c` 是否只保留启动逻辑，无 GIF 业务状态机。
- 静态检查公开 API：C/C++ 兼容头文件使用 `extern "C"` guard。
- 运行级验收需要上板确认：开机显示首张 GIF、BOOT 下一张、串口 `next/prev/goto/list/status` 正常。
- Git 工作流：当前分支 `feature/gif-viewer` 可直接提交；验证通过后仅 stage 相关文件，建议提交信息 `feat: expose gif viewer app component`。
