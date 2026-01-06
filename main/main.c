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

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_init_sta();
    uart0_init(115200);

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
