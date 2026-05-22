#ifndef ASR_CHAT_CLIENT_H
#define ASR_CHAT_CLIENT_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include "audio_player.h"
#include "common.h"

#ifndef ASR_CHAT_HOST
#define ASR_CHAT_HOST ""
#endif
#ifndef ASR_CHAT_PORT
#define ASR_CHAT_PORT 9000
#endif

class AsrChatClient {
public:
  AsrChatClient();

  bool connect();
  void loop();
  bool runVoiceRound(uint16_t max_record_seconds = 10);

  /* 空闲时头部姿态：TTS 结束后延迟低头；TTS 期间不重复触发。 */
  void updateAttentionDisplay();

private:
  static constexpr const char* kHost = ASR_CHAT_HOST;
  static constexpr uint16_t kPort = ASR_CHAT_PORT;
  static constexpr size_t kFrameSamples20ms = 320;  // 16kHz * 0.02s

  WebSocketsClient ws_;
  bool ready_ = false;
  /* 本轮下行收尾标志：pbSignalTtsRoundComplete() 触发，等价于「pb 序列已完整结束」。
   * 现协议不再依赖服务端 tts_end 事件（tts_end 多数情况下不会到），所以名字也不带 tts_。 */
  bool reply_done_ = false;
  bool server_started_reply_ = false;
  /* tts_active_：服务端 tts_start 置 true、收完 pb_end / pbSignalTtsRoundComplete 置 false。
   * 仅给 attention display 的 should_wake 判断用，覆盖整段「下行播音」窗口。 */
  bool tts_active_ = false;
  unsigned long last_ping_ms_ = 0;
  uint32_t round_id_ = 0;
  /* WebSocket 断在本轮内时置位，用于结束等待并区分日志（避免打成「tts completed」）。 */
  bool disconnect_abort_round_ = false;

  /* -----------------------------------------------------------------------
   * pb v1 下行播放序列（JSON + 紧随 binary PCM）：
   * - 维护当前 req、idx、以及“下一帧必须是 binary”的期望。
   * - 音频使用 audio_stream_pcm16_begin/push/end，占用 pipeline，支持 sr/ch。
   * - pb_ack 以“已入队(idx)”语义上报，audio_buf_ms 做回压参考；并带舵机当前读角与 X/Y 软件行程。
   * - action：replace（默认打断）/ append（同 req 不打断）/ opportunistic（OLED·舵机·音频三队列均空且
   *   无在途流式尾音时：跨 req 无 BIN pb_single 可走侧信道；将新开 req 时可 drain+reset；任一 worker
   *   忙则整条 opportunistic 忽略。BIN 侧 opportunistic 在三队列任一非空时跳过 PCM 入队）。
   * ----------------------------------------------------------------------- */
  enum class PbEnqueueAction : uint8_t { kReplace = 0, kAppend = 1, kOpportunistic = 2 };
  static PbEnqueueAction parsePbEnqueueAction(const JsonDocument& doc);
  PbEnqueueAction pb_pending_enqueue_action_ = PbEnqueueAction::kReplace;

  bool pb_active_ = false;
  String pb_req_;
  /* asr_rejected / error / 断线后服务端仍可能送达同一 req 的 pb_chunk+BIN；若已 pbReset 则 next_idx
   * 归零，会误触发 idx not monotonic。记录被中止的 req：丢弃尾帧，直至同 req 的 pb_start|pb_single
   * idx==0（合法新流）或收到不同 req。 */
  String pb_suppress_tail_req_;
  uint32_t pb_next_idx_ = 0;
  bool pb_expect_bin_ = false;
  size_t pb_expect_bin_len_ = 0;
  uint32_t pb_sr_ = 0;
  uint8_t pb_ch_ = 0;
  String pb_fmt_;
  uint32_t pb_pending_idx_ = 0;
  uint32_t pb_pending_chunk_ms_ = 0;
  String pb_pending_anim_json_;
  bool pb_pending_has_servo_ = false;
  int pb_servo_xm_ = 2;
  int pb_servo_ym_ = 2;
  int pb_servo_x_ = 0;
  int pb_servo_y_ = 0;
  uint16_t pb_servo_ms_ = 0;
  bool pb_audio_stream_started_ = false;
  unsigned long pb_last_buf_decay_ms_ = 0;
  int32_t pb_audio_buf_ms_est_ = 0;
  uint32_t pb_last_ack_idx_ = 0;
  bool pb_end_waiting_bin_ = false;
  uint32_t pb_end_idx_ = 0;
  /* pb_end 最后一包若在 WS 回调里调 audio_stream_pcm16_end 会长时间占住回调线程，主循环仍
   * 在跑 → runVoiceRound 看到 reply_done=0 且 pb_stream=1。将 end 推迟到 loop() 主上下文执行。 */
  bool pb_deferred_stream_end_pending_ = false;
  uint8_t pb_deferred_stream_end_ch_ = 1;

