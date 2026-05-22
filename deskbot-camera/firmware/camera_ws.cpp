#include "camera_ws.h"
#include "deskbot_camera_config.h"
#include "deskbot_device_id.h"
#include "deskbot_ws_config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include "esp_camera.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdio>

static const char kHost[] = DESKBOT_WS_HOST;
static constexpr uint16_t kPort = DESKBOT_WS_PORT;
static const char kBasePath[] = "/camera";

// 由 deskbot.local.env 的 CAMERA_UPLOAD_FPS 编译注入；0 = 连续发送。
static constexpr uint32_t kSendIntervalMs = DESKBOT_CAMERA_UPLOAD_INTERVAL_MS;

static char ws_path[96];
static WebSocketsClient ws;
static volatile bool ws_connected = false;

static const char *wsTypeName(WStype_t t) {
  switch (t) {
    case WStype_DISCONNECTED: return "DISCONNECTED";
    case WStype_CONNECTED: return "CONNECTED";
    case WStype_TEXT: return "TEXT";
    case WStype_BIN: return "BIN";
    case WStype_ERROR: return "ERROR";
    case WStype_PING: return "PING";
    case WStype_PONG: return "PONG";
    default: return "?";
  }
}

static void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      ws_connected = true;
      Serial.printf("[camera_ws] CONNECTED to ws://%s:%u%s\r\n",
                    kHost, (unsigned)kPort, ws_path);
      break;
    case WStype_DISCONNECTED:
      if (ws_connected) {
        Serial.println("[camera_ws] DISCONNECTED");
      } else {
        Serial.printf("[camera_ws] connect failed to ws://%s:%u%s (will retry)\r\n",
                      kHost, (unsigned)kPort, ws_path);
      }
      ws_connected = false;
      break;
    case WStype_ERROR:
      Serial.printf("[camera_ws] ERROR len=%u payload=%.*s\r\n",
                    (unsigned)length, (int)length, payload ? (const char*)payload : "");
      break;
    case WStype_TEXT:
      Serial.printf("[camera_ws] RX TEXT len=%u: %.*s\r\n",
                    (unsigned)length, (int)length, (const char*)payload);
      break;
    default:
      Serial.printf("[camera_ws] event=%s len=%u\r\n", wsTypeName(type), (unsigned)length);
      break;
  }
}

static void cameraWsTask(void *) {
  bool wifi_was_up = false;
  bool begun = false;
  uint32_t last_send_ms = 0;
  uint32_t last_status_ms = 0;
  uint32_t frames_sent = 0;
  uint32_t bytes_sent = 0;
  uint32_t status_frames_prev = 0;
  uint32_t status_bytes_prev = 0;

  for (;;) {
    bool wifi_up = (WiFi.status() == WL_CONNECTED);

    if (wifi_up && !wifi_was_up) {
      Serial.printf("[camera_ws] WiFi up, connecting to ws://%s:%u%s\r\n",
                    kHost, (unsigned)kPort, ws_path);
      ws.begin(kHost, kPort, ws_path);
      ws.setReconnectInterval(2500);
      ws.enableHeartbeat(15000, 3000, 2);
      begun = true;
    }
    wifi_was_up = wifi_up;

    if (begun) {
      ws.loop();
    }

    uint32_t now = millis();

    if (now - last_status_ms >= 5000) {
      uint32_t dt = now - last_status_ms;
      uint32_t df = frames_sent - status_frames_prev;
      uint32_t db = bytes_sent - status_bytes_prev;
      float fps = dt ? (df * 1000.0f / dt) : 0.0f;
      float kbps = dt ? (db * 8.0f / dt) : 0.0f;  // bits/ms == kbit/s
      last_status_ms = now;
      status_frames_prev = frames_sent;
      status_bytes_prev = bytes_sent;
      Serial.printf("[camera_ws] status: wifi=%s ws=%s frames=%u bytes=%u fps=%.1f kbps=%.0f\r\n",
                    wifi_up ? "up" : "down",
                    ws_connected ? "connected" : "disconnected",
                    (unsigned)frames_sent, (unsigned)bytes_sent, fps, kbps);
    }

    if (ws_connected && (now - last_send_ms) >= kSendIntervalMs) {
      camera_fb_t *fb = esp_camera_fb_get();
      if (fb) {
        if (fb->format == PIXFORMAT_JPEG && fb->len > 0) {
          if (ws.sendBIN(fb->buf, fb->len)) {
            frames_sent++;
            bytes_sent += fb->len;
          } else {
            Serial.println("[camera_ws] sendBIN failed");
          }
        }
        esp_camera_fb_return(fb);
      }
      last_send_ms = now;
    } else {
      // 未连接时少占 CPU
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // 让出一下 CPU，给 ws.loop()/其他任务调度（同时避免 WDT）
    vTaskDelay(1);
  }
}

void camera_ws_init(void) {
#if !DESKBOT_CAMERA_UPLOAD_JPEG
  Serial.println("[camera_ws] disabled (DESKBOT_CAMERA_UPLOAD_JPEG=0)");
  return;
#endif
  if (!deskbot_ws_configured()) {
    Serial.println("[camera_ws] DESKBOT_WS_HOST not set; copy platformio.local.ini.example "
                   "to platformio.local.ini and define host");
    return;
  }
  snprintf(ws_path, sizeof(ws_path), "%s?device_id=%s", kBasePath, deskbot_device_id());
  ws.onEvent(onWsEvent);
  Serial.printf("[camera_ws] init target=ws://%s:%u%s fps=%d interval=%ums\r\n",
                kHost, (unsigned)kPort, ws_path, (int)DESKBOT_CAMERA_UPLOAD_FPS, (unsigned)kSendIntervalMs);
  BaseType_t ok = xTaskCreatePinnedToCore(cameraWsTask, "camera_ws", 12288, nullptr, 1, nullptr, 0);
  if (ok != pdPASS) {
    Serial.println("[camera_ws] task create failed");
  }
}
