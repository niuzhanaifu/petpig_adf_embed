#ifndef __PIG_SERVER_H
#define __PIG_SERVER_H

#include "driver/i2s_std.h"
#include "esp_system.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_vad.h"
#include <freertos/queue.h>
#include <esp_websocket_client.h>

#define PIG_SERVRE    "pig_server"
#define WEBSOCKET_TAG "pig_websocket"
#define WEBSOCKET_SND "pig_websocket_rcv"
#define WIFI_TAG      "pig_wifi"
#define ES_TAG        "ES8311"
#define REV_TAG       "i2s_recive"
#define SND_TAG       "i2s_send"
#define RING_BUF      "ring_buf"

typedef enum {
    BEGIN_VOICE = 0,
    DURING_VOICE = 1,
    END_VOICE = 2
} voice_status_tag;

#define RING_BUF_MAX (128 * 1024)

#define CHANNELS        1      // 单声道
#define BIT_DEPTH       16     // 16位采样
#define SAMPLE_SIZE     (BIT_DEPTH / 8)
#define FRAME_SIZE      1024  // VAD 处理帧大小（512样本 = 32ms @16kHz）
// #define MAX_RECV_BUF_SIZE (FRAME_SIZE * SAMPLE_SIZE * CHANNELS)
#define MAX_RECV_BUF_SIZE (2400)
#define xQueue_Item  (4)
#define xQueue_Size  (MAX_RECV_BUF_SIZE)
#define SEND_I2S_SZIE (2*MAX_RECV_BUF_SIZE)

typedef struct {
    uint8_t *buffer;       // 缓冲区数据
    size_t capacity;       // 缓冲区总容量（建议设为音频帧大小的整数倍，如16384字节）
    size_t in;             // 写入指针
    size_t out;            // 读取指针
    size_t len;            // 当前有效数据长度
    SemaphoreHandle_t mutex; // 互斥锁（保护多线程访问）
    SemaphoreHandle_t sem;  // 信号量（数据可用时唤醒播放线程）
} RingBuffer;

typedef struct
{
    i2s_chan_handle_t tx_handle;
    i2s_chan_handle_t rx_handle;
    int recive_music_status;            //麦克风是否收到了声音
    int64_t last_recv_time;             //上一次收到声音的时间
    int16_t recive_data[MAX_RECV_BUF_SIZE]; //麦克风收到的数据
    bool request_tag;                   //是否从服务器收到了回包
    int64_t last_send_time;             //上一次给i2s输出语音的时间
    size_t bytes_read;                  //麦克风读到的数据大小，每时每刻都在读取
    size_t bytes_write;                 //扬声器输出的数据大小
    esp_websocket_client_handle_t client; //拍拍猪的websocket_client
    vad_handle_t vad;
    QueueHandle_t send_websocket_queue; //向服务端发送数据的消息队列
    bool wifi_status;
    RingBuffer *audio_ringbuf;          //音频数据环形缓冲区
    bool shock;                         //震动片振动发声音
} pignet_server_t, *pignet_server_handle_t;


#endif