/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
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
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define WEB_URL "http://192.168.1.7:8000/smart/chat"
// 用于存储完整的HTTP响应数据
static char *response_data = NULL;
static int response_data_len = 0;

static const char *TAG = "i2s_es8311";

static const char err_reason[][30] = {"input param is invalid",
                                      "operation timeout"
                                     };

/* Only for test: Import music file as buffer */
extern const uint8_t music_pcm_start[] asm("_binary_ack2_wav_start");
extern const uint8_t music_pcm_end[]   asm("_binary_ack2_wav_end");

static esp_err_t es8311_codec_init(void)
{
    /* Initialize I2C peripheral */
    //todo:配置优化，需要再看下芯片手册和源码如何配置优化
#if !defined(CONFIG_EXAMPLE_BSP)
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
#else
    ESP_ERROR_CHECK(bsp_i2c_init());
#endif

    /* Initialize es8311 codec */
    es8311_handle_t es_handle = es8311_create(I2C_NUM, ES8311_ADDRRES_0);
    ESP_RETURN_ON_FALSE(es_handle, ESP_FAIL, TAG, "es8311 create failed");
    const es8311_clock_config_t es_clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = USER_MCLK_FREQ_HZ,
        .sample_frequency = USER_VOICE_VOLUME
    };

    ESP_ERROR_CHECK(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
    ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(es_handle, USER_VOICE_VOLUME * USER_MCLK_MULTIPLE, USER_VOICE_VOLUME), TAG, "set es8311 sample frequency failed");
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(es_handle, USER_VOICE_VOLUME, NULL), TAG, "set es8311 volume failed");
    ESP_RETURN_ON_ERROR(es8311_microphone_config(es_handle, false), TAG, "set es8311 microphone failed");
#if CONFIG_EXAMPLE_MODE_ECHO
    ESP_RETURN_ON_ERROR(es8311_microphone_gain_set(es_handle, EXAMPLE_MIC_GAIN), TAG, "set es8311 microphone gain failed");
#endif
    return ESP_OK;
}

