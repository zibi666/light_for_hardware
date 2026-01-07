#include "audio_player.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdbool.h>
#include "audio_hw.h"
#include "audio_sdcard.h"
#include "xl9555_keys.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AUDIO_TASK_STACK   (10 * 1024)
#define AUDIO_TASK_PRIO    5
#define VOLUME_TASK_STACK  3072
#define VOLUME_TASK_PRIO   4
#define AUDIO_IO_BUF_SIZE  4096
#define MAX_TRACKS         64
#define MAX_NAME_LEN       128

static const char *TAG = "audio_player";

typedef struct
{
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t channels;
    uint32_t data_offset;
    uint32_t data_size;
} audio_wav_info_t;

static TaskHandle_t s_audio_task = NULL;
static TaskHandle_t s_volume_task = NULL;
static QueueHandle_t s_cmd_queue = NULL;
static bool s_inited = false;
static bool s_stop = false;
static volatile bool s_paused = false;
static size_t s_track_index = 0;

typedef enum
{
    AUDIO_CMD_NONE = 0,
    AUDIO_CMD_NEXT,
    AUDIO_CMD_PREV,
} audio_cmd_t;

static bool is_wav_file(const char *name)
{
    size_t len = strlen(name);
    if (len < 4)
    {
        return false;
    }
    const char *ext = name + len - 4;
    return (strcasecmp(ext, ".wav") == 0);
}

static esp_err_t wav_parse(FIL *file, audio_wav_info_t *info)
{
    UINT br = 0;
    uint8_t header[12];

    FRESULT fr = f_read(file, header, sizeof(header), &br);
    if (fr != FR_OK || br != sizeof(header))
    {
        return ESP_FAIL;
    }

    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0)
    {
        return ESP_FAIL;
    }

    bool fmt_found = false;
    bool data_found = false;
    uint16_t audio_format = 0;

    while (!data_found)
    {
        struct __attribute__((packed))
        {
            char id[4];
            uint32_t size;
        } chunk;

        fr = f_read(file, &chunk, sizeof(chunk), &br);
        if (fr != FR_OK || br != sizeof(chunk))
        {
            return ESP_FAIL;
        }

        uint32_t next_pos = f_tell(file) + chunk.size;

        if (memcmp(chunk.id, "fmt ", 4) == 0)
        {
            struct __attribute__((packed))
            {
                uint16_t format;
                uint16_t channels;
                uint32_t sample_rate;
                uint32_t byte_rate;
                uint16_t block_align;
                uint16_t bits_per_sample;
            } fmt_hdr;

            fr = f_read(file, &fmt_hdr, sizeof(fmt_hdr), &br);
            if (fr != FR_OK || br != sizeof(fmt_hdr))
            {
                return ESP_FAIL;
            }

            audio_format = fmt_hdr.format;
            info->channels = fmt_hdr.channels;
            info->sample_rate = fmt_hdr.sample_rate;
            info->bits_per_sample = fmt_hdr.bits_per_sample;
            fmt_found = true;
        }
        else if (memcmp(chunk.id, "data", 4) == 0)
        {
            info->data_offset = f_tell(file);
            info->data_size = chunk.size;
            data_found = true;
        }

        if (!data_found)
        {
            if (chunk.size % 2)
            {
                next_pos += 1; /* word alignment */
            }
            f_lseek(file, next_pos);
        }
    }

    if (!fmt_found || audio_format != 1)
    {
        return ESP_FAIL;
    }

    f_lseek(file, info->data_offset);
    return ESP_OK;
}

