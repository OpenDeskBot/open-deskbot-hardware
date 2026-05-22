// ESP32-S3 firmware (use board: ESP32S3 Dev Module or matching variant).
#include "net.h"
#include "oled.h"
#include "audio_player.h"
#include "audio_capture.h"
#include "asr_chat_client.h"
#include "head.h"
#include "cmd.h"
#include "act.h"

unsigned long loop_start_time;

WifiClient wifiClient;
AsrChatClient asrChatClient;

void websocket_loop() {
    asrChatClient.loop();
}

void setup() {
  Serial.begin(115200);
  Serial.flush();
  // Set default log level
  log_set_level(LOG_LEVEL_INFO);
  log_info("Initializing...");
  setup_oled();
  setup_FFat();
  setup_head();
  setup_led();
  wifiClient.setup_wifi();
  wifiClient.setup_udp();
  setup_audio();
  mic_capture_setup();
  audio_play_task_setup();
  oled_println("\nReady.");
  display_task_setup();
  log_info("[BOOT] firmware=%s build=%s %s", VERSION, __DATE__, __TIME__);
  log_info("[BOOT] chat pipeline=asr_chat (host=%s port=%u; set ASR_CHAT_HOST in platformio.local.ini)",
           ASR_CHAT_HOST, (unsigned)ASR_CHAT_PORT);
  log_info("PSRAM: size=%u free=%u（大段 WS 下行 / 压缩等需要 PSRAM；若为 0 需启用 BOARD_HAS_PSRAM)",
           (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
  log_info("%s is Ready.", PRODUCT_NAME);
  last_time = millis();
}

void loop() {
  handle_cmd();
  if (!start_chat) {
    start_chat = true;
  }
  websocket_loop();

  if (millis() - loop_start_time > 1000) {
    wifiClient.send_ip();
    loop_start_time = millis();
  }

  if (start_chat) {
    start_chat = false;
    loop_start_time = millis();
    log_info("[CHAT] enter asr_chat loop (max 10 min)");
    while (millis() - loop_start_time < 600000) {  // max chat time 10 minutes
      /* 空闲时头部姿态由 updateAttentionDisplay() 基于 tts_active_ 驱动（TTS 结束后延迟低头）。 */
      log_info("[CHAT] running asr_chat voice round...");
      if (!asrChatClient.runVoiceRound(RECORD_TIME)) {
        log_error("[CHAT] asr_chat round failed, leave chat");
        blink_led(COLOR_RED, 3);
        break;
      }
      websocket_loop();
      /* 与 runVoiceRound 尾部 kPostTtsEchoGuardMs 叠加：降低扬声器→麦克风回声导致的连环对话 */
      delay(300);
    }
    log_info("[CHAT] leave chat loop");
  }

  if (enable_act && millis() - last_time > random(6, 10) * 1000) {
    random_act();
    last_time = millis();
  }

  delay(10);
}

// {"factory": "reboot"}
// {"factory": "reset_wifi"}
// {"factory": "adjust_x -10"}   -left, +right
// {"factory": "adjust_y -10"}    -up, +down
