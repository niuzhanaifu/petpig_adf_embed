/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/temperature_sensor.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_check.h"
#include "esp_spiffs.h"
#include "cJSON.h"
#include "esp_vad.h"

#include "audio_pipeline.h"
#include "audio_element.h"
#include "audio_common.h"
#include "audio_recorder.h"
#include "esp_psram.h"

#include "esp_audio.h"
#include "filter_resample.h"
#include "raw_stream.h"
#include "recorder_sr.h"
#include "pig_wifi.h"
#include "esp_websocket_client.h"
#include "speaker_config.h"
#include "es8311.h"
#include "pig_server.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define TEST_IN_MYSELF
#ifdef TEST_IN_MYSELF
    #define WEB_URI "ws://192.168.255.160:8000/smart/chat"
#else
    #define WEB_URI "ws://115.190.136.178:8000/ws/smart/chat"
#endif

static const char *TAG = "common_pignet";

static void enable_ns4150(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << NS4150B_CTRL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(NS4150B_CTRL, 1);
    ESP_LOGI(ES_TAG, "enable enable_ns4150!");
    return;
}

static esp_err_t es8311_codec_init(void)
{
    /* Initialize I2C peripheral */
    ESP_LOGI(TAG, "Using user es8311_codec_init!");
    const i2c_config_t es_i2c_cfg = {
        .sda_io_num = I2C_SDA_IO,
        .scl_io_num = I2C_SCL_IO,
        .mode = I2C_MODE_MASTER,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_NUM, &es_i2c_cfg), TAG, "config i2c failed");
    ESP_RETURN_ON_ERROR(i2c_driver_install(I2C_NUM, I2C_MODE_MASTER,  0, 0, 0), TAG, "install i2c driver failed");

    /* Initialize es8311 codec */
    es8311_handle_t es_handle = es8311_create(I2C_NUM, ES8311_ADDRRES_0);
    ESP_RETURN_ON_FALSE(es_handle, ESP_FAIL, TAG, "es8311 create failed");
    const es8311_clock_config_t es_clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = USER_MCLK_FREQ_HZ,
        .sample_frequency = USER_SAMPLE_RATE
    };
    ESP_ERROR_CHECK(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
    ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(es_handle, USER_SAMPLE_RATE * USER_MCLK_MULTIPLE, USER_SAMPLE_RATE), TAG, "set es8311 sample frequency failed");
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(es_handle, USER_VOICE_VOLUME, NULL), TAG, "set es8311 volume failed");
    ESP_RETURN_ON_ERROR(es8311_microphone_config(es_handle, false), TAG, "set es8311 microphone failed");
    // ESP_RETURN_ON_ERROR(es8311_microphone_gain_set(es_handle, ES8311_MIC_GAIN_12DB), TAG, "set es8311 microphone gain failed");
    return ESP_OK;
}

static esp_err_t i2s_driver_init(pignet_server_handle_t p)
{
    ESP_LOGI(TAG, "Using user i2s_driver_init");
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &p->tx_handle, &p->rx_handle));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(USER_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK_IO,
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = USER_MCLK_MULTIPLE;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(p->rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(p->rx_handle));

    std_cfg.clk_cfg.sample_rate_hz = USER_SAMPLE_RATE;
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(p->tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(p->tx_handle));

    return ESP_OK;
}

RingBuffer* ringbuf_init(size_t capacity) {
    RingBuffer *rb = malloc(sizeof(RingBuffer));
    rb->buffer = malloc(capacity);
    rb->capacity = capacity;
    rb->in = rb->out = 0;
    rb->mutex = xSemaphoreCreateMutex();
    return rb;
}

size_t ringbuf_write(RingBuffer *rb, const uint8_t *data, size_t len) {
    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    size_t write_len = len;
    if (write_len > rb->capacity) {
        data += (write_len - rb->capacity);
        write_len = rb->capacity;
    }
    size_t used = (rb->in + rb->capacity - rb->out) % rb->capacity;
    size_t space = rb->capacity - used;
    if (write_len > space) {
        // 覆盖旧数据，调整out指针
        ESP_LOGE(RING_BUF, "ring buf is full!");
        rb->out = (rb->out + (write_len - space)) % rb->capacity;
    }
    size_t first = MIN(write_len, rb->capacity - rb->in);
    memcpy(rb->buffer + rb->in, data, first);
    if (write_len > first) {
        memcpy(rb->buffer, data + first, write_len - first);
    }
    rb->in = (rb->in + write_len) % rb->capacity;
    xSemaphoreGive(rb->mutex);
    return write_len;
}

