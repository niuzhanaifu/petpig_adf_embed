#ifndef __PIG_SERVER_H
#define __PIG_SERVER_H

#include "driver/i2s_std.h"
#include "esp_system.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_vad.h"
#include "audio_pipeline.h"
#include "audio_element.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include <esp_websocket_client.h>

#define PIG_SERVRE    "pig_server"
#define WEBSOCKET_TAG "pig_websocket"
#define WEBSOCKET_SND "pig_websocket_rcv"
#define WIFI_TAG      "pig_wifi"
#define ES_TAG        "ES8311"
#define REV_TAG       "i2s_recive"
#define SND_TAG       "i2s_send"
#define RING_BUF      "ring_buf"
#define PIG_STATUS    "PIG_STATUS"

#define HC_GPIO_INPUT_PIN_SEL  (1ULL << HC_GPIO_INPUT_IO)

// 麦克风的不同收音状态
typedef enum {
    MIC_SALIENT = 0,
    MIC_DETECT = 1,
    BEGIN_VOICE = 2,
    DURING_VOICE = 3,
    END_VOICE = 4
} voice_status_tag;

// 从服务器接收数据流的不同状态
typedef enum {
    SERVER_STREAM_IS_FINAL = 0,
    SERVER_STREAM_TEXT_RESPONSE = 1,
    SERVER_STREAM_START = 2,
    SERVER_STREAM_COMPLETE = 3,
    SERVER_STREAM_ERR = 4,
    SERVER_STREAM_ASR_ERROR = 5,
} server_stream_status_t;

// 拍拍猪的状态,不同的状态将来有不同的灯语
typedef enum {
    PET_PIG_IDLE = 0,
    PET_PIG_DISCONNECT_INTERNET = 1,
    PET_PIG_DISCONNECT_SERVER = 2,
    PET_PIG_SPEAKERING = 3,
    PET_PIG_MIC_WORKING = 4,
    PET_PIG_MAX_STATUS = 5
} pet_pig_status_t;

#define RING_BUF_MAX      (96 * 1024)
#define MIC_BUF_MAX       (48 * 1024)
#define MAX_RECV_BUF_SIZE (3 * 1024)
#define SEND_I2S_SZIE     (2*MAX_RECV_BUF_SIZE)

typedef struct {
    // 缓冲区数据
    uint8_t *buffer;
    // 缓冲区总容量
    size_t capacity;
    // 写入指针
    size_t in;
    // 读取指针
    size_t out;
    // 当前有效数据长度
    size_t len;
    // 互斥锁（保护多线程访问）
    SemaphoreHandle_t mutex;
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
    int8_t                        recive_data[MAX_RECV_BUF_SIZE];
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
    //对端服务器是否已经结束连接
    bool                          server_finish;
    //音频流
    audio_pipeline_handle_t       pipeline;
    //读取音频流的handle
    audio_element_handle_t        raw_read;
    //重编码音频流的handle
    audio_element_handle_t        filter;
    //写入音频流的handle
    audio_element_handle_t        raw_write;
    //唤醒网络模型
    esp_wn_iface_t               *wakenet;
    //加载模型数据
    model_iface_data_t           *model_data;
    //模型需要加载的数据
    int                           audio_chunksize;
} pignet_server_t, *pignet_server_handle_t;

#endif