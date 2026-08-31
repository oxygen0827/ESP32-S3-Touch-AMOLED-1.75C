# 仓库结构

[English](repository-structure.md) | [简体中文](repository-structure_ZH.md)

本仓库对维护中的示例采用标准 Waveshare ESP32 产品布局：

```text
examples/esp-idf/              第一方 ESP-IDF 示例
examples/arduino/examples/     第一方 Arduino 草图
examples/arduino/libraries/    草图使用的随仓库 Arduino 库
config/                        共享配置说明和覆盖项
docs/                          维护者和仓库说明
.github/                       CI 和协作模板
Firmware/                      工厂烧录和恢复二进制
Schematic/                     硬件原理图资源
releases/                      CI 固件打包和产物工具
```

历史版本化示例根目录已由上述标准目录取代。CI 发现逻辑只扫描标准源码目录；过渡期间，
手动触发仍接受旧路径字符串作为选择器别名。

## 源码与二进制边界

- ESP-IDF 源码示例位于 `examples/esp-idf/`。
- 第一方 Arduino 草图位于 `examples/arduino/examples/`。
- 随仓库提供的 Arduino 库位于 `examples/arduino/libraries/`。
- 工厂固件二进制位于 `Firmware/`，仅作为烧录和恢复产物，不是 CI 构建输出。
- CI 生成的固件包是由 `releases/package_firmware.py` 产生的工作流产物。
