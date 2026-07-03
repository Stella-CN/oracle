# ESP32-S3 SD Card GIF Playback Fix Plan

## Summary

- Move GIF resources from flash/mmap assets to the SD card path `/sdcard/assets/gif`.
- Fix the black GIF area while labels and button switching still work.
- Root cause: each 300x300 RGB565 GIF frame needs about 180 KB, but the project used LVGL's built-in 64 KB heap, so `lv_gif` could fail to allocate its frame buffer without visible diagnostics because LVGL logging was disabled.

## Key Changes

- Use SD-only storage:
  - Mount the microSD card with the BSP SDMMC/FATFS helper.
  - Scan `/sdcard/assets/gif` at startup.
  - Sort `.gif` files by name and build a runtime playlist.
  - Remove app dependency on `esp_mmap_assets`, `esp_lv_fs`, `mmap_generate_assets.h`, and the flash assets partition generation step.
- Use LVGL stdio filesystem:
  - Enable LVGL stdio FS with drive letter `S:`.
  - Map `S:<file>` to `/sdcard/assets/gif/<file>`.
- Fix and harden GIF playback:
  - Switch LVGL allocation to C library malloc so allocations can use ESP-IDF heap/PSRAM.
  - Enable LVGL warn logging.
  - Check `lv_gif_is_loaded()` after setting the source.
  - Guard empty lists, path overflow, malformed `goto` arguments, and REPL registration order.

## Interfaces / Config

- SD card GIF directory: `/sdcard/assets/gif`.
- LVGL source path format: `S:<gif-name>`.
- SD card format requirement: FAT32 with `.gif` files in `/sdcard/assets/gif`; extension matching is case-insensitive.
- No public API is added.

## Test Plan

- Build with `IDF_PATH=/Users/lvjiaqing/.espressif/v6.0.1/esp-idf ninja -C build`.
- Confirm `assets.bin` is no longer generated or flashed.
- Confirm `main` no longer depends on `esp_mmap_assets` or `esp_lv_fs`.
- Runtime checks:
  - SD card mounted successfully.
  - GIF files are found and listed.
  - Screen displays GIF animation, not just the black background and label.
  - BOOT button and console commands `list`, `next`, `prev`, `goto <index>` work.
  - Missing SD card, empty directory, corrupted GIF, and overlong filenames fail cleanly without crashing.

## References

- Waveshare ESP32-S3-Touch-LCD-1.85B hardware documentation: https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.85B/
- ESP-IDF FatFS/VFS documentation: https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/api-reference/storage/fatfs.html
- ESP-IDF SDMMC Host documentation: https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/api-reference/peripherals/sdmmc_host.html
- LVGL GIF source behavior: string sources are opened through `GIF_openFile()`.
