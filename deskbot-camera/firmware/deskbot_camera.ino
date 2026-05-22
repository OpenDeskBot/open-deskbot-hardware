#include "esp_camera.h"
#include <WiFi.h>
#include <list>
#include <vector>
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "img_converters.h"
#include "avi_recorder.h"
#include "face_pos_ws.h"
#include "camera_ws.h"
#include "deskbot_device_id.h"
#include "deskbot_ws_config.h"
#include "deskbot_camera_config.h"
#include "wifi_provision.h"
#if DESKBOT_CAMERA_LOCAL_FACE_DETECT && defined(BOARD_HAS_PSRAM)
#include "human_face_detect_msr01.hpp"
#include "human_face_detect_mnp01.hpp"
#endif

//
// WARNING!!! PSRAM IC required for UXGA resolution and high JPEG quality
//            Ensure ESP32 Wrover Module or other board with PSRAM is selected
//            Partial images will be transmitted if image exceeds buffer size
//
//            You must select partition scheme from the board menu that has at least 3MB APP space.
//            Face Recognition is DISABLED for ESP32 and ESP32-S2, because it takes up from 15
//            seconds to process single frame. Face Detection is ENABLED if PSRAM is enabled as well

// ===================
// Select camera model
// ===================
#define CAMERA_MODEL_XIAO_ESP32S3 // Has PSRAM
#include "camera_pins.h"

// ===========================
// WiFi — 由 flash_camera.sh 从 deskbot.local.env 生成 firmware/wifi_defaults.h
// 未配置时进入热点 Deskbot_Camera 配网门户（见 wifi_provision.cpp）
// ===========================

void startCameraServer();
void setupLedFlash(int pin);

#ifdef BOARD_HAS_PSRAM
#if DESKBOT_CAMERA_LOCAL_FACE_DETECT
static constexpr uint32_t kFaceDetectIntervalMs = DESKBOT_CAMERA_FACE_DETECT_INTERVAL_MS;

static void faceDetectDelay(uint32_t min_ms = 0) {
  uint32_t ms = kFaceDetectIntervalMs;
  if (ms < min_ms) {
    ms = min_ms;
  }
  if (ms > 0) {
    vTaskDelay(pdMS_TO_TICKS(ms));
  } else {
    vTaskDelay(1);
  }
}

static void logAutoFaceResults(std::list<dl::detect::result_t> *results) {
  if (results == nullptr || results->empty()) {
    return;
  }

  Serial.printf("AUTO_FACE: DETECTED count=%u\r\n", (unsigned)results->size());
  int idx = 0;
  bool sent_ws = false;
  for (std::list<dl::detect::result_t>::iterator it = results->begin(); it != results->end(); ++it, ++idx) {
    if (it->keypoint.size() >= 10) {
      Serial.printf(
        "AUTO_FACE_KP[%d]: LE(%d,%d) ML(%d,%d) NOSE(%d,%d) RE(%d,%d) MR(%d,%d)\r\n",
        idx,
        (int)it->keypoint[0], (int)it->keypoint[1],
        (int)it->keypoint[2], (int)it->keypoint[3],
        (int)it->keypoint[4], (int)it->keypoint[5],
        (int)it->keypoint[6], (int)it->keypoint[7],
        (int)it->keypoint[8], (int)it->keypoint[9]
      );
      if (!sent_ws) {
        float c = it->score;
        if (c < 0.f) c = 0.f;
        else if (c > 1.f) c = 1.f;
        face_pos_ws_send_landmarks(&it->keypoint[0], c);
        sent_ws = true;
      }
    }
  }
}

