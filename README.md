<div align="center">
  <h1>ESP32-S3-Touch-AMOLED-1.75C</h1>
  <p><strong>ESP32-S3 1.75-inch 466 x 466 QSPI AMOLED touch development board</strong></p>
  <p><strong>English</strong> | <a href="README_ZH.md">简体中文</a></p>
  <p>
    <a href="https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C/actions/workflows/examples.yml"><img alt="Build Examples" src="https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C/actions/workflows/examples.yml/badge.svg"></a>
    <a href="https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C/releases/latest"><img alt="Latest Release" src="https://img.shields.io/github/v/release/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C"></a>
    <a href="LICENSE"><img alt="License" src="https://img.shields.io/github/license/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C"></a>
  </p>
  <p>
    <a href="https://www.waveshare.com/esp32-s3-touch-amoled-1.75c.htm">🌐 Product</a> &middot;
    <a href="https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C/releases/latest">📦 Firmware</a> &middot;
    <a href="02_Example/ESP-IDF-v5.5.5/">🧩 ESP-IDF</a> &middot;
    <a href="02_Example/Arduino-v3.3.10/">🔧 Arduino</a> &middot;
    <a href="docs/">📚 Documentation</a>
  </p>
  <p><img src="docs/images/esp32-s3-touch-amoled-1.75c.jpg" alt="ESP32-S3-Touch-AMOLED-1.75C development board"></p>
</div>

---

## ✨ Overview

This repository provides example software, CI-built flashable firmware packages,
factory recovery firmware, schematics, and maintainer documentation for the
Waveshare ESP32-S3-Touch-AMOLED-1.75C.

The board combines an ESP32-S3 with a compact square AMOLED display, capacitive
touch, motion sensing, power management, and audio interfaces in a watch-style
development platform.

## 🖥️ Hardware Overview

| Feature | Device / interface |
| --- | --- |
| MCU | ESP32-S3 |
| Display | 1.75-inch 466 x 466 QSPI AMOLED using CO5300 |
| Touch | CST9217 capacitive touch controller over I2C |
| Power management | AXP2101 |
| Motion sensor | QMI8658 six-axis IMU |
| Audio | Dual digital microphones via ES7210 ADC; ES8311 audio codec |
| Board support | Managed component: `waveshare/esp32_s3_touch_amoled_1_75c` (`^3.0.0`) |
| Hardware files | [Schematic](resource/原理图/) |

## 📦 Firmware Releases

The fastest way to try an example is to use a ready-to-flash package from the
[latest release](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C/releases/latest).

1. Download the `*-combined.zip` package for the example and framework version
   you need.
2. Extract the archive and install esptool with
   `python -m pip install esptool`.
3. Connect the board over USB.
4. Run `flash_combined.bat COMx` on Windows or
   `./flash_combined.sh /dev/ttyACM0` on Linux.
5. Reset the board if it does not restart automatically.

> [!NOTE]
> Combined images are flashed at offset `0x0`. Each package also contains the
> original split binaries, flash arguments, helper scripts, and checksums.

Factory recovery images under [03_Firmware](03_Firmware/) are separate from
CI-generated example firmware. See
[Firmware Artifacts](docs/firmware.md) for details.

## 🧪 Examples

### ESP-IDF

| Example | Focus |
| --- | --- |
| [01_AXP2101](02_Example/ESP-IDF-v5.5.5/01_AXP2101/) | Power management and battery telemetry |
| [02_lvgl_demo_v9](02_Example/ESP-IDF-v5.5.5/02_lvgl_demo_v9/) | LVGL 9 display demo |
| [03_esp-brookesia](02_Example/ESP-IDF-v5.5.5/03_esp-brookesia/) | ESP-Brookesia application UI |
| [04_Immersive_block](02_Example/ESP-IDF-v5.5.5/04_Immersive_block/main/main.c) | Motion-driven LVGL block demo |
| [05_Spec_Analyzer](02_Example/ESP-IDF-v5.5.5/05_Spec_Analyzer/) | Microphone spectrum analyzer |

