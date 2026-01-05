#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SLEEP_STAGE_UNKNOWN = 0,
    SLEEP_STAGE_WAKE = 1,
    SLEEP_STAGE_REM = 2,
    SLEEP_STAGE_NREM = 3
} sleep_stage_t;

typedef struct {
    float respiratory_rate_bpm;  // 当前窗口的呼吸频率 (次/分)
    float motion_index;          // 当前窗口的体动强度指标
    uint32_t duration_seconds;   // 该窗口的持续时间，默认为 60s
} sleep_epoch_t;

typedef struct {
    sleep_stage_t stage;
    float respiratory_rate_bpm;
    float motion_index;
} sleep_stage_result_t;

typedef struct {
    float resp_rate_threshold;   // RRthres = mean(RR) + std(RR)
    float motion_threshold;      // Movthres = mean(Mov) + std(Mov)
    float wake_motion_threshold; // 用于判定醒来的体动平均阈值
} sleep_thresholds_t;

typedef struct {
    uint32_t wake_seconds;
    uint32_t rem_seconds;
    uint32_t nrem_seconds;
    float rem_ratio;             // REM 占比（相对于总睡眠时长）
    float sleep_efficiency;      // 睡眠效率 = (REM + NREM) / 总时长
    float average_resp_rate;
    float average_motion;
    float sleep_score;           // 简易 0-100 评分
} sleep_quality_report_t;

/**
 * @brief 按论文公式计算阈值。
 *        RRthres = mean(RR) + std(RR)
 *        Movthres = mean(Mov) + std(Mov)
 *        wake_motion_threshold = mean(Mov)
 */
void sleep_analysis_compute_thresholds(const sleep_epoch_t *epochs,
                                       size_t count,
                                       sleep_thresholds_t *out_thresholds);

/**
 * @brief 依据阈值进行睡眠阶段判定，包含 REM 纠错逻辑。
 */
void sleep_analysis_detect_stages(const sleep_epoch_t *epochs,
                                  size_t count,
                                  const sleep_thresholds_t *thresholds,
                                  sleep_stage_result_t *out_results);

/**
 * @brief 基于阶段结果做睡眠质量评估（效率、占比与简单评分）。
 */
void sleep_analysis_build_quality(const sleep_epoch_t *epochs,
                                  const sleep_stage_result_t *stages,
                                  size_t count,
                                  sleep_quality_report_t *out_report);

#ifdef __cplusplus
}
#endif
