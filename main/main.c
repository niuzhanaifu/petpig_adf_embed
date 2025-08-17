/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_check.h"
#include "cJSON.h"
#include "esp_vad.h"

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

static const char *TAG = "i2s_es8311";

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
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
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
    char                   buffer[MAX_RECV_BUF_SIZE] = {0};
    int                    send_res = 0;

    while (1) {
        memset(buffer, 0, MAX_RECV_BUF_SIZE);
        size_t read_len = ringbuf_read(p->mic_ringbuf, (uint8_t *)buffer, MAX_RECV_BUF_SIZE, pdMS_TO_TICKS(100));
        if (read_len == 0) {
            vTaskDelay(10 / portTICK_PERIOD_MS);
            continue;
        }
        if (esp_websocket_client_is_connected(p->client) && wifi_connected() == ESP_OK && p->request_tag == false) {
            if (p->recive_music_status != DURING_VOICE) {
                send_voice_tag_to_server(p, BEGIN_VOICE);
                // vTaskDelay(300 / portTICK_PERIOD_MS);
            }
            send_res = esp_websocket_client_send_bin(p->client, buffer, read_len, pdMS_TO_TICKS(1000));
            if (send_res != -1) {
                ESP_LOGW(WEBSOCKET_SND, "send %d msg to server success", send_res);
                p->recive_music_status = DURING_VOICE;
                p->last_recv_time = esp_timer_get_time();
            }
        } else {
            ESP_LOGW(WEBSOCKET_SND, "Don't allow send voice binary to socket server");
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
        ret = i2s_channel_read(p->rx_handle, p->recive_data, MAX_RECV_BUF_SIZE, &p->bytes_read, 1000);
        if (ret != ESP_OK) {
            ESP_LOGE(REV_TAG, "i2s read failed, %d", ret);
            continue;
        }
        int vad_result = vad_process(p->vad, p->recive_data, USER_SAMPLE_RATE, 30);

        if (vad_result == 1 && p->request_tag == false && p->shock == true) {
            ringbuf_write(p->mic_ringbuf, (uint8_t *)p->recive_data, p->bytes_read);
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
    ESP_LOGI(SND_TAG, "i2s_send start");
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
                ESP_LOGW(TAG, "op_code:%d, Received msg:%d, msg:%s", data->op_code, data->data_len, data->data_ptr);
                char *json_str = calloc(data->data_len + 1, 1);
                if (json_str) {
                    memcpy(json_str, data->data_ptr, data->data_len);
                    json_str[data->data_len] = '\0';
                    cJSON *json = cJSON_Parse(json_str);
                    if (json) {
                        // const cJSON *audio_data = cJSON_GetObjectItem(json, "message");
                        // if (cJSON_IsString(audio_data)) {
                        //     ESP_LOGI(TAG, "Base64 audio_data (string): %s", audio_data->valuestring);
                        // } else {
                        //     ESP_LOGW(TAG, "audio_data not found or not a string");
                        // }
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
            ESP_LOGE(TAG, "RECOVER pig_server->request_tag = false");
        }

        if (pig_server->server_finish == true) {
            esp_websocket_client_start(pig_server->client);
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
    pig_server->mic_ringbuf = ringbuf_init(MIC_BUF_MAX);
    pig_server->vad = vad_create(VAD_MODE_2);
    pig_server->recive_music_status = END_VOICE;
    pig_server->request_tag = false;
    pig_server->server_finish = false;
    pig_server->shock = false;

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
        xTaskCreate(i2s_recieve, "i2s_recieve", 8192, (void *)pig_server, 5, NULL);
    #endif
    xTaskCreate(i2s_send, "i2s_send", 8192, (void *)pig_server, 5, NULL);
    xTaskCreate(send_msg_websocket_task, "send_msg_websocket_task", 8192, (void *)pig_server, 5, NULL);
    xTaskCreate(time_check_tag, "time_check_tag", 4096, (void *)pig_server, 5, NULL);

    /* Step6:  Initialize websocket app */
    websocket_app_start((void *)pig_server);

    /* step7: 使能震动 */
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = HC_GPIO_INPUT_PIN_SEL,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(1);
    gpio_isr_handler_add(HC_GPIO_INPUT_IO, gpio_isr_handler, (void*)pig_server);

    while(1) {
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    }
}
