#ifndef Head_h
#define Head_h

#include <stddef.h>
#include <ESP32Servo.h>
#include "common.h"

// Servo
#define X_PIN 12
#define Y_PIN 13
/* STEP/SERVO_DELAY 决定舵机斜坡速度：每个 SERVO_DELAY(ms) 步进 STEP(°)。
   原值 1°/10ms=100°/s 明显偏慢；改为 3°/5ms≈600°/s，兼顾顺滑与响应。 */
#define STEP 3
#define SERVO_DELAY 5
#define X_CENTER_FILE "/X_CENTER.txt"
#define Y_CENTER_FILE "/Y_CENTER.txt"

/** 水平舵机硬件行程（°）；超出会顶到线材。 */
#define X_MIN_LIMIT 30
#define X_MAX_LIMIT 150
#define X_OFFSET 90
/** 垂直舵机硬件行程（°）；超出会顶到线材/结构。 */
#define Y_MIN_LIMIT 65
#define Y_MAX_LIMIT 135
#define Y_OFFSET 45
extern int X_CENTER;
extern int Y_CENTER;
extern int X_MIN;
extern int X_MAX;
extern int Y_MIN;
extern int Y_MAX;

/** Y 轴舵机安装方向与逻辑坐标相反：以 Y_CENTER 为轴在 PWM 角与逻辑角之间互转。 */
inline int head_y_logic_to_pwm(int y_logic) { return 2 * Y_CENTER - y_logic; }
inline int head_y_pwm_to_logic(int y_pwm) { return 2 * Y_CENTER - y_pwm; }

extern Servo servo_x;
extern Servo servo_y;

/** 与下行 JSON `servo.xm` / `servo.ym` 一致；motor 队列内 `MotorCmd` 使用同一编码。 */
constexpr uint8_t HEAD_SERVO_ABS = 0;
constexpr uint8_t HEAD_SERVO_REL = 1;
constexpr uint8_t HEAD_SERVO_HOLD = 2;

/** 预制动作：由 head_preset() 展开为基础相对/绝对移动序列（与 head_nod 等等价）。 */
enum class HeadPreset : uint8_t {
  Center,
  TurnLeft,
  TurnRight,
  LookUp,
  LookDown,
  Nod,
  Shake,
  /** 与 head_roll_right() 相同轨迹（俯视头部顺时针画圈）。 */
  CircleCW,
  /** 与 head_roll_left() 相同轨迹。 */
  CircleCCW,
};

void head_preset(HeadPreset preset);

#define HEAD_ACT_CENTER()      head_preset(HeadPreset::Center)
#define HEAD_ACT_TURN_LEFT()   head_preset(HeadPreset::TurnLeft)
#define HEAD_ACT_TURN_RIGHT()  head_preset(HeadPreset::TurnRight)
#define HEAD_ACT_LOOK_UP()     head_preset(HeadPreset::LookUp)
#define HEAD_ACT_LOOK_DOWN()   head_preset(HeadPreset::LookDown)
#define HEAD_ACT_NOD()         head_preset(HeadPreset::Nod)
#define HEAD_ACT_SHAKE()       head_preset(HeadPreset::Shake)
#define HEAD_ACT_CIRCLE_CW()   head_preset(HeadPreset::CircleCW)
#define HEAD_ACT_CIRCLE_CCW()  head_preset(HeadPreset::CircleCCW)

// Functions
void servo_init();  
void setup_head();  
void adjust_x_center(int offset);
void adjust_y_center(int offset);  
void update_x_center();
void update_y_center();
void head_move(int x_offset = 0, int y_offset = 0, int servo_delay = SERVO_DELAY);  
/** 绝对角（度），双轴同时到位；单轴请用 head_move_abs_x / head_move_abs_y。 */
void head_move_abs(int x_deg, int y_deg, int servo_delay = SERVO_DELAY);
void head_move_abs_x(int x_deg, int servo_delay = SERVO_DELAY);
void head_move_abs_y(int y_deg, int servo_delay = SERVO_DELAY);
/** 高级接口：step_deg=每 20ms 一拍最大转角(°)，0=默认 1°；hold_ms=到位后停顿；tick_ms 保留兼容；async 的 ms 同 JSON `servo.ms`（墙钟预算）。 */
void head_move_ex(int x_offset, int y_offset, uint8_t step_deg = 0, uint16_t hold_ms = 0, uint16_t tick_ms = 0);
void head_move_abs_ex(int x_deg, int y_deg, uint8_t step_deg = 0, uint16_t hold_ms = 0, uint16_t tick_ms = 0);
/** 与 `servo` JSON 同形入队（仅 async）：xm/ym 为 HEAD_SERVO_*，ms 非 0 时为本段墙钟预算。
 *  pb_ack_after_done 为真且 pb_ack_req 非空时，motor_task 在本段 ramp 结束后投递一条 ack（主循环取）。 */
void head_servo_cmd_async(uint8_t xm, uint8_t ym, int x, int y, uint8_t step_deg, uint16_t ms,
                          bool pb_ack_after_done = false, uint32_t pb_ack_idx = 0, const char* pb_ack_req = nullptr);
/** 非阻塞取一条「舵机 ramp 完成」触发的 pb_ack 元数据；无消息返回 false。 */
bool head_take_pb_motor_ack_done(char* req_out, size_t req_cap, uint32_t* idx_out);
void head_drain_pb_motor_ack_queue();
/** 异步版本：仅入队 motor_task，不等待执行完成，立即返回。
 *  pb v1 下行播放路径专用：BIN 处理（onWebSocketEvent）不再被 chunk_ms 阻塞，
 *  反压改由音频队列满 + 服务端 pb_ack.audio_buf_ms 流控承担；
 *  舵机命令在 motor_task 里串行 ramp+hold 完成（每条 ≈ chunk_ms），
 *  与音频/OLED 三路独立任务通过 chunk_ms 各自计时实现松散同步。
 *  cmd.cpp / 动画等其它需要"动作完成后再继续"的场景仍用 head_move_abs_ex。 */
void head_move_abs_ex_async(int x_deg, int y_deg, uint8_t step_deg = 0, uint16_t hold_ms = 0, uint16_t tick_ms = 0,
                            uint16_t ms = 0);
void head_center(int servo_delay = SERVO_DELAY);  
void head_right(int offset = 0);  
void head_left(int offset = 0);  
void head_down(int offset = 0);  
void head_up(int offset = 0);  
void head_nod(int servo_delay = 1);  
void head_shake(int servo_delay = 1);  
void head_roll_left(int servo_delay = SERVO_DELAY);  
void head_roll_right(int servo_delay = SERVO_DELAY);  
/** 非阻塞排空 motor 的 FreeRTOS 输入队列（尚未被 motor_task 取走的 cmd）；对丢弃的 sync cmd
 *  give done_sem，避免调用方永久阻塞。当前已在执行的 ramp 不受影响。 */
void head_clear_motor_pending();

/** 与 head_clear_motor_pending 相同：打断排队中的舵机序列（如旧 pb），迎接新 pb_start；
 *  当前姿态保留，下一段 async 仍从当前角 ramp。 */
void head_motor_reset();

unsigned head_motor_input_queue_depth();

#endif