static esp_err_t i2s_driver_init(pignet_server_handle_t p)
{
    ESP_LOGI(TAG, "Using user i2s_driver_init");
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &p->tx_handle, &p->rx_handle));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(USER_VOICE_VOLUME),
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

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(p->tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(p->rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(p->tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(p->rx_handle));
    return ESP_OK;
}

// 环形缓冲区结构体（线程安全版）
typedef struct {
    uint8_t *buffer;       // 缓冲区数据
    size_t capacity;       // 缓冲区总容量（建议设为音频帧大小的整数倍，如16384字节）
    size_t in;             // 写入指针
    size_t out;            // 读取指针
    size_t len;            // 当前有效数据长度
    SemaphoreHandle_t mutex; // 互斥锁（保护多线程访问）
    SemaphoreHandle_t sem;  // 信号量（数据可用时唤醒播放线程）
} RingBuffer;

// 初始化环形缓冲区
RingBuffer* ringbuf_init(size_t capacity) {
    RingBuffer *rb = malloc(sizeof(RingBuffer));
    rb->buffer = malloc(capacity);
    rb->capacity = capacity;
    rb->in = rb->out = rb->len = 0;
    rb->mutex = xSemaphoreCreateMutex();
    rb->sem = xSemaphoreCreateCounting(capacity, 0); // 计数信号量，最多capacity个数据
    return rb;
}

// 写入环形缓冲区（线程安全）
size_t ringbuf_write(RingBuffer *rb, const uint8_t *data, size_t len) {
    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    size_t write_len = MIN(len, rb->capacity - rb->len);
    if (write_len == 0) {
        xSemaphoreGive(rb->mutex);
        ESP_LOGE(TAG, "缓冲区满");
        return 0; // 缓冲区满
    }
    // 分两段写入（处理环形边界）
    size_t first = MIN(write_len, rb->capacity - rb->in);
    memcpy(rb->buffer + rb->in, data, first);
    size_t second = write_len - first;
    if (second > 0) {
        memcpy(rb->buffer, data + first, second);
    }
    // 更新指针和长度
    rb->in = (rb->in + write_len) % rb->capacity;
    rb->len += write_len;
    xSemaphoreGive(rb->mutex);
    // 发送信号量，唤醒播放线程
    xSemaphoreGive(rb->sem);
    return write_len;
}

// 从环形缓冲区读取（线程安全）
size_t ringbuf_read(RingBuffer *rb, uint8_t *data, size_t len, TickType_t timeout) {
    // 等待数据可用（超时时间设为音频播放所需时间，避免无限阻塞）
    if (xSemaphoreTake(rb->sem, timeout) != pdTRUE) {
        return 0; // 超时（缓冲区空）
    }
    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    if (rb->len < 2*1024) {
        xSemaphoreGive(rb->mutex);
        return 0;
    }
    size_t read_len = MIN(len, rb->len);
    // 分两段读取
    size_t first = MIN(read_len, rb->capacity - rb->out);
    memcpy(data, rb->buffer + rb->out, first);
    size_t second = read_len - first;
    if (second > 0) {
        memcpy(data + first, rb->buffer, second);
    }
    // 更新指针和长度
    rb->out = (rb->out + read_len) % rb->capacity;
    rb->len -= read_len;
    xSemaphoreGive(rb->mutex);
    return read_len;
}

static RingBuffer *audio_ringbuf = NULL;
static pignet_server_handle_t g_pig_server = NULL;
static TaskHandle_t play_task_handle = NULL;

//TODO 所有任务的args均使用封装好的结构体传参，禁止使用全局的static变量！！!
static void i2s_recieve(void *args)
{
    esp_err_t ret = ESP_OK;
    ESP_LOGI(TAG, "[i2s_recive] start");
    pignet_server_handle_t p = (pignet_server_t *)args;
    while(1) {
        memset(p->recive_data, 0, MAX_RECV_BUF_SIZE);
        //recive_music = false;
        /* Read sample data from mic */
        ret = i2s_channel_read(p->rx_handle, p->recive_data, MAX_RECV_BUF_SIZE, &p->bytes_read, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[echo] i2s read failed, %s", err_reason[ret == ESP_ERR_TIMEOUT]);
            continue;
        }
        int vad_result = vad_process(p->vad, p->recive_data, SAMPLE_RATE, 30);

        // 如果检测到人声，发送到消息队列
        if (vad_result == 1) {
            ESP_LOGI(TAG, "Voice detected! Sending to queue");
        } else {
            // 可在此处添加静音处理逻辑
            ESP_LOGD(TAG, "Silence detected");
        }
        //todo:这里麦克风时刻收音，需要有一个滤波算法过滤环境音
        //todo:这里麦克风接收到数据后，要给服务器发起请求，只有当收到了服务器的回包后，在收到回包的callback里通知扬声器发出声音
        p->recive_music = true;
    }
}

//todo:需要加锁保护收到的数据与写入的数据，当前的实现方式不安全，debug使用
static void i2s_send(void *args)
{
    esp_err_t ret = ESP_OK;
    ESP_LOGI(TAG, "[i2s_send]  start");
    const size_t i2s_block_size = 32*1024; // 每次I2S写入的块大小（根据音频格式调整）
    uint8_t *i2s_buffer = malloc(i2s_block_size);
    if (i2s_buffer == NULL) {
        ESP_LOGE(TAG, "I2S play buffer malloc failed");
        vTaskDelete(NULL);
    }

    uint8_t *data_ptr = (uint8_t *)music_pcm_start;
    pignet_server_handle_t p = (pignet_server_t *)args;

    while(1) {
        /* Write music to earphone */
        size_t read_len = ringbuf_read(audio_ringbuf, i2s_buffer, i2s_block_size, pdMS_TO_TICKS(10));
        if (read_len == 0) {
            // ESP_LOGW(TAG, "Ring buffer underflow (卡顿警告)");
            continue; // 缓冲区空，可填充静音数据避免爆音
        }

        // 写入I2S（非阻塞，超时时间短）
        size_t bytes_written;
        //todo，这里写入的是从服务器拿到的数据
        // ret = i2s_channel_write(p->tx_handle, data_ptr, music_pcm_end - data_ptr, &p->bytes_write, portMAX_DELAY);
        ret = i2s_channel_write(p->tx_handle, i2s_buffer, read_len, &p->bytes_write, pdMS_TO_TICKS(500));
        if (ret != ESP_OK) {
            /* Since we set timeout to 'portMAX_DELAY' in 'i2s_channel_write'
               so you won't reach here unless you set other timeout value,
               if timeout detected, it means write operation failed. */
            ESP_LOGE(TAG, "[music] i2s write failed, %s", err_reason[ret == ESP_ERR_TIMEOUT]);
        }
        if (p->bytes_write > 0) {
            p->recive_music = false;
            ESP_LOGI(TAG, "[music] i2s music played, %d bytes are written.", p->bytes_write);
        } else {
            ESP_LOGE(TAG, "[music] i2s music play failed.");
        }
        data_ptr = (uint8_t *)music_pcm_start;
    }
}

// TODO i2s_music和i2s_echo将来仅用来测试，不用于正常功能
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
            ESP_LOGE(TAG, "[music] i2s write failed, %s", err_reason[ret == ESP_ERR_TIMEOUT]);
            abort();
        }
        if (bytes_write > 0) {
            p->recive_music = false;
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
    int *mic_data = malloc(MAX_RECV_BUF_SIZE);
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
            ESP_LOGE(TAG, "[echo] i2s read failed, %s", err_reason[ret == ESP_ERR_TIMEOUT]);
            abort();
        }
        /* Write sample data to earphone */
        ret = i2s_channel_write(p->tx_handle, mic_data, MAX_RECV_BUF_SIZE, &bytes_write, 1000);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[echo] i2s write failed, %s", err_reason[ret == ESP_ERR_TIMEOUT]);
            abort();
        }
        if (bytes_read != bytes_write) {
            ESP_LOGW(TAG, "[echo] %d bytes read but only %d bytes are written", bytes_read, bytes_write);
        }
    }
    vTaskDelete(NULL);
}

