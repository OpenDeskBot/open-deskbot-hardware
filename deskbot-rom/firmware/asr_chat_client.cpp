#include "asr_chat_client.h"

#include <ArduinoJson.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "oled.h"
#include "audio_capture.h"
#include "esp_heap_caps.h"
#include "head.h"
#include "mbedtls/base64.h"

/* 动作 dispatch 在 cmd.cpp；此处仅 forward declare，避免拉入 net.h 等。 */
extern void executeCommand(String cmd);

static int pb_json_number_to_int(JsonVariantConst v, int defv) {
  if (v.isNull()) {
    return defv;
  }
  return (int)lround(v.as<double>());
}

extern AsrChatClient asrChatClient;

namespace {
/* 上一轮 TTS 刚结束就进入下一轮拾音时，喇叭尾音/房间反射易进麦克风 → ASR 连环触发
 *（短词如「老板」尤其明显）。本轮收尾后先静音窗口 + 泵 WS，再让下一轮从 flush 开始采音。 */
constexpr uint32_t kPostTtsEchoGuardMs = 500;

/* pb v1 下行 JSON 整包打日志（过长截断，避免串口/堆压力）。 */
inline void pb_log_rx_json_doc(const JsonDocument& doc) {
  String line;
  line.reserve(768);
  serializeJson(doc, line);
  constexpr size_t kMax = 3000;
  if (line.length() > kMax) {
    line.remove(kMax);
    line += F("...(trunc)");
  }
  log_info("[PB] rx %s", line.c_str());
}

/* opportunistic：OLED / 舵机 / 音频播放任务队列任一有积压则视为「worker 忙」，本条 opportunistic
 * 不得生效（跨 req 无 BIN pb_single 直接忽略；将触发新开 req 的 opportunistic 也不 drain+reset）。
 * 另计 I2S 流式仍在播（队列已空、尾段在播），与「三队列」一并作为忽略条件，避免尾音期误接侧信道。 */
inline bool pb_opportunistic_workers_busy_ignore() {
  return oled_render_input_queue_depth() > 0u || head_motor_input_queue_depth() > 0u ||
         audio_play_input_queue_depth() > 0u || audio_play_stream_pcm_active();
}

/* BIN 侧 opportunistic 丢音频：仅用三队列（不含 stream_active），避免同 req 多片 BIN 在 stream 已开时被整段跳过。 */
inline bool pb_opportunistic_audio_backpressure_busy() {
  return oled_render_input_queue_depth() > 0u || head_motor_input_queue_depth() > 0u ||
         audio_play_input_queue_depth() > 0u;
}
}  // namespace

AsrChatClient::PbEnqueueAction AsrChatClient::parsePbEnqueueAction(const JsonDocument& doc) {
  String a = doc["action"].is<String>() ? doc["action"].as<String>() : String("");
  a.toLowerCase();
  if (a == "append") {
    return PbEnqueueAction::kAppend;
  }
  if (a == "opportunistic") {
    return PbEnqueueAction::kOpportunistic;
  }
  return PbEnqueueAction::kReplace;
}

AsrChatClient::AsrChatClient() {}

void AsrChatClient::pbReset(bool stop_audio) {
  const uint8_t pb_ch_before_reset = pb_ch_;
  pb_active_ = false;
  pb_req_.remove(0);
  pb_next_idx_ = 0;
  pb_expect_bin_ = false;
  pb_expect_bin_len_ = 0;
  pb_sr_ = 0;
  pb_ch_ = 0;
  pb_fmt_.remove(0);
  pb_pending_idx_ = 0;
  pb_pending_chunk_ms_ = 0;
  pb_pending_anim_json_.remove(0);
  pb_pending_has_servo_ = false;
  pb_servo_xm_ = 2;
  pb_servo_ym_ = 2;
  pb_servo_x_ = 0;
  pb_servo_y_ = 0;
  pb_servo_ms_ = 0;
  pb_last_ack_idx_ = 0;
  pb_end_waiting_bin_ = false;
  pb_end_idx_ = 0;

  pb_ack_out_pending_ = false;
  pb_ack_out_req_.remove(0);
  pb_ack_out_idx_ = 0;
  pb_ack_out_buf_ms_ = 0;
  pb_last_pb_ack_sent_wall_ms_ = 0;
  pb_ack_bypass_throttle_ = false;
  pb_pending_enqueue_action_ = PbEnqueueAction::kReplace;
  head_drain_pb_motor_ack_queue();

  pb_audio_buf_ms_est_ = 0;
  pb_last_buf_decay_ms_ = millis();
  if (stop_audio) {
    if (pb_deferred_stream_end_pending_) {
      pb_deferred_stream_end_pending_ = false;
      audio_stream_pcm16_end(pb_deferred_stream_end_ch_);
    } else if (pb_audio_stream_started_) {
      const uint8_t ch_end = (pb_ch_before_reset == 0 || pb_ch_before_reset > 2) ? 1 : pb_ch_before_reset;
      audio_stream_pcm16_end(ch_end);
    }
    pb_audio_stream_started_ = false;
  }
}

void AsrChatClient::pbProtocolError(const char* why) {
  log_warn("[PB] protocol error: %s (req=%s expect_bin=%d expect_len=%u next_idx=%u)",
           why ? why : "?", pb_req_.c_str(), (int)pb_expect_bin_, (unsigned)pb_expect_bin_len_,
           (unsigned)pb_next_idx_);
  pbReset(/*stop_audio=*/true);
  /* 协议错位后兜底标记本轮 reply 结束，避免 runVoiceRound 等 30s 超时。 */
  pbSignalTtsRoundComplete();
}

void AsrChatClient::pbSubmitAnimIfAny(uint32_t chunk_ms, PbEnqueueAction action) {
  if (pb_pending_anim_json_.isEmpty()) {
    return;
  }
  if (action == PbEnqueueAction::kOpportunistic && pb_opportunistic_audio_backpressure_busy()) {
    return;
  }
  oled_render_submit_pb_vector_json(pb_pending_anim_json_.c_str(), pb_pending_anim_json_.length(), chunk_ms);
}

/* pb 舵机：在 budget_ms 内按 50Hz(20ms/拍) 走完主行程。步进上限取经验值，过大易抖、过小易超时；
 * 尾段剩余 <20ms 时 motor_task 不再 write，由 MotorCmd.ms 对齐 chunk。 */
static constexpr uint8_t k_pb_servo_max_step_deg = 12;

static uint8_t compute_step_deg_for_ms(int dx_deg, int dy_deg, uint16_t ms) {
  int d = abs(dx_deg);
  if (abs(dy_deg) > d) d = abs(dy_deg);
  if (d <= 0) return 1;
  if (ms < 20) ms = 20;
  int ticks = (int)((ms + 19) / 20);  // motor_task 固定 20ms/tick
  if (ticks <= 0) ticks = 1;
  int step = (d + ticks - 1) / ticks;
  if (step < 1) step = 1;
  if (step > (int)k_pb_servo_max_step_deg) step = (int)k_pb_servo_max_step_deg;
  return (uint8_t)step;
}

