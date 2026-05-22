#ifndef OLED_H
#define OLED_H

#include <Adafruit_SSD1306.h>
#include "common.h"

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

/** Adafruit_SSD1306 每次 I2C 传输前后会 setClock(clkDuring/clkAfter)，覆盖 Wire.setClock。
 *  要提高刷屏速率必须把时钟放在构造函数里，而不是只在 setup 里调 Wire。 */
#ifndef OLED_I2C_CLOCK_DURING_HZ
#define OLED_I2C_CLOCK_DURING_HZ 1000000UL
#endif
#ifndef OLED_I2C_CLOCK_AFTER_HZ
#define OLED_I2C_CLOCK_AFTER_HZ 100000UL
#endif

extern Adafruit_SSD1306 oled;

void setup_oled();

/** clearDisplay / display 薄封装（与历史 timed 命名兼容）。 */
void oled_clear_display_timed();
void oled_display_timed();

void oled_print(String text, int delay_time = 100);
void oled_println(String text, int delay_time = 100);

/* OLED 异步渲染队列：pb 矢量帧 JSON 由独立 FreeRTOS 任务串行消费。 */
enum OledScene : uint8_t {
  OLED_SCENE_PB_VECTOR_JSON = 0,
  OLED_SCENE_RESET,
};

/** 启动 OLED 渲染任务；幂等。须在 setup() 末尾调用。 */
void display_task_setup();

/** 提交 pb v1 的矢量动画帧（JSON 字符串）。
 * - 会深拷贝 json 到堆上；渲染任务消费后自动释放。
 * - chunk_ms>0：在本段时间内按 wall-clock 从上一关键帧插值到本帧并多次刷新；与本段音频对齐。
 *   若剩余时间不足以安全完成一次 display（约 13ms），则跳过本次 display 仅延时，避免拖慢节拍。
 * - chunk_ms==0：单帧立即显示（仍可与上一帧做 t=1 的插值几何），不占额定时长。
 * - 图元：`elements` 下 `nose` / `mouth` / `eye_l` / `eye_r` 与可选 `extra` 为数组；每项必有字符串 `shape`，
 *   几何与可选 `c`/`color`（0 黑、1 白、2 反色，默认 1）。`shape` 为 `text`/`print`/`label` 时用
 *   `text` 或 `s` 或 `str` 传字符串（正文最多 8 字节，超出丢弃），`x`/`y` 为左上角光标，可选 `size`/`text_size`（1–3）。
 *   具体 shape 名与字段以固件 `oled.cpp` 中 `json_fill_layer` 为准（与 Adafruit_GFX 绘制 API 对应）。 */
void oled_render_submit_pb_vector_json(const char* json, size_t json_len, uint32_t chunk_ms = 0,
                                       bool wait_done = false);

/** 打断渲染：drain 队列里所有未渲染 req（释放 json_payload + 唤醒 sync caller 防永久阻塞），
 *  再排队一个 OLED_SCENE_RESET 到队尾。渲染任务完成"当前正在渲染的 req"（含 vTaskDelay）后
 *  立即收到 RESET → noop（保留当前画面），用于通知"旧 pb 序列已被打断、迎接新 pb_start"。
 *  与 audio_play_reset / head_motor_reset 配套使用。 */
void oled_render_reset();

unsigned oled_render_input_queue_depth();

#endif
