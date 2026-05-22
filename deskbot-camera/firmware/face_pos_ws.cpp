#include "face_pos_ws.h"
#include "deskbot_camera_config.h"
#include "deskbot_device_id.h"
#include "deskbot_ws_config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstring>

static constexpr size_t kWsMsgMax = 512;
static constexpr int kQueueLen = 8;

static const char kHost[] = DESKBOT_WS_HOST;
static constexpr uint16_t kPort = DESKBOT_WS_PORT;
static const char kBasePath[] = "/device_pipeline";

static char ws_path[96];

static WebSocketsClient ws;
static QueueHandle_t msg_q = nullptr;
static volatile bool ws_connected = false;

static const char *wsTypeName(WStype_t t) {
  switch (t) {
    case WStype_DISCONNECTED: return "DISCONNECTED";
    case WStype_CONNECTED: return "CONNECTED";
    case WStype_TEXT: return "TEXT";
    case WStype_BIN: return "BIN";
    case WStype_ERROR: return "ERROR";
    case WStype_FRAGMENT_TEXT_START: return "FRAG_TEXT_START";
    case WStype_FRAGMENT_BIN_START: return "FRAG_BIN_START";
    case WStype_FRAGMENT: return "FRAG";
    case WStype_FRAGMENT_FIN: return "FRAG_FIN";
    case WStype_PING: return "PING";
    case WStype_PONG: return "PONG";
    default: return "?";
  }
}

static void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      ws_connected = true;
      Serial.printf("[face_pos_ws] CONNECTED to ws://%s:%u%s\r\n",
                    kHost, (unsigned)kPort, ws_path);
      break;
    case WStype_DISCONNECTED:
      if (ws_connected) {
        Serial.println("[face_pos_ws] DISCONNECTED");
      } else {
        Serial.printf("[face_pos_ws] connect failed to ws://%s:%u%s (will retry)\r\n",
                      kHost, (unsigned)kPort, ws_path);
      }
      ws_connected = false;
      break;
    case WStype_ERROR:
      Serial.printf("[face_pos_ws] ERROR len=%u payload=%.*s\r\n",
                    (unsigned)length, (int)length, payload ? (const char*)payload : "");
      break;
    case WStype_TEXT:
      Serial.printf("[face_pos_ws] RX TEXT len=%u: %.*s\r\n",
                    (unsigned)length, (int)length, (const char*)payload);
      break;
    default:
      Serial.printf("[face_pos_ws] event=%s len=%u\r\n", wsTypeName(type), (unsigned)length);
      break;
  }
}

static void wsWorker(void *) {
  bool wifi_was_up = false;
  bool begun = false;
  uint32_t last_status_log_ms = 0;

  for (;;) {
    bool wifi_up = (WiFi.status() == WL_CONNECTED);

    if (wifi_up && !wifi_was_up) {
      Serial.printf("[face_pos_ws] WiFi up, connecting to ws://%s:%u%s\r\n",
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
    if (now - last_status_log_ms >= 5000) {
      last_status_log_ms = now;
      Serial.printf("[face_pos_ws] status: wifi=%s ws=%s\r\n",
                    wifi_up ? "up" : "down",
                    ws_connected ? "connected" : "disconnected");
    }

    char buf[kWsMsgMax];
    int sent = 0;
    int dropped = 0;
    while (msg_q && xQueueReceive(msg_q, buf, 0) == pdTRUE) {
      if (ws_connected) {
        if (ws.sendTXT(buf)) {
          sent++;
        } else {
          dropped++;
        }
      } else {
        dropped++;
      }
    }
    if (sent || dropped) {
      Serial.printf("[face_pos_ws] tx sent=%d dropped=%d\r\n", sent, dropped);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void face_pos_ws_init(void) {
#if !DESKBOT_CAMERA_LOCAL_FACE_DETECT
  Serial.println("[face_pos_ws] disabled (DESKBOT_CAMERA_LOCAL_FACE_DETECT=0)");
  return;
#endif
  if (!deskbot_ws_configured()) {
    Serial.println("[face_pos_ws] DESKBOT_WS_HOST not set; skipping (see platformio.local.ini.example)");
    return;
  }
  if (msg_q) {
    return;
  }
  msg_q = xQueueCreate(kQueueLen, kWsMsgMax);
  if (!msg_q) {
    Serial.println("[face_pos_ws] queue create failed");
    return;
  }
  snprintf(ws_path, sizeof(ws_path), "%s?role=pub&device_id=%s", kBasePath, deskbot_device_id());
  ws.onEvent(onWsEvent);
  Serial.printf("[face_pos_ws] init target=ws://%s:%u%s\r\n",
                kHost, (unsigned)kPort, ws_path);
  BaseType_t ok = xTaskCreatePinnedToCore(wsWorker, "face_pos_ws", 10240, nullptr, 2, nullptr, 0);
  if (ok != pdPASS) {
    Serial.println("[face_pos_ws] task create failed");
  }
}

void face_pos_ws_send_landmarks(const int *kp10, float confidence) {
  if (!msg_q || !kp10) {
    return;
  }

  if (confidence < 0.f) {
    confidence = 0.f;
  } else if (confidence > 1.f) {
    confidence = 1.f;
  }

  char buf[kWsMsgMax];
  int n = snprintf(
      buf, sizeof(buf),
      "{\"device_id\":\"%s\",\"points\":{"
      "\"left_eye\":[%d,%d],\"right_eye\":[%d,%d],\"nose\":[%d,%d],"
      "\"mouth_left\":[%d,%d],\"mouth_right\":[%d,%d]},"
      "\"confidence\":%.5f}",
      deskbot_device_id(), kp10[0], kp10[1], kp10[6], kp10[7], kp10[4], kp10[5], kp10[2], kp10[3],
      kp10[8], kp10[9], confidence);
  if (n <= 0 || (size_t)n >= sizeof(buf)) {
    return;
  }

  if (xQueueSend(msg_q, buf, 0) != pdTRUE) {
    // 队列满则丢弃该帧，避免阻塞检测线程
  }
}