bool AsrChatClient::pbApplyServoIfAny(uint32_t chunk_ms, uint32_t chunk_idx, bool defer_ack_if_enqueued,
                                      PbEnqueueAction action, const char* defer_ack_req_cstr) {
  if (!pb_pending_has_servo_) {
    return false;
  }
  if (action == PbEnqueueAction::kOpportunistic && pb_opportunistic_audio_backpressure_busy()) {
    return false;
  }
  const int xm = pb_servo_xm_;
  const int ym = pb_servo_ym_;
  const int x = pb_servo_x_;
  const int y = pb_servo_y_;

  uint32_t budget_ms = chunk_ms;
  if (budget_ms == 0) {
    budget_ms = pb_servo_ms_;
  }
  if (budget_ms == 0) {
    budget_ms = 20;
  }

  int x_now = servo_x.read();
  int y_now = head_y_pwm_to_logic(servo_y.read());
  int x_target = x_now;
  int y_target = y_now;
  bool drive_x = true;
  bool drive_y = true;

  if (xm == (int)HEAD_SERVO_HOLD) {
    drive_x = false;
  } else if (xm == (int)HEAD_SERVO_ABS) {
    x_target = constrain(x, X_MIN, X_MAX);
  } else if (xm == (int)HEAD_SERVO_REL) {
    x_target = constrain(x_now + x, X_MIN, X_MAX);
  } else {
    drive_x = false;
  }

  if (ym == (int)HEAD_SERVO_HOLD) {
    drive_y = false;
  } else if (ym == (int)HEAD_SERVO_ABS) {
    y_target = constrain(y, Y_MIN, Y_MAX);
  } else if (ym == (int)HEAD_SERVO_REL) {
    y_target = constrain(y_now + y, Y_MIN, Y_MAX);
  } else {
    drive_y = false;
  }

  if (!drive_x && !drive_y) {
    log_warn("[PB] servo skip (both axes HOLD/unknown mode): xm=%d ym=%d json_xy=(%d,%d)", xm, ym, x, y);
    return false;
  }

  const int dx = drive_x ? (x_target - x_now) : 0;
  const int dy = drive_y ? (y_target - y_now) : 0;
  if (dx == 0 && dy == 0) {
    /* 常见：相对指令已顶在 X_MIN/X_MAX（或 Y）软件行程，constrain 后目标=当前读数 → 水平/垂直不再动。 */
    log_warn("[PB] servo noop (0° after clamp): xm=%d ym=%d json_xy=(%d,%d) read=(%d,%d) tgt=(%d,%d) X[%d,%d] Y[%d,%d]",
             xm, ym, x, y, x_now, y_now, x_target, y_target, X_MIN, X_MAX, Y_MIN, Y_MAX);
    return false;
  }
  int dmax = 0;
  if (drive_x) {
    dmax = abs(dx);
  }
  if (drive_y) {
    dmax = (abs(dy) > dmax) ? abs(dy) : dmax;
  }

  uint16_t ms_budget = (budget_ms > 65535u) ? 65535u : (uint16_t)budget_ms;
  uint8_t step_deg = compute_step_deg_for_ms(dx, dy, ms_budget);

  log_info("[PB] servo enqueue: xm=%d ym=%d json=(%d,%d) read=(%d,%d) tgt=(%d,%d) d=(%d,%d) dmax=%d step=%u ms=%u chunk_ms=%u X[%d,%d] Y[%d,%d]",
           xm, ym, x, y, x_now, y_now, x_target, y_target, dx, dy, dmax, (unsigned)step_deg, (unsigned)ms_budget,
           (unsigned)chunk_ms, X_MIN, X_MAX, Y_MIN, Y_MAX);

  /* head_servo_cmd_async：与 JSON servo 同形入队，motor_task 再 read()+解析 xm/ym。 */
  const char* ack_req = defer_ack_req_cstr;
  if (ack_req == nullptr || ack_req[0] == '\0') {
    ack_req = pb_req_.c_str();
  }
  if (defer_ack_if_enqueued && ack_req[0] != '\0') {
    head_servo_cmd_async((uint8_t)xm, (uint8_t)ym, x, y, step_deg, ms_budget, /*pb_ack_after_done=*/true,
                         chunk_idx, ack_req);
  } else {
    head_servo_cmd_async((uint8_t)xm, (uint8_t)ym, x, y, step_deg, ms_budget);
  }
  return true;
}

void AsrChatClient::pbUpdateAudioBufDecayWall() {
  unsigned long now = millis();
  if (pb_last_buf_decay_ms_ == 0) {
    pb_last_buf_decay_ms_ = now;
  }
  if (pb_audio_stream_started_ && pb_audio_buf_ms_est_ > 0) {
    int32_t dec = (int32_t)(now - pb_last_buf_decay_ms_);
    if (dec > 0) {
      pb_audio_buf_ms_est_ -= dec;
      if (pb_audio_buf_ms_est_ < 0) pb_audio_buf_ms_est_ = 0;
    }
  }
  pb_last_buf_decay_ms_ = now;
}

void AsrChatClient::pbScheduleMotorAck(const char* req_cstr, uint32_t idx) {
  if (req_cstr == nullptr || req_cstr[0] == '\0') {
    return;
  }
  pbUpdateAudioBufDecayWall();
  pb_ack_out_req_ = req_cstr;
  pb_ack_out_idx_ = idx;
  pb_ack_out_buf_ms_ = pb_audio_buf_ms_est_;
  pb_ack_out_pending_ = true;
  pb_ack_bypass_throttle_ = true;
  if (idx > pb_last_ack_idx_) {
    pb_last_ack_idx_ = idx;
  }
}

void AsrChatClient::pbFlushAckForForeignReq(const String& req, uint32_t idx) {
  pbUpdateAudioBufDecayWall();
  pb_ack_out_req_ = req;
  pb_ack_out_idx_ = idx;
  pb_ack_out_buf_ms_ = pb_audio_buf_ms_est_;
  pb_ack_out_pending_ = true;
  pb_ack_bypass_throttle_ = true;
  flushPendingPbAck();
}

void AsrChatClient::pbMaybeAck(uint32_t idx) {
  if (!pb_active_ || pb_req_.isEmpty()) {
    return;
  }

  pbUpdateAudioBufDecayWall();

  if (idx < pb_last_ack_idx_) {
    return;
  }
  pb_last_ack_idx_ = idx;
  pb_ack_out_req_ = pb_req_;
  pb_ack_out_idx_ = idx;
  pb_ack_out_buf_ms_ = pb_audio_buf_ms_est_;
  pb_ack_out_pending_ = true;
}

void AsrChatClient::flushPendingPbAck() {
  if (!pb_ack_out_pending_) {
    return;
  }
  if (!ws_.isConnected()) {
    pb_ack_out_pending_ = false;
    return;
  }
  const unsigned long now_wall = millis();
  if (!pb_ack_bypass_throttle_ &&
      (pb_last_pb_ack_sent_wall_ms_ != 0) &&
      (now_wall - pb_last_pb_ack_sent_wall_ms_ < 280UL)) {
    return;
  }
  pb_ack_bypass_throttle_ = false;
  pb_last_pb_ack_sent_wall_ms_ = now_wall;
  pb_ack_out_pending_ = false;
  const int servo_x_deg = servo_x.read();
  const int servo_y_deg = head_y_pwm_to_logic(servo_y.read());
  String msg;
  msg.reserve(220);
  msg = "{\"type\":\"pb_ack\",\"req\":\"";
  msg += pb_ack_out_req_;
  msg += "\",\"idx\":";
  msg += String((unsigned)pb_ack_out_idx_);
  msg += ",\"audio_buf_ms\":";
  msg += String((int)pb_ack_out_buf_ms_);
  msg += ",\"servo\":{\"x\":";
  msg += String(servo_x_deg);
  msg += ",\"y\":";
  msg += String(servo_y_deg);
  msg += ",\"x_min\":";
  msg += String(X_MIN);
  msg += ",\"x_max\":";
  msg += String(X_MAX);
  msg += ",\"y_min\":";
  msg += String(Y_MIN);
  msg += ",\"y_max\":";
  msg += String(Y_MAX);
  msg += "}}";
  log_info("[PB] tx %s", msg.c_str());
  sendJson(msg);
}

