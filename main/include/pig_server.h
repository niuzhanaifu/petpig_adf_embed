#ifndef __PIG_SERVER_H
#define __PIG_SERVER_H

#include "driver/i2s_std.h"
#include "esp_system.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_vad.h"
#include <esp_websocket_client.h>

#define PIG_SERVRE    "pig_server"
#define WEBSOCKET_TAG "pig_websocket"
#define WEBSOCKET_SND "pig_websocket_rcv"
#define WIFI_TAG      "pig_wifi"
#define ES_TAG        "ES8311"
#define REV_TAG       "i2s_recive"
#define SND_TAG       "i2s_send"
#define RING_BUF      "ring_buf"

#define HC_GPIO_INPUT_IO       GPIO_NUM_18
#define HC_GPIO_INPUT_PIN_SEL  (1ULL << HC_GPIO_INPUT_IO)

typedef enum {
    BEGIN_VOICE = 0,
    DURING_VOICE = 1,
    END_VOICE = 2
} voice_status_tag;

#define RING_BUF_MAX (96 * 1024)
#define MIC_BUF_MAX (48 * 1024)
#define MAX_RECV_BUF_SIZE (3 * 1024)
#define SEND_I2S_SZIE (2*MAX_RECV_BUF_SIZE)

typedef struct {
    uint8_t *buffer;       // 缓冲区数据
    size_t capacity;       // 缓冲区总容量（建议设为音频帧大小的整数倍，如16384字节）
    size_t in;             // 写入指针
    size_t out;            // 读取指针
    size_t len;            // 当前有效数据长度
    SemaphoreHandle_t mutex; // 互斥锁（保护多线程访问）
    // SemaphoreHandle_t sem;  // 信号量（数据可用时唤醒播放线程）
} RingBuffer;

typedef struct
{
    i2s_chan_handle_t             tx_handle;
    i2s_chan_handle_t             rx_handle;
    //麦克风的状态
    int                           recive_music_status;
    //上一次麦克风收到声音的时间
    int64_t                       last_recv_time;
    //麦克风收到的数据
    int16_t                       recive_data[MAX_RECV_BUF_SIZE];
    //是否从服务器收到了回包
    bool                          request_tag;
    //上一次给i2s输出语音的时间
    int64_t                       last_send_time;
    //麦克风读到的数据大小，每时每刻都在读取
    size_t                        bytes_read;
    //扬声器输出的数据大小
    size_t                        bytes_write;
    //拍拍猪的websocket_client
    esp_websocket_client_handle_t client;
    // vad句柄
    vad_handle_t                  vad;
    //音频数据环形缓冲区
    RingBuffer                   *audio_ringbuf;
    //mic数据环形缓冲区
    RingBuffer                   *mic_ringbuf;
    //震动片振动发声音
    bool                          shock;
    //对端服务器是否已经结束连接
    bool                          server_finish;
} pignet_server_t, *pignet_server_handle_t;

#endif