static esp_err_t play_single(const char *path, size_t track_count, bool *interrupted)
{
    if (interrupted)
    {
        *interrupted = false;
    }

    FIL file;
    FRESULT fr = f_open(&file, path, FA_READ);
    if (fr != FR_OK)
    {
        ESP_LOGW(TAG, "open %s failed: %d", path, fr);
        return ESP_FAIL;
    }

    audio_wav_info_t info = {0};
    if (wav_parse(&file, &info) != ESP_OK)
    {
        ESP_LOGW(TAG, "skip non-wav: %s", path);
        f_close(&file);
        return ESP_FAIL;
    }

    esp_err_t hw_ret = audio_hw_configure(info.sample_rate, info.bits_per_sample, info.channels);
    if (hw_ret != ESP_OK)
    {
        f_close(&file);
        return hw_ret;
    }

    hw_ret = audio_hw_start();
    if (hw_ret != ESP_OK)
    {
        f_close(&file);
        return hw_ret;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(AUDIO_IO_BUF_SIZE, MALLOC_CAP_DEFAULT);
    if (!buf)
    {
        ESP_LOGE(TAG, "malloc audio buffer failed");
        f_close(&file);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "play %s (%lu Hz, %u bit, %u ch)", path, (unsigned long)info.sample_rate, info.bits_per_sample, info.channels);

    bool was_paused = false;
    while (!s_stop)
    {
        audio_cmd_t cmd;
        if (s_cmd_queue && xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE)
        {
            if (cmd == AUDIO_CMD_NEXT)
            {
                s_track_index = (s_track_index + 1) % track_count;
            }
            else if (cmd == AUDIO_CMD_PREV)
            {
                s_track_index = (s_track_index == 0) ? (track_count - 1) : (s_track_index - 1);
            }
            if (interrupted)
            {
                *interrupted = true;
            }
            break;
        }

        UINT br = 0;
        fr = f_read(&file, buf, AUDIO_IO_BUF_SIZE, &br);
        if (fr != FR_OK || br == 0)
        {
            break;
        }

        if (s_paused)
        {
            if (!was_paused)
            {
                audio_hw_stop();
                was_paused = true;
                ESP_LOGI(TAG, "playback paused");
            }
            while (s_paused && !s_stop)
            {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (s_stop)
            {
                break;
            }
            ESP_ERROR_CHECK(audio_hw_start());
            was_paused = false;
            ESP_LOGI(TAG, "playback resumed");
        }

        size_t written = audio_hw_write(buf, br, pdMS_TO_TICKS(500));
        if (written == 0)
        {
            ESP_LOGW(TAG, "i2s write stalled");
            break;
        }
    }

    free(buf);
    audio_hw_stop();
    f_close(&file);
    return ESP_OK;
}

static void audio_task(void *args)
{
    static char tracks[MAX_TRACKS][MAX_NAME_LEN];
    size_t track_count = 0;

    while (!s_stop)
    {
        if (!audio_sdcard_is_mounted())
        {
            if (audio_sdcard_mount() != ESP_OK)
            {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        FF_DIR dir;
        FRESULT fr = f_opendir(&dir, AUDIO_MUSIC_DIR_FAT);
        if (fr != FR_OK)
        {
            ESP_LOGW(TAG, "dir %s missing, waiting for files", AUDIO_MUSIC_DIR_FAT);
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }

        track_count = 0;
        FILINFO finfo;
        while (!s_stop && track_count < MAX_TRACKS)
        {
            fr = f_readdir(&dir, &finfo);
            if (fr != FR_OK || finfo.fname[0] == 0)
            {
                break;
            }

            if (!is_wav_file(finfo.fname))
            {
                continue;
            }

            strlcpy(tracks[track_count], finfo.fname, MAX_NAME_LEN);
            track_count++;
        }

        f_closedir(&dir);

        if (track_count == 0)
        {
            ESP_LOGI(TAG, "no wav files in %s", AUDIO_MUSIC_DIR);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (s_track_index >= track_count)
        {
            s_track_index = 0;
        }

        while (!s_stop)
        {
            /* drain pending commands before selecting next track */
            audio_cmd_t cmd;
            while (s_cmd_queue && xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE)
            {
                if (cmd == AUDIO_CMD_NEXT)
                {
                    s_track_index = (s_track_index + 1) % track_count;
                }
                else if (cmd == AUDIO_CMD_PREV)
                {
                    s_track_index = (s_track_index == 0) ? (track_count - 1) : (s_track_index - 1);
                }
            }

            char full_path[260];
            snprintf(full_path, sizeof(full_path), "%s/%s", AUDIO_MUSIC_DIR_FAT, tracks[s_track_index]);
            bool interrupted = false;
            play_single(full_path, track_count, &interrupted);

            if (s_stop)
            {
                break;
            }

            if (!interrupted)
            {
                /* auto advance to next track */
                s_track_index = (s_track_index + 1) % track_count;
            }
        }
    }

    s_audio_task = NULL;
    vTaskDelete(NULL);
}

static void volume_task(void *args)
{
    const uint8_t step = 2;
    while (!s_stop)
    {
        uint8_t key = xl9555_keys_scan(0);
        if (key == XL9555_KEY1 || key == XL9555_KEY3)
        {
            uint8_t vol = audio_hw_get_volume();
            if (key == XL9555_KEY1 && vol > 0)
            {
                vol = (vol > step) ? (uint8_t)(vol - step) : 0;
            }
            else if (key == XL9555_KEY3 && vol < 33)
            {
                vol = (vol + step > 33) ? 33 : (uint8_t)(vol + step);
            }

            ESP_ERROR_CHECK(audio_hw_set_volume(vol));
            ESP_LOGI(TAG, "volume set to %u", vol);
        }
        else if (key == XL9555_KEY2)
        {
            s_paused = !s_paused;
            ESP_LOGI(TAG, "playback %s", s_paused ? "paused" : "resumed");
        }
        else if (key == XL9555_KEY0 && s_cmd_queue)
        {
            audio_cmd_t cmd = AUDIO_CMD_PREV;
            if (xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE)
            {
                ESP_LOGI(TAG, "track cmd: prev");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(120));
    }

    s_volume_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_player_init(void)
{
    if (s_inited)
    {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(audio_hw_init(), TAG, "hw init fail");
    ESP_RETURN_ON_ERROR(xl9555_keys_init(), TAG, "keys init fail");
    ESP_RETURN_ON_ERROR(audio_sdcard_mount(), TAG, "sd mount fail");
    s_inited = true;
    return ESP_OK;
}

esp_err_t audio_player_start(void)
{
    if (!s_inited)
    {
        ESP_RETURN_ON_ERROR(audio_player_init(), TAG, "init before start");
    }

    if (s_audio_task)
    {
        return ESP_OK;
    }

    if (!s_cmd_queue)
    {
        s_cmd_queue = xQueueCreate(8, sizeof(audio_cmd_t));
        ESP_RETURN_ON_FALSE(s_cmd_queue != NULL, ESP_FAIL, TAG, "queue alloc fail");
    }

    s_stop = false;
    BaseType_t res = xTaskCreate(audio_task, "audio_player", AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIO, &s_audio_task);
    BaseType_t res_vol = xTaskCreate(volume_task, "audio_volume", VOLUME_TASK_STACK, NULL, VOLUME_TASK_PRIO, &s_volume_task);
    return (res == pdPASS && res_vol == pdPASS) ? ESP_OK : ESP_FAIL;
}

void audio_player_stop(void)
{
    if (!s_audio_task && !s_volume_task)
    {
        return;
    }

    s_stop = true;
    while (s_audio_task || s_volume_task)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (s_cmd_queue)
    {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
    }
}

bool audio_player_is_running(void)
{
    return s_audio_task != NULL;
}
