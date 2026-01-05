# ESP32-S3 R60ABD1 睡眠检测固件

面向 R60ABD1 毫米波睡眠雷达的 ESP-IDF 项目，完成数据采集、睡眠分期与质量评估，并通过 HTTP 上报心率/呼吸数据。

## 功能概览
- 串口解析雷达协议：心率、呼吸、体动参数（DP4）、存在/越界状态、睡眠综合/整晚质量帧。
- 睡眠分期与质量评估：基于论文实现的三阶段检测（清醒/REM/非REM），并计算效率、REM 占比、简易评分。
- 暖机与入睡判定：60s 暖机丢弃数据；最近 1 分钟（测试配置，可调）体动均值<5 且呼吸 10–25 判定入睡，入睡前数据不参与阈值计算但计入清醒时长。
- 越界清零：目标离开范围时清零生命体征缓存，避免残留数据污染阈值。
- HTTP 上报：按 30s 周期上传最新心率/呼吸（可在 `http_request.c` 配置服务器地址）。

## 目录结构
- `main/main.c`：任务创建、串口解析、状态机（暖机/入睡/睡眠）、分期与日志。
- `components/BSP/Protocol/`：协议打包解析。
- `components/BSP/HTTP/`：Wi-Fi STA 与 HTTP 客户端。
- `components/BSP/SleepAnalysis/`：C++ 睡眠分析核心（阈值计算、分期、质量评分）。

## 关键参数（可在 main.c 顶部调整）
- `WARMUP_MS`：暖机时长，默认 60000 ms。
- `EPOCH_MS`：分期窗口长度，默认 30000 ms。
- `ONSET_WINDOW_EPOCHS`：入睡判定窗口（以 epoch 计数）；当前为 2（1 分钟，用于测试）。正式使用可调回 10（5 分钟）。
- `MOTION_ONSET_MAX` / `RESP_ONSET_MIN/MAX`：入睡判定的体动/呼吸阈值。

## 判定逻辑简述
1) 暖机：前 60s 数据丢弃，不入队。
2) 入睡判定：最近 `ONSET_WINDOW_EPOCHS` 的体动均值<5，呼吸均值 10–25，即标记入睡，从下一窗口起进入 ACTIVE。
3) 阈值与分期：仅对 ACTIVE 后的数据计算阈值和分期；入睡前窗口全部标记为清醒，仅计入时长。
4) 越界：收到越界状态（ctrl 0x07 cmd 0x07 status 0x00）时清零心率/呼吸/体动累积。
5) 日志：未入睡阶段打印“未入睡/暖机或未满足条件”；ACTIVE 阶段打印分期/评分。

## 构建与烧录
```sh
export IDF_PATH=/home/lm/esp/v5.5.1/esp-idf  # 根据实际路径
idf.py set-target esp32s3
idf.py build flash monitor
```

## HTTP 配置
在 `components/BSP/HTTP/http_request.c` 修改：
- `WIFI_SSID` / `WIFI_PASS`
- `SERVER_URL`

## 常见问题
- 心率为 0：多因体动大/姿势不对/越界；保持静止、胸前对准雷达，确认“目标在范围内”。
- 阈值异常告警：仅在 ACTIVE 且有睡眠窗口时才检查；未入睡阶段不会输出告警。
- 入睡迟迟不触发：降低体动噪声，或临时调高 `MOTION_ONSET_MAX` / 缩短 `ONSET_WINDOW_EPOCHS` 进行测试。

## 参考
- 论文：applsci-13-04468-v2.pdf（doc/）
- 雷达手册：R60ABD1_用户手册_V3.5.pdf（doc/）
