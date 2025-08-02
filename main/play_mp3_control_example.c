/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "driver/gpio.h"
#if IP_NAPT
#include "lwip/lwip_napt.h"
#endif
#include "lwip/err.h"
#include "lwip/sys.h"

#include "driver/i2s_std.h"
#include "esp_system.h"
#include "esp_check.h"
#include "example_config.h"
#include "es8311.h"
#include "pig_server.h"
#include "esp_efuse_table.h"
#include "esp_http_client.h"
#include "inc_wifi.h"
#include <esp_http_server.h>
#include <esp_websocket_client.h>
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "esp_vad.h"
#include <errno.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define TEST_URI "ws://192.168.4.160:8765/smart/chat"
#define WEB_URI "ws://115.190.136.178:8000/ws/smart/chat"
#define XIKAS_URI "ws://192.168.1.42:8000/ws/smart/chat"

static const char *TAG = "i2s_es8311";

static esp_err_t es8311_codec_init(void)
{
    /* Initialize I2C peripheral */
    //todo:配置优化，需要再看下芯片手册和源码如何配置优化
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
    chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &p->tx_handle, &p->rx_handle));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(USER_SAMPLE_RATE),
        // .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
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

    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(p->tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(p->tx_handle));

    return ESP_OK;
}

RingBuffer* ringbuf_init(size_t capacity) {
    RingBuffer *rb = malloc(sizeof(RingBuffer));
    rb->buffer = malloc(capacity);
    rb->capacity = capacity;
    rb->in = rb->out = rb->len = 0;
    rb->mutex = xSemaphoreCreateMutex();
    rb->sem = xSemaphoreCreateCounting(capacity, 0); // 计数信号量，最多capacity个数据
    return rb;
}

size_t ringbuf_write(RingBuffer *rb, const uint8_t *data, size_t len) {
    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    size_t write_len = MIN(len, rb->capacity - rb->len);
    if (write_len == 0) {
        xSemaphoreGive(rb->mutex);
        ESP_LOGE(RING_BUF, "ring buf is full!");
        return 0;
    }
    size_t first = MIN(write_len, rb->capacity - rb->in);
    memcpy(rb->buffer + rb->in, data, first);
    size_t second = write_len - first;
    if (second > 0) {
        memcpy(rb->buffer, data + first, second);
    }

    rb->in = (rb->in + write_len) % rb->capacity;
    rb->len += write_len;
    xSemaphoreGive(rb->mutex);
    xSemaphoreGive(rb->sem);
    return write_len;
}

size_t ringbuf_read(RingBuffer *rb, uint8_t *data, size_t len, TickType_t timeout) {
    if (xSemaphoreTake(rb->sem, timeout) != pdTRUE) {
        return 0;
    }
    if (rb->len < SEND_I2S_SZIE) {
        return 0;
    }
    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    size_t read_len = MIN(len, rb->len);

    size_t first = MIN(read_len, rb->capacity - rb->out);
    memcpy(data, rb->buffer + rb->out, first);
    size_t second = read_len - first;
    if (second > 0) {
        memcpy(data + first, rb->buffer, second);
    }

    rb->out = (rb->out + read_len) % rb->capacity;
    rb->len -= read_len;
    xSemaphoreGive(rb->mutex);
    return read_len;
}

