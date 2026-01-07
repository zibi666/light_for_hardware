# ESP32-S3 R60ABD1 睡眠与音乐固件

面向 R60ABD1 毫米波睡眠雷达的 ESP-IDF 项目，集成睡眠分期/上报与 SD 卡音乐播放（ES8388 + XL9555 按键）。

## 功能概览
- 雷达串口协议：心率、呼吸、体动（DP4）、存在/越界状态、睡眠综合与整晚质量帧解析。
- 睡眠分期与质量评估：清醒/REM/非REM 三阶段，输出效率、REM 占比、评分；入睡前窗口强制标记清醒。
- 入睡判定：60s 暖机；最近 1 分钟体动均值 <5 且呼吸 10–25 判定入睡，可在 App 模块宏调整。
- HTTP 上报：30s 周期推送心率与呼吸，可配置服务器地址与 Wi‑Fi。
- SD 卡音乐播放：自动挂载 `/sdcard/MUSIC`，扫描 WAV 播放；KEY0/KEY2 上一曲/下一曲即刻生效；KEY1/KEY3 音量减/加。
- 音频硬件：ES8388 I2S 播放，XL9555 控制 SPK_EN 及按键扫描。

## 模块划分
- `main/main.c`：仅做 NVS/Wi‑Fi/UART 初始化并启动业务与音频任务。
- `components/BSP/App/`：`app_controller_start()` 统一启动上传、睡眠分期、UART 解析任务，保留阈值与判定逻辑。
- `components/BSP/Audio/`：ES8388 硬件驱动、SD 卡挂载、WAV 播放与按键音量/曲目控制。
- `components/BSP/Input/`：XL9555 按键与扬声器使能。
- `components/BSP/Protocol/`：雷达协议打包与解析。
- `components/BSP/HTTP/`：Wi‑Fi STA 与 HTTP 客户端。
- `components/BSP/SleepAnalysis/`：C++ 睡眠分析核心（阈值、分期、质量评分）。

## 关键参数（位于 App 模块顶部）
- `WARMUP_MS`：暖机时长，默认 60000 ms。
- `EPOCH_MS`：分期窗口，默认 30000 ms。
- `ONSET_WINDOW_EPOCHS`：入睡判定窗口（以 epoch 计），当前 2（1 分钟测试配置，可调回 10）。
- `MOTION_ONSET_MAX` / `RESP_ONSET_MIN/MAX`：入睡体动与呼吸阈值。

## 使用说明
- 准备 SD 卡：在 FAT 根目录创建 `MUSIC`，放入 WAV 文件（16-bit PCM）。
- 按键：KEY0 上一曲，KEY2 下一曲，KEY1 音量减，KEY3 音量加；SPK_EN 由 XL9555 下拉启动喇叭。
- 开机自动：Wi‑Fi STA 连接、HTTP 周期上报、睡眠分析任务、音乐播放与按键监听。

## 构建与烧录
```sh
export IDF_PATH=/home/lm/esp/v5.5.1/esp-idf  # 按实际路径
idf.py set-target esp32s3
idf.py build flash monitor
```

## 配置项
- `components/BSP/HTTP/http_request.c`：`WIFI_SSID`、`WIFI_PASS`、`SERVER_URL`。
- `components/BSP/App/app_controller.c`：入睡阈值与时间窗宏。
- `components/BSP/Audio/audio_sdcard.h`：SD 挂载点与音乐目录宏。

## 常见问题
- 心率/呼吸为 0：可能越界或体动大，确保目标在范围内并保持静止。
- 入睡迟迟不触发：适当调高 `MOTION_ONSET_MAX` 或缩短 `ONSET_WINDOW_EPOCHS` 做测试。
- 无声或按键无效：确认 SD 卡 `MUSIC` 下有 WAV，检查 XL9555 按键与 SPK_EN 连接，查看日志是否挂载或 I2S 报错。

## 参考
- 论文：applsci-13-04468-v2.pdf（doc/）
- 雷达手册：R60ABD1_用户手册_V3.5.pdf（doc/）
