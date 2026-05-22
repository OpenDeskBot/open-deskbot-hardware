#pragma once

// 依赖 Arduino 库管理器安装: "WebSockets" by Markus Sattler / Links2004
// https://github.com/Links2004/arduinoWebSockets

void face_pos_ws_init(void);

// 10 个整数: 与 esp-dl 两阶段人脸顺序一致
// LE(0,1), ML(2,3), NOSE(4,5), RE(6,7), MR(8,9)
void face_pos_ws_send_landmarks(const int *kp10, float confidence);