void AsrChatClient::pbSignalTtsRoundComplete() {
  reply_done_ = true;
  tts_active_ = false;
}

bool AsrChatClient::pbDispatchChunkPreamble(uint32_t chunk_ms, uint32_t chunk_idx, PbEnqueueAction action) {
  /* 三路均异步入队（音频→audio_play_q / OLED→s_queue drop-oldest / 舵机→s_motor_queue async），
   * 调用方（onWebSocketEvent BIN 上下文）立即返回继续收下一帧。
   * 节拍闸：音频 PCM 实际播放速率 = chunk_ms；motor_task 在 chunk_ms 内 ramp+hold；
   * OLED 渲染后 vTaskDelay(chunk_ms) 持帧。三任务独立按 chunk_ms 计时，松散同步。
   * 反压：音频队列满（32 帧 ≈ 3~6s）+ 服务端基于 pb_ack.audio_buf_ms 限速。 */
  flushPendingPbAck();
  const bool had_servo = pb_pending_has_servo_;
  pbSubmitAnimIfAny(chunk_ms, action);
  return pbApplyServoIfAny(chunk_ms, chunk_idx, had_servo, action);
}

bool AsrChatClient::pbParseAndStage(const JsonDocument& doc) {
  String type = doc["type"].is<String>() ? doc["type"].as<String>() : String("");
  if (type != "pb_start" && type != "pb_chunk" && type != "pb_end" && type != "pb_single") {
    return false;
  }
  pb_log_rx_json_doc(doc);
  String req = doc["req"].is<String>() ? doc["req"].as<String>() : String("");
  if (req.isEmpty()) {
    pbProtocolError("missing req");
    return true;
  }

  uint32_t idx = doc["idx"].is<uint32_t>() ? doc["idx"].as<uint32_t>() : 0;

  if (!pb_suppress_tail_req_.isEmpty()) {
    if (req != pb_suppress_tail_req_) {
      pb_suppress_tail_req_.remove(0);
    } else if (!((type == "pb_start" || type == "pb_single") && idx == 0u)) {
      log_info("[PB] ignore stale pb after abort type=%s idx=%u req=%s", type.c_str(), (unsigned)idx,
               req.c_str());
      return true;
    } else {
      pb_suppress_tail_req_.remove(0);
    }
  }

  uint32_t chunk_ms = doc["chunk_ms"].is<uint32_t>() ? doc["chunk_ms"].as<uint32_t>() : 0;
  const PbEnqueueAction chunk_action = parsePbEnqueueAction(doc);

  bool next_bin_early = false;
  if (!doc["audio"].isNull()) {
    next_bin_early = (doc["audio"]["next_bin"] | 0) == 1;
  }

  /* opportunistic + 不同 req 的 pb_single（无 BIN）：侧信道，不打断、不 drain+reset。
   * 任一脚本队列非空或流式仍在播时整条忽略（见 pb_opportunistic_workers_busy_ignore）。 */
  if (type == "pb_single" && chunk_action == PbEnqueueAction::kOpportunistic && req != pb_req_ &&
      !next_bin_early) {
    if (pb_opportunistic_workers_busy_ignore()) {
      log_warn("[PB] opportunistic cross-req pb_single ignored (workers busy) inj_req=%s main_req=%s",
               req.c_str(), pb_req_.c_str());
      return true;
    }
    String backup_anim = pb_pending_anim_json_;
    const bool backup_has = pb_pending_has_servo_;
    const int backup_xm = pb_servo_xm_;
    const int backup_ym = pb_servo_ym_;
    const int backup_x = pb_servo_x_;
    const int backup_y = pb_servo_y_;
    const uint16_t backup_ms = pb_servo_ms_;

    pb_pending_anim_json_.remove(0);
    if (!doc["anim"].isNull()) {
      String s;
      serializeJson(doc["anim"], s);
      pb_pending_anim_json_ = s;
    }
    pb_pending_has_servo_ = false;
    if (!doc["servo"].isNull()) {
      JsonObjectConst servo = doc["servo"].as<JsonObjectConst>();
      if (!servo.isNull()) {
        pb_servo_xm_ = constrain(pb_json_number_to_int(servo["xm"], 2), 0, 2);
        pb_servo_ym_ = constrain(pb_json_number_to_int(servo["ym"], 2), 0, 2);
        pb_servo_x_ = pb_json_number_to_int(servo["x"], 0);
        pb_servo_y_ = pb_json_number_to_int(servo["y"], 0);
        pb_servo_ms_ = (uint16_t)constrain(pb_json_number_to_int(servo["ms"], 0), 0, 65535);
        pb_pending_has_servo_ = true;
      }
    }
    pbSubmitAnimIfAny(chunk_ms, PbEnqueueAction::kOpportunistic);
    const bool servo_enqueued =
        pbApplyServoIfAny(chunk_ms, idx, /*defer_ack_if_enqueued=*/true, PbEnqueueAction::kOpportunistic,
                          req.c_str());
    if (!servo_enqueued) {
      pbFlushAckForForeignReq(req, idx);
    }
    pb_pending_anim_json_ = backup_anim;
    pb_pending_has_servo_ = backup_has;
    pb_servo_xm_ = backup_xm;
    pb_servo_ym_ = backup_ym;
    pb_servo_x_ = backup_x;
    pb_servo_y_ = backup_y;
    pb_servo_ms_ = backup_ms;
    log_info("[PB] opportunistic cross-req pb_single side-channel inj_req=%s main_req=%s idx=%u",
             req.c_str(), pb_req_.c_str(), (unsigned)idx);
    return true;
  }
  if (type == "pb_single" && chunk_action == PbEnqueueAction::kOpportunistic && pb_active_ && req != pb_req_ &&
      next_bin_early) {
    log_warn("[PB] opportunistic cross-req pb_single with audio.next_bin not supported (inj_req=%s)",
             req.c_str());
    return true;
  }

  /* req 切换：默认 drain+reset（replace / 换 req）。append 同 req 不打断；opportunistic 跨 req 无 BIN pb_single
   * 走侧信道（见上文）。opportunistic 且将新开序列时，若 worker 忙则整条忽略、不 reset。 */
  const bool req_boundary = (!pb_active_) || (req != pb_req_);
  const bool force_restart_same_req =
      (chunk_action == PbEnqueueAction::kReplace) &&
      ((type == "pb_start" || type == "pb_single") && idx == 0);
  const bool is_new_req = req_boundary || force_restart_same_req;
  if (is_new_req && chunk_action == PbEnqueueAction::kOpportunistic && pb_opportunistic_workers_busy_ignore()) {
    log_warn("[PB] opportunistic new sequence ignored (workers busy) type=%s req=%s main_req=%s",
             type.c_str(), req.c_str(), pb_req_.c_str());
    return true;
  }
  if (is_new_req) {
    /* 内部打断协议：drain + 入队 reset 到三个 worker 队列尾部。
     * - 三个 task 完成"当前正在执行的 job"（音频 chunk / OLED 帧 / 舵机 ramp+hold）后立即
     *   收到 reset，做收尾（音频停流 + zero DMA / OLED 保留画面 / motor 清 pending）。
     * - 不阻塞本调用：caller 仅 drain + 入队，不等待 worker 处理完成。
     * - 替代 pbReset(stop_audio=true) 里的 audio_stream_pcm16_end 同步等流尾——更快、不阻塞 WS 回调。 */
    audio_play_reset();
    oled_render_reset();
    head_motor_reset();
    /* pbReset 仅清 pb 内部状态变量（stop_audio=false 跳过其内部 audio_stream_pcm16_end）。
     * audio_play_reset 已让流停下，这里手动同步 pb 侧的 stream 标志位。 */
    pbReset(/*stop_audio=*/false);
    pb_audio_stream_started_ = false;
    pb_deferred_stream_end_pending_ = false;

    pb_active_ = true;
    pb_req_ = req;
    pb_next_idx_ = 0;
    const char* act_s = (chunk_action == PbEnqueueAction::kAppend)         ? "append"
                        : (chunk_action == PbEnqueueAction::kOpportunistic) ? "opportunistic"
                                                                            : "replace";
    log_info("[PB] new sequence: drain+reset 3 queues for req=%s type=%s idx=%u action=%s",
             req.c_str(), type.c_str(), (unsigned)idx, act_s);
  }

  if (!pb_active_ || req != pb_req_) {
    pbProtocolError("req mismatch after reset");
    return true;
  }

  if (idx != pb_next_idx_) {
    pbProtocolError("idx not monotonic");
    return true;
  }

  if (doc["sr"].is<uint32_t>()) {
    pb_sr_ = doc["sr"].as<uint32_t>();
  }
  if (doc["ch"].is<int>()) {
    pb_ch_ = (uint8_t)doc["ch"].as<int>();
  } else if (doc["ch"].is<double>()) {
    pb_ch_ = (uint8_t)doc["ch"].as<double>();
  } else if (doc["ch"].is<uint8_t>()) {
    pb_ch_ = doc["ch"].as<uint8_t>();
  }
  if (doc["fmt"].is<String>()) {
    pb_fmt_ = doc["fmt"].as<String>();
  }

  pb_pending_anim_json_.remove(0);
  if (!doc["anim"].isNull()) {
    String s;
    serializeJson(doc["anim"], s);
    pb_pending_anim_json_ = s;
  }

  pb_pending_has_servo_ = false;
  if (!doc["servo"].isNull()) {
    JsonObjectConst servo = doc["servo"].as<JsonObjectConst>();
    if (!servo.isNull()) {
      pb_servo_xm_ = constrain(pb_json_number_to_int(servo["xm"], 2), 0, 2);
      pb_servo_ym_ = constrain(pb_json_number_to_int(servo["ym"], 2), 0, 2);
      pb_servo_x_ = pb_json_number_to_int(servo["x"], 0);
      pb_servo_y_ = pb_json_number_to_int(servo["y"], 0);
      pb_servo_ms_ = (uint16_t)constrain(pb_json_number_to_int(servo["ms"], 0), 0, 65535);
      pb_pending_has_servo_ = true;
    }
  }

  pb_expect_bin_ = false;
  pb_expect_bin_len_ = 0;
  bool next_bin = false;
  if (!doc["audio"].isNull()) {
    next_bin = (doc["audio"]["next_bin"] | 0) == 1;
  }
  if (next_bin) {
    if (chunk_ms == 0) {
      pbProtocolError("audio.next_bin but missing chunk_ms");
      return true;
    }
    if (pb_sr_ == 0 || pb_ch_ == 0 || pb_fmt_.isEmpty()) {
      pbProtocolError("audio.next_bin but missing sr/ch/fmt");
      return true;
    }
    if (pb_fmt_ != "s16le") {
      pbProtocolError("unsupported fmt (need s16le)");
      return true;
    }
    pb_expect_bin_ = true;
    /* 期望长度：协议文档建议严格匹配，但实际服务端可能存在取整/裁剪导致的 1–数个 sample 误差。
     * 这里保留“期望值”用于诊断与粗错位检测，但 binary 处理时允许小幅偏差。 */
    pb_expect_bin_len_ =
        (size_t)((uint64_t)chunk_ms * (uint64_t)pb_sr_ * (uint64_t)pb_ch_ * 2ULL / 1000ULL);
    pb_pending_idx_ = idx;
    pb_pending_chunk_ms_ = chunk_ms;
    log_info("[PB] expect BIN req=%s type=%s idx=%u chunk_ms=%u expect_bytes=%u sr=%u ch=%u fmt=%s",
             pb_req_.c_str(), type.c_str(), (unsigned)idx, (unsigned)chunk_ms,
             (unsigned)pb_expect_bin_len_, (unsigned)pb_sr_, (unsigned)pb_ch_, pb_fmt_.c_str());
    if (type == "pb_end" || type == "pb_single") {
      pb_end_waiting_bin_ = true;
      pb_end_idx_ = idx;
    } else {
      pb_end_waiting_bin_ = false;
    }
    pb_pending_enqueue_action_ = chunk_action;
    /* BIN 到达后在 onWebSocketEvent(BIN) 里读完并入队 PCM，再调 pbDispatchChunkPreamble。 */
  } else {
    const bool sequence_end = (type == "pb_end" || type == "pb_single");
    const bool had_servo = pb_pending_has_servo_;
    pbSubmitAnimIfAny(chunk_ms, chunk_action);
    const bool servo_enqueued = pbApplyServoIfAny(chunk_ms, idx, had_servo, chunk_action);
    if (!servo_enqueued) {
      pbMaybeAck(idx);
    }
    if (sequence_end) {
      /* 允许“最后一片无音频”的情况：收尾释放 pipeline。pb_single 语义为单包整轮，与 pb_end 同收尾。 */
      log_info("[PB] complete end_no_bin req=%s type=%s idx=%u",
               pb_req_.c_str(), type.c_str(), (unsigned)idx);
      if (pb_audio_stream_started_) {
        const uint8_t ch_end = (pb_ch_ == 0 || pb_ch_ > 2) ? 1 : pb_ch_;
        audio_stream_pcm16_end(ch_end);
        pb_audio_stream_started_ = false;
      }
      pb_end_waiting_bin_ = false;
      pb_end_idx_ = 0;
      pb_active_ = false;
      pbSignalTtsRoundComplete();
      pb_ack_bypass_throttle_ = true;
      flushPendingPbAck();
    }
  }

  pb_next_idx_ = idx + 1;
  return true;
}