// 流式解包程序
static SemaphoreHandle_t shutdown_sema;

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
        // ESP_LOGI(TAG, "WEBSOCKET_EVENT_DATA");
        ESP_LOGI(TAG, "Received opcode=%d", data->op_code);
        if (data->op_code == 0x2) { // Opcode 0x2 indicates binary data
            ESP_LOG_BUFFER_HEX("Received binary data", data->data_ptr, data->data_len);
            esp_err_t ret = i2s_channel_write(pig_server->tx_handle, data->data_ptr, data->data_len, &pig_server->bytes_write, portMAX_DELAY);
            if (ret == ESP_OK) {
                ESP_LOGE(TAG, "i2s write success!");
            }
        } else if (data->op_code == 0x08 && data->data_len == 2) {
            ESP_LOGW(TAG, "Received closed message with code=%d", 256 * data->data_ptr[0] + data->data_ptr[1]);
        } else {
            // ESP_LOGW(TAG, "Received=%.*s\n\n", data->data_len, (char *)data->data_ptr);
        }

        // If received data contains json structure it succeed to parse
        if (data == NULL || data->data_ptr == NULL || data->data_len == 0) {
            ESP_LOGE(TAG, "Invalid data pointer");
            break;
        }
        size_t decode_length = 0;
        int decode_res = mbedtls_base64_decode(NULL, 0, &decode_length, (const unsigned char *)data->data_ptr, data->data_len);
            // ESP_LOGW(TAG, "Json={'message': '%s'}, decode size:%d", name->valuestring, decode_length);
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
            ESP_LOGW(TAG, "解码后的数据（16进制）:");
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
            size_t written = ringbuf_write(audio_ringbuf, (uint8_t *)decoded_data, decode_length);
            if (written != decode_length) {
                ESP_LOGW(TAG, "Ring buffer overflow (数据丢失)");
            }
            // 清理资源
            free(decoded_data);
            ESP_LOGW(TAG, "Total payload length=%d, data_len=%d, current payload offset=%d\r\n", data->payload_len, data->data_len, data->payload_offset);
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
    esp_websocket_client_config_t websocket_cfg = {};
    shutdown_sema = xSemaphoreCreateBinary();

    websocket_cfg.uri = "ws://192.168.253.160:8765";

    ESP_LOGI(TAG, "Connecting to %s...", websocket_cfg.uri);

    esp_websocket_client_handle_t client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)args);

    esp_websocket_client_start(client);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
    // Sending text data
    // ESP_LOGI(TAG, "Sending fragmented text message");
    // memset(data, 'a', sizeof(data));
    // esp_websocket_client_send_text_partial(client, data, sizeof(data), portMAX_DELAY);
    // memset(data, 'b', sizeof(data));
    // esp_websocket_client_send_cont_msg(client, data, sizeof(data), portMAX_DELAY);
    // esp_websocket_client_send_fin(client, portMAX_DELAY);
    // vTaskDelay(1000 / portTICK_PERIOD_MS);

    // // Sending binary data
    // ESP_LOGI(TAG, "Sending fragmented binary message");
    // char binary_data[5];
    // memset(binary_data, 0, sizeof(binary_data));
    // esp_websocket_client_send_bin_partial(client, binary_data, sizeof(binary_data), portMAX_DELAY);
    // memset(binary_data, 1, sizeof(binary_data));
    // esp_websocket_client_send_cont_msg(client, binary_data, sizeof(binary_data), portMAX_DELAY);
    // esp_websocket_client_send_fin(client, portMAX_DELAY);
    // vTaskDelay(1000 / portTICK_PERIOD_MS);

    // // Sending text data longer than ws buffer (default 1024)
    // ESP_LOGI(TAG, "Sending text longer than ws buffer (default 1024)");
    // const int size = 2000;
    // char *long_data = malloc(size);
    // memset(long_data, 'a', size);
    // esp_websocket_client_send_text(client, long_data, size, portMAX_DELAY);
    // free(long_data);
}

