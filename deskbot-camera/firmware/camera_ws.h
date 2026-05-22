#pragma once

// 把摄像头每帧 JPEG 通过 WebSocket 二进制发送到服务器，由服务器做识别。
// 依赖 Arduino "WebSockets" 库 (Links2004/arduinoWebSockets)
// 在 WiFi 已连接后调用 camera_ws_init()。
void camera_ws_init(void);