bool AsrChatClient::connect() {
  if (ws_.isConnected()) {
    return true;
  }
  if (kHost[0] == '\0') {
    log_error("[ASR_CHAT] ASR_CHAT_HOST not set; copy platformio.local.ini.example "
              "to platformio.local.ini and define -DASR_CHAT_HOST=\\\"<host>\\\"");
    return false;
  }
  ready_ = false;
  reply_done_ = false;
  server_started_reply_ = false;

  char path[64];
  snprintf(path, sizeof(path), "/asr_chat?device_id=%s", get_device_id());
  log_info("[ASR_CHAT] connecting ws://%s:%u%s", kHost, (unsigned)kPort, path);
  ws_.begin(kHost, kPort, path);
  ws_.onEvent([this](WStype_t type, uint8_t* payload, size_t length) {
    this->onWebSocketEvent(type, payload, length);
  });
  ws_.setReconnectInterval(2000);

  unsigned long start = millis();
  while (!ws_.isConnected() && millis() - start < 4000) {
    ws_.loop();
    delay(5);
  }
  if (!ws_.isConnected()) {
    log_error("[ASR_CHAT] connect timeout");
    return false;
  }

  start = millis();
  while (!ready_ && millis() - start < 3000) {
    ws_.loop();
    delay(5);
  }
  if (!ready_) {
    log_warn("[ASR_CHAT] no ready event, continue anyway");
  } else {
    log_info("[ASR_CHAT] ready received");
  }
  return true;
}

