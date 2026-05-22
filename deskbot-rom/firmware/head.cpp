#include "head.h"

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

int X_CENTER = 90;
int Y_CENTER = 90;
int X_MIN = X_MIN_LIMIT;
int X_MAX = X_MAX_LIMIT;
int Y_MIN = Y_MIN_LIMIT;
int Y_MAX = Y_MAX_LIMIT;

Servo servo_x;
Servo servo_y;

/* ---------------------------------------------------------------
 * Motor task：把舵机斜坡推进搬到独立 FreeRTOS 任务里。
 *
 * 目的：让"播放音频 / 录音 / 刷动画"在等待头部动作完成期间不再被 delay()
 * 占满 CPU。head_move 等公开接口仍然是"同步语义"（返回时动作已完成），
 * 等待是 binary semaphore，调度器会把 CPU 让给音频等高优先级 task。
 *
 * 与 asr_chat_client.cpp 的 face tracking（writeMicroseconds 高频写入）
 * 的关系不变：两条路径仍然可能并发写舵机；约定 face tracking 在 head_*
 * 命令期间退让，命令结束后通过 servo.read() 重同步。
 * ------------------------------------------------------------- */

struct HeadPbMotorAckMsg {
  char req[40];
  uint32_t idx;
};
static QueueHandle_t s_pb_motor_ack_q = nullptr;

