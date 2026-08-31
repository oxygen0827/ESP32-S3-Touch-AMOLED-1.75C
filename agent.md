# Agent 工作规则

## 开发前必读

在开始任何开发、修改代码、编译、烧录、上板运行或资料核对之前，必须先阅读与当前任务相关的 `开发经验.md`。

- C6 任务：先阅读本目录 `开发经验.md`（工程 `03_Clare_C6`）。
- S3 任务（ESP32-S3-Touch-AMOLED-1.75C，工程 `04_Clare_S3`）：同样先阅读本目录 `开发经验.md`，其中 2026-08-29 条目是 S3 移植的完整坑清单。
- 文档中记录的已知引脚、工具链、分区、硬件限制和历史修复应作为当前操作的前置条件，不得重复引入已知问题。

## 环境与固定操作（无需询问用户，直接执行）

- IDF 只有 `~/esp/esp-idf-v5.5.3`，且 Python venv 是 py3.9 版。每次构建前：
  ```bash
  export IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf5.5_py3.9_env
  . ~/esp/esp-idf-v5.5.3/export.sh
  ```
- **改了 `sdkconfig.defaults` 或 `sdkconfig.local` 后，必须 `rm -f build/sdkconfig.private && idf.py reconfigure` 再 build**（生成配置"首写优先"，不删则新值静默不生效）。
- 烧录后必须抓 ≥120 秒串口日志验证（boot self-test 三绿：session/transcribe/host err=0），未验证不得向用户报告成功。串口抓取用 `开发经验.md`/HANDOFF 里的 pyserial 模板（idf.py monitor 在自动化通道不可用）。
- 串口消失时先 `ls /dev/cu.usb*` 重新枚举；板子带电池可能脱机运行，提示用户把 USB 插回 Mac 而非充电器。

## 已知情况的自动处置守则（遇到即按此操作，不必等用户指示）

### 1. Wi-Fi `reason=201 NO_AP_FOUND`

先核对 SSID 字节，不要盲目重试或改代码：

```bash
grep CLARE_WIFI_SSID build/sdkconfig.private | hexdump -C
```

- iPhone 热点 `oxygen’s iPhone` 的撇号必须是 U+2018（字节 `e2 80 98`），若是 `27`（ASCII）则用 `printf 'CONFIG_CLARE_WIFI_SSID="oxygen\xe2\x80\x98s iPhone"\n'` 修正 sdkconfig.local，再按上面流程重建。
- 字节正确仍 201 → 提示用户确认热点/AP 在线（iPhone 需停留在"个人热点"页面）。

### 2. esptool `No serial data received` 且板子在重启环

抓 5 秒串口确认有周期性复位日志后，直接指导用户进 ROM 下载模式（一次即可）：

1. 拔掉 USB 线；2. 按住板子 **BOOT** 键；3. 插回 USB；4. 两秒后松开 BOOT。
黑屏正常（ROM 不驱动 AMOLED）。用户确认后立即烧录。**不要连续空 retry esptool 超过 2 次。**

### 3. S3 上出现内存类错误（白屏 / malloc fail / alloc failed）

看到 `spi_master: Failed to allocate priv TX buffer`、`wifi: malloc buffer fail`、`alloc primary buffer ... failed`、`Draw bitmap failed: ESP_ERR_NO_MEM` 任一，检查内部 RAM 预算三件套是否在 sdkconfig.defaults 且已生效（grep build/sdkconfig.private）：

- `CONFIG_LV_USE_CLIB_MALLOC=y`（LVGL 大块进 PSRAM）
- `CONFIG_ESP_WIFI_IRAM_OPT=n`、`CONFIG_ESP_WIFI_RX_IRAM_OPT=n`（Wi-Fi 代码出 IRAM）
- LVGL 帧缓冲必须保持 `ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG`（内部 DMA RAM 零拷贝；**禁止**改成 WITH_PSRAM，SPI DMA 读不了 PSRAM，会退化成中转分配失败导致白屏）

### 4. 屏幕颜色/显示异常

LVGL 9.5 无 `LV_COLOR_16_SWAP`，esp_lvgl_adapter 对 QSPI 面板固定不做字节交换——**不要**往字节交换方向查。优先抓日志看 `Draw bitmap failed`（刷新失败=内存问题，见守则 3），再查面板 init 序列与 `esp_lcd_panel_set_gap`（CO5300 必须 `set_gap(0x06, 0)`）。中文显示为"口"或渲染文字时 LoadProhibited → 自定义字库是 lv_font_conv 压缩格式，检查 `CONFIG_LV_USE_FONT_COMPRESSED=y` 已生效（grep build/sdkconfig.private）。

