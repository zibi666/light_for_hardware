/**
 ******************************************************************************
 * @file        main.c
 * @author      limi
 * @version     V1.0
 * @date        2025-12-29
 * @brief       UART测试R60ABD1模块
 ******************************************************************************/

#include <stdio.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include <string.h>
#include "uart.h"
#include "protocol.h"
#include "http_request.h"
#include "sleep_analysis.h"

static int g_heart_rate = 0;
static int g_breathing_rate = 0;
static float g_motion_index = 0.0f;
static float g_motion_accum = 0.0f;
static uint32_t g_motion_samples = 0;

typedef enum {
    SLEEP_WARMUP = 0,
    SLEEP_SETTLING,
    SLEEP_ACTIVE
} sleep_state_t;

static sleep_state_t g_sleep_state = SLEEP_WARMUP;
static TickType_t g_start_tick = 0;
static size_t g_sleep_start_index = SIZE_MAX; // 首个被纳入阈值计算的索引

#define WARMUP_MS            60000U      // 60 秒暖机
#define EPOCH_MS             30000U      // 30 秒窗口
#define ONSET_WINDOW_EPOCHS  2U          // 1 分钟 (2*30s)
#define MOTION_ONSET_MAX     5.0f        // 入睡判定体动均值阈值
#define RESP_ONSET_MIN       10.0f       // 入睡判定呼吸下限
#define RESP_ONSET_MAX       25.0f       // 入睡判定呼吸上限

#define MAX_SLEEP_EPOCHS 512
static sleep_epoch_t g_epochs[MAX_SLEEP_EPOCHS];
static sleep_stage_result_t g_stage_results[MAX_SLEEP_EPOCHS];
static size_t g_epoch_count = 0;
static sleep_thresholds_t g_thresholds = {0};

static const char *stage_to_str(sleep_stage_t s)
{
    switch (s)
    {
    case SLEEP_STAGE_WAKE: return "清醒";
    case SLEEP_STAGE_REM:  return "REM";
    case SLEEP_STAGE_NREM: return "非REM";
    default: return "未知";
    }
}

