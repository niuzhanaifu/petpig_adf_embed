import asyncio
import base64
import os
from aiohttp import web

AUDIO_FILE_PATH = r"E:\ESP-IDF\PROJECT\ESP32_TEST\main\ack2.wav"

async def send_streaming_data(ws):
    """
    向客户端发送流式二进制数据（Base64编码），每次发送4KB，每100ms一次，发送完再从头开始。
    """
    try:
        print(f"开始流式发送: {AUDIO_FILE_PATH}")
        total_size = os.path.getsize(AUDIO_FILE_PATH)
        print(f"文件总大小: {total_size} 字节")

        while True:
            with open(AUDIO_FILE_PATH, 'rb') as f:
                count = 0
                while True:
                    chunk = f.read(4 * 1024)
                    if not chunk:
                        break
                    encoded = base64.b64encode(chunk).decode('utf-8')
                    await ws.send_str(encoded)
                    # await ws.send(chunk)
                    count += 1
                    await asyncio.sleep(0.1)
                print(f"本轮发送完成，共发送 {count} 块数据。等待 5 秒语音播放完成重启...")
            await asyncio.sleep(5)
    except Exception as e:
        print(f"发送过程中出错: {e}")

async def websocket_handler(request):
    print(f"新客户端连接: {request.remote}")
    ws = web.WebSocketResponse()
    await ws.prepare(request)

    send_task = asyncio.create_task(send_streaming_data(ws))

    try:
        async for msg in ws:
            if msg.type == web.WSMsgType.TEXT:
                print(f"收到客户端消息: {msg.data}")
            elif msg.type == web.WSMsgType.ERROR:
                print(f"WebSocket连接出错: {ws.exception()}")
    finally:
        send_task.cancel()
        print(f"客户端断开连接: {request.remote}")

    return ws

app = web.Application()
app.router.add_get('/smart/chat', websocket_handler)

if __name__ == '__main__':
    print("启动 WebSocket 服务，监听 ws://0.0.0.0:8765/smart/chat")
    web.run_app(app, host='0.0.0.0', port=8765)