### Arduino

| Example | Focus |
| --- | --- |
| [01_HelloWorld](02_Example/Arduino-v3.3.10/01_HelloWorld/) | Display bring-up |
| [02_GFX_AsciiTable](02_Example/Arduino-v3.3.10/02_GFX_AsciiTable/) | GFX text and character rendering |
| [03_LVGL_AXP2101_ADC_Data](02_Example/Arduino-v3.3.10/03_LVGL_AXP2101_ADC_Data/) | LVGL power telemetry UI |
| [04_LVGL_QMI8658_ui](02_Example/Arduino-v3.3.10/04_LVGL_QMI8658_ui/) | LVGL IMU data UI |
| [05_LVGL_Widgets](02_Example/Arduino-v3.3.10/05_LVGL_Widgets/) | LVGL widgets, touch input, and display interaction |
| [06_ES7210](02_Example/Arduino-v3.3.10/06_ES7210/) | ES7210 microphone input |
| [07_ES8311](02_Example/Arduino-v3.3.10/07_ES8311/) | ES8311 audio output |

Bundled Arduino libraries live under
[`01_Arduino_Libraries`](01_Arduino_Libraries/). Their upstream
library examples are intentionally excluded from the product CI matrix.

## 🛠️ Supported Toolchains

| Surface | Version | Firmware builds |
| --- | --- | ---: |
| ESP-IDF source tree | `v5.5.5` | 5 |
| Arduino source tree | `3.3.10` | 7 |
| Release `v1.0.1`, ESP-IDF | `v5.5.4` | 5 |
| Release `v1.0.1`, ESP-IDF | `v6.0.2` | 5 |
| Release `v1.0.1`, Arduino-ESP32 | `3.3.10` | 7 |

The [Build Examples workflow](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C/actions/workflows/examples.yml)
runs a lightweight policy job and two discovery jobs, then selects up to 17
firmware build jobs from the changed-file scope. Each successful build is
packaged as a flashable combined firmware artifact. See
[Continuous Integration](docs/ci.md) for matrix and dispatch details.

## 🗂️ Repository Layout

| Path | Purpose |
| --- | --- |
| [`02_Example/ESP-IDF-v5.5.5/`](02_Example/ESP-IDF-v5.5.5/) | First-party ESP-IDF projects |
| [`02_Example/Arduino-v3.3.10/`](02_Example/Arduino-v3.3.10/) | First-party Arduino sketches |
| [`01_Arduino_Libraries/`](01_Arduino_Libraries/) | Bundled Arduino libraries |
| [`03_Firmware/`](03_Firmware/) | Factory, CI-release and XiaoZhi binaries |
| [`04_Community/`](04_Community/) | Community projects linked by the board page |
| [`resource/`](resource/) | Hardware documents, webpage snapshots and media |
| [`releases/`](releases/) | Packaging, artifact download, and release tools |
| [`Schematic/`](Schematic/) | Public schematic files |
| [`config/`](config/) | Shared ESP-IDF configuration overlays |
| [`docs/`](docs/) | Repository, CI, component, and firmware notes |

## 📚 Documentation

- [Repository Structure](docs/repository-structure.md)
- [Continuous Integration](docs/ci.md)
- [Components](docs/components.md)
- [Firmware Artifacts](docs/firmware.md)
- [ESP-Brookesia Notes](docs/brookesia.md)
- [Release Tools](releases/README.md)
- [Downloaded resource index](resource/资料索引.md)

## 🤝 Support and Contributions

Contributions and reproducible issue reports are welcome. Include the example
path, framework version, reproduction steps, expected behavior, actual
behavior, and relevant serial logs.

- [Contributing Guide](CONTRIBUTING.md)
- [Support](SUPPORT.md)
- [Security Policy](SECURITY.md)
- [Open an Issue](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C/issues/new/choose)

## 📄 License

This repository is licensed under the Apache License 2.0. See
[LICENSE](LICENSE).