size_t ringbuf_read(RingBuffer *rb, uint8_t *data, size_t len, TickType_t timeout) {
    if (xSemaphoreTake(rb->mutex, timeout) != pdTRUE) {
        return 0;
    }
    size_t used = (rb->in + rb->capacity - rb->out) % rb->capacity;
    size_t read_len = MIN(len, used);
    if (read_len == 0) {
        xSemaphoreGive(rb->mutex);
        return 0;
    }
    size_t first = MIN(read_len, rb->capacity - rb->out);
    memcpy(data, rb->buffer + rb->out, first);
    if (read_len > first) {
        memcpy(data + first, rb->buffer, read_len - first);
    }
    rb->out = (rb->out + read_len) % rb->capacity;
    xSemaphoreGive(rb->mutex);
    return read_len;
}

static void send_voice_tag_to_server(pignet_server_handle_t pig_server, int type)
{
    int                send_res = -1;
    cJSON             *root = cJSON_CreateObject();

    if (type == BEGIN_VOICE) {
        cJSON_AddStringToObject(root, "type", "streaming_voice_start");
        cJSON_AddNumberToObject(root, "sample_rate", 16000);
        cJSON_AddNumberToObject(root, "input_sample_rate", 48000);
        cJSON_AddStringToObject(root, "format", "pcm");
        cJSON_AddNumberToObject(root, "child_age", 5);
        cJSON_AddStringToObject(root, "user_name", "test_user");
    } else if(type == END_VOICE) {
        cJSON_AddStringToObject(root, "type", "streaming_voice_end");
        cJSON_AddNumberToObject(root, "child_age", 5);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    if (esp_websocket_client_is_connected(pig_server->client) && wifi_connected() == ESP_OK) {
        send_res = esp_websocket_client_send_text(pig_server->client, json_str, strlen(json_str), pdMS_TO_TICKS(1000));
        if (send_res != -1) {
            ESP_LOGW(WEBSOCKET_SND, "send %d msg to server success", send_res);
        }
        if (type == END_VOICE) {
            pig_server->recive_music_status = MIC_SALIENT;
        } else if (type == BEGIN_VOICE) {
            pig_server->recive_music_status = DURING_VOICE;
        }
        ESP_LOGI(WEBSOCKET_SND, "send tag:%d, text:%s to server, res:%d", type, json_str, send_res);
    } else {
        ESP_LOGW(WEBSOCKET_SND, "WebSocket or internet not connect, don't send msg!");
    }
    cJSON_Delete(root);
    free(json_str);
}

static void send_msg_to_server_task(void *arg)
{
    pignet_server_handle_t   p = (pignet_server_handle_t)(arg);
    int                      send_res = 0;
    int16_t                 *wakenet_buffer = NULL;

    while (audio_pipeline_get_state(p->pipeline, AEL_STATE_RUNNING) != ESP_OK) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    wakenet_buffer = (int16_t *) malloc(p->audio_chunksize);
    if (wakenet_buffer == NULL) {
        ESP_LOGE(WEBSOCKET_SND, "Wakenet buffer malloc failed:%s", strerror(errno));
        vTaskDelete(NULL);
    }

    while (1) {
        size_t read_len = raw_stream_read(p->raw_write, wakenet_buffer, p->audio_chunksize);
        // size_t read_len = raw_stream_read(p->raw_write, buffer, MAX_RECV_BUF_SIZE);
        // size_t read_len = ringbuf_read(p->mic_ringbuf, (uint8_t *)buffer, MAX_RECV_BUF_SIZE, pdMS_TO_TICKS(100));
        if (read_len == 0) {
            vTaskDelay(10 / portTICK_PERIOD_MS);
            continue;
        }

        if (p->recive_music_status == MIC_SALIENT) {
            wakenet_state_t state = p->wakenet->detect(p->model_data, wakenet_buffer);
            if (state != WAKENET_DETECTED) {
                continue;
            }
            ESP_LOGE(WEBSOCKET_SND, "voice has been detect!\n");
            p->recive_music_status = MIC_DETECT;
            xEventGroupSetBits(p->default_audio_play, PLAY_DEFAULT_AUDIO);
        }

        if (esp_websocket_client_is_connected(p->client) && p->recive_music_status != MIC_DETECT && p->request_tag == false) {
            if (p->recive_music_status != DURING_VOICE) {
                send_voice_tag_to_server(p, BEGIN_VOICE);
            }
            send_res = esp_websocket_client_send_bin(p->client, wakenet_buffer, read_len, pdMS_TO_TICKS(1000));
            if (send_res != -1) {
                ESP_LOGW(WEBSOCKET_SND, "send %d msg to server success", send_res);
                p->recive_music_status = DURING_VOICE;
                p->last_recv_time = esp_timer_get_time();
            }
        } else {
            ESP_LOGW(WEBSOCKET_SND, "Don't allow send voice binary to socket server, mic_status:%d, request_tag:%d", p->recive_music_status, p->request_tag);
        }
    }
    free(wakenet_buffer);
    vTaskDelete(NULL);
}

static void receive_data_from_mic_task(void *args)
{
    esp_err_t              ret = ESP_OK;
    pignet_server_handle_t p = (pignet_server_t *)args;
    int64_t                current_time = esp_timer_get_time();
    int                    vad_result = VAD_SILENCE;

    ESP_LOGI(REV_TAG, "start receive_data_from_mic_task task!");
    while (audio_pipeline_get_state(p->pipeline, AEL_STATE_RUNNING) != ESP_OK) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    while(1) {
        memset(p->recive_data, 0, MAX_RECV_BUF_SIZE);
        ret = i2s_channel_read(p->rx_handle, p->recive_data, MAX_RECV_BUF_SIZE, &p->bytes_read, 1000);
        if (ret != ESP_OK) {
            ESP_LOGE(REV_TAG, "i2s read failed, %d", ret);
            continue;
        }

        vad_result = vad_process(p->vad, p->recive_data, USER_SAMPLE_RATE, 30);
        if (vad_result == VAD_SPEECH && p->request_tag == false) {
            raw_stream_write(p->raw_read, (char *)p->recive_data, p->bytes_read);
            // ringbuf_write(p->mic_ringbuf, (uint8_t *)p->recive_data, p->bytes_read);
        } else {
            ESP_LOGD(REV_TAG, "Silence detected:%d, or have recive request:%d", vad_result, p->request_tag);
            current_time = esp_timer_get_time();
            if (p->recive_music_status == DURING_VOICE && (current_time - p->last_recv_time) >= 3*1000*1000) {
                send_voice_tag_to_server(p, END_VOICE);
                p->last_recv_time = current_time;
            }
        }
    }
}

static void send_data_to_mic_task(void *args)
{
    ESP_LOGI(SND_TAG, "send_data_to_mic_task start");
    esp_err_t               ret = ESP_OK;
    pignet_server_handle_t  p = (pignet_server_t *)args;
    uint8_t                *i2s_buffer = malloc(MAX_RECV_BUF_SIZE);
    size_t                  read_len = 0;
    if (i2s_buffer == NULL) {
        ESP_LOGE(SND_TAG, "I2S play buffer malloc failed:%s", strerror(errno));
        vTaskDelete(NULL);
    }

    while(1) {
        read_len = ringbuf_read(p->audio_ringbuf, i2s_buffer, MAX_RECV_BUF_SIZE, pdMS_TO_TICKS(200));
        if (read_len == 0) {
            vTaskDelay(10 / portTICK_PERIOD_MS);
            continue;
        }

        p->request_tag = true;
        p->last_send_time = esp_timer_get_time();
        ret = i2s_channel_write(p->tx_handle, i2s_buffer, read_len, &p->bytes_write, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(SND_TAG, "[music] i2s write failed, %d", ret);
        }
        if (p->bytes_write == read_len) {
            ESP_LOGI(SND_TAG, "[music] i2s music played, %d bytes are written.", p->bytes_write);
        } else {
            ESP_LOGE(SND_TAG, "[music] i2s music play failed.");
        }
    }
    if (i2s_buffer) {
        free(i2s_buffer);
    }
    vTaskDelete(NULL);
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    pignet_server_handle_t      pig_server = (pignet_server_handle_t)handler_args;
    switch (event_id) {
        case WEBSOCKET_EVENT_BEGIN:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_BEGIN");
            break;
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
            pig_server->server_finish = false;
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 0x2) {
                ESP_LOGI(TAG, "Received binary data:%d", data->data_len);
                if (data->data_len >= 0) {
                    size_t written = ringbuf_write(pig_server->audio_ringbuf, (uint8_t *)data->data_ptr, data->data_len);
                }
                break;
            } else if (data->op_code == 0x08 && data->data_len == 2) {
                ESP_LOGW(TAG, "Received closed message with code=%d", 256 * data->data_ptr[0] + data->data_ptr[1]);
                break;
            } else {
                // ESP_LOGW(TAG, "op_code:%d, Received msg:%d, msg:%s", data->op_code, data->data_len, data->data_ptr);
                char *json_str = calloc(data->data_len + 1, 1);
                if (json_str) {
                    memcpy(json_str, data->data_ptr, data->data_len);
                    json_str[data->data_len] = '\0';
                    cJSON *json = cJSON_Parse(json_str);
                    if (json) {
                        cJSON_Delete(json);
                    } else {
                        ESP_LOGW(TAG, "Failed to parse JSON");
                    }
                    free(json_str);
                }
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_ERROR");
            break;
        case WEBSOCKET_EVENT_FINISH:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_FINISH");
            pig_server->server_finish = true;
            break;
        }
}

static void websocket_app_start(void *args)
{
    pignet_server_handle_t p = (pignet_server_handle_t)args;
    esp_websocket_client_config_t websocket_cfg = {.buffer_size = 4096,
                                                   .reconnect_timeout_ms = 2000};
    websocket_cfg.uri = WEB_URI;

    ESP_LOGI(TAG, "Connecting to %s...", websocket_cfg.uri);

    p->client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(p->client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)args);
    esp_websocket_client_start(p->client);
}

