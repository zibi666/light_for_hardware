#include "rtc_service.h"

#include <sys/time.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RTC_TAG "rtc_service"
#define RTC_DEFAULT_SYNC_MS (30 * 60 * 1000)

static TaskHandle_t s_rtc_task = NULL;
static uint32_t s_rtc_interval_ms = RTC_DEFAULT_SYNC_MS;

static void rtc_prepare_timezone(void)
{
    setenv("TZ", "CST-8", 1); /* 中国标准时间，UTC+8 */
    tzset();
}

void rtc_set_time(int year, int mon, int mday, int hour, int min, int sec)
{
    struct tm time_set;
    time_t second;

    time_set.tm_year  = year - 1900;
    time_set.tm_mon   = mon - 1;
    time_set.tm_mday  = mday;
    time_set.tm_hour  = hour;
    time_set.tm_min   = min;
    time_set.tm_sec   = sec;
    time_set.tm_isdst = -1;

    second = mktime(&time_set);

    struct timeval val = {
        .tv_sec = second,
        .tv_usec = 0
    };

    settimeofday(&val, NULL);
}

bool rtc_get_time(rtc_calendar_t *out_calendar)
{
    if (!out_calendar) {
        return false;
    }

    time_t second;
    struct tm time_block;

    time(&second);
    localtime_r(&second, &time_block);

    out_calendar->year  = time_block.tm_year + 1900;
    out_calendar->month = time_block.tm_mon + 1;
    out_calendar->date  = time_block.tm_mday;
    out_calendar->week  = time_block.tm_wday;
    out_calendar->hour  = time_block.tm_hour;
    out_calendar->min   = time_block.tm_min;
    out_calendar->sec   = time_block.tm_sec;
    return true;
}

bool rtc_sync_time_from_ntp(uint32_t wait_ms)
{
    rtc_prepare_timezone();

    esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.ntsc.ac.cn");
    esp_sntp_setservername(1, "ntp.aliyun.com");
    esp_sntp_setservername(2, "cn.pool.ntp.org");
    esp_sntp_init();

    uint32_t elapsed = 0;
    const uint32_t step = 500;
    while (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && elapsed < wait_ms) {
        vTaskDelay(pdMS_TO_TICKS(step));
        elapsed += step;
    }

    if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) {
        ESP_LOGW(RTC_TAG, "NTP sync timeout");
        return false;
    }

    rtc_calendar_t now;
    rtc_get_time(&now);
    ESP_LOGI(RTC_TAG, "NTP synced: %04u-%02u-%02u %02u:%02u:%02u", now.year, now.month, now.date,
             now.hour, now.min, now.sec);
    return true;
}

static void rtc_sync_task(void *arg)
{
    while (1) {
        bool ok = rtc_sync_time_from_ntp(20000);
        ESP_LOGI(RTC_TAG, "NTP sync %s", ok ? "ok" : "fail");
        vTaskDelay(pdMS_TO_TICKS(s_rtc_interval_ms));
    }
}

esp_err_t rtc_start_periodic_sync(uint32_t interval_ms)
{
    if (interval_ms >= 5000) {
        s_rtc_interval_ms = interval_ms;
    }

    if (s_rtc_task) {
        return ESP_OK;
    }

    BaseType_t r = xTaskCreate(rtc_sync_task, "rtc_ntp_sync", 3072, NULL, 4, &s_rtc_task);
    if (r != pdPASS) {
        s_rtc_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void rtc_stop_periodic_sync(void)
{
    if (s_rtc_task) {
        vTaskDelete(s_rtc_task);
        s_rtc_task = NULL;
    }
}