static void autoFaceDetectTask(void *pvParameters) {
  HumanFaceDetectMSR01 s1(0.1F, 0.5F, 10, 0.2F);
  HumanFaceDetectMNP01 s2(0.5F, 0.3F, 5);
  (void)pvParameters;

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("AUTO_FACE: CAMERA_CAPTURE_FAILED");
      faceDetectDelay(200);
      continue;
    }

    if (fb->width > 400) {
      esp_camera_fb_return(fb);
      faceDetectDelay();
      continue;
    }

    if (fb->format == PIXFORMAT_RGB565) {
      std::list<dl::detect::result_t> &candidates = s1.infer((uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3});
      std::list<dl::detect::result_t> &results = s2.infer((uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3}, candidates);
      logAutoFaceResults(&results);
      esp_camera_fb_return(fb);
      faceDetectDelay();
      continue;
    }

    size_t out_len = fb->width * fb->height * 3;
    const int fb_w = fb->width;
    const int fb_h = fb->height;
    uint8_t *out_buf = (uint8_t *)malloc(out_len);
    if (!out_buf) {
      esp_camera_fb_return(fb);
      Serial.println("AUTO_FACE: RGB_BUFFER_ALLOC_FAILED");
      faceDetectDelay(200);
      continue;
    }

    bool ok = fmt2rgb888(fb->buf, fb->len, fb->format, out_buf);
    esp_camera_fb_return(fb);
    if (!ok) {
      free(out_buf);
      Serial.println("AUTO_FACE: RGB_CONVERT_FAILED");
      faceDetectDelay(200);
      continue;
    }

    std::list<dl::detect::result_t> &candidates = s1.infer((uint8_t *)out_buf, {fb_h, fb_w, 3});
    std::list<dl::detect::result_t> &results = s2.infer((uint8_t *)out_buf, {fb_h, fb_w, 3}, candidates);
    logAutoFaceResults(&results);
    free(out_buf);
    faceDetectDelay();
  }
}
#endif /* DESKBOT_CAMERA_LOCAL_FACE_DETECT */
#endif /* BOARD_HAS_PSRAM */

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  Serial.printf("PSRAM found: %s\n", psramFound() ? "YES" : "NO");
  Serial.printf("PSRAM size : %u bytes\n", (unsigned)ESP.getPsramSize());
  Serial.printf("Free PSRAM : %u bytes\n", (unsigned)ESP.getFreePsram());
#ifdef BOARD_HAS_PSRAM
  Serial.println("BOARD_HAS_PSRAM is DEFINED -> face detect compiled IN");
#else
  Serial.println("BOARD_HAS_PSRAM NOT defined -> face detect compiled OUT!");
#endif

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG; // for streaming
  //config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      // Limit the frame size when PSRAM is not available
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    // Best option for face detection/recognition
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1); // flip it back
    s->set_brightness(s, 1); // up the brightness just a bit
    s->set_saturation(s, -2); // lower the saturation
  }
  // 默认设置为 QVGA(320x240)，便于直接进行人脸检测/识别
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_QVGA);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

// Setup LED FLash if LED pin is defined in camera_pins.h
#if defined(LED_GPIO_NUM)
  setupLedFlash(LED_GPIO_NUM);
#endif

  // Initialize SD card (GPIO 21 = CS on XIAO ESP32S3 Sense)
  bool sdReady = SD.begin(21);
  if (sdReady) {
    Serial.println("SD Card Mounted.");
  } else {
    Serial.println("SD Card Mount Failed! Recording disabled.");
  }

  if (!wifi_provision_connect()) {
    Serial.println("WiFi connect failed");
    return;
  }

  Serial.printf("[BOOT] ws host=%s port=%u device_id=%s (set DESKBOT_WS_HOST in platformio.local.ini)\r\n",
                DESKBOT_WS_HOST, (unsigned)DESKBOT_WS_PORT, deskbot_device_id());
  Serial.printf("[BOOT] vision: upload_jpeg=%d upload_fps=%d local_face_detect=%d face_fps=%d\r\n",
                (int)DESKBOT_CAMERA_UPLOAD_JPEG, (int)DESKBOT_CAMERA_UPLOAD_FPS,
                (int)DESKBOT_CAMERA_LOCAL_FACE_DETECT, (int)DESKBOT_CAMERA_FACE_DETECT_FPS);

#if DESKBOT_CAMERA_LOCAL_FACE_DETECT
  face_pos_ws_init();
#endif
#if DESKBOT_CAMERA_UPLOAD_JPEG
  camera_ws_init();
#endif
  startCameraServer();

#if DESKBOT_CAMERA_LOCAL_FACE_DETECT && defined(BOARD_HAS_PSRAM)
  xTaskCreatePinnedToCore(autoFaceDetectTask, "auto_face_task", 12288, NULL, 1, NULL, 1);
  Serial.printf("AUTO_FACE: background detection task started (fps=%d interval=%ums)\r\n",
                (int)DESKBOT_CAMERA_FACE_DETECT_FPS, (unsigned)kFaceDetectIntervalMs);
#elif DESKBOT_CAMERA_LOCAL_FACE_DETECT
  Serial.println("AUTO_FACE: local face detect requested but PSRAM unavailable — disabled");
#endif

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");

  if (sdReady) {
    startRecording();
  }
}

void loop() {
  // Do nothing. Everything is done in another task by the web server
  delay(10000);
}