static void send_voice_tag_to_server(pignet_server_handle_t pig_server, int type)
{
    int                send_res = -1;
    wifi_ap_record_t   ap_record = {0};
    cJSON             *root = cJSON_CreateObject();

    if (type == BEGIN_VOICE) {
        cJSON_AddStringToObject(root, "type", "streaming_voice_start");
        cJSON_AddNumberToObject(root, "sample_rate", 16000);
        cJSON_AddStringToObject(root, "format", "wav");
        cJSON_AddNumberToObject(root, "child_age", 5);
        cJSON_AddNumberToObject(root, "input_sample_rate", 48000);
        cJSON_AddStringToObject(root, "user_name", "test_user");
    } else if(type == END_VOICE) {
        cJSON_AddStringToObject(root, "type", "streaming_voice_end");
        cJSON_AddNumberToObject(root, "child_age", 5);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    if (esp_websocket_client_is_connected(pig_server->client) && esp_wifi_sta_get_ap_info(&ap_record) == ESP_OK) {
        send_res = esp_websocket_client_send_text(pig_server->client, json_str, strlen(json_str), pdMS_TO_TICKS(1000));
        if (send_res != -1) {
            ESP_LOGW(WEBSOCKET_SND, "send %d msg to server success", send_res);
        }
        if (type == END_VOICE) {
            pig_server->recive_music_status = END_VOICE;
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

static void send_msg_websocket_task(void *arg)
{
    pignet_server_handle_t p = (pignet_server_handle_t)(arg);
    char                   buffer[xQueue_Size] = {0};
    size_t                 encode_length = 0;
    int                    encode_res = 0;
    int                    send_res = 0;
    wifi_ap_record_t       ap_record = {0};
    cJSON                 *root = NULL;

    while (1) {
        memset(buffer, 0, xQueue_Size);
        if (xQueueReceive(p->send_websocket_queue, buffer, pdMS_TO_TICKS(50))) {
            // encode_length = 0;
            // encode_res = mbedtls_base64_encode(NULL, 0, &encode_length, (const unsigned char *)buffer, sizeof(buffer));
            // if (encode_length == 0) {
            //     continue;
            // }
            // char *encoded_data = (char*)malloc(encode_length);
            // if (encoded_data == NULL) {
            //     ESP_LOGW(WEBSOCKET_SND, "malloc %d failed, res:%s, now heap size: %d bytes", encode_length, strerror(errno), heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
            //     continue;
            // }
            // encode_res = mbedtls_base64_encode((unsigned char *)encoded_data, encode_length, &encode_length, (const unsigned char *)buffer, sizeof(buffer));
            // if (encode_res != 0) {
            //     ESP_LOGW(WEBSOCKET_SND, "Base64解码失败: %d\n", encode_res);
            //     free(encoded_data);
            //     continue;
            // }
            // root = cJSON_CreateObject();
            // cJSON_AddStringToObject(root, "audio_format", "wav");
            // cJSON_AddStringToObject(root, "audio_data", encoded_data);

            // char *json_str = cJSON_PrintUnformatted(root);
            if (esp_websocket_client_is_connected(p->client) && esp_wifi_sta_get_ap_info(&ap_record) == ESP_OK && p->request_tag == false) {
                if (p->recive_music_status != DURING_VOICE) {
                    send_voice_tag_to_server(p, BEGIN_VOICE);
                }
                send_res = esp_websocket_client_send_bin(p->client, buffer, xQueue_Size, pdMS_TO_TICKS(1000));
                if (send_res != -1) {
                    ESP_LOGW(WEBSOCKET_SND, "send %d msg to server success", send_res);
                    p->recive_music_status = DURING_VOICE;
                    p->last_recv_time = esp_timer_get_time();
                }
            } else {
                ESP_LOGW(WEBSOCKET_SND, "WebSocket 未连接，消息丢弃");
            }
            // cJSON_Delete(root);
            // free(json_str);
            root = NULL;
        }
    }
    vTaskDelete(NULL);
}

static void i2s_recieve(void *args)
{
    esp_err_t              ret = ESP_OK;
    pignet_server_handle_t p = (pignet_server_t *)args;
    int64_t                current_time = esp_timer_get_time();

    ESP_LOGI(REV_TAG, "start i2s_recieve task");
    while(1) {
        memset(p->recive_data, 0, MAX_RECV_BUF_SIZE);
        /* Read sample data from mic */
        // if (p->shock) {
        //     ESP_LOGE(REV_TAG, "P->SHOCK IS TRUE");
        // }
        ret = i2s_channel_read(p->rx_handle, p->recive_data, MAX_RECV_BUF_SIZE, &p->bytes_read, 1000);
        if (ret != ESP_OK) {
            ESP_LOGE(REV_TAG, "i2s read failed, %d", ret);
            continue;
        }
        int vad_result = vad_process(p->vad, p->recive_data, USER_SAMPLE_RATE, 30);

        if (vad_result == 1 && p->request_tag == false && p->shock == true) {
            xQueueSend(p->send_websocket_queue, p->recive_data, pdMS_TO_TICKS(100));
        } else {
            ESP_LOGD(REV_TAG, "Silence detected:%d, or have recive request:%d, or shock status:%d", vad_result, p->request_tag, p->shock);
            current_time = esp_timer_get_time();
            if (p->recive_music_status == DURING_VOICE && (current_time - p->last_recv_time) >= 2*1000*1000) {
                send_voice_tag_to_server(p, END_VOICE);
                p->last_recv_time = current_time;
            }
        }
    }
}

static void i2s_send(void *args)
{
    esp_err_t ret = ESP_OK;
    ESP_LOGI(SND_TAG, "[i2s_send]  start");
    const size_t i2s_block_size = xQueue_Size; // 每次I2S写入的块大小（根据音频格式调整）
    uint8_t *i2s_buffer = malloc(i2s_block_size);
    if (i2s_buffer == NULL) {
        ESP_LOGE(SND_TAG, "I2S play buffer malloc failed");
        vTaskDelete(NULL);
    }

    pignet_server_handle_t p = (pignet_server_t *)args;

    while(1) {
        size_t read_len = ringbuf_read(p->audio_ringbuf, i2s_buffer, i2s_block_size, pdMS_TO_TICKS(200));
        if (read_len == 0) {
            continue;
        }

        size_t bytes_written;
        p->request_tag = true;
        p->last_send_time = esp_timer_get_time();
        ret = i2s_channel_write(p->tx_handle, i2s_buffer, read_len, &p->bytes_write, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(SND_TAG, "[music] i2s write failed, %d", ret);
        }
        if (p->bytes_write > 0) {
            ESP_LOGI(SND_TAG, "[music] i2s music played, %d bytes are written.", p->bytes_write);
        } else {
            ESP_LOGE(SND_TAG, "[music] i2s music play failed.");
        }
    }
}

#ifdef CONFIG_DEBUG_SMALL_PIG
/* Only for test: Import music file as buffer */
extern const uint8_t music_pcm_start[] asm("_binary_ack2_wav_start");
extern const uint8_t music_pcm_end[]   asm("_binary_ack2_wav_end");
static void i2s_music(void *args)
{
    esp_err_t ret = ESP_OK;
    size_t bytes_write = 0;
    uint8_t *data_ptr = (uint8_t *)music_pcm_start;
    pignet_server_handle_t p = (pignet_server_t *)args;
    /* (Optional) Disable TX channel and preload the data before enabling the TX channel,
     * so that the valid data can be transmitted immediately */
    ESP_ERROR_CHECK(i2s_channel_disable(p->tx_handle));
    ESP_ERROR_CHECK(i2s_channel_preload_data(p->tx_handle, data_ptr, music_pcm_end - data_ptr, &bytes_write));
    data_ptr += bytes_write;  // Move forward the data pointer

    /* Enable the TX channel */
    ESP_ERROR_CHECK(i2s_channel_enable(p->tx_handle));
    while (1) {
        /* Write music to earphone */
        ret = i2s_channel_write(p->tx_handle, data_ptr, music_pcm_end - data_ptr, &bytes_write, portMAX_DELAY);
        if (ret != ESP_OK) {
            /* Since we set timeout to 'portMAX_DELAY' in 'i2s_channel_write'
               so you won't reach here unless you set other timeout value,
               if timeout detected, it means write operation failed. */
            ESP_LOGE(TAG, "[music] i2s write failed, %d", ret);
            abort();
        }
        if (bytes_write > 0) {
            // ESP_LOGI(TAG, "[music] i2s music played, %d bytes are written.", bytes_write);
        } else {
            ESP_LOGE(TAG, "[music] i2s music play failed.");
            abort();
        }
        data_ptr = (uint8_t *)music_pcm_start;
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
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

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    pignet_server_handle_t pig_server = (pignet_server_handle_t)handler_args;
    switch (event_id) {
    case WEBSOCKET_EVENT_BEGIN:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_BEGIN");
        break;
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x2) { // Opcode 0x2 indicates binary data
            ESP_LOGI(TAG, "Received binary data:%d", data->data_len);
            if (data->data_len >= 0) {
                size_t written = ringbuf_write(pig_server->audio_ringbuf, (uint8_t *)data->data_ptr, data->data_len);
            }
            break;
        } else if (data->op_code == 0x08 && data->data_len == 2) {
            ESP_LOGW(TAG, "Received closed message with code=%d", 256 * data->data_ptr[0] + data->data_ptr[1]);
            break;
        } else {
            ESP_LOGW(TAG, "op_code:%d, Received msg:%d, msg:%s", data->op_code, data->data_len, data->data_ptr);
            char *json_str = calloc(data->data_len + 1, 1);
            if (json_str) {
                memcpy(json_str, data->data_ptr, data->data_len);
                json_str[data->data_len] = '\0';

                // 解析 JSON
                cJSON *json = cJSON_Parse(json_str);
                if (json) {
                    // 提取 base64 字段（假设 key 为 "audio_data"）
                    const cJSON *audio_data = cJSON_GetObjectItem(json, "message");
                    if (cJSON_IsString(audio_data)) {
                        ESP_LOGI(TAG, "Base64 audio_data (string): %s", audio_data->valuestring);
                    } else {
                        ESP_LOGW(TAG, "audio_data not found or not a string");
                    }

                    cJSON_Delete(json);
                } else {
                    ESP_LOGW(TAG, "Failed to parse JSON");
                }

                free(json_str);
            }
        }

        // If received data contains json structure it succeed to parse
        if (data == NULL || data->data_ptr == NULL || data->data_len == 0) {
            ESP_LOGE(TAG, "invalid data, don't send");
            break;
        }
        size_t decode_length = 0;
        int decode_res = mbedtls_base64_decode(NULL, 0, &decode_length, (const unsigned char *)data->data_ptr, data->data_len);
        if (decode_length == 0) {
            ESP_LOGW(TAG, "decode failed!,decode length = 0");
            break;
        }
        unsigned char *decoded_data = (unsigned char*)malloc(decode_length);
        if (decoded_data == NULL) {
            ESP_LOGW(TAG, "内存分配失败\n");
            break;
        }

        decode_res = mbedtls_base64_decode(decoded_data, decode_length, &decode_length, (const unsigned char *)data->data_ptr, data->data_len);
        if (decode_res != 0) {
            ESP_LOGW(TAG, "Base64解码失败: %d\n", decode_res);
            free(decoded_data);
            break;
        }
        // ESP_LOGW(TAG, "解码后的数据（16进制）:");
        // 每行打印16个字节，便于阅读
        // for (size_t i = 0; i < decode_length; i++) {
        //     // 每个字节用两位16进制表示，不足补0
        //     printf("%02X ", decoded_data[i]);
        //     // 每16个字节换行
        //     if ((i + 1) % 16 == 0) {
        //         printf("\n");
        //     }
        // }
        // 最后一行如果未满16个字节，手动换行
        // if (decode_length % 16 != 0) {
        //     printf("\n");
        // }
        size_t written = ringbuf_write(pig_server->audio_ringbuf, (uint8_t *)decoded_data, decode_length);
        if (written != decode_length) {
            ESP_LOGW(TAG, "Ring buffer overflow (数据丢失)");
        }
        // 清理资源
        free(decoded_data);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_ERROR");
        break;
    case WEBSOCKET_EVENT_FINISH:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_FINISH");
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
        vTaskDelay(pdMS_TO_TICKS(100));  // 每隔 100ms 检查一次

        int64_t current_time_us = esp_timer_get_time();
        int64_t delta_us = current_time_us - pig_server->last_send_time;

        if (delta_us >= 500*1000 && pig_server->request_tag == true) {
            pig_server->request_tag = false;
            pig_server->last_send_time = current_time_us;  // 重置时间
            ESP_LOGE(TAG, "RECOVER pig_server->request_tag = false");
        }
    }
}

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

static void gpio_isr_handler(void* arg) {
    pignet_server_handle_t p = (pignet_server_handle_t)arg;
    p->shock = !p->shock;
}

#define GPIO_INPUT_IO     GPIO_NUM_18
#define GPIO_INPUT_PIN_SEL  (1ULL << GPIO_INPUT_IO)
void app_main(void)
{
    /* Step1: Initialize Net and NVS */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Step2: Initialize pig server */
    pignet_server_handle_t pig_server = (pignet_server_handle_t)calloc(1, sizeof(pignet_server_t));
    if (pig_server == NULL) {
        ESP_LOGE(PIG_SERVRE, "calloc failed pig_server");
        abort();
    }
    pig_server->audio_ringbuf = ringbuf_init(RING_BUF_MAX);
    pig_server->send_websocket_queue = xQueueCreate(xQueue_Item, xQueue_Size);
    pig_server->vad = vad_create(VAD_MODE_4);
    pig_server->recive_music_status = END_VOICE;
    pig_server->request_tag = false;
    pig_server->shock = false;

    /* Step3:  Initialize es8311 */
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

    /* Step4:  Initialize and connect wifi */
    ret = init_wifi();
    if (ret == ESP_OK) {
        ESP_LOGI(PIG_SERVRE, "init wif success!\n");
    }

    /* Step5:  Initialize task */
    // xTaskCreate(i2s_echo, "i2s_echo", 8192, (void *)pig_server, 5, NULL);
    xTaskCreate(i2s_recieve, "i2s_recieve", 8192, (void *)pig_server, 5, NULL);
    xTaskCreate(i2s_send, "i2s_send", 8192, (void *)pig_server, 5, NULL);
    xTaskCreate(send_msg_websocket_task, "send_msg_websocket_task", 8192, (void *)pig_server, 4, NULL);
    xTaskCreate(time_check_tag, "time_check_tag", 4096, (void *)pig_server, 5, NULL);

    /* Step6:  Initialize websocket app */
    websocket_app_start((void *)pig_server);

    /* step7: 震动 */
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE,  // 上升/下降沿都触发
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = GPIO_INPUT_PIN_SEL,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE
    };
    gpio_config(&io_conf);

    // 安装 GPIO 中断服务
    gpio_install_isr_service(1);
    gpio_isr_handler_add(GPIO_INPUT_IO, gpio_isr_handler, (void*)pig_server);


    while(1) {
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    }
}
