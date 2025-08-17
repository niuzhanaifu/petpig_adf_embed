import asyncio
import websockets
import json
import wave
import time
import base64
import pyaudio
import queue
import threading
from pathlib import Path

AUDIO_SAVE_PATH = r"E:\ESP-IDF\PROJECT\ESP32_TEST\main\111.wav"

class VoiceRecognitionClient:
    def __init__(self, server_url, wav_file_path):
        self.server_url = server_url
        self.wav_file_path = wav_file_path
        self.chunk_duration_ms = 1  # 200毫秒
        self.sample_rate = 16000
        # self.chunk_size = int(self.sample_rate * self.chunk_duration_ms / 1000) * 2  # 16位=2字节
        self.chunk_size = 2048

        # 音频播放相关
        self.audio_queue = queue.Queue()
        self.pyaudio = pyaudio.PyAudio()
        self.audio_stream = None
        self.is_playing = False
        self.playback_thread = None

    def start_audio_playback(self):
        """启动音频播放线程"""
        self.is_playing = True
        self.playback_thread = threading.Thread(target=self._audio_playback_worker)
        self.playback_thread.daemon = True
        self.playback_thread.start()
        print("🎧 音频播放线程已启动")

    def stop_audio_playback(self):
        """停止音频播放"""
        self.is_playing = False
        if self.playback_thread:
            self.playback_thread.join(timeout=2)
        if self.audio_stream:
            self.audio_stream.stop_stream()
            self.audio_stream.close()
        print("🛑 音频播放已停止")

    def _audio_playback_worker(self):
        """音频播放工作线程"""
        try:
            # 创建音频输出流
            self.audio_stream = self.pyaudio.open(
                format=pyaudio.paInt16,  # 16位音频
                channels=1,  # 单声道
                rate=self.sample_rate,  # 采样率
                output=True,  # 输出流
                frames_per_buffer=1024  # 缓冲区大小
            )

            print(f"🔊 音频输出流已创建 (采样率: {self.sample_rate}Hz)")

            # 持续从队列读取并播放音频
            while self.is_playing:
                try:
                    # 从队列获取音频数据（超时1秒）
                    audio_data = self.audio_queue.get(timeout=1)

                    if audio_data is None:  # 结束信号
                        break

                    # 播放音频
                    self.audio_stream.write(audio_data)

                except queue.Empty:
                    # 队列为空，继续等待
                    continue
                except Exception as e:
                    print(f"❌ 播放音频时出错: {e}")

        except Exception as e:
            print(f"❌ 创建音频流时出错: {e}")
        finally:
            if self.audio_stream:
                self.audio_stream.stop_stream()
                self.audio_stream.close()

    async def send_voice_stream(self):
        """发送语音流到服务器"""
        try:
            # 打开WAV文件
            # with wave.open(self.wav_file_path, 'rb') as wav_file:
                # 验证音频格式
                # if wav_file.getnchannels() != 1:
                #     raise ValueError("WAV文件必须是单声道")
                # if wav_file.getframerate() != self.sample_rate:
                #     raise ValueError(f"WAV文件采样率必须是{self.sample_rate}Hz")
                # if wav_file.getsampwidth() != 2:
                #     raise ValueError("WAV文件必须是16位采样")

                # 读取所有音频数据（纯PCM数据，不包含WAV头）
                # audio_data = wav_file.readframes(wav_file.getnframes())
            with open(self.wav_file_path, "rb") as pcm_file:
                audio_data = pcm_file.read()
                print(f"读取了 {len(audio_data)} 字节PCM音频数据")

            # 连接WebSocket服务器
            async with websockets.connect(self.server_url) as websocket:
                print(f"已连接到服务器: {self.server_url}")

                # 启动音频播放
                self.start_audio_playback()

                # 第一步：发送开始消息（JSON格式）
                start_message = {
                    "type": "streaming_voice_start",
                    "sample_rate": self.sample_rate,
                    "input_sample_rate": 48000,
                    "format": "pcm",
                    "child_age": 5,
                    "user_name": "测试用户"
                }
                await websocket.send(json.dumps(start_message))
                print("已发送开始消息")

                # 等待服务器响应
                response = await websocket.recv()
                print(f"服务器响应: {response}")

                # 创建异步任务来接收服务器消息
                receive_task = asyncio.create_task(self.receive_messages(websocket))

                # 第二步：直接发送纯PCM音频数据（不包含WAV头）
                chunk_id = 0
                for i in range(0, len(audio_data), self.chunk_size):
                    chunk = audio_data[i:i + self.chunk_size]

                    if chunk:
                        # 直接发送二进制PCM数据
                        await websocket.send(chunk)
                        print(f"已发送音频块 {chunk_id}, 大小: {len(chunk)} 字节")

                        # 模拟实时流，每200ms发送一块
                        await asyncio.sleep(self.chunk_duration_ms / 1000)

                        chunk_id += 1

                # 第三步：发送结束消息（JSON格式）
                end_message = {
                    "type": "streaming_voice_end",
                    "child_age": 5
                }
                await websocket.send(json.dumps(end_message))
                print("已发送结束消息")

                # 等待服务器处理完成并发送最终响应
                print("\n等待服务器响应...")
                print("按 Ctrl+C 手动断开连接\n")

                try:
                    # 持续等待，直到用户手动中断
                    while True:
                        await asyncio.sleep(1)

                except KeyboardInterrupt:
                    print("\n用户手动中断，准备断开连接...")
                    receive_task.cancel()

                except asyncio.CancelledError:
                    print("接收任务已取消")

        except FileNotFoundError:
            print(f"找不到WAV文件: {self.wav_file_path}")
        except websockets.exceptions.WebSocketException as e:
            print(f"WebSocket连接错误: {e}")
        except KeyboardInterrupt:
            print("\n用户中断程序")
        except Exception as e:
            print(f"发生错误: {e}")
            import traceback
            traceback.print_exc()
        finally:
            # 清理资源
            self.stop_audio_playback()
            self.pyaudio.terminate()

    async def receive_messages(self, websocket):
        """接收服务器消息的异步任务"""
        try:
            while True:
                message = await websocket.recv()

                # 处理文本消息（JSON）
                if isinstance(message, str):
                    try:
                        data = json.loads(message)
                        msg_type = data.get("type", "")

                        if msg_type == "streaming_recognition_update":
                            text = data.get('text', '')
                            is_final = data.get('is_final', False)
                            print(f"🗣️ {'最终' if is_final else '实时'}识别: {text}")
                        elif msg_type == "text_response":
                            print(f"\n🤖 AI回复: {data.get('response_text', '')}")
                            print(f"   类型: {data.get('response_type', '')}")
                        elif msg_type == "stream_start":
                            print(f"\n🎵 开始接收音频流...")
                        elif msg_type == "stream_complete":
                            print(f"✅ 音频流接收完成，共 {data.get('total_chunks_sent', 0)} 块")
                            # 发送结束信号到音频队列
                            self.audio_queue.put(None)
                        elif msg_type == "error":
                            print(f"❌ 错误: {data.get('message', '')}")
                        elif msg_type == "asr_error":
                            print(f"❌ ASR错误: {data.get('error', '')}")
                        elif msg_type == "streaming_voice_start_ack":
                            print(f"✅ 服务器确认开始流式传输")
                        else:
                            print(f"📨 收到消息: {data}")

                    except json.JSONDecodeError:
                        print(f"收到非JSON消息: {message}")

                # 处理二进制消息（音频）
                elif isinstance(message, bytes):
                    print(f"🔊 收到音频数据: {len(message)} 字节")
                    with open(AUDIO_SAVE_PATH, "ab") as f:
                        f.write(message)
                    # 将音频数据加入播放队列
                    self.audio_queue.put(message)
                    print(f"   📦 音频队列大小sssss: {self.audio_queue.qsize()}")

        except websockets.exceptions.ConnectionClosed:
            print("服务器关闭了连接")
        except asyncio.CancelledError:
            print("接收任务被取消")
        except Exception as e:
            print(f"接收消息时出错: {e}")


async def main():
    # 配置参数
    # SERVER_URL = "ws://127.0.0.1:8000/ws/smart/chat"  # 修正URL路径
    SERVER_URL = "ws://115.190.136.178:8000/ws/smart/chat"  # 修正URL路径
    WAV_FILE = r"E:\ESP-IDF\PROJECT\ESP32_TEST\main\rcv2.pcm"  # 请指定你的WAV文件路径

    # 创建客户端并开始测试
    client = VoiceRecognitionClient(SERVER_URL, WAV_FILE)
    await client.send_voice_stream()


if __name__ == "__main__":
    # 运行异步主函数
    asyncio.run(main())