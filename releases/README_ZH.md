# 发布脚本

[English](README.md) | [简体中文](README_ZH.md)

此目录中的脚本用于打包 CI 构建输出、下载工作流产物、验证合并固件并暂存 GitHub
Release 资源。

发布固件必须来自成功的标签工作流。以下命令可用于脚本开发，但本地固件构建不能作为
发布输入。

## 固件包内容

`package_firmware.py` 读取框架构建输出并创建 `<artifact-name>-combined.zip`，其中包含：

- `bin/` 下的原始偏移寻址二进制；
- 从 `0x0` 烧录的 `bin/<artifact-name>-combined.bin`；
- shell 和 Windows 使用的分段及合并烧录脚本；
- 分段及合并命令文本文件；
- 包含来源、版本、偏移、大小和 SHA256 校验和的 `manifest.json`；
- 固件包 README。

ESP-IDF 的偏移和 esptool 参数来自 `flasher_args.json`。Arduino 优先使用导出的
`.merged.bin`；若不存在，则推导标准 ESP32-S3 二进制布局。

## 打包 ESP-IDF 构建

```bash
python3 releases/package_firmware.py \
  --framework esp-idf \
  --project examples/esp-idf/02_lvgl_demo_v9 \
  --build-dir build/02_lvgl_demo_v9-v6.0.2 \
  --name ESP32-S3-Touch-AMOLED-1.75C-02_lvgl_demo_v9-v6.0.2 \
  --framework-version v6.0.2 \
  --target esp32s3 \
  --git-sha <git-sha> \
  --output-dir release-artifacts
```

## 打包 Arduino 构建

```bash
python3 releases/package_firmware.py \
  --framework arduino \
  --project examples/arduino/examples/01_HelloWorld \
  --build-dir build/01_HelloWorld-3.3.11 \
  --name ESP32-S3-Touch-AMOLED-1.75C-01_HelloWorld-arduino-3.3.11 \
  --framework-version 3.3.11 \
  --target esp32s3 \
  --git-sha <git-sha> \
  --output-dir release-artifacts
```

## 下载 CI 产物

从已完成的工作流运行中下载全部固件，并保留内部合并 ZIP：

```bash
python3 releases/download_artifacts.py \
  --run-id <run-id> \
  --keep-archives \
  --clean
```

省略 `--run-id` 时，下载器选择当前分支上最新成功的 `examples.yml` 运行。可使用
`--artifact <name>` 下载单个产物，或使用 `--pattern "firmware-esp-idf-*"` 选择子集。

产物解压到 `releases/downloads/run-<run-id>/`。身份验证使用 `GH_TOKEN`、
`GITHUB_TOKEN` 或当前 GitHub CLI 登录。

## 暂存发布资源

标签工作流成功后，验证并暂存全部 17 个固件压缩包：

```bash
python3 releases/prepare_release_assets.py \
  --input-dir releases/downloads/run-<run-id> \
  --output-dir releases/dist/v1.0.1 \
  --version v1.0.1 \
  --git-sha <tag-commit-sha> \
  --clean
```

脚本会拒绝缺少合并镜像、偏移不正确、校验和不匹配、产物名重复、Git SHA 混用或压缩包
数量异常的输入。验证通过后，脚本复制 ZIP 并写入 `manifest-combined-assets.json`。

将 17 个 ZIP、合并资源清单以及 [`v1.0.1_ZH.md`](v1.0.1_ZH.md) 发布说明上传到
GitHub Release。