static void upload_data_task(void *pvParameters)
{
    while (1)
    {
        if (!wifi_wait_connected(5000))
        {
            printf("Wi-Fi未连接，跳过上传\n");
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        health_data_t data = {
            .heart_rate = g_heart_rate,
            .breathing_rate = g_breathing_rate
        };

        if (g_heart_rate > 0 || g_breathing_rate > 0)
        {
            printf("正在上传数据 - 心率:%d 呼吸:%d\n", data.heart_rate, data.breathing_rate);
            http_send_health_data(&data);
        }
        else
        {
            printf("暂无有效数据，跳过上传\n");
        }

        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

static void sleep_stage_task(void *pvParameters)
{
    const TickType_t period = pdMS_TO_TICKS(EPOCH_MS);
    sleep_quality_report_t report;
    g_start_tick = xTaskGetTickCount();

    while (1)
    {
        // 暖机阶段：直接丢弃数据，清空累积，等待 60s
        if (g_sleep_state == SLEEP_WARMUP) {
            TickType_t elapsed = xTaskGetTickCount() - g_start_tick;
            if (elapsed < pdMS_TO_TICKS(WARMUP_MS)) {
                g_motion_accum = 0.0f;
                g_motion_samples = 0;
                g_breathing_rate = 0;
                g_heart_rate = 0;
                vTaskDelay(period);
                continue;
            }
            g_sleep_state = SLEEP_SETTLING;
            g_sleep_start_index = SIZE_MAX;
        }

        float motion_avg = (g_motion_samples > 0) ? (g_motion_accum / (float)g_motion_samples) : g_motion_index;
        sleep_epoch_t epoch = {
            .respiratory_rate_bpm = (g_breathing_rate > 0) ? (float)g_breathing_rate : 0.0f,
            .motion_index = motion_avg,
            .duration_seconds = 30
        };

        g_motion_accum = 0.0f;
        g_motion_samples = 0;

        if (g_epoch_count >= MAX_SLEEP_EPOCHS)
        {
            memmove(&g_epochs[0], &g_epochs[1], (MAX_SLEEP_EPOCHS - 1) * sizeof(sleep_epoch_t));
            memmove(&g_stage_results[0], &g_stage_results[1], (MAX_SLEEP_EPOCHS - 1) * sizeof(sleep_stage_result_t));
            if (g_sleep_start_index != SIZE_MAX) {
                if (g_sleep_start_index > 0) g_sleep_start_index -= 1;
                else g_sleep_start_index = 0;
            }
            g_epoch_count = MAX_SLEEP_EPOCHS - 1;
        }

        g_epochs[g_epoch_count] = epoch;
        g_epoch_count++;

        // 入睡判定：仅在 SETTLING 状态下检查最近 5 分钟
        if (g_sleep_state == SLEEP_SETTLING && g_epoch_count >= ONSET_WINDOW_EPOCHS) {
            float m_sum = 0.0f, r_sum = 0.0f;
            size_t start = g_epoch_count - ONSET_WINDOW_EPOCHS;
            for (size_t i = start; i < g_epoch_count; ++i) {
                m_sum += g_epochs[i].motion_index;
                r_sum += g_epochs[i].respiratory_rate_bpm;
            }
            float m_mean = m_sum / (float)ONSET_WINDOW_EPOCHS;
            float r_mean = r_sum / (float)ONSET_WINDOW_EPOCHS;
            if (m_mean < MOTION_ONSET_MAX && r_mean >= RESP_ONSET_MIN && r_mean <= RESP_ONSET_MAX) {
                g_sleep_state = SLEEP_ACTIVE;
                g_sleep_start_index = g_epoch_count; // 从下一个 epoch 开始计入阈值
                printf("[睡眠] 检测到入睡，m=%.2f r=%.2f\n", m_mean, r_mean);
            }
        }

        // 预睡期的 epoch 强制标记为清醒，且不进入阈值计算
        for (size_t i = 0; i < g_epoch_count; ++i) {
            if (g_sleep_start_index == SIZE_MAX || i < g_sleep_start_index) {
                g_stage_results[i].stage = SLEEP_STAGE_WAKE;
                g_stage_results[i].respiratory_rate_bpm = g_epochs[i].respiratory_rate_bpm;
                g_stage_results[i].motion_index = g_epochs[i].motion_index;
            }
        }

        const sleep_epoch_t *sleep_ptr = NULL;
        size_t sleep_count = 0;
        if (g_sleep_start_index != SIZE_MAX && g_sleep_start_index < g_epoch_count) {
            sleep_ptr = &g_epochs[g_sleep_start_index];
            sleep_count = g_epoch_count - g_sleep_start_index;
        }

        if (sleep_count > 0) {
            sleep_analysis_compute_thresholds(sleep_ptr, sleep_count, &g_thresholds);
            sleep_analysis_detect_stages(sleep_ptr, sleep_count, &g_thresholds, &g_stage_results[g_sleep_start_index]);
        } else {
            memset(&g_thresholds, 0, sizeof(g_thresholds));
        }

        sleep_analysis_build_quality(g_epochs, g_stage_results, g_epoch_count, &report);

        if (g_epoch_count >= 5) {
            if (g_thresholds.motion_threshold < 1.0f || g_thresholds.resp_rate_threshold < 5.0f) {
                printf("[睡眠][告警] 阈值异常: RR_thres=%.2f Mov_thres=%.2f (可能输入数据为 0)\n",
                       g_thresholds.resp_rate_threshold,
                       g_thresholds.motion_threshold);
            }
        }

        if (g_epoch_count > 0)
        {
            const sleep_stage_result_t *last = &g_stage_results[g_epoch_count - 1];
            if (g_sleep_state != SLEEP_ACTIVE || sleep_count == 0) {
                printf("[睡眠] 未入睡/暖机或未满足条件，强制清醒。呼吸:%.1f 体动:%.2f\n",
                       last->respiratory_rate_bpm,
                       last->motion_index);
            } else {
                printf("[睡眠] 阶段:%s 呼吸:%.1f 体动:%.2f 评分:%.1f 效率:%.2f REM占比:%.2f\n",
                       stage_to_str(last->stage),
                       last->respiratory_rate_bpm,
                       last->motion_index,
                       report.sleep_score,
                       report.sleep_efficiency,
                       report.rem_ratio);
            }
        }

        vTaskDelay(period);
    }
}



/**
 * @brief       程序入口
 * @param       无
 * @retval      无
 */
void app_main(void)
{
    esp_err_t ret;
    uint16_t len = 0;
    uint8_t rx_buf[128] = {0};

    ret = nvs_flash_init();         /* 初始化NVS */

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_init_sta();
    xTaskCreate(upload_data_task, "upload_data_task", 4096, NULL, 5, NULL);
    xTaskCreate(sleep_stage_task, "sleep_stage_task", 4096, NULL, 5, NULL);

    uart0_init(115200);             /* 初始化串口0 */

    /* 发送开启心率监测指令 */
    uint8_t tx_buf[32];
    uint16_t tx_len = sizeof(tx_buf);
    if (protocol_pack_heart_rate_switch(1, tx_buf, &tx_len) == 0)
    {
        uart_write_bytes(USART_UX, (const char *)tx_buf, tx_len);
        printf("已发送心率使能命令\n");
    }

    while(1)
    {
        uart_get_buffered_data_len(USART_UX, (size_t*) &len);

        if (len > 0)
        {
            // 读取数据
            int rx_len = uart_read_bytes(USART_UX, rx_buf, (len > sizeof(rx_buf) ? sizeof(rx_buf) : len), 100);
            
            if (rx_len > 0) {
                // 尝试解析数据帧
                uint8_t ctrl, cmd;
                uint8_t *data_ptr;
                uint16_t data_len;
                
                int parse_res = protocol_parse_frame(rx_buf, rx_len, &ctrl, &cmd, &data_ptr, &data_len);
                
                if (parse_res == 0) {
                    // 解析成功
                    if (ctrl == CTRL_HEART_RATE && cmd == CMD_HEART_RATE_REPORT) {
                        if (data_len == 1) {
                            uint8_t heart_rate = data_ptr[0];
                            g_heart_rate = heart_rate;
                            printf("心率: %d bpm\n", heart_rate);
                        }
                    } else if (ctrl == CTRL_HEART_RATE && cmd == CMD_HEART_RATE_SWITCH) {
                        printf("收到心率开关响应\n");
                    } else if (ctrl == CTRL_HUMAN_PRESENCE) {
                        switch (cmd) {
                            case CMD_BODY_MOVEMENT: // 体动参数
                                if (data_len == 1) {
                                    uint8_t movement = data_ptr[0];
                                    g_motion_index = (float)movement; // 0~100 连续值
                                    g_motion_accum += g_motion_index;
                                    g_motion_samples++;
                                    printf("体动参数: %d\n", movement);
                                }
                                break;
                            case CMD_HUMAN_DISTANCE: // 人体距离
                                /* 屏蔽人体距离输出
                                if (data_len == 2) {
                                    uint16_t distance = (data_ptr[0] << 8) | data_ptr[1];
                                    printf("人体距离: %d cm\n", distance);
                                }
                                */
                                break;
                            case CMD_HUMAN_ORIENTATION: // 人体方位
                                /* 屏蔽人体方位输出
                                if (data_len == 6) {
                                    int16_t x = (data_ptr[0] << 8) | data_ptr[1];
                                    int16_t y = (data_ptr[2] << 8) | data_ptr[3];
                                    int16_t z = (data_ptr[4] << 8) | data_ptr[5];
                                    
                                    // 处理符号位 (最高位为1代表负数)
                                    // 注意：int16_t 直接转换可能依赖编译器实现，这里手动处理更稳妥
                                    // 但通常补码系统下，如果协议定义最高位是符号位且后15位是数值，
                                    // 且负数不是补码形式而是原码形式（即 0x8064 = -100），则需要特殊处理。
                                    // 根据描述： "0 代表正数, 1 代表负数... 取剩余的 15 位转换为十进制"
                                    // 这意味着是 "符号-数值" 表示法 (Sign-Magnitude)，而不是补码。
                                    
                                    int val_x = ((data_ptr[0] & 0x7F) << 8) | data_ptr[1];
                                    if (data_ptr[0] & 0x80) val_x = -val_x;

                                    int val_y = ((data_ptr[2] & 0x7F) << 8) | data_ptr[3];
                                    if (data_ptr[2] & 0x80) val_y = -val_y;

                                    int val_z = ((data_ptr[4] & 0x7F) << 8) | data_ptr[5];
                                    if (data_ptr[4] & 0x80) val_z = -val_z;

                                    printf("人体方位: X=%d cm, Y=%d cm, Z=%d cm\n", val_x, val_y, val_z);
                                }
                                */
                                break;
                            case CMD_MOTION_INFO: // 运动信息
                                if (data_len == 1) {
                                    uint8_t motion = data_ptr[0];
                                    // 仍保留状态输出，但以连续体动参数为主
                                    if (motion == 0x01) printf("运动信息: 静止\n");
                                    else if (motion == 0x02) printf("运动信息: 活跃\n");
                                    else printf("运动信息: 未知 (%02X)\n", motion);
                                }
                                break;
                            default:
                                printf("未知存在帧: Cmd=%02X\n", cmd);
                                break;
                        }
                    } else if (ctrl == CTRL_BREATH && cmd == CMD_BREATH_VALUE) {
                        if (data_len == 1) {
                            uint8_t breath = data_ptr[0];
                            g_breathing_rate = breath;
                            printf("呼吸频率: %d 次/分\n", breath);
                        }
                    } else if (ctrl == 0x01 && cmd == 0x01) {
                        // 系统心跳包
                        // printf("系统心跳正常\n"); // 可选：如果不想刷屏可以注释掉
                    } else if (ctrl == 0x07 && cmd == 0x07) {
                        // 位置越界状态
                        if (data_len == 1) {
                            uint8_t status = data_ptr[0];
                            if (status == 0x00) {
                                printf("状态: 目标在范围外，清零生命体征缓存\n");
                                g_motion_index = 0.0f;
                                g_motion_accum = 0.0f;
                                g_motion_samples = 0;
                                g_breathing_rate = 0;
                                g_heart_rate = 0;
                            } else if (status == 0x01) {
                                printf("状态: 目标在范围内\n");
                            } else {
                                printf("状态: 越界未知 (%02X)\n", status);
                            }
                        }
                    } else if (ctrl == CTRL_SLEEP) {
                        if (cmd == CMD_SLEEP_COMPREHENSIVE) {
                            // 睡眠综合状态上报 (每10分钟)
                            if (data_len == 8) {
                                printf("=== 睡眠综合状态 (10min) ===\n");
                                printf("存在信息: %s\n", data_ptr[0] == 0x01 ? "有人" : "无人");
                                
                                const char* sleep_status = "未知";
                                switch(data_ptr[1]) {
                                    case 0x00: sleep_status = "深睡"; break;
                                    case 0x01: sleep_status = "浅睡"; break;
                                    case 0x02: sleep_status = "清醒"; break;
                                    case 0x03: sleep_status = "离床"; break;
                                }
                                printf("睡眠状态: %s\n", sleep_status);
                                printf("平均呼吸: %d 次/分\n", data_ptr[2]);
                                printf("平均心跳: %d 次/分\n", data_ptr[3]);
                                printf("翻身次数: %d\n", data_ptr[4]);
                                printf("大幅体动: %d%%\n", data_ptr[5]);
                                printf("小幅体动: %d%%\n", data_ptr[6]);
                                printf("呼吸暂停: %d\n", data_ptr[7]);
                                printf("==========================\n");
                            }
                        } else if (cmd == CMD_SLEEP_QUALITY) {
                            // 睡眠质量分析上报 (整晚)
                            if (data_len == 12) {
                                printf("=== 睡眠质量分析报告 (整晚) ===\n");
                                printf("睡眠评分: %d 分\n", data_ptr[0]);
                                uint16_t total_time = (data_ptr[1] << 8) | data_ptr[2];
                                printf("睡眠时长: %d 分钟\n", total_time);
                                printf("清醒占比: %d%%\n", data_ptr[3]);
                                printf("浅睡占比: %d%%\n", data_ptr[4]);
                                printf("深睡占比: %d%%\n", data_ptr[5]);
                                printf("离床时长: %d 分钟\n", data_ptr[6]);
                                printf("离床次数: %d 次\n", data_ptr[7]);
                                printf("翻身次数: %d 次\n", data_ptr[8]);
                                printf("平均呼吸: %d 次/分\n", data_ptr[9]);
                                printf("平均心跳: %d 次/分\n", data_ptr[10]);
                                printf("呼吸暂停: %d 次\n", data_ptr[11]);
                                printf("=============================\n");
                            }
                        }
                    } else {
                        printf("未知帧: Ctrl=%02X, Cmd=%02X\n", ctrl, cmd);
                    }
                } else {
                    // 解析失败，打印原始数据以便调试
                    printf("原始数据: ");
                    for (int i = 0; i < rx_len; i++) {
                        printf("%02X ", rx_buf[i]);
                    }
                    printf("\n");
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));    /* 延时10ms */
    }
}
