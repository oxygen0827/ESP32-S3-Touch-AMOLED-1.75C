# Brookesia

[English](brookesia.md) | [简体中文](brookesia_ZH.md)

`examples/esp-idf/03_esp-brookesia/` 是由本仓库维护源码的富界面示例，包含在
ESP-IDF CI 中。

更新共用 UI、LVGL、显示、触摸或音频依赖前，应先通过 CI 检查 Brookesia 兼容性。
如果后续上游 Brookesia 版本改变了支持的 ESP-IDF 范围，应同步更新示例清单和本文档。

## 运行说明

Brookesia 和富 LVGL 示例使用与 ESP-IDF v6 兼容的 LVGL/Brookesia 自定义内存路径。
除非已经在两个 ESP-IDF CI 版本上重新进行硬件验证，否则应保持
`CONFIG_LV_MEM_CUSTOM` 和 `CONFIG_ESP_BROOKESIA_MEMORY_USE_CUSTOM` 启用。

`examples/esp-idf/03_esp-brookesia/` 保留在 CI 中，用于编译和产物覆盖。即使 CI
通过，显示、触摸、PMU 或传感器初始化等运行问题仍需在开发板上验证。