static void time_check_tag(void *arg)
{
    pignet_server_handle_t pig_server = (pignet_server_handle_t)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));

        int64_t current_time_us = esp_timer_get_time();
        int64_t delta_us = current_time_us - pig_server->last_send_time;

        if (delta_us >= 500*1000 && pig_server->request_tag == true) {
            pig_server->request_tag = false;
            pig_server->last_send_time = current_time_us;
            ESP_LOGE(TAG, "recover pig_server->request_tag = false");
        }

        if (pig_server->server_finish == true) {
            esp_websocket_client_start(pig_server->client);
        }
    }
    vTaskDelete(NULL);
}

#ifdef CONFIG_DEBUG_SMALL_PIG
/* Only for test: Import music file as buffer */
extern const uint8_t music_pcm_start[] asm("_binary_rcv_pcm_start");
extern const uint8_t music_pcm_end[]   asm("_binary_rcv_pcm_end");
static void i2s_music(void *args)
{
    pignet_server_handle_t p = (pignet_server_t *)args;
    const uint8_t *data_ptr = music_pcm_start;
    size_t total_len = music_pcm_end - music_pcm_start;

    while (esp_websocket_client_is_connected(p->client) == false) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    send_voice_tag_to_server(p, BEGIN_VOICE);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    while ((size_t)(data_ptr - music_pcm_start) < total_len) {
        size_t chunk_size = (total_len - (data_ptr - music_pcm_start)) > MAX_RECV_BUF_SIZE ? MAX_RECV_BUF_SIZE : (total_len - (data_ptr - music_pcm_start));
        uint8_t temp_buf[MAX_RECV_BUF_SIZE];
        memcpy(temp_buf, data_ptr, chunk_size);
        vTaskDelay(200 / portTICK_PERIOD_MS);
        ringbuf_write(p->mic_ringbuf, (uint8_t *)temp_buf, chunk_size);
        ESP_LOGI(SND_TAG, "send msg to server success:%d", chunk_size);
        data_ptr += chunk_size;
    }
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    send_voice_tag_to_server(p, END_VOICE);
    vTaskDelete(NULL);
}

