#include "sleep_analysis.h"

#include <algorithm>
#include <cmath>

namespace {
float safe_duration(const sleep_epoch_t &epoch) {
    return epoch.duration_seconds > 0 ? static_cast<float>(epoch.duration_seconds) : 60.0f;
}

float clamp(float v, float lo, float hi) {
    return std::max(lo, std::min(v, hi));
}

struct Moments {
    float mean = 0.0f;
    float stddev = 0.0f;
};

Moments compute_moments(const sleep_epoch_t *epochs, size_t count, bool use_motion) {
    if (count == 0 || epochs == nullptr) {
        return {};
    }

    float sum = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        sum += use_motion ? epochs[i].motion_index : epochs[i].respiratory_rate_bpm;
    }
    const float mean = sum / static_cast<float>(count);

    float var = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        const float v = use_motion ? epochs[i].motion_index : epochs[i].respiratory_rate_bpm;
        const float d = v - mean;
        var += d * d;
    }
    const float stddev = count > 1 ? std::sqrt(var / static_cast<float>(count - 1)) : 0.0f;

    return {mean, stddev};
}

float median3(float a, float b, float c) {
    return a + b + c - std::max({a, b, c}) - std::min({a, b, c});
}
}

extern "C" void sleep_analysis_compute_thresholds(const sleep_epoch_t *epochs,
                                                   size_t count,
                                                   sleep_thresholds_t *out_thresholds) {
    if (out_thresholds == nullptr) {
        return;
    }
    sleep_thresholds_t defaults{20.0f, 1.5f, 0.5f};
    *out_thresholds = defaults;

    if (epochs == nullptr || count == 0) {
        return;
    }

    const Moments rr = compute_moments(epochs, count, /*use_motion=*/false);
    const Moments mv = compute_moments(epochs, count, /*use_motion=*/true);

    out_thresholds->resp_rate_threshold = rr.mean + rr.stddev;
    out_thresholds->motion_threshold = mv.mean + mv.stddev;
    out_thresholds->wake_motion_threshold = mv.mean;
}

extern "C" void sleep_analysis_detect_stages(const sleep_epoch_t *epochs,
                                              size_t count,
                                              const sleep_thresholds_t *thresholds,
                                              sleep_stage_result_t *out_results) {
    if (epochs == nullptr || thresholds == nullptr || out_results == nullptr) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        float prev = (i == 0) ? epochs[i].motion_index : epochs[i - 1].motion_index;
        float curr = epochs[i].motion_index;
        float next = (i + 1 < count) ? epochs[i + 1].motion_index : epochs[i].motion_index;
        const float motion_smoothed = median3(prev, curr, next);

        const bool rem_primary = epochs[i].respiratory_rate_bpm > thresholds->resp_rate_threshold;
        const bool high_motion = motion_smoothed > thresholds->motion_threshold;
        const bool wake_motion = motion_smoothed > thresholds->wake_motion_threshold;

        sleep_stage_t stage = SLEEP_STAGE_NREM;
        if (wake_motion) {
            stage = SLEEP_STAGE_WAKE;
        } else if (rem_primary) {
            stage = high_motion ? SLEEP_STAGE_WAKE : SLEEP_STAGE_REM;
        }

        out_results[i].stage = stage;
        out_results[i].respiratory_rate_bpm = epochs[i].respiratory_rate_bpm;
        out_results[i].motion_index = motion_smoothed;
    }
}

extern "C" void sleep_analysis_build_quality(const sleep_epoch_t *epochs,
                                              const sleep_stage_result_t *stages,
                                              size_t count,
                                              sleep_quality_report_t *out_report) {
    if (out_report == nullptr) {
        return;
    }
    *out_report = {};

    if (epochs == nullptr || stages == nullptr || count == 0) {
        return;
    }

    float total_seconds = 0.0f;
    float sleep_seconds = 0.0f;
    float rem_seconds = 0.0f;
    float nrem_seconds = 0.0f;
    float wake_seconds = 0.0f;
    float resp_sum = 0.0f;
    float motion_sum = 0.0f;

    for (size_t i = 0; i < count; ++i) {
        const float dur = safe_duration(epochs[i]);
        total_seconds += dur;
        resp_sum += epochs[i].respiratory_rate_bpm;
        motion_sum += stages[i].motion_index;

        switch (stages[i].stage) {
            case SLEEP_STAGE_WAKE:
                wake_seconds += dur;
                break;
            case SLEEP_STAGE_REM:
                rem_seconds += dur;
                sleep_seconds += dur;
                break;
            case SLEEP_STAGE_NREM:
                nrem_seconds += dur;
                sleep_seconds += dur;
                break;
            default:
                break;
        }
    }

    out_report->wake_seconds = static_cast<uint32_t>(wake_seconds);
    out_report->rem_seconds = static_cast<uint32_t>(rem_seconds);
    out_report->nrem_seconds = static_cast<uint32_t>(nrem_seconds);
    out_report->sleep_efficiency = (total_seconds > 0.0f) ? (sleep_seconds / total_seconds) : 0.0f;
    out_report->rem_ratio = (sleep_seconds > 0.0f) ? (rem_seconds / sleep_seconds) : 0.0f;
    out_report->average_resp_rate = resp_sum / static_cast<float>(count);
    out_report->average_motion = motion_sum / static_cast<float>(count);

    const float efficiency_score = clamp(out_report->sleep_efficiency * 100.0f, 0.0f, 100.0f);
    const float rem_pct = out_report->rem_ratio * 100.0f;
    const float rem_score = clamp(100.0f - std::fabs(rem_pct - 22.0f) * 3.0f, 0.0f, 100.0f);
    const float stability_score = clamp(100.0f - out_report->average_motion * 5.0f, 0.0f, 100.0f);

    const float weighted = 0.6f * efficiency_score + 0.3f * rem_score + 0.1f * stability_score;
    out_report->sleep_score = clamp(weighted, 0.0f, 100.0f);
}