  /* pb_ack 不得在 WebSocket onEvent 回调里 sendTXT（部分库会死锁），只入队到 loop() 再发。 */
  bool pb_ack_out_pending_ = false;
  String pb_ack_out_req_;
  uint32_t pb_ack_out_idx_ = 0;
  int32_t pb_ack_out_buf_ms_ = 0;
  /** 纯音频 pb_ack 节流（ms）；舵机完成 ack 走 pb_ack_bypass_throttle_ 立即发。 */
  unsigned long pb_last_pb_ack_sent_wall_ms_ = 0;
  bool pb_ack_bypass_throttle_ = false;

  /* 空闲头部姿态：updateAttentionDisplay() 状态机。
   *   UNINIT：开机后未驱动过；首次进入若 should_wake=false 立刻低头（不等 2s）。
   *   WAKEUP：tts_active_ 为真。
   *   SLEEP：持续 ≥2s 无 TTS 时低头到 Y_CENTER+kSleepHeadDownDeg。 */
  enum DisplayState : uint8_t {
    DISPLAY_UNINIT = 0,
    DISPLAY_WAKEUP = 1,
    DISPLAY_SLEEP = 2,
  };
  DisplayState display_state_ = DISPLAY_UNINIT;
  /* 上一次 should_wake=true 的时间戳。!should_wake 时拿这个判 dwell ≥ 2s 才切 sleep。
   * 0 = 自上电以来从未 wake 过 → 首次进 sleep 不等 2s（开机直接 sleep 是合理初值）。 */
  unsigned long last_should_wake_ms_ = 0;
  static constexpr unsigned long kIdleEnterDelayMs = 2000;
  /* 沉默低头幅度（相对 Y_CENTER）：30° 表达"放空"姿态又不顶到 Y_MAX 极限。 */
  static constexpr int kSleepHeadDownDeg = 30;

  void onWebSocketEvent(WStype_t type, uint8_t* payload, size_t length);
  bool sendAudioJsonPcm16(const int16_t* pcm, size_t samples);
  bool sendJson(const String& msg);

  /* ASR 半双工：下行播音窗口内上行改发静音（仍 record 排空 mic 队列）。 */
  bool shouldSuppressMicUplink() const;

  void pbReset(bool stop_audio);
  void pbProtocolError(const char* why);
  bool pbApplyServoIfAny(uint32_t chunk_ms, uint32_t chunk_idx, bool defer_ack_if_enqueued, PbEnqueueAction action,
                         const char* defer_ack_req_cstr = nullptr);
  void pbSubmitAnimIfAny(uint32_t chunk_ms, PbEnqueueAction action);
  void pbUpdateAudioBufDecayWall();
  void pbScheduleMotorAck(const char* req_cstr, uint32_t idx);
  void pbFlushAckForForeignReq(const String& req, uint32_t idx);
  void pbMaybeAck(uint32_t idx);
  void flushPendingPbAck();
  /* pb 序列播完（或仅动画无流）时置本轮下行完成，解除 runVoiceRound 等待。 */
  void pbSignalTtsRoundComplete();
  bool pbParseAndStage(const JsonDocument& doc);
  /* next_bin：纯音频时节流 pb_ack；含舵机时 ack 在 motor_task ramp 结束后由 loop 取队列发出。 */
  bool pbDispatchChunkPreamble(uint32_t chunk_ms, uint32_t chunk_idx, PbEnqueueAction action);
};

#endif