static void i2s_echo(void *args)
{
    pignet_server_handle_t p = (pignet_server_t *)args;
    int16_t *mic_data = malloc(MAX_RECV_BUF_SIZE); // 单通道输入
    if (!mic_data) {
        ESP_LOGE(TAG, "[echo] No memory for read data buffer");
        abort();
    }

    esp_err_t ret = ESP_OK;
    size_t bytes_read = 0;
    size_t bytes_write = 0;
    ESP_LOGI(TAG, "[echo] Echo start");

    while (1) {
        memset(mic_data, 0, MAX_RECV_BUF_SIZE);

        /* Read sample data from mic */
        ret = i2s_channel_read(p->rx_handle, mic_data, MAX_RECV_BUF_SIZE, &bytes_read, 1000);
        if (ret != ESP_OK) {
            ESP_LOGE(REV_TAG, "[echo] i2s read failed, %d", ret);
            abort();
        }

        /* Write sample data to earphone */
        ret = i2s_channel_write(p->tx_handle, mic_data, bytes_read, &bytes_write, 1000);
        if (ret != ESP_OK) {
            ESP_LOGE(SND_TAG, "[echo] i2s write failed, %d", ret);
            abort();
        }

        if (bytes_read != bytes_write) {
            ESP_LOGW(SND_TAG, "[echo] %d bytes stereo prepared but only %d bytes are written", bytes_read * 2, bytes_write);
        }
    }

    free(mic_data);
    vTaskDelete(NULL);
}
#endif