void AsrChatClient::loop() {
  auto flush_deferred_pb_stream_end = [this]() {
    if (!pb_deferred_stream_end_pending_) {
      return;
    }
    pb_deferred_stream_end_pending_ = false;
    audio_stream_pcm16_end(pb_deferred_stream_end_ch_);
  };
  flush_deferred_pb_stream_end();
  ws_.loop();
  flush_deferred_pb_stream_end();
  /* 舵机 async ramp 完成后投递的 pb_ack（不阻塞 motor_task / WS 回调）。 */
  {
    char reqb[48];
    uint32_t midx = 0;
    while (head_take_pb_motor_ack_done(reqb, sizeof(reqb), &midx)) {
      pbScheduleMotorAck(reqb, midx);
    }
  }
  /* pb_ack 必须在 RX 回调之外发送，否则部分 WebSockets 实现会在 sendTXT 时死锁，表现为 pb_start 后卡住。 */
  flushPendingPbAck();
  if (ws_.isConnected() && millis() - last_ping_ms_ > 15000) {
    sendJson("{\"type\":\"ping\"}");
    last_ping_ms_ = millis();
  }
  /* 空闲头部姿态步进：状态稳定时 if 判定即返回。 */
  if (!pb_active_) {
    updateAttentionDisplay();
  }
}

void AsrChatClient::updateAttentionDisplay() {
  unsigned long now = millis();
  /* 现协议下唤醒只看 tts_active_：服务端 tts_start ↔ pb 收尾窗口。 */
  const bool should_wake = tts_active_;

  if (should_wake) {
    last_should_wake_ms_ = now;
    if (display_state_ != DISPLAY_WAKEUP) {
      log_info("[ATTENTION] -> WAKEUP (tts=%d)", (int)tts_active_);
      display_state_ = DISPLAY_WAKEUP;
    }
    return;
  }

  /* !should_wake：开机首次（last_should_wake_ms_==0）直接进 sleep；
   * 之前 wake 过 → 距上次 should_wake ≥ 2s 才进 sleep（抖动抑制）。 */
  bool first_time = (last_should_wake_ms_ == 0) && (display_state_ == DISPLAY_UNINIT);
  bool dwell_done = (last_should_wake_ms_ != 0) &&
                    (now - last_should_wake_ms_ >= kIdleEnterDelayMs);
  if (display_state_ != DISPLAY_SLEEP && (first_time || dwell_done)) {
    log_info("[ATTENTION] -> SLEEP (dwell=%lums first=%d)",
             last_should_wake_ms_ == 0 ? 0UL : (now - last_should_wake_ms_),
             (int)first_time);
    /* 沉默动作（低头）：只动 Y、保持 X 在当前位置（避免突兀横扫）。 */
    int idle_y_target = constrain(Y_CENTER + kSleepHeadDownDeg, Y_MIN, Y_MAX);
    int y_now = head_y_pwm_to_logic(servo_y.read());
    int dy = idle_y_target - y_now;
    if (dy != 0) {
      head_move(0, dy);
    }
    display_state_ = DISPLAY_SLEEP;
  }
}

bool AsrChatClient::sendJson(const String& msg) {
  if (!ws_.isConnected()) {
    return false;
  }
  String payload = msg;
  if (!ws_.sendTXT(payload)) {
    /* write() 失败（如 errno 104 Connection reset）时尽快排空播放队列，避免短 PCM 在 I2S 里循环。 */
    audio_play_emergency_flush();
    return false;
  }
  return true;
}

bool AsrChatClient::shouldSuppressMicUplink() const {
  return tts_active_ || pb_audio_stream_started_ || pb_deferred_stream_end_pending_ ||
         audio_play_stream_pcm_active();
}

bool AsrChatClient::sendAudioJsonPcm16(const int16_t* pcm, size_t samples) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pcm);
  const size_t byte_len = samples * sizeof(int16_t);
  size_t b64_len = 0;
  mbedtls_base64_encode(nullptr, 0, &b64_len, bytes, byte_len);
  if (b64_len == 0) {
    return false;
  }

  unsigned char* b64 = new unsigned char[b64_len + 1];
  if (!b64) {
    return false;
  }
  size_t actual_len = 0;
  int rc = mbedtls_base64_encode(b64, b64_len + 1, &actual_len, bytes, byte_len);
  if (rc != 0) {
    delete[] b64;
    return false;
  }
  b64[actual_len] = '\0';

  String payload;
  payload.reserve(actual_len + 64);
  payload = "{\"type\":\"audio\",\"codec\":\"pcm16\",\"data\":\"";
  payload += reinterpret_cast<const char*>(b64);
  payload += "\"}";
  delete[] b64;
  return sendJson(payload);
}