void app_main(void)
{
    printf("Hello world!\n");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    audio_ringbuf = ringbuf_init(64 * 1024);

    /* 初始化拍拍猪服务结构体 */
    pignet_server_handle_t pig_server = (pignet_server_handle_t)calloc(1, sizeof(pignet_server_t));
    if (pig_server == NULL) {
        ESP_LOGE(TAG, "calloc failed pig_server");
        return;
    }

    /* 控制功放引脚，使能扬声器 */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << NS4150B_CTRL),  // 选择要配置的 GPIO 引脚
        .mode = GPIO_MODE_OUTPUT,               // 设置为输出模式
        .pull_up_en = GPIO_PULLUP_DISABLE,      // 禁用上拉电阻
        .pull_down_en = GPIO_PULLDOWN_DISABLE,  // 禁用下拉电阻
        .intr_type = GPIO_INTR_DISABLE          // 禁用中断
    };

    // 应用 GPIO 配置
    gpio_config(&io_conf);

    // 将 GPIO 输出高电平
    gpio_set_level(NS4150B_CTRL, 1);

    ESP_LOGI("GPIO", "GPIO %d 已设置为高电平", NS4150B_CTRL);

    printf("i2s es8311 codec example start\n-----------------------------\n");
    /* Initialize i2s peripheral */
    if (i2s_driver_init(pig_server) != ESP_OK) {
        ESP_LOGE(TAG, "i2s driver init failed");
        abort();
    } else {
        ESP_LOGI(TAG, "i2s driver init success");
    }

    // 4. 初始化VAD
    // vad = vad_create(VAD_MODE_3, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS);

    /* Initialize i2c peripheral and config es8311 codec by i2c */
    if (es8311_codec_init() != ESP_OK) {
        ESP_LOGE(TAG, "es8311 codec init failed");
        abort();
    } else {
        ESP_LOGI(TAG, "es8311 codec init success");
    }

    pig_server->vad = vad_create(VAD_MODE_4);

    xTaskCreate(i2s_recieve, "i2s_recieve", 8192, (void *)pig_server, 5, NULL);
    xTaskCreate(i2s_send, "i2s_send", 8192, (void *)pig_server, 5, NULL);
    // xTaskCreate(send_data_to_server, "send_data_to_server", 8192, (void *)pig_server, 4, NULL);

    /* 初始化wifi */
    ret = init_wifi();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "init wif success!\n");
    }

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    websocket_app_start((void *)pig_server);

    while(1) {
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        // http_post_json_data(pig_server);
        // http_rest_with_baidu();
        // ESP_LOGI(TAG, "hello world!\n");
    }
}