namespace {

/** 点头/摇头段之间的停顿（ms），放在 motor_task 的 hold_ms 里用 vTaskDelay，避免 delay() 占满调度。 */
constexpr uint16_t k_head_gesture_hold_ms = 15;

/** 斜坡每步节拍：与常见舵机 50Hz PWM 周期对齐，用 vTaskDelayUntil 固定 20ms。 */
static const TickType_t k_motor_ramp_period_ticks = pdMS_TO_TICKS(20);

/** 与下行 JSON `servo` 同形（另含斜坡/同步字段）。xm/ym 见 head.h HEAD_SERVO_*。 */
struct MotorCmd {
  uint8_t xm;
  uint8_t ym;
  int x;
  int y;
  /** 非 0：本段墙钟总预算（ms），斜坡尾 <20ms 不再 write；此时忽略 hold_ms。 */
  uint16_t ms;
  uint16_t hold_ms;
  uint8_t step_deg;
  uint16_t tick_ms;
  SemaphoreHandle_t notify_sem;
  /** async 且非 nullptr：本 ramp 结束后投递一条 pb_ack（req/idx 入队供主循环取）。 */
  bool pb_ack_after_done;
  uint32_t pb_ack_idx;
  char pb_ack_req[40];
};

QueueHandle_t     s_motor_queue       = nullptr;
TaskHandle_t      s_motor_task        = nullptr;
/* 同步等待用专属 binary semaphore，避免和其它模块共享的 task notification
   slot 0 互相吞通知。多 caller 并发提交 sync 命令时用 caller_mutex 串行化，
   保证 sem 与"对应那一条命令"一一对应。 */
SemaphoreHandle_t s_motor_done_sem    = nullptr;
SemaphoreHandle_t s_motor_caller_lock = nullptr;

void motor_task(void* /*arg*/) {
  MotorCmd cmd{};
  for (;;) {
    if (xQueueReceive(s_motor_queue, &cmd, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    int x = servo_x.read();
    int y = head_y_pwm_to_logic(servo_y.read());
    int x_target = x;
    int y_target = y;
    if (cmd.xm == HEAD_SERVO_HOLD) {
      /* x_target 保持为读数 */
    } else if (cmd.xm == HEAD_SERVO_ABS) {
      x_target = constrain(cmd.x, X_MIN, X_MAX);
    } else if (cmd.xm == HEAD_SERVO_REL) {
      x_target = constrain(x + cmd.x, X_MIN, X_MAX);
    } else {
      /* 非法值：不驱动 X */
    }

    if (cmd.ym == HEAD_SERVO_HOLD) {
    } else if (cmd.ym == HEAD_SERVO_ABS) {
      y_target = constrain(cmd.y, Y_MIN, Y_MAX);
    } else if (cmd.ym == HEAD_SERVO_REL) {
      y_target = constrain(y + cmd.y, Y_MIN, Y_MAX);
    } else {
    }

    const int step = (cmd.step_deg > 0) ? cmd.step_deg : 1;

    /* 步进收敛：每 20ms 一拍，走 min(step, |diff|)，避免在目标 ±step 区间反复震荡。
     * 不要在 while 内每圈 read() 覆盖 x/y：read 滞后/量化会导致永远达不到 x_target，
     * motor_task 死循环，后续队列命令全部卡住。斜坡状态用进入循环前的 read + 本循环内累加即可。 */
    const uint32_t t0_ms = (cmd.ms > 0) ? millis() : 0;
    TickType_t last_wake = xTaskGetTickCount();
    while (x != x_target || y != y_target) {
      if (cmd.ms > 0) {
        const uint32_t elapsed_ms = millis() - t0_ms;
        if (elapsed_ms >= cmd.ms) {
          break;
        }
        const uint32_t rem_ms = cmd.ms - elapsed_ms;
        /* 不足一整拍 20ms 时不再改 PWM，直接睡到预算结束，避免尾段再挤一拍 vTaskDelayUntil(20) 拖出 chunk。 */
        if (rem_ms < 20) {
          if (rem_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(rem_ms));
          }
          break;
        }
      }
      if (x != x_target) {
        int dx = x_target - x;
        int sx = (dx >  step) ?  step
               : (dx < -step) ? -step
               :                 dx;
        x += sx;
        servo_x.write(x);
      }
      if (y != y_target) {
        int dy = y_target - y;
        int sy = (dy >  step) ?  step
               : (dy < -step) ? -step
               :                 dy;
        y += sy;
        servo_y.write(head_y_logic_to_pwm(y));
      }
      vTaskDelayUntil(&last_wake, k_motor_ramp_period_ticks);
    }

    if (cmd.ms > 0) {
      const uint32_t elapsed_ms = millis() - t0_ms;
      if (elapsed_ms < cmd.ms) {
        vTaskDelay(pdMS_TO_TICKS(cmd.ms - elapsed_ms));
      }
    } else if (cmd.hold_ms > 0) {
      vTaskDelay(pdMS_TO_TICKS(cmd.hold_ms));
    }

    if (cmd.notify_sem) {
      xSemaphoreGive(cmd.notify_sem);
    } else if (cmd.pb_ack_after_done && cmd.pb_ack_req[0] != '\0' && s_pb_motor_ack_q) {
      HeadPbMotorAckMsg out{};
      strncpy(out.req, cmd.pb_ack_req, sizeof(out.req) - 1);
      out.req[sizeof(out.req) - 1] = '\0';
      out.idx = cmd.pb_ack_idx;
      (void)xQueueSend(s_pb_motor_ack_q, &out, 0);
    }
  }
}

/** 丢弃 FreeRTOS 队列里尚未被 motor_task 取走的命令；对每条带 notify_sem 的 cmd give，避免 sync 调用方永久阻塞。 */
void drain_motor_queue_nonblocking() {
  if (!s_motor_queue) {
    return;
  }
  MotorCmd dropped{};
  while (xQueueReceive(s_motor_queue, &dropped, 0) == pdTRUE) {
    if (dropped.notify_sem) {
      xSemaphoreGive(dropped.notify_sem);
    }
    /* 丢弃的 async 舵机不投递 pb_ack（序列已打断）。 */
  }
}

void ensure_motor_task() {
  if (!s_pb_motor_ack_q) {
    s_pb_motor_ack_q = xQueueCreate(8, sizeof(HeadPbMotorAckMsg));
  }
  if (s_motor_queue && s_motor_task && s_motor_done_sem && s_motor_caller_lock) {
    return;
  }
  if (!s_motor_queue) {
    /* 队列容量 32：与音频/OLED 队列对齐，pb 路径 async 入队后由 motor_task 串行
     * 在 chunk_ms 内执行 ramp+hold；稳态每帧入队 1 出队 1，32 槽位足以吸收瞬时抖动。 */
    s_motor_queue = xQueueCreate(32, sizeof(MotorCmd));
  }
  if (!s_motor_done_sem) {
    s_motor_done_sem = xSemaphoreCreateBinary();
  }
  if (!s_motor_caller_lock) {
    s_motor_caller_lock = xSemaphoreCreateMutex();
  }
  if (!s_motor_task) {
    /* core 1 (APP_CPU) 跑业务任务；优先级 3，比 act/anim 等中低任务高，
       比音频任务（5）低，确保音频 i2s_write 永远优先。 */
    xTaskCreatePinnedToCore(motor_task, "motor", 3072, nullptr, 3, &s_motor_task, APP_CPU_NUM);
  }
}

/* 提交一条 MotorCmd；sync=true 时阻塞等待执行完成。
 *
 * 同步等待用专属 binary semaphore（被 caller_mutex 序列化保护），主任务
 * 挂起期间 FreeRTOS 会调度其它 task（音频/网络/动画），所以"等待头动完成"
 * 不再吃 CPU。
 *
 * 公开接口多以 sync=true 调用：返回时舵机已到目标位置；下一次相对移动
 * 用 servo.read() 计算增量仍然准确。 */
void submit_motor(uint8_t xm, int x, uint8_t ym, int y, uint16_t hold_ms, uint8_t step_deg, uint16_t tick_ms, uint16_t ms,
                    bool sync) {
  ensure_motor_task();

  MotorCmd cmd{};
  cmd.xm       = xm;
  cmd.ym       = ym;
  cmd.x        = x;
  cmd.y        = y;
  cmd.ms       = ms;
  cmd.hold_ms  = hold_ms;
  cmd.step_deg = step_deg;
  cmd.tick_ms  = tick_ms;
  cmd.pb_ack_after_done = false;
  cmd.pb_ack_idx        = 0;
  cmd.pb_ack_req[0]     = '\0';

  if (sync) {
    /* 串行化所有 sync caller，避免两个 caller 同时使用 done_sem 错乱。
       async 路径走 else 分支，不会争抢这把锁。 */
    xSemaphoreTake(s_motor_caller_lock, portMAX_DELAY);
    /* 清空可能残留的旧 give（理论上不该有，防御性处理）。 */
    xSemaphoreTake(s_motor_done_sem, 0);
    cmd.notify_sem = s_motor_done_sem;
    xQueueSend(s_motor_queue, &cmd, portMAX_DELAY);
    xSemaphoreTake(s_motor_done_sem, portMAX_DELAY);
    xSemaphoreGive(s_motor_caller_lock);
  } else {
    cmd.notify_sem = nullptr;
    xQueueSend(s_motor_queue, &cmd, portMAX_DELAY);
  }
}

void _head_servo_cmd_async_impl(uint8_t xm, uint8_t ym, int x, int y, uint8_t step_deg, uint16_t ms,
                                bool pb_ack_after_done, uint32_t pb_ack_idx, const char* pb_ack_req) {
  ensure_motor_task();
  MotorCmd cmd{};
  cmd.xm       = xm;
  cmd.ym       = ym;
  cmd.x        = x;
  cmd.y        = y;
  cmd.ms       = ms;
  cmd.hold_ms  = 0;
  cmd.step_deg = step_deg;
  cmd.tick_ms  = 0;
  cmd.notify_sem = nullptr;
  if (pb_ack_after_done && pb_ack_req && pb_ack_req[0] != '\0') {
    cmd.pb_ack_after_done = true;
    cmd.pb_ack_idx        = pb_ack_idx;
    strncpy(cmd.pb_ack_req, pb_ack_req, sizeof(cmd.pb_ack_req) - 1);
    cmd.pb_ack_req[sizeof(cmd.pb_ack_req) - 1] = '\0';
  } else {
    cmd.pb_ack_after_done = false;
    cmd.pb_ack_idx        = 0;
    cmd.pb_ack_req[0]     = '\0';
  }
  xQueueSend(s_motor_queue, &cmd, portMAX_DELAY);
}

}  // namespace

void head_servo_cmd_async(uint8_t xm, uint8_t ym, int x, int y, uint8_t step_deg, uint16_t ms,
                          bool pb_ack_after_done, uint32_t pb_ack_idx, const char* pb_ack_req) {
  _head_servo_cmd_async_impl(xm, ym, x, y, step_deg, ms, pb_ack_after_done, pb_ack_idx, pb_ack_req);
}

void head_preset(HeadPreset preset) {
  switch (preset) {
    case HeadPreset::Center:
      head_center();
      break;
    case HeadPreset::TurnLeft:
      head_left(20);
      break;
    case HeadPreset::TurnRight:
      head_right(20);
      break;
    case HeadPreset::LookUp:
      head_up(20);
      break;
    case HeadPreset::LookDown:
      head_down(20);
      break;
    case HeadPreset::Nod:
      head_nod();
      break;
    case HeadPreset::Shake:
      head_shake();
      break;
    case HeadPreset::CircleCW:
      head_roll_right();
      break;
    case HeadPreset::CircleCCW:
      head_roll_left();
      break;
    default:
      break;
  }
}

void servo_init() {
  /* 提升 LEDC PWM 分辨率：默认 10 bit @ 50Hz → 1 tick ≈ 19.5us ≈ 1.85°，
   * 1° / 2° 步进经常被量化吃掉，舵机表现为"咔咔咔"跳。改 14 bit 后
   * 1 tick ≈ 1.22us ≈ 0.117°，足够支持 0.5° 亚度级流畅控制。
   * 必须在 attach() 之前调用（库注释明确要求）。head_move 等整数 angle
   * 调用不受影响——分辨率更高只会让 read() 更准、退出条件更稳。 */
  servo_x.setTimerWidth(14);
  servo_y.setTimerWidth(14);
  servo_x.attach(X_PIN);
  servo_y.attach(Y_PIN);
  servo_x.write(X_CENTER);
  servo_y.write(head_y_logic_to_pwm(Y_CENTER));

  ensure_motor_task();
}

void setup_head() {
  update_x_center();
  update_y_center();
  servo_init();
  head_center();
  delay(500);
  log_info("Head is Ready.");
}

void adjust_x_center(int offset) {
  File file = FFat.open(X_CENTER_FILE, FILE_WRITE);
  if (file) {
    X_CENTER = constrain(X_CENTER + offset, X_MIN_LIMIT, X_MAX_LIMIT);
    file.print(String(X_CENTER));
    file.close();
    log_info("[Factory] Adjust X_CENTER to %d", X_CENTER);
  } else {
    log_error("Could not write file %s", X_CENTER_FILE);
  }
}

void adjust_y_center(int offset) {
  File file = FFat.open(Y_CENTER_FILE, FILE_WRITE);
  if (file) {
    Y_CENTER = constrain(Y_CENTER + offset, Y_MIN_LIMIT, Y_MAX_LIMIT);
    file.print(String(Y_CENTER));
    file.close();
    log_info("[Factory] Adjust Y_CENTER to %d", Y_CENTER);
  } else {
    log_error("Could not write file %s", Y_CENTER_FILE);
  }
}

void update_x_center() {
  File file = FFat.open(X_CENTER_FILE, FILE_READ);
  if (file) {
    String valueStr = file.readString();
    file.close();
    X_CENTER = constrain(valueStr.toInt(), X_MIN_LIMIT, X_MAX_LIMIT);
  } else {
    adjust_x_center(0);
  }
  log_debug("X center angle: %d", X_CENTER);
}

void update_y_center() {
  File file = FFat.open(Y_CENTER_FILE, FILE_READ);
  if (file) {
    String valueStr = file.readString();
    file.close();
    Y_CENTER = constrain(valueStr.toInt(), Y_MIN_LIMIT, Y_MAX_LIMIT);
  } else {
    adjust_y_center(0);
  }
  log_debug("Y center angle: %d", Y_CENTER);
}

void head_move(int x_offset, int y_offset, int servo_delay) {
  (void)servo_delay;
  submit_motor(HEAD_SERVO_REL, x_offset, HEAD_SERVO_REL, y_offset, /*hold_ms=*/0, /*step_deg=*/0, /*tick_ms=*/0, /*ms=*/0, /*sync=*/true);
}

void head_move_abs(int x_deg, int y_deg, int servo_delay) {
  (void)servo_delay;
  submit_motor(HEAD_SERVO_ABS, x_deg, HEAD_SERVO_ABS, y_deg, /*hold_ms=*/0, /*step_deg=*/0, /*tick_ms=*/0, /*ms=*/0, /*sync=*/true);
}

void head_move_abs_x(int x_deg, int servo_delay) {
  (void)servo_delay;
  submit_motor(HEAD_SERVO_ABS, x_deg, HEAD_SERVO_HOLD, 0, /*hold_ms=*/0, /*step_deg=*/0, /*tick_ms=*/0, /*ms=*/0, /*sync=*/true);
}

void head_move_abs_y(int y_deg, int servo_delay) {
  (void)servo_delay;
  submit_motor(HEAD_SERVO_HOLD, 0, HEAD_SERVO_ABS, y_deg, /*hold_ms=*/0, /*step_deg=*/0, /*tick_ms=*/0, /*ms=*/0, /*sync=*/true);
}

void head_move_ex(int x_offset, int y_offset, uint8_t step_deg, uint16_t hold_ms, uint16_t tick_ms) {
  submit_motor(HEAD_SERVO_REL, x_offset, HEAD_SERVO_REL, y_offset, hold_ms, step_deg, tick_ms, /*ms=*/0, /*sync=*/true);
}

void head_move_abs_ex(int x_deg, int y_deg, uint8_t step_deg, uint16_t hold_ms, uint16_t tick_ms) {
  submit_motor(HEAD_SERVO_ABS, x_deg, HEAD_SERVO_ABS, y_deg, hold_ms, step_deg, tick_ms, /*ms=*/0, /*sync=*/true);
}

void head_move_abs_ex_async(int x_deg, int y_deg, uint8_t step_deg, uint16_t hold_ms, uint16_t tick_ms, uint16_t ms) {
  /* sync=false：仅 xQueueSend 入队，立即返回。
   * 用于 pb v1 下行播放：让 BIN 收包不再被 chunk_ms 阻塞，舵机命令在 motor_task
   * 里独立按 ramp+hold(=chunk_ms) 串行执行；同步靠各任务自身的 chunk_ms 节拍。 */
  submit_motor(HEAD_SERVO_ABS, x_deg, HEAD_SERVO_ABS, y_deg, hold_ms, step_deg, tick_ms, ms, /*sync=*/false);
}

void head_center(int servo_delay) {
  head_move_abs(X_CENTER, Y_CENTER, servo_delay);
}

void head_right(int offset) {
  submit_motor(HEAD_SERVO_REL, offset, HEAD_SERVO_HOLD, 0, /*hold_ms=*/0, /*step_deg=*/0, /*tick_ms=*/0, /*ms=*/0, /*sync=*/true);
}

void head_left(int offset) {
  submit_motor(HEAD_SERVO_REL, -offset, HEAD_SERVO_HOLD, 0, /*hold_ms=*/0, /*step_deg=*/0, /*tick_ms=*/0, /*ms=*/0, /*sync=*/true);
}

void head_down(int offset) {
  submit_motor(HEAD_SERVO_HOLD, 0, HEAD_SERVO_REL, offset, /*hold_ms=*/0, /*step_deg=*/0, /*tick_ms=*/0, /*ms=*/0, /*sync=*/true);
}

void head_up(int offset) {
  submit_motor(HEAD_SERVO_HOLD, 0, HEAD_SERVO_REL, -offset, /*hold_ms=*/0, /*step_deg=*/0, /*tick_ms=*/0, /*ms=*/0, /*sync=*/true);
}

void head_nod(int servo_delay) {
  (void)servo_delay;
  for (int i = 0; i < 2; i++) {
    submit_motor(HEAD_SERVO_REL, 0, HEAD_SERVO_REL, 20, k_head_gesture_hold_ms, /*step_deg=*/0, /*tick_ms=*/0, /*ms=*/0, /*sync=*/true);
    submit_motor(HEAD_SERVO_REL, 0, HEAD_SERVO_REL, -20, k_head_gesture_hold_ms, /*step_deg=*/0, /*tick_ms=*/0, /*ms=*/0, /*sync=*/true);
  }
}

void head_shake(int servo_delay) {
  (void)servo_delay;
  submit_motor(HEAD_SERVO_REL, -10, HEAD_SERVO_REL, 0, k_head_gesture_hold_ms, /*step_deg=*/0, /*tick_ms=*/0, /*ms=*/0, /*sync=*/true);
  submit_motor(HEAD_SERVO_REL, 20, HEAD_SERVO_REL, 0, k_head_gesture_hold_ms, /*step_deg=*/0, /*tick_ms=*/0, /*ms=*/0, /*sync=*/true);
  submit_motor(HEAD_SERVO_REL, -20, HEAD_SERVO_REL, 0, k_head_gesture_hold_ms, /*step_deg=*/0, /*tick_ms=*/0, /*ms=*/0, /*sync=*/true);
  submit_motor(HEAD_SERVO_REL, 20, HEAD_SERVO_REL, 0, k_head_gesture_hold_ms, /*step_deg=*/0, /*tick_ms=*/0, /*ms=*/0, /*sync=*/true);
  submit_motor(HEAD_SERVO_REL, -10, HEAD_SERVO_REL, 0, k_head_gesture_hold_ms, /*step_deg=*/0, /*tick_ms=*/0, /*ms=*/0, /*sync=*/true);
}

void head_roll_left(int servo_delay) {
  head_center();
  head_down(Y_OFFSET / 2 / 2 + 5);
  head_move(-X_OFFSET / 2, -Y_OFFSET / 2 / 2, servo_delay);
  head_move(X_OFFSET / 2, -Y_OFFSET / 2 / 2, servo_delay);
  head_move(X_OFFSET / 2, Y_OFFSET / 2 / 2, servo_delay);
  head_move(-X_OFFSET / 2, Y_OFFSET / 2 / 2, servo_delay);
  head_center();
}

void head_roll_right(int servo_delay) {
  head_center();
  head_down(Y_OFFSET / 2 / 2 + 5);
  head_move(X_OFFSET / 2, -Y_OFFSET / 2 / 2, servo_delay);
  head_move(-X_OFFSET / 2, -Y_OFFSET / 2 / 2, servo_delay);
  head_move(-X_OFFSET / 2, Y_OFFSET / 2 / 2, servo_delay);
  head_move(X_OFFSET / 2, Y_OFFSET / 2 / 2, servo_delay);
  head_center();
}

void head_clear_motor_pending() {
  ensure_motor_task();
  drain_motor_queue_nonblocking();
}

void head_motor_reset() {
  head_clear_motor_pending();
}

bool head_take_pb_motor_ack_done(char* req_out, size_t req_cap, uint32_t* idx_out) {
  if (!s_pb_motor_ack_q || req_out == nullptr || req_cap < 2 || idx_out == nullptr) {
    return false;
  }
  HeadPbMotorAckMsg m{};
  if (xQueueReceive(s_pb_motor_ack_q, &m, 0) != pdTRUE) {
    return false;
  }
  strncpy(req_out, m.req, req_cap - 1);
  req_out[req_cap - 1] = '\0';
  *idx_out = m.idx;
  return true;
}

void head_drain_pb_motor_ack_queue() {
  if (!s_pb_motor_ack_q) {
    return;
  }
  HeadPbMotorAckMsg m{};
  while (xQueueReceive(s_pb_motor_ack_q, &m, 0) == pdTRUE) {
  }
}

unsigned head_motor_input_queue_depth(void) {
  ensure_motor_task();
  return s_motor_queue ? (unsigned)uxQueueMessagesWaiting(s_motor_queue) : 0u;
}