bool AsrChatClient::runVoiceRound(uint16_t max_record_seconds) {
  if (!connect()) {
    return false;
  }

  if (max_record_seconds == 0 || max_record_seconds > 60) {
    max_record_seconds = 10;
  }

  round_id_++;
  disconnect_abort_round_ = false;
  reply_done_ = false;
  server_started_reply_ = false;
  /* 每轮开头清掉上一轮 pb/PCM 残余；但若 WS 仍在推本轮 TTS（pb_active），此处 pbReset 会停流并清
   * expect_bin → 后续 BIN 变「unexpected」、idx 全错位（见 CHAT 重叠发起 runVoiceRound 与下行并行）。
   * 仅在没有进行中 pb 序列时才硬清。 */
  if (!pb_active_ && !pb_expect_bin_) {
    pbReset(/*stop_audio=*/true);
  } else {
    log_warn("[ASR_CHAT] round=%u start: skip initial pbReset (pb_active=%d expect_bin=%d req=%s next_idx=%u)",
             (unsigned)round_id_, (int)pb_active_, (int)pb_expect_bin_, pb_req_.c_str(), (unsigned)pb_next_idx_);
  }
  last_ping_ms_ = millis();

  const size_t total_samples = static_cast<size_t>(max_record_seconds) * SAMPLE_RATE;
  size_t samples_recorded = 0;  /* 本轮 mic 采样总样点（含未发送的 pre-roll），用于决定何时结束录音 */
  size_t samples_sent = 0;       /* 真正上行到服务端的样点数，仅用于日志/统计 */
  int16_t frame[kFrameSamples20ms];
  static int16_t silence_pcm[kFrameSamples20ms]; /* 半双工播音窗口上行用，全零 */
  bool voice_seen = false;
  unsigned long silence_start = 0;
  const unsigned long silence_end_ms = 1200;

  /* 「整轮基本静音就不上传」门控 + pre-roll 预滚缓冲：
   * - 触发有声（mean > SOUND_THRESHOLD）之前的帧不上行，仅滚动缓存最近 ~kPreVoiceFrames 帧；
   * - 一旦触发，按时间顺序把缓存全部排队上行，紧接着本帧及后续帧也上行；
   * - 整轮始终未触发：跳过 flush，直接结束本轮（不打扰服务端 ASR/LLM）。
   * 每帧 320 采样×2B = 640B；30 帧 ≈ 600ms ≈ 19.2KB，分配在 PSRAM。 */
  constexpr size_t kPreVoiceFrames = 30;
  static int16_t* prebuf = nullptr;
  if (prebuf == nullptr) {
    const size_t bytes = kPreVoiceFrames * kFrameSamples20ms * sizeof(int16_t);
    prebuf = static_cast<int16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (prebuf == nullptr) {
      /* PSRAM 不可用时退到内部 RAM；仍失败就放弃 pre-roll，门控仍生效（首帧会丢点开头）。 */
      prebuf = static_cast<int16_t*>(malloc(bytes));
      if (prebuf == nullptr) {
        log_warn("[ASR_CHAT] pre-roll buffer alloc failed (%u bytes), no pre-roll", (unsigned)bytes);
      }
    }
  }
  size_t prebuf_head = 0;   /* 下一个要写入的 slot */
  size_t prebuf_count = 0;  /* 当前已缓存的帧数 (≤ kPreVoiceFrames) */

  log_info("[ASR_CHAT] round=%u start streaming up to %us @16k/mono/pcm16 (gated, pre-roll %ums)",
           (unsigned)round_id_, (unsigned)max_record_seconds,
           (unsigned)(prebuf ? kPreVoiceFrames * 20 : 0));
  /* 丢掉 Idle / 上一轮 tail 积压的帧，第一段上行从「现在开始」取样。*/
  mic_capture_flush_queue();
  while (samples_recorded < total_samples) {
    record(frame, kFrameSamples20ms);
    enhanceVoice(frame, kFrameSamples20ms);
    const bool duplex_suppress = shouldSuppressMicUplink();
    size_t mean = calculate_mean(frame, kFrameSamples20ms);
    const bool active = !duplex_suppress && (mean > SOUND_THRESHOLD);

    if (active) {
      if (!voice_seen) {
        /* 首次触发：先把 pre-roll 按时间顺序排队上行，再走「本帧」上行分支。 */
        if (prebuf != nullptr && prebuf_count > 0) {
          size_t idx = (prebuf_head + (kPreVoiceFrames - prebuf_count)) % kPreVoiceFrames;
          for (size_t i = 0; i < prebuf_count; ++i) {
            int16_t* fp = &prebuf[idx * kFrameSamples20ms];
            const int16_t* uplink = duplex_suppress ? silence_pcm : fp;
            if (!sendAudioJsonPcm16(uplink, kFrameSamples20ms)) {
              log_error("[ASR_CHAT] send pre-roll frame failed");
              return false;
            }
            samples_sent += kFrameSamples20ms;
            idx = (idx + 1) % kPreVoiceFrames;
          }
          log_info("[ASR_CHAT] round=%u voice trigger (mean=%u), flushed %u pre-roll frames",
                   (unsigned)round_id_, (unsigned)mean, (unsigned)prebuf_count);
          prebuf_count = 0;
          prebuf_head = 0;
        } else {
          log_info("[ASR_CHAT] round=%u voice trigger (mean=%u, no pre-roll)",
                   (unsigned)round_id_, (unsigned)mean);
        }
      }
      voice_seen = true;
      silence_start = 0;
    } else if (voice_seen) {
      if (silence_start == 0) {
        silence_start = millis();
      } else if (millis() - silence_start >= silence_end_ms) {
        break;
      }
    }

    if (voice_seen) {
      const int16_t* uplink = duplex_suppress ? silence_pcm : frame;
      if (!sendAudioJsonPcm16(uplink, kFrameSamples20ms)) {
        log_error("[ASR_CHAT] send audio frame failed");
        return false;
      }
      samples_sent += kFrameSamples20ms;
    } else if (prebuf != nullptr) {
      /* 还没触发：写入 pre-roll 环形缓冲（满则覆盖最旧）。 */
      int16_t* slot = &prebuf[prebuf_head * kFrameSamples20ms];
      if (duplex_suppress) {
        memset(slot, 0, kFrameSamples20ms * sizeof(int16_t));
      } else {
        memcpy(slot, frame, kFrameSamples20ms * sizeof(int16_t));
      }
      prebuf_head = (prebuf_head + 1) % kPreVoiceFrames;
      if (prebuf_count < kPreVoiceFrames) {
        ++prebuf_count;
      }
    }

    samples_recorded += kFrameSamples20ms;
    loop();
    if (server_started_reply_) {
      log_info("[ASR_CHAT] round=%u server started reply, stop uplink", (unsigned)round_id_);
      break;
    }
    delay(5);
  }

  if (!voice_seen && !server_started_reply_) {
    /* 整轮基本是静音：不发 audio JSON、不发 flush、不进等下行循环。
     * 清一下 pb 状态与 mic 队列，跟正常路径退出时保持一致。 */
    log_info("[ASR_CHAT] round=%u skipped (no voice detected over %ums, %u prebuf frames discarded)",
             (unsigned)round_id_,
             (unsigned)(samples_recorded * 1000UL / SAMPLE_RATE),
             (unsigned)prebuf_count);
    pbReset(/*stop_audio=*/true);
    audio_stream_pcm16_stop();
    mic_capture_flush_queue();
    return true;
  }

  if (!server_started_reply_) {
    if (!sendJson("{\"type\":\"flush\"}")) {
      log_error("[ASR_CHAT] send flush failed");
      return false;
    }
    log_info("[ASR_CHAT] round=%u flush sent (uploaded %u samples), mic uplink continues until reply done",
             (unsigned)round_id_, (unsigned)samples_sent);
  } else {
    log_info("[ASR_CHAT] round=%u skip flush (server already replying, uploaded %u samples)",
             (unsigned)round_id_, (unsigned)samples_sent);
  }

  unsigned long wait_start = millis();
  unsigned long last_wait_log_ms = wait_start;
  /* 与下行并行：flush 后仍按帧上行 PCM（保持时间线）；ASR 半双工——播音窗口内改发静音。
   * 退出条件：本轮 reply 收尾完成（reply_done_）且 pb 流不再活跃且 deferred end 已 flush。
   * 现协议不再依赖服务端 tts_end 事件——pb_end 最后一片 BIN 之后由 pbSignalTtsRoundComplete()
   * 置位 reply_done_。 */
  while ((!reply_done_ || pb_audio_stream_started_ || pb_deferred_stream_end_pending_) &&
         millis() - wait_start < 30000) {
    record(frame, kFrameSamples20ms);
    enhanceVoice(frame, kFrameSamples20ms);
    const int16_t* uplink = shouldSuppressMicUplink() ? silence_pcm : frame;
    if (!sendAudioJsonPcm16(uplink, kFrameSamples20ms)) {
      log_error("[ASR_CHAT] post-flush uplink send failed");
      return false;
    }
    loop();
    unsigned long now_w = millis();
    if (now_w - last_wait_log_ms >= 5000) {
      last_wait_log_ms = now_w;
      log_info("[ASR_CHAT] round=%u post-flush uplink... reply_done=%d pb_stream=%d defer_end=%d elapsed_ms=%lu ws_ok=%d",
               (unsigned)round_id_, (int)reply_done_, (int)pb_audio_stream_started_,
               (int)pb_deferred_stream_end_pending_, (unsigned long)(now_w - wait_start),
               (int)ws_.isConnected());
    }
    delay(5);
  }
  if (!reply_done_) {
    log_warn("[ASR_CHAT] round=%u reply wait timeout (reply_done=0)", (unsigned)round_id_);
    /* 30s 没等到下行结束：硬停 pb 流 + 清状态，避免 I2S/队列残留导致尾音循环；并结束本轮语义。 */
    tts_active_ = false;
    if (!pb_req_.isEmpty()) {
      pb_suppress_tail_req_ = pb_req_;
    }
    pbReset(/*stop_audio=*/true);
    audio_stream_pcm16_stop();
    pbSignalTtsRoundComplete();
    mic_capture_flush_queue();
  } else if (disconnect_abort_round_) {
    log_warn("[ASR_CHAT] round=%u reply phase ended (websocket disconnected)", (unsigned)round_id_);
    disconnect_abort_round_ = false;
  } else {
    log_info("[ASR_CHAT] round=%u tts completed", (unsigned)round_id_);
  }

  {
    unsigned long until = millis() + kPostTtsEchoGuardMs;
    while (millis() < until) {
      loop();
      delay(10);
    }
  }
  return true;
}

