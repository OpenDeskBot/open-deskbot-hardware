#pragma once

/* WebSocket 后端（opendesk-service 或自建网关）
 *
 * 在 platformio.local.ini 中覆盖，例如：
 *   '-DDESKBOT_WS_HOST="192.168.1.100"'
 *   -DDESKBOT_WS_PORT=9000
 *
 * device_id 与主控 ROM 共用：在仓库根目录 deskbot.local.env 设置 DEVICE_ID，
 * flash_*.sh 烧录时自动写入 DESKBOT_DEVICE_ID 编译宏。
 * DESKBOT_WS_HOST 留空则跳过 /camera 与 /device_pipeline 连接（本地 HTTP 预览仍可用）。
 */

#ifndef DESKBOT_WS_HOST
#define DESKBOT_WS_HOST ""
#endif

#ifndef DESKBOT_WS_PORT
#define DESKBOT_WS_PORT 9000
#endif

static inline bool deskbot_ws_configured() {
  return DESKBOT_WS_HOST[0] != '\0';
}