static void player_default_audio_task(void *arg)
{
    pignet_server_handle_t pig_server = (pignet_server_handle_t)arg;
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs0",
        .partition_label = "src_audio",
        .max_files = 5,
        .format_if_mount_failed = true
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        vTaskDelete(NULL);
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    const char *file_path = "/spiffs0/action.wav";
    FILE *f = fopen(file_path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file %s", file_path);
        vTaskDelete(NULL);
    }

    // 获取文件大小
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);
    ESP_LOGI(TAG, "File size: %ld bytes", file_size);

    // 分配内存存储音频数据
    uint8_t *audio_buf = (uint8_t *)malloc(file_size);
    if (!audio_buf) {
        ESP_LOGE(TAG, "malloc failed!");
        fclose(f);
        vTaskDelete(NULL);
    }

    fread(audio_buf, 1, file_size, f);
    fclose(f);

    ESP_LOGI(TAG, "Read audio data into RAM, start playing...");

    size_t read_bytes = 0, written_bytes = 0;
    while (1) {
        xEventGroupWaitBits(pig_server->default_audio_play,
                            PLAY_DEFAULT_AUDIO,
                            pdTRUE,   // 播放后清除事件位
                            pdFALSE,  // 不需要同时等待多个bit
                            portMAX_DELAY);
        read_bytes = 0;
        ESP_LOGE(TAG, "Start to play default audio");
        while (read_bytes < file_size) {
            if ((file_size - read_bytes) < 1024) {
                written_bytes = ringbuf_write(pig_server->audio_ringbuf, (uint8_t *)audio_buf + read_bytes, file_size - read_bytes);
            } else {
                written_bytes = ringbuf_write(pig_server->audio_ringbuf, (uint8_t *)audio_buf + read_bytes, 1024);
            }
            read_bytes += written_bytes;
            vTaskDelay(20 / portTICK_PERIOD_MS);
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
        pig_server->recive_music_status = BEGIN_VOICE;
        pig_server->last_recv_time = esp_timer_get_time();
    }
    vTaskDelete(NULL);
}

static int input_cb_for_afe(int16_t *buffer, int buf_sz, void *user_ctx, TickType_t ticks)
{
    ESP_LOGI(TAG, "into input_cb_for_afe");
    vTaskDelay(100 / portTICK_PERIOD_MS);
    return 0;
    // return raw_stream_read(raw_read, (char *)buffer, buf_sz);
}