void AsrChatClient::onWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  /* 下行音频：仅 pb v1（JSON 声明 audio.next_bin 后的裸 PCM）。不再支持 WAV 整包 / 分片旧协议。 */
  if (type == WStype_CONNECTED) {
    log_info("[ASR_CHAT] connected");
    return;
  }
  if (type == WStype_DISCONNECTED) {
    log_warn("[ASR_CHAT] disconnected");
    audio_play_emergency_flush();
    ready_ = false;
    /* pb 流未 end / tts_end 未到时若不收尾会卡「等下行结束」环；断线即结束本轮并清 TTS 窗口。 */
    disconnect_abort_round_ = true;
    if (!pb_req_.isEmpty()) {
      pb_suppress_tail_req_ = pb_req_;
    }
    pbReset(/*stop_audio=*/true);
    pbSignalTtsRoundComplete();
    return;
  }
  if (type == WStype_TEXT) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, length)) {
      String raw((const char*)payload, length);
      log_info("[ASR_CHAT] text(raw): %s", raw.c_str());
      return;
    }
    String t = doc["type"].is<String>() ? doc["type"].as<String>() : String("");

    /* pb v1：优先处理 pb_*。 */
    if (t.startsWith("pb_")) {
      if (t == "pb_cancel") {
        pb_log_rx_json_doc(doc);
        String req = doc["req"].is<String>() ? doc["req"].as<String>() : String("");
        if (req.isEmpty() || (!pb_req_.isEmpty() && req == pb_req_)) {
          pbReset(/*stop_audio=*/true);
        }
        return;
      }
      pbParseAndStage(doc);
      return;
    }
    if (t == "ready") {
      ready_ = true;
      log_info("[ASR_CHAT] event ready");
    } else if (t == "asr_start") {
      server_started_reply_ = true;
      log_info("[ASR_CHAT] event asr_start");
    } else if (t == "asr_text" || t == "llm_text") {
      server_started_reply_ = true;
      String text = doc["text"].is<String>() ? doc["text"].as<String>() : String("");
      log_info("[ASR_CHAT] %s: %s", t.c_str(), text.c_str());
      /* llm_text.actions（v2 协议新增）：服务端把已解析、去重、去空白的动作数组直接放在
       * 这里，与后续独立的 actions 事件内容一致。我们的实际执行只走 actions 事件分支
       * （见下方），这里仅日志预览，便于在 actions 事件被服务端漏发或 json_ok=false 兜
       * 底空数组时，从 llm_text 也能看到 LLM 实际意图，方便排查。 */
      if (t == "llm_text") {
        bool json_ok = doc["json_ok"].is<bool>() ? doc["json_ok"].as<bool>() : true;
        JsonArrayConst arr = doc["actions"].as<JsonArrayConst>();
        size_t n = arr.isNull() ? 0 : arr.size();
        if (n > 0) {
          String preview;
          preview.reserve(n * 16);
          for (size_t i = 0; i < n; ++i) {
            if (i) preview += ',';
            preview += arr[i].as<String>();
          }
          log_info("[ASR_CHAT] llm_text actions(preview) json_ok=%d count=%u: [%s]",
                   (int)json_ok, (unsigned)n, preview.c_str());
        } else if (!json_ok) {
          log_warn("[ASR_CHAT] llm_text json_ok=false (LLM 未返回约定 JSON，actions 为空)");
        }
      }
    } else if (t == "actions") {
      /* actions：直接在回调栈内 executeCommand（不重入 ws_.loop）。未识别动作由 cmd 内 log_warn。 */
      JsonArrayConst arr = doc["actions"].as<JsonArrayConst>();
      size_t n = arr.isNull() ? 0 : arr.size();
      String request_id = doc["request_id"].is<String>() ? doc["request_id"].as<String>() : String("");
      if (n == 0) {
        /* 协议规定空数组不下发本事件；真到了说明服务端口径变了或字段缺失，记一行 warn。 */
        log_warn("[ASR_CHAT] actions event with empty array (req=%s)", request_id.c_str());
      } else {
        log_info("[ASR_CHAT] actions event count=%u req=%s", (unsigned)n, request_id.c_str());
        for (size_t i = 0; i < n; ++i) {
          String action_name = arr[i].as<String>();
          action_name.trim();
          if (action_name.isEmpty()) {
            continue;
          }
          log_info("[ASR_CHAT] action[%u/%u] -> %s", (unsigned)(i + 1), (unsigned)n,
                   action_name.c_str());
          executeCommand(action_name);
        }
      }
    } else if (t == "tts_start") {
      server_started_reply_ = true;
      tts_active_ = true;
      String text = doc["text"].is<String>() ? doc["text"].as<String>() : String("");
      log_info("[ASR_CHAT] tts_start: %s", text.c_str());
    } else if (t == "tts_end") {
      /* 兼容旧协议：现协议不再依赖该事件（收尾由 pb_end 最后一片 BIN 驱动）。
       * 仅作兜底——若服务端仍发 tts_end，我们关闭还在 opened 的 pb 流并复用 pb 收尾路径。 */
      if (pb_audio_stream_started_) {
        const uint8_t ch_end = (pb_ch_ == 0 || pb_ch_ > 2) ? 1 : pb_ch_;
        audio_stream_pcm16_end(ch_end);
        pb_audio_stream_started_ = false;
      }
      pbSignalTtsRoundComplete();
      log_info("[ASR_CHAT] tts_end (legacy)");
    } else if (t == "face_info") {
      /* 协议已废弃，机器人不再做 face tracking；忽略整个事件。 */
    } else if (t == "error" || t == "tts_error" || t == "asr_rejected") {
      String msg = doc["message"].is<String>() ? doc["message"].as<String>() : String("");
      String detail = doc["detail"].is<String>() ? doc["detail"].as<String>() : String("");
      String text = doc["text"].is<String>() ? doc["text"].as<String>() : String("");
      if (t == "asr_rejected") {
        log_warn("[ASR_CHAT] %s message=%s detail=%s text=%s", t.c_str(), msg.c_str(), detail.c_str(),
                 text.c_str());
      } else {
        log_error("[ASR_CHAT] %s message=%s detail=%s text=%s", t.c_str(), msg.c_str(), detail.c_str(),
                  text.c_str());
      }
      if (!pb_req_.isEmpty()) {
        pb_suppress_tail_req_ = pb_req_;
      }
      pbReset(/*stop_audio=*/true);
      pbSignalTtsRoundComplete();
    } else if (t == "pong") {
      // no-op
    } else {
      String raw((const char*)payload, length);
      log_info("[ASR_CHAT] text: %s", raw.c_str());
    }
    return;
  }
  if (type == WStype_BIN) {
    if (pb_active_ && pb_expect_bin_) {
      if ((length & 1u) != 0u || length == 0u) {
        pbProtocolError("binary length not even/empty");
        return;
      }
      /* 允许小幅长度误差：服务端 chunk_ms 是“对齐语义”，但 PCM bytes 可能因取整/裁剪
       * 在边界处差 1–数个 samples（你给的日志里 118ms@24k 公式应 5664B，实际 5650/5652B）。
       * - 若误差在 3ms 内：warn 后继续。
       * - 若误差巨大（>50%）：认为错位，按协议错位处理。 */
      if (pb_expect_bin_len_ > 0) {
        size_t expected = pb_expect_bin_len_;
        size_t tol = (size_t)((uint64_t)pb_sr_ * (uint64_t)pb_ch_ * 2ULL * 3ULL / 1000ULL) + 8u;  // ~3ms + 常数
        size_t lo = (expected > tol) ? (expected - tol) : 0u;
        size_t hi = expected + tol;
        if (length < lo || length > hi) {
          if (length < expected / 2u || length > expected * 2u) {
            pbProtocolError("binary length wildly mismatched");
            return;
          }
          log_warn("[PB] binary len=%u not near expected=%u (tol=%u) req=%s idx=%u chunk_ms=%u",
                   (unsigned)length, (unsigned)expected, (unsigned)tol, pb_req_.c_str(),
                   (unsigned)pb_pending_idx_, (unsigned)pb_pending_chunk_ms_);
        }
      }
      if (pb_sr_ == 0 || pb_ch_ == 0 || pb_fmt_ != "s16le") {
        pbProtocolError("binary without valid audio params");
        return;
      }

      /* 快照：push 可能阻塞并 yield；紧随 pb_end / pb_single(JSON+next_bin) 的 BIN 在协议上唯一，只信
       * pb_end_waiting_bin_，避免 pb_end_idx_ 与 pb_pending_idx_ 偶发不一致导致永不收尾
       *（runVoiceRound 卡在 reply_done=0 且 pb_stream=1）。 */
      const bool closing_pb_end_bin = pb_end_waiting_bin_;
      const uint32_t snap_pb_end_idx = pb_end_idx_;
      const uint8_t ch_for_stream_end = pb_ch_;

      log_info("[PB] rx BIN req=%s idx=%u end_idx=%u len=%u expect=%u samples=%u stream_on=%d closing_end=%d",
               pb_req_.c_str(), (unsigned)pb_pending_idx_, (unsigned)snap_pb_end_idx, (unsigned)length,
               (unsigned)pb_expect_bin_len_, (unsigned)(length / 2), (int)pb_audio_stream_started_,
               (int)closing_pb_end_bin);

      const bool had_servo_bin = pb_pending_has_servo_;
      const uint32_t pending_idx_snap = pb_pending_idx_;
      const uint32_t chunk_ms_after_bin = pb_pending_chunk_ms_;
      const PbEnqueueAction bin_act = pb_pending_enqueue_action_;
      const bool skip_audio =
          (bin_act == PbEnqueueAction::kOpportunistic) && pb_opportunistic_audio_backpressure_busy();

      uint8_t* pcm_bytes = (uint8_t*)heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      uint32_t free_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
      if (!pcm_bytes) {
        pcm_bytes = (uint8_t*)heap_caps_malloc(length, MALLOC_CAP_DEFAULT);
        free_caps = MALLOC_CAP_DEFAULT;
      }
      if (!pcm_bytes) {
        pbProtocolError("pcm alloc failed");
        return;
      }
      memcpy(pcm_bytes, payload, length);
      const size_t samples = length / 2;

      if (!skip_audio) {
        if (!pb_audio_stream_started_) {
          if (!audio_stream_pcm16_begin(pb_sr_, pb_ch_, /*volume_ratio=*/0.3f)) {
            heap_caps_free(pcm_bytes);
            pbProtocolError("audio stream begin failed");
            return;
          }
          pb_audio_stream_started_ = true;
          pb_last_buf_decay_ms_ = millis();
          pb_audio_buf_ms_est_ = 0;
        }
        if (!audio_stream_pcm16_push_owned((int16_t*)pcm_bytes, samples, free_caps, /*volume_ratio=*/0.3f)) {
          heap_caps_free(pcm_bytes);
          pbProtocolError("pcm push failed");
          return;
        }
        pb_audio_buf_ms_est_ += (int32_t)pb_pending_chunk_ms_;
      } else {
        heap_caps_free(pcm_bytes);
      }

      pb_expect_bin_ = false;
      pb_expect_bin_len_ = 0;

      const bool servo_enqueued = pbDispatchChunkPreamble(chunk_ms_after_bin, pending_idx_snap, bin_act);
      /* 无舵机、或舵机未入队（noop/opportunistic 丢弃）：在此 ack；舵机已入队由 ramp 完成后再 ack。 */
      if (!had_servo_bin || !servo_enqueued) {
        pbMaybeAck(pending_idx_snap);
        flushPendingPbAck();
      }

      pb_pending_chunk_ms_ = 0;
      pb_pending_anim_json_.remove(0);
      pb_pending_has_servo_ = false;

      /* pb_end / pb_single 最后一片：preamble（OLED/舵机）后再关流并退出 pb（快照防 yield 重入）。 */
      if (closing_pb_end_bin) {
        const bool stream_alive = pb_audio_stream_started_;
        pb_deferred_stream_end_ch_ =
            (ch_for_stream_end == 0 || ch_for_stream_end > 2) ? 1 : ch_for_stream_end;
        pb_deferred_stream_end_pending_ = stream_alive;
        pb_audio_stream_started_ = false;
        pb_end_waiting_bin_ = false;
        pb_end_idx_ = 0;
        pb_active_ = false;
        log_info("[PB] complete end_bin req=%s pending_idx=%u end_idx=%u (defer pcm16_end to loop)",
                 pb_req_.c_str(), (unsigned)pb_pending_idx_, (unsigned)snap_pb_end_idx);
        pbSignalTtsRoundComplete();
        pb_ack_bypass_throttle_ = true;
        flushPendingPbAck();
      }
      return;
    }

    if (pb_active_) {
      pbProtocolError("unexpected BIN (expect JSON per pb v1)");
    } else if (!pb_suppress_tail_req_.isEmpty()) {
      /* 与 asr_rejected/断线 后丢弃的 JSON 尾帧对应的 PCM，静默吞掉避免刷屏。 */
    } else {
      log_warn("[ASR_CHAT] drop unexpected BIN len=%u", (unsigned)length);
    }
    return;
  }
  if (type == WStype_FRAGMENT_BIN_START || type == WStype_FRAGMENT || type == WStype_FRAGMENT_FIN) {
    log_warn("[ASR_CHAT] drop WebSocket FRAGMENT (unsupported; use pb v1 single-frame BIN) type=%d len=%u",
             (int)type, (unsigned)length);
    return;
  }
}
