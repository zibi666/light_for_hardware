/**
 ******************************************************************************
 * @file        main.c
 * @author      limi
 * @version     V1.0
 * @date        2025-12-29
 * @brief       UART测试R60ABD1模块
 ******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "http_request.h"
#include "uart.h"
#include "audio_player.h"
#include "app_controller.h"
#include "rtc_service.h"
#include "alarm_music.h"
#include "xl9555_keys.h"

static void time_print_task(void *arg)
{
    rtc_calendar_t now;
    while (1)
    {
        if (rtc_get_time(&now) && rtc_time_is_valid())
        {
            printf("当前时间: %04u-%02u-%02u %02u:%02u:%02u\n",
                   now.year, now.month, now.date, now.hour, now.min, now.sec);
        }
        else
        {
            printf("当前时间未同步\n");
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

static QueueHandle_t s_beep_queue = NULL;

static void beep_task(void *arg)
{
    int duration_ms = 0;
    while (1)
    {
        if (xQueueReceive(s_beep_queue, &duration_ms, portMAX_DELAY) == pdTRUE)
        {
            xl9555_beep_on();
            vTaskDelay(pdMS_TO_TICKS(duration_ms));
            xl9555_beep_off();
        }
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_init_sta();
    
    /* Start periodic RTC sync task (will do initial 60s sync on first run) */
    if (rtc_start_periodic_sync(10 * 60 * 1000) != ESP_OK)
    {
        printf("RTC NTP校时任务启动失败\n");
    }
    else
    {
        xTaskCreate(time_print_task, "time_print", 3072, NULL, 4, NULL);
    }
    uart0_init(115200);

    /* 只初始化音频播放器，不启动，等待闹钟触发时再播放 */
    if (audio_player_init() != ESP_OK)
    {
        printf("音频播放初始化失败\n");
    }

    if (xl9555_beep_init() == ESP_OK)
    {
        xl9555_beep_off();
        s_beep_queue = xQueueCreate(4, sizeof(int));
        if (s_beep_queue)
        {
            xTaskCreate(beep_task, "beep_task", 2048, NULL, 4, NULL);
        }
    }

    /* 初始化闹钟音乐模块 */
    if (alarm_music_init() != ESP_OK)
    {
        printf("闹钟音乐初始化失败\n");
    }

    /* 启动闹钟音乐任务 */
    if (alarm_music_start() != ESP_OK)
    {
        printf("闹钟音乐任务启动失败\n");
    }

    if (alarm_service_start(10000, alarm_music_ring_callback, NULL) != ESP_OK)
    {
        printf("闹钟服务启动失败\n");
    }

    if (app_controller_start() != ESP_OK)
    {
        printf("业务任务启动失败\n");
    }

    vTaskDelete(NULL);
}
