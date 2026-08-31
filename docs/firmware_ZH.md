# 固件产物

[English](firmware.md) | [简体中文](firmware_ZH.md)

本仓库包含两类相互独立的固件来源：带标签的 CI 构建和工厂恢复镜像。

## 发布固件

GitHub Actions 从以下目录构建由源码维护的示例：

- `examples/esp-idf/`
- `examples/arduino/`

每个发布产物都是 `*-combined.zip`，其中同时包含：

- 从偏移地址 `0x0` 烧录的单个 `*-combined.bin` 镜像；
- 原始引导程序、分区表、应用程序及其他带偏移地址的二进制文件。

常规安装请使用 `flash_combined.sh` 或 `flash_combined.bat`。只有明确需要原始布局时，
才使用分段镜像脚本。两种命令形式也会保存在文本文件中。

固件包清单会记录源 Git SHA、框架版本、目标、二进制偏移、大小和 SHA256 校验和。
GitHub Release 中的 `manifest-combined-assets.json` 提供全部 ZIP 的校验和。

## 工厂恢复固件

`Firmware/ESP32-S3-Touch-AMOLED-1.75C-FactoryOnly-260114.bin` 是已发布的工厂烧录和
恢复镜像。它不是由 CI 生成，也不属于源码示例构建，不会重新打包为发布示例。

仅在恢复工厂演示镜像时使用它。需要可复现的示例固件时，请使用带标签的 Release 包。

## 烧录布局安全

请使用每个固件包清单记录的烧录布局。不要混用不同固件包中的引导程序、分区表或应用。
如果监视器报告应用校验和不匹配，请重新烧录完整合并镜像，并使用同一 CI 构建中的
ELF 解码日志。

## 生成文件

- CI 打包输出：`release-artifacts/`
- 本地打包输出：`releases/dist/`
- 下载的 CI 产物：`releases/downloads/`

这些路径已被 Git 忽略，不得提交。发布产物仅在标签工作流成功并通过发布暂存脚本验证后，
从 GitHub 上传。