### 5. TLS/WSS 类报错（S3 上不应再出现）

S3 有 8MB PSRAM + `MBEDTLS_EXTERNAL_MEM_ALLOC=y`，X509_ALLOC/MPI_ALLOC/esp-aes 分配失败整类不应出现。若出现，先确认上述配置生效且没有被切回 INSECURE；S3 上证书验证必须保持开启（cert_pem 固定链）。

### 6. 服务器断连/报错/拒绝（Connection Lost、403、满屏 Network error）

先对照 vocat 参考实现（`/Users/hsh/vibecoding/confer-sum/vocat/products/ws_meeting_demo/main/`，生产验证）逐项核对，而不是改服务器或加重试：

- WS 音频消息必须**逐字节对齐**参考：`{"type":"audio","data":"<b64>"}` 单帧发送、无多余字段（speaker 等）、3200B/100ms 一条、client `buffer_size=16384`（小于消息体会被客户端强制分片，后端会掐线）。
- 同一时刻只给服务器**一路音频流**：进 Ask 前先暂停会议音频（参考 `transcribe_ws_pause`），且暂停标志要在 host WS 握手**之前**置位。
- `network_timeout_ms=5000`、发送超时 500ms；转写通道纯发送不读取。
- 诊断三件套已常驻 clare_net.cpp：每条入站消息打 `recv kind=.. type=.. len=..`、服务器 error 消息内容、rx 超限 peek。抓日志先看这三类行。

### 7. 新增大缓冲（>1KB static/全局数组）必须先定内存去处

`SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` 不覆盖函数级/命名空间 static。任何新大缓冲：

- 常驻的 → `EXT_RAM_BSS_ATTR`（include `esp_attr.h`）；
- 临时用的 → 用的时候 `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`，用完 free。

烧录后看 boot 日志 `free_internal`：健康线 ≥ 50KB / largest ≥ 30KB。低于此值 WS 客户端任务（10KB 栈）会创建失败，整网全挂。

### 8. 音频播报无声的排查顺序

1. `clare_audio_init_from_codec()` 是否在 app_main 调用（boot 日志应有 `clare_audio: ready: ...`）——历史缺口，C6 工程同样没有；
2. 日志里有没有 `tts stream begin/end`（有=服务器语音到了，查播放链；没有=查消息协议，见守则 6）；
3. 播放链验证：开机自检音（embedded MP3，`Boot chime playback err=0`）。

### 9. WS 客户端生命周期纪律

- stop/destroy 必须全程持音频发送锁（防发送途中释放）；
- 断连事件里必须清理句柄（排队到主任务做，不能在 WS 任务里自毁），否则后续连接全部 INVALID_STATE；
- 销毁路径不做任何可能阻塞 LVGL/音频任务超过秒级的等待。
- **连接握手期间音频任务必须休眠**（`if (!s_host_connected) continue`）：向未连上的通道空发会触发断连看门狗误杀会议通道，还会抢 CPU 拖垮 TLS 握手。
- TTS/音频解码播放必须走独立任务+抖动缓冲，**禁止**在 WS 客户端任务里同步解码播放（codec 写阻塞会反压 TCP，语音必断续）；流跨多条 answer_audio 消息保持，只在 done/停止时关闭。
- **解码错误只原地重置解码器，不得退出播放任务或清空缓冲**（否则会丢整句语音，坑 14）；连续 8 次无成功帧才放弃。
- 服务器会无预警掐断长连 WSS（无规律）：**会议通道必须有自动重连监督器**（退避重连 3 次→重建会话→全失败才提示用户），断线/重连期间音频任务休眠丢帧、不喂看门狗。

## 问题记录

开发过程中发现新的问题、workaround 或硬件限制时，应在对应板卡的 `开发经验.md` 中记录现象、原因、修复方式和验证结果。

## 成功后的经验归档

当某个功能、例程或 App 开发完成并验证成功后，不能只报告成功结果；必须在对应板卡的 `开发经验.md` 中完整补充本次开发过程中的问题与解决方案。

- 记录所有实际遇到的问题，包括编译错误、运行错误、硬件/引脚不匹配、工具链或依赖问题、分区配置、权限/连接问题、环境限制、警告和临时 workaround。
- 每条记录应说明现象、原因（已确认或当前判断）、采取的解决方案、涉及的文件/配置，以及最终验证结果。
- 即使问题未阻止功能成功，或通过调整环境绕过，也必须记录，不能因最终成功而省略。
- 在向用户报告"功能/App 已成功"之前，先完成经验文档的更新，并检查记录与实际日志、构建产物和硬件验证结果一致。
