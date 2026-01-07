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
#include "nvs_flash.h"
#include "http_request.h"
#include "uart.h"
#include "audio_player.h"
#include "app_controller.h"
#include "rtc_service.h"

static void time_print_task(void *arg)
{
    rtc_calendar_t now;
    while (1)
    {
        if (rtc_get_time(&now))
        {
            printf("当前时间: %04u-%02u-%02u %02u:%02u:%02u\n",
                   now.year, now.month, now.date, now.hour, now.min, now.sec);
        }
        else
        {
            printf("当前时间读取失败\n");
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
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
    if (rtc_start_periodic_sync(30 * 60 * 1000) != ESP_OK)
    {
        printf("RTC NTP校时任务启动失败\n");
    }
    else
    {
        xTaskCreate(time_print_task, "time_print", 2048, NULL, 4, NULL);
    }
    uart0_init(115200);

    if (alarm_service_start(60000, NULL, NULL) != ESP_OK)
    {
        printf("闹钟服务启动失败\n");
    }

    if (audio_player_start() != ESP_OK)
    {
        printf("音频播放任务启动失败\n");
    }

    if (app_controller_start() != ESP_OK)
    {
        printf("业务任务启动失败\n");
    }

    vTaskDelete(NULL);
}