static esp_err_t rec_engine_cb(audio_rec_evt_t *event, void *user_data)
{
    return ESP_OK;
}

void app_main(void)
{
    /* Step1:  Initialize Net and NVS */
    esp_err_t ret;
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Free memory: %" PRIu32 " bytes PSRAM memory %zu bytes", esp_get_free_heap_size(), esp_psram_get_size());
    // 语音识别AFE代码，暂时保留
    // char *audio_sr_input_fmt = "RM";
    // recorder_sr_cfg_t recorder_sr_cfg = DEFAULT_RECORDER_SR_CFG(audio_sr_input_fmt, "model", AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    // recorder_sr_cfg.afe_cfg->afe_ringbuf_size = 30;
    // recorder_sr_cfg.afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_INTERNAL;
    // recorder_sr_cfg.afe_cfg->wakenet_init = true;
    // recorder_sr_cfg.afe_cfg->vad_init = false;
    // // recorder_sr_cfg.afe_cfg->vad_mode = VAD_MODE_4;
    // recorder_sr_cfg.multinet_init = false;
    // recorder_sr_cfg.rb_size = 2048;
    // recorder_sr_cfg.afe_cfg->pcm_config.mic_num = 1;
    // recorder_sr_cfg.afe_cfg->pcm_config.ref_num = 1;
    // recorder_sr_cfg.afe_cfg->pcm_config.total_ch_num = 1;
    // recorder_sr_cfg.afe_cfg->pcm_config.sample_rate = 16000;
    // recorder_sr_cfg.afe_cfg->wakenet_mode = DET_MODE_90;
    // audio_rec_cfg_t cfg = AUDIO_RECORDER_DEFAULT_CFG();
    // cfg.read = (recorder_data_read_t)&input_cb_for_afe;
    // cfg.sr_handle = recorder_sr_create(&recorder_sr_cfg, &cfg.sr_iface);
    // if (cfg.sr_handle == NULL) {
    //     printf("recorder_sr_create failed\n");
    //     goto LOOP;
    // }
    // cfg.event_cb = rec_engine_cb;
    // cfg.vad_off = 1000;
    // audio_rec_handle_t  recorder = audio_recorder_create(&cfg);
    // printf("recorder : %p\n", recorder);

    /* Step2: Initialize and connect wifi */
    ret = init_wifi();
    if (ret == ESP_OK) {
        ESP_LOGI(PIG_SERVRE, "init wif success!\n");
    }

    /* Step3: Initialize pig server */
    pignet_server_handle_t pig_server = (pignet_server_handle_t)calloc(1, sizeof(pignet_server_t));
    if (pig_server == NULL) {
        ESP_LOGE(PIG_SERVRE, "calloc failed pig_server");
        abort();
    }
    pig_server->audio_ringbuf = ringbuf_init(RING_BUF_MAX);
    // pig_server->mic_ringbuf = ringbuf_init(MIC_BUF_MAX);
    pig_server->vad = vad_create(VAD_MODE_2);
    pig_server->recive_music_status = MIC_SALIENT;
    pig_server->request_tag = false;
    pig_server->server_finish = false;
    pig_server->default_audio_play = xEventGroupCreate();

    /* Step4:  Initialize es8311 */
    if (i2s_driver_init(pig_server) != ESP_OK) {
        ESP_LOGE(PIG_SERVRE, "i2s driver init failed");
        abort();
    } else {
        ESP_LOGI(PIG_SERVRE, "i2s driver init success");
    }
    enable_ns4150();
    if (es8311_codec_init() != ESP_OK) {
        ESP_LOGE(PIG_SERVRE, "es8311 codec init failed");
        abort();
    } else {
        ESP_LOGI(PIG_SERVRE, "es8311 codec init success");
    }

    /* Step5:  Initialize task */
    #ifdef CONFIG_DEBUG_SMALL_PIG
        xTaskCreate(i2s_music, "i2s_music", 8192, (void *)pig_server, 5, NULL);
    #else
        xTaskCreate(receive_data_from_mic_task, "receive_data_from_mic_task", 4096, (void *)pig_server, 5, NULL);
    #endif
    xTaskCreate(send_data_to_mic_task, "send_data_to_mic_task", 8192, (void *)pig_server, 5, NULL);
    xTaskCreate(send_msg_to_server_task, "send_msg_to_server_task", 8192, (void *)pig_server, 5, NULL);
    xTaskCreate(time_check_tag, "time_check_tag", 2048, (void *)pig_server, 5, NULL);

    /* Step6:  Initialize websocket app */
    websocket_app_start((void *)pig_server);

    /* Step7:  Initialize wake word detection */
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(PIG_SERVRE, "No srnodel model found");
        abort();
    }
    char *model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, "hilexin");
    if (model_name == NULL) {
        ESP_LOGE(PIG_SERVRE, "No hilexin model found");
        abort();
    }
    pig_server->wakenet = (esp_wn_iface_t*)esp_wn_handle_from_name(model_name);
    if (pig_server->wakenet == NULL) {
        ESP_LOGE(PIG_SERVRE, "No wakenet handle found");
        abort();
    }
    pig_server->model_data = pig_server->wakenet->create(model_name, DET_MODE_95);
    if (pig_server->model_data == NULL) {
        ESP_LOGE(PIG_SERVRE, "create wakenet model data failed!");
        abort();
    }
    pig_server->audio_chunksize = pig_server->wakenet->get_samp_chunksize(pig_server->model_data) * sizeof(int16_t);
    ESP_LOGI(PIG_SERVRE, "pig_server->audio_chunksize:%d", pig_server->audio_chunksize);

    /* Step8:  Initialize audio pipeline */
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pig_server->pipeline = audio_pipeline_init(&pipeline_cfg);

    raw_stream_cfg_t raw_cfg_in = RAW_STREAM_CFG_DEFAULT();
    raw_cfg_in.type = AUDIO_STREAM_READER;
    pig_server->raw_read = raw_stream_init(&raw_cfg_in);

    rsp_filter_cfg_t filter_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    filter_cfg.src_rate = USER_SAMPLE_RATE;
    filter_cfg.src_ch = I2S_SLOT_MODE_STEREO;
    filter_cfg.dest_rate = USER_SAMPLE_RATE;
    filter_cfg.dest_ch = I2S_SLOT_MODE_MONO;
    pig_server->filter = rsp_filter_init(&filter_cfg);

    raw_stream_cfg_t raw_cfg_out = RAW_STREAM_CFG_DEFAULT();
    raw_cfg_out.type = AUDIO_STREAM_WRITER;
    pig_server->raw_write = raw_stream_init(&raw_cfg_out);

    audio_pipeline_register(pig_server->pipeline, pig_server->raw_read, "raw_in");
    audio_pipeline_register(pig_server->pipeline, pig_server->filter, "filter");
    audio_pipeline_register(pig_server->pipeline, pig_server->raw_write, "raw_out");

    const char *link_tag[3] = {"raw_in", "filter", "raw_out"};
    audio_pipeline_link(pig_server->pipeline, &link_tag[0], 3);
    audio_pipeline_run(pig_server->pipeline);

    // step9: Create player default audio task
    xTaskCreate(player_default_audio_task, "player_default_audio_task", 4096, (void *)pig_server, 5, NULL);

    // step10: monitor free size and temperature
    ESP_LOGI(TAG, "Install temperature sensor, expected temp ranger range: 10~50 ℃");
    temperature_sensor_handle_t temp_sensor = NULL;
    temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
    ESP_ERROR_CHECK(temperature_sensor_install(&temp_sensor_config, &temp_sensor));

    ESP_LOGI(TAG, "Enable temperature sensor");
    ESP_ERROR_CHECK(temperature_sensor_enable(temp_sensor));

    ESP_LOGI(TAG, "Read temperature");
    float tsens_value;

    while(1) {
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        ESP_ERROR_CHECK(temperature_sensor_get_celsius(temp_sensor, &tsens_value));
        ESP_LOGI(TAG, "Temperature value %.02f ℃", tsens_value);
        ESP_LOGI(TAG, "Free memory: %" PRIu32 " bytes PSRAM memory %zu bytes", esp_get_free_heap_size(), esp_psram_get_size());
    }
}
