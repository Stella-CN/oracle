# gif-viewer 分支全面优化计划

## Summary

在 `feature/gif-viewer` 分支上进行深度重构，目标是把当前集中在 `main/main.c` 的 GIF 播放、SD 扫描、UI、控制台、状态管理拆分为职责清晰的模块，并将 GIF 数据源从“整文件读入 RAM”改为“基于 LVGL FS 的流式读取”。保留当前 factory 分区策略，不引入 OTA 分区调整。

参考资料：

- [ESP-IDF v6.0.1 Heap Memory Allocation](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/api-reference/system/mem_alloc.html)
- [ESP-IDF FatFs](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/storage/fatfs.html)
- [ESP-IDF SDMMC Host](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/sdmmc_host.html)
- [LVGL 9.4 Threading](https://lvgl.io/docs/open/9.4/details/integration/overview/threading)

## Public Interfaces And Types

新增/整理内部模块接口，保持应用对外行为不变：

- `gif_playlist`: 负责扫描 `/sdcard/assets/gif`、过滤 `.gif`、排序、保存绝对路径和 LVGL FS 路径。
- `gif_player`: 负责 `AnimatedGIF` 生命周期、LVGL image 对象、帧缓冲、timer、播放/暂停/切换/状态快照。
- `sd_media`: 负责 SD 卡挂载、4-bit 到 1-bit fallback、挂载状态和错误信息。
- `app_console`: 注册 `dbg_console` 命令，命令只读取线程安全快照或投递操作请求。
- 可选 `app_ui`: 如果 UI 标签、状态提示、锁屏逻辑仍然过长，则拆出轻量 UI 辅助层。

核心数据结构：

- `gif_playlist_item_t`: `name`、`sd_path`、`lvgl_path`、`size_bytes`。
- `gif_player_config_t`: LVGL parent、显示区域、loop 策略、内存 caps 策略。
- `gif_player_status_t`: 当前索引、文件名、尺寸、帧号、播放状态、heap/psram 余量、最近错误码。

## Key Changes

- 流式 GIF 解码：
  - 不再在正常播放路径中整文件 `malloc` 到 RAM。
  - 使用 LVGL FS stdio driver，传入 `S:<filename>` 给 `GIF_openFile`。
  - `sdkconfig.defaults` 固化 `CONFIG_LV_USE_FS_STDIO`、`CONFIG_LV_FS_STDIO_LETTER`、`CONFIG_LV_FS_STDIO_PATH`、`CONFIG_LV_FS_STDIO_CACHE_SIZE`。
  - 继续保留单帧 RGB565 frame buffer，优先用 PSRAM，没有 PSRAM 时明确降级或报错。
- 主流程重构：
  - `app_main` 只保留系统初始化、SD 初始化、播放列表加载、播放器启动、按键事件分发。
  - GIF 切换通过统一 API 执行，确保先停止 timer、关闭 decoder、释放旧 buffer，再打开新文件。
  - 播放状态集中在 `gif_player` 内部，外部读取必须通过快照函数，避免 console 与主循环直接并发访问共享指针。
- BSP 加固：
  - 补全 display/touch 初始化的错误传播。
  - `bsp_audio_init()` 失败清理后把 `i2s_tx_chan`、`i2s_rx_chan` 置空。
  - 对 I2C bus 创建、复用、销毁路径补齐返回值检查，避免半初始化状态继续运行。
- 依赖与构建可复现：
  - `.devcontainer/Dockerfile` 固定 `espressif/idf:v6.0.1`。
  - 根 `CMakeLists.txt` 固定 ESP32-S3 target。
  - BSP 组件 LVGL 约束收紧到 LVGL 9 系。
  - `dbg_console` 的 CMake 依赖补齐 `heap`，注册命令增加参数校验和错误日志。

## Test Plan

- 构建验证：
  - `IDF_PATH=/Users/lvjiaqing/.espressif/v6.0.1/esp-idf ninja -C build`
  - 若 CMake target 或 sdkconfig defaults 调整后需要重新配置，执行等价的 ESP-IDF reconfigure/build 流程。
- 静态检查：
  - 确认 `main/main.c` 显著瘦身，模块职责清晰。
  - `rg "load_gif_into_memory|source_data|source_size"` 确认整文件 RAM 播放路径已移除。
  - `rg "GIF_openFile|LV_FS_STDIO"` 确认流式路径启用。
- 运行场景：
  - SD 卡不存在：显示明确错误，console 可查看状态，不崩溃。
  - GIF 目录为空：显示空列表状态，不创建无效播放器。
  - 多个 GIF 切换：BOOT 按键和 console 命令均可切换，旧资源释放正常。
  - 大 GIF 文件：不出现整文件 RAM 峰值，heap/psram 余量稳定。
  - 损坏 GIF：打开失败后清理资源，保留可继续切换到下一个文件的状态。
  - 颜色验证：用红、绿、蓝三色测试 GIF 确认 RGB565 byte order 与屏幕显示一致。

## Assumptions

- 当前工作分支为 `feature/gif-viewer`。
- ESP-IDF 版本以本机 `/Users/lvjiaqing/.espressif/v6.0.1/esp-idf` 为准。
- GIF 文件仍位于 SD 卡 `/sdcard/assets/gif`。
- LVGL 版本固定在 9.x；当前代码依赖 LVGL 9 API，不兼容 LVGL 8。
- 每次代码改动完成后按 `git-workflow` 生成提交说明并提交。
