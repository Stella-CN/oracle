# ESP32-S3 SD BSP Helper Optimization Plan

## Summary

- Target stack: ESP-IDF v6.0.1, ESP32-S3 SDMMC, FatFs/VFS, LVGL 9.x AnimatedGIF, and the local Waveshare BSP fork ported to IDF 6.
- Chosen strategy: use the BSP/ESP-IDF SD mount helper path and remove the application-owned MBR/GPT/SFD probing and FatFs diskio layer.
- Behavior change: SD cards must be mountable by `esp_vfs_fat_sdmmc_mount()` as FAT/FAT32. Custom partition-offset mounting and exFAT diagnostics are intentionally removed.

## References

- ESP-IDF v6.0.1 FatFs/VFS SD mount API: https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/api-reference/storage/fatfs.html
- ESP-IDF v6.0.1 SDMMC Host bus width: https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/api-reference/peripherals/sdmmc_host.html
- Waveshare ESP32-S3-Touch-LCD-1.85B upstream: https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85B

## Implementation Notes

- Add `bsp_sdcard_mount_with_width()` to the BSP and keep `bsp_sdcard_mount()` compatible.
- In `main/main.c`, mount SD through BSP helper with 4-bit first, then 1-bit fallback.
- Remove application-level SD sector parsing, FatFs diskio registration, and unused SD global state.
- Keep `CONFIG_LV_USE_GIF=y` because the manual player uses LVGL's `AnimatedGIF.h`; remove stale LVGL stdio FS settings.
- Remove the unused SPIFFS `assets` partition and expand the factory app partition.
- Harden GIF startup cleanup and keep disposal method 3 as an explicit low-memory degradation.

## Verification

- `git diff --check`: passed.
- Static cleanup check passed: `main/main.c` no longer contains `diskio_impl.h`, `sdmmc_cmd.h`, custom FatFs diskio registration, or the removed app-level SD probing symbols.
- Build passed with `IDF_PATH=/Users/lvjiaqing/.espressif/v6.0.1/esp-idf ninja -C build`.
  - Output image: `build/test.bin`.
  - App size: `0xb7090`; app partition size: `0xff0000`.
- Runtime checks still require hardware: SD mounted with FAT/FAT32, `/sdcard/assets/gif` scanning, missing SD, missing directory, empty directory, corrupt GIF, button switching, and console commands.

## Field Fix: SD Mount Failed / USB-Free Boot Stuck

- Observed log: `failed to mount card (13)` maps to FatFs `FR_NO_FILESYSTEM`.
- Root cause analysis:
  - `esp_vfs_fat_sdmmc_mount()` initialized and read the card, but FatFs did not find a directly mountable volume.
  - ESP-IDF v6.0.1 FatFs has MBR/SFD auto-scan, but GPT scanning is compiled out because `FF_LBA64=0`; exFAT is also compiled out with `FF_FS_EXFAT=0`.
  - The BSP LFN warning was stale: IDF 6 uses `CONFIG_FATFS_LFN_*`, not `CONFIG_FATFS_LONG_FILENAMES`.
  - `dbg_console_start()` ran before SD status update and was wrapped by `ESP_ERROR_CHECK()`, so a console-device failure could leave the display at `starting...`.
- Fix:
  - Keep the official `esp_vfs_fat_sdmmc_mount()` path first.
  - Add a BSP-contained offset fallback for FAT/FAT32 volumes in SFD/MBR/GPT when the official mount path fails.
  - Keep application SD logic simple: `main.c` still calls only the BSP helper.
  - Start SD mounting before the debug REPL and make debug console startup non-fatal.
  - Fix the stale long-filename warning condition.
- Build after fix passed with `IDF_PATH=/Users/lvjiaqing/.espressif/v6.0.1/esp-idf ninja -C build`.
  - Output image: `build/test.bin`.
  - App size: `0xb8300`; app partition size: `0xff0000`.

## Field Fix: Offset Fallback Stack Overflow

- Observed log: `***ERROR*** A stack overflow in task main has been detected` immediately after entering the BSP offset fallback.
- Root cause: the fallback probe path kept multiple 512-byte SD sector buffers on the main task stack (`sector0`, GPT header/table sector, and FAT VBR sector). Nested SDMMC/FatFs calls pushed the task over its stack limit.
- Fix: allocate SD sector probe buffers from DMA-capable internal heap with `heap_caps_calloc()` and release them on every exit path.
- Build after fix passed with `IDF_PATH=/Users/lvjiaqing/.espressif/v6.0.1/esp-idf ninja -C build`.
  - Output image: `build/test.bin`.
  - App size: `0xb8330`; app partition size: `0xff0000`.
