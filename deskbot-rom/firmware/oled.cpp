#include "oled.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <ArduinoJson.h>

Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET,
                      OLED_I2C_CLOCK_DURING_HZ, OLED_I2C_CLOCK_AFTER_HZ);

void oled_clear_display_timed() { oled.clearDisplay(); }

void oled_display_timed() { oled.display(); }

void setup_oled() {
  if (!oled.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    log_error("OLED Initiate Failed.");
    while (true);  // Pause
  } else {
    log_info("OLED is Ready (I2C during OLED xfer %lu Hz, restore %lu Hz).",
             (unsigned long)OLED_I2C_CLOCK_DURING_HZ,
             (unsigned long)OLED_I2C_CLOCK_AFTER_HZ);
    oled_clear_display_timed();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled_println(String(PRODUCT_NAME) + " v" + String(VERSION), 500);
  }
}

/* oled_print/println：boot 阶段 banner / wifi 配网模式调用，渲染任务尚未启动
 * 或处于阻塞配网流程，没有并发；vTaskDelay 与任务安全一致。 */
void oled_print(String text, int delay_time) {
  oled.print(text);
  oled_display_timed();
  vTaskDelay(pdMS_TO_TICKS(delay_time));
}

void oled_println(String text, int delay_time) {
  oled.println(text);
  oled_display_timed();
  vTaskDelay(pdMS_TO_TICKS(delay_time));
}

namespace {

/* pb v1 矢量帧：同层同下标且 shape 一致时在 chunk 内按 t 插值；否则画本帧。
 * 若本段剩余时间 < kPbOledDisplayBudgetMs，则跳过本次 display，仅延时以对齐 chunk_ms。 */
static constexpr uint32_t kPbOledDisplayBudgetMs = 13;
static constexpr uint8_t  kPbMaxPrimsPerLayer   = 16;
/** text 图元正文最多保留 8 字节（UTF-8 按字节计，超长截断；含结尾 NUL 共 9 字节）。 */
static constexpr size_t kPbMaxTextChars = 8;

/* pb v1 图元：shape 字符串见 json_fill_layer；与 Adafruit_GFX（SSD1306 基类）API 对齐。 */
enum class PbShape : uint8_t {
  None = 0,
  Rect,
  Circle,
  Line,
  Pixel,
  HLine,
  VLine,
  RectOutline,
  CircleOutline,
  Ellipse,
  EllipseFill,
  Triangle,
  TriangleFill,
  RoundRect,
  RoundRectOutline,
  RotatedRectOutline,
  RotatedRectFill,
  Text,
};

struct StoredPrim {
  PbShape shape;
  uint8_t color; /* 0=BLACK, 1=WHITE, 2=INVERSE（SSD1306_*） */
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  int16_t r;
  int16_t x1;
  int16_t y1;
  int16_t x2;
  int16_t y2;
  int16_t angle; /* 仅 rotated_rect_*：度，逆时针为正（与 Adafruit_GFX 一致） */
  uint8_t text_size; /* 仅 Text：1–3，与 setTextSize 一致 */
  char    text[kPbMaxTextChars + 1];
};

struct StoredLayer {
  uint8_t     count;
  StoredPrim  prims[kPbMaxPrimsPerLayer];
};

static StoredLayer s_prev_nose{};
static StoredLayer s_prev_mouth{};
static StoredLayer s_prev_eye_l{};
static StoredLayer s_prev_eye_r{};
static StoredLayer s_prev_extra{};
static bool        s_have_prev = false;

/** pb 矢量解析用的「当前帧」图层；放静态区避免 oled_render 任务栈过大。仅渲染任务串行访问。 */
static StoredLayer s_pb_curr_nose{};
static StoredLayer s_pb_curr_mouth{};
static StoredLayer s_pb_curr_eye_l{};
static StoredLayer s_pb_curr_eye_r{};
static StoredLayer s_pb_curr_extra{};

static void pb_vector_interp_reset() {
  s_have_prev = false;
  memset(&s_prev_nose, 0, sizeof(s_prev_nose));
  memset(&s_prev_mouth, 0, sizeof(s_prev_mouth));
  memset(&s_prev_eye_l, 0, sizeof(s_prev_eye_l));
  memset(&s_prev_eye_r, 0, sizeof(s_prev_eye_r));
  memset(&s_prev_extra, 0, sizeof(s_prev_extra));
}

static int lerp_i16(int16_t a, int16_t b, float t) {
  return (int)lroundf((1.f - t) * (float)a + t * (float)b);
}

static void layer_clear(StoredLayer* L) {
  L->count = 0;
}

static uint8_t pb_json_color(JsonObjectConst it) {
  int v = 1;
  if (!it["c"].isNull()) {
    v = it["c"].as<int>();
  } else if (!it["color"].isNull()) {
    v = it["color"].as<int>();
  }
  if (v <= 0) {
    return 0;
  }
  if (v >= 2) {
    return 2;
  }
  return 1;
}

static uint16_t pb_draw_color(uint8_t c) {
  if (c == 0) {
    return SSD1306_BLACK;
  }
  if (c >= 2) {
    return SSD1306_INVERSE;
  }
  return SSD1306_WHITE;
}

static void json_fill_layer(JsonArrayConst arr, StoredLayer* out) {
  layer_clear(out);
  if (arr.isNull()) {
    return;
  }
  for (JsonObjectConst it : arr) {
    if (out->count >= kPbMaxPrimsPerLayer) {
      break;
    }
    StoredPrim& p = out->prims[out->count];
    memset(&p, 0, sizeof(p));
    p.color = pb_json_color(it);
    const char* shape = it["shape"] | "";
    if (strcmp(shape, "rect") == 0 || strcmp(shape, "fill_rect") == 0) {
      p.shape = PbShape::Rect;
      p.x = (int16_t)(it["x"] | 0);
      p.y = (int16_t)(it["y"] | 0);
      p.w = (int16_t)(it["w"] | 0);
      p.h = (int16_t)(it["h"] | 0);
    } else if (strcmp(shape, "rect_outline") == 0 || strcmp(shape, "draw_rect") == 0) {
      p.shape = PbShape::RectOutline;
      p.x = (int16_t)(it["x"] | 0);
      p.y = (int16_t)(it["y"] | 0);
      p.w = (int16_t)(it["w"] | 0);
      p.h = (int16_t)(it["h"] | 0);
    } else if (strcmp(shape, "circle") == 0 || strcmp(shape, "fill_circle") == 0) {
      p.shape = PbShape::Circle;
      p.x = (int16_t)(it["x"] | 0);
      p.y = (int16_t)(it["y"] | 0);
      p.r = (int16_t)(it["r"] | 0);
    } else if (strcmp(shape, "circle_outline") == 0 || strcmp(shape, "draw_circle") == 0) {
      p.shape = PbShape::CircleOutline;
      p.x = (int16_t)(it["x"] | 0);
      p.y = (int16_t)(it["y"] | 0);
      p.r = (int16_t)(it["r"] | 0);
    } else if (strcmp(shape, "line") == 0) {
      p.shape = PbShape::Line;
      p.x1 = (int16_t)(it["x1"] | 0);
      p.y1 = (int16_t)(it["y1"] | 0);
      p.x2 = (int16_t)(it["x2"] | 0);
      p.y2 = (int16_t)(it["y2"] | 0);
    } else if (strcmp(shape, "pixel") == 0 || strcmp(shape, "point") == 0) {
      p.shape = PbShape::Pixel;
      p.x = (int16_t)(it["x"] | 0);
      p.y = (int16_t)(it["y"] | 0);
    } else if (strcmp(shape, "hline") == 0 || strcmp(shape, "h_line") == 0) {
      p.shape = PbShape::HLine;
      p.x = (int16_t)(it["x"] | 0);
      p.y = (int16_t)(it["y"] | 0);
      p.w = (int16_t)(it["w"] | 0);
    } else if (strcmp(shape, "vline") == 0 || strcmp(shape, "v_line") == 0) {
      p.shape = PbShape::VLine;
      p.x = (int16_t)(it["x"] | 0);
      p.y = (int16_t)(it["y"] | 0);
      p.h = (int16_t)(it["h"] | 0);
    } else if (strcmp(shape, "ellipse") == 0 || strcmp(shape, "draw_ellipse") == 0) {
      p.shape = PbShape::Ellipse;
      p.x = (int16_t)(it["x"] | 0);
      p.y = (int16_t)(it["y"] | 0);
      p.w = (int16_t)(it["rw"] | it["w"] | 0);
      p.h = (int16_t)(it["rh"] | it["h"] | 0);
    } else if (strcmp(shape, "ellipse_fill") == 0 || strcmp(shape, "fill_ellipse") == 0) {
      p.shape = PbShape::EllipseFill;
      p.x = (int16_t)(it["x"] | 0);
      p.y = (int16_t)(it["y"] | 0);
      p.w = (int16_t)(it["rw"] | it["w"] | 0);
      p.h = (int16_t)(it["rh"] | it["h"] | 0);
    } else if (strcmp(shape, "triangle") == 0 || strcmp(shape, "draw_triangle") == 0) {
      p.shape = PbShape::Triangle;
      p.x = (int16_t)(it["x0"] | it["x"] | 0);
      p.y = (int16_t)(it["y0"] | it["y"] | 0);
      p.x1 = (int16_t)(it["x1"] | 0);
      p.y1 = (int16_t)(it["y1"] | 0);
      p.x2 = (int16_t)(it["x2"] | 0);
      p.y2 = (int16_t)(it["y2"] | 0);
    } else if (strcmp(shape, "triangle_fill") == 0 || strcmp(shape, "fill_triangle") == 0) {
      p.shape = PbShape::TriangleFill;
      p.x = (int16_t)(it["x0"] | it["x"] | 0);
      p.y = (int16_t)(it["y0"] | it["y"] | 0);
      p.x1 = (int16_t)(it["x1"] | 0);
      p.y1 = (int16_t)(it["y1"] | 0);
      p.x2 = (int16_t)(it["x2"] | 0);
      p.y2 = (int16_t)(it["y2"] | 0);
    } else if (strcmp(shape, "round_rect") == 0 || strcmp(shape, "fill_round_rect") == 0) {
      p.shape = PbShape::RoundRect;
      p.x = (int16_t)(it["x"] | 0);
      p.y = (int16_t)(it["y"] | 0);
      p.w = (int16_t)(it["w"] | 0);
      p.h = (int16_t)(it["h"] | 0);
      p.r = (int16_t)(it["radius"] | it["r"] | 0);
    } else if (strcmp(shape, "round_rect_outline") == 0 || strcmp(shape, "draw_round_rect") == 0) {
      p.shape = PbShape::RoundRectOutline;
      p.x = (int16_t)(it["x"] | 0);
      p.y = (int16_t)(it["y"] | 0);
      p.w = (int16_t)(it["w"] | 0);
      p.h = (int16_t)(it["h"] | 0);
      p.r = (int16_t)(it["radius"] | it["r"] | 0);
    } else if (strcmp(shape, "rotated_rect_outline") == 0 || strcmp(shape, "draw_rotated_rect") == 0) {
      p.shape = PbShape::RotatedRectOutline;
      p.x = (int16_t)(it["x"] | 0);
      p.y = (int16_t)(it["y"] | 0);
      p.w = (int16_t)(it["w"] | 0);
      p.h = (int16_t)(it["h"] | 0);
      p.angle = (int16_t)(it["angle"] | 0);
    } else if (strcmp(shape, "rotated_rect_fill") == 0 || strcmp(shape, "fill_rotated_rect") == 0) {
      p.shape = PbShape::RotatedRectFill;
      p.x = (int16_t)(it["x"] | 0);
      p.y = (int16_t)(it["y"] | 0);
      p.w = (int16_t)(it["w"] | 0);
      p.h = (int16_t)(it["h"] | 0);
      p.angle = (int16_t)(it["angle"] | 0);
    } else if (strcmp(shape, "text") == 0 || strcmp(shape, "print") == 0 || strcmp(shape, "label") == 0) {
      p.shape = PbShape::Text;
      p.x = (int16_t)(it["x"] | 0);
      p.y = (int16_t)(it["y"] | 0);
      const char* tstr = nullptr;
      if (!it["text"].isNull()) {
        tstr = it["text"].as<const char*>();
      } else if (!it["s"].isNull()) {
        tstr = it["s"].as<const char*>();
      } else if (!it["str"].isNull()) {
        tstr = it["str"].as<const char*>();
      }
      if (!tstr || !tstr[0]) {
        continue;
      }
      strncpy(p.text, tstr, kPbMaxTextChars);
      p.text[kPbMaxTextChars] = '\0';
      int tsz = 1;
      if (!it["size"].isNull()) {
        tsz = it["size"].as<int>();
      } else if (!it["text_size"].isNull()) {
        tsz = it["text_size"].as<int>();
      }
      if (tsz < 1) {
        tsz = 1;
      }
      if (tsz > 3) {
        tsz = 3;
      }
      p.text_size = (uint8_t)tsz;
    } else {
      continue;
    }
    out->count++;
  }
}

static void stored_from_elements_v(JsonVariantConst elements_v, StoredLayer* nose, StoredLayer* mouth,
                                   StoredLayer* eye_l, StoredLayer* eye_r, StoredLayer* extra) {
  layer_clear(nose);
  layer_clear(mouth);
  layer_clear(eye_l);
  layer_clear(eye_r);
  layer_clear(extra);
  if (elements_v.isNull()) {
    return;
  }
  JsonObjectConst elements = elements_v.as<JsonObjectConst>();
  if (!elements["nose"].isNull()) {
    json_fill_layer(elements["nose"].as<JsonArrayConst>(), nose);
  }
  if (!elements["mouth"].isNull()) {
    json_fill_layer(elements["mouth"].as<JsonArrayConst>(), mouth);
  }
  if (!elements["eye_l"].isNull()) {
    json_fill_layer(elements["eye_l"].as<JsonArrayConst>(), eye_l);
  }
  if (!elements["eye_r"].isNull()) {
    json_fill_layer(elements["eye_r"].as<JsonArrayConst>(), eye_r);
  }
  if (!elements["extra"].isNull()) {
    json_fill_layer(elements["extra"].as<JsonArrayConst>(), extra);
  }
}

static void draw_prim(const StoredPrim& p) {
  const uint16_t col = pb_draw_color(p.color);
  switch (p.shape) {
    case PbShape::Rect:
      if (p.w > 0 && p.h > 0) {
        oled.fillRect(p.x, p.y, p.w, p.h, col);
      }
      break;
    case PbShape::RectOutline:
      if (p.w > 0 && p.h > 0) {
        oled.drawRect(p.x, p.y, p.w, p.h, col);
      }
      break;
    case PbShape::Circle:
      if (p.r > 0) {
        oled.fillCircle(p.x, p.y, p.r, col);
      }
      break;
    case PbShape::CircleOutline:
      if (p.r > 0) {
        oled.drawCircle(p.x, p.y, p.r, col);
      }
      break;
    case PbShape::Line:
      oled.drawLine(p.x1, p.y1, p.x2, p.y2, col);
      break;
    case PbShape::Pixel:
      oled.drawPixel(p.x, p.y, col);
      break;
    case PbShape::HLine:
      if (p.w != 0) {
        oled.drawFastHLine(p.x, p.y, p.w, col);
      }
      break;
    case PbShape::VLine:
      if (p.h != 0) {
        oled.drawFastVLine(p.x, p.y, p.h, col);
      }
      break;
    case PbShape::Ellipse:
      if (p.w > 0 && p.h > 0) {
        oled.drawEllipse(p.x, p.y, p.w, p.h, col);
      }
      break;
    case PbShape::EllipseFill:
      if (p.w > 0 && p.h > 0) {
        oled.fillEllipse(p.x, p.y, p.w, p.h, col);
      }
      break;
    case PbShape::Triangle:
      oled.drawTriangle(p.x, p.y, p.x1, p.y1, p.x2, p.y2, col);
      break;
    case PbShape::TriangleFill:
      oled.fillTriangle(p.x, p.y, p.x1, p.y1, p.x2, p.y2, col);
      break;
    case PbShape::RoundRect:
      if (p.w > 0 && p.h > 0 && p.r > 0) {
        oled.fillRoundRect(p.x, p.y, p.w, p.h, p.r, col);
      }
      break;
    case PbShape::RoundRectOutline:
      if (p.w > 0 && p.h > 0 && p.r > 0) {
        oled.drawRoundRect(p.x, p.y, p.w, p.h, p.r, col);
      }
      break;
    case PbShape::RotatedRectOutline:
      if (p.w > 0 && p.h > 0) {
        oled.drawRotatedRect(p.x, p.y, p.w, p.h, p.angle, col);
      }
      break;
    case PbShape::RotatedRectFill:
      if (p.w > 0 && p.h > 0) {
        oled.fillRotatedRect(p.x, p.y, p.w, p.h, p.angle, col);
      }
      break;
    case PbShape::Text:
      if (p.text[0] != '\0') {
        uint8_t sz = p.text_size ? p.text_size : 1;
        if (sz > 3) {
          sz = 3;
        }
        oled.setTextSize(sz);
        oled.setTextColor(col);
        oled.setCursor(p.x, p.y);
        oled.print(p.text);
        oled.setTextSize(1);
      }
      break;
    case PbShape::None:
    default:
      break;
  }
}

static void draw_prim_lerp(const StoredPrim* prev, const StoredPrim& curr, float t) {
  if (!prev || prev->shape != curr.shape || curr.shape == PbShape::None) {
    draw_prim(curr);
    return;
  }
  const uint16_t col = pb_draw_color(curr.color);
  switch (curr.shape) {
    case PbShape::Rect:
    case PbShape::RectOutline: {
      int x = lerp_i16(prev->x, curr.x, t);
      int y = lerp_i16(prev->y, curr.y, t);
      int w = lerp_i16(prev->w, curr.w, t);
      int h = lerp_i16(prev->h, curr.h, t);
      if (w < 1) {
        w = 1;
      }
      if (h < 1) {
        h = 1;
      }
      if (curr.shape == PbShape::Rect) {
        oled.fillRect(x, y, w, h, col);
      } else {
        oled.drawRect(x, y, w, h, col);
      }
    } break;
    case PbShape::Circle:
    case PbShape::CircleOutline: {
      int x = lerp_i16(prev->x, curr.x, t);
      int y = lerp_i16(prev->y, curr.y, t);
      int r = lerp_i16(prev->r, curr.r, t);
      if (r > 0) {
        if (curr.shape == PbShape::Circle) {
          oled.fillCircle(x, y, r, col);
        } else {
          oled.drawCircle(x, y, r, col);
        }
      }
    } break;
    case PbShape::Line: {
      int x1 = lerp_i16(prev->x1, curr.x1, t);
      int y1 = lerp_i16(prev->y1, curr.y1, t);
      int x2 = lerp_i16(prev->x2, curr.x2, t);
      int y2 = lerp_i16(prev->y2, curr.y2, t);
      oled.drawLine(x1, y1, x2, y2, col);
    } break;
    case PbShape::Pixel: {
      int x = lerp_i16(prev->x, curr.x, t);
      int y = lerp_i16(prev->y, curr.y, t);
      oled.drawPixel(x, y, col);
    } break;
    case PbShape::HLine: {
      int x = lerp_i16(prev->x, curr.x, t);
      int y = lerp_i16(prev->y, curr.y, t);
      int w = lerp_i16(prev->w, curr.w, t);
      if (w != 0) {
        oled.drawFastHLine(x, y, w, col);
      }
    } break;
    case PbShape::VLine: {
      int x = lerp_i16(prev->x, curr.x, t);
      int y = lerp_i16(prev->y, curr.y, t);
      int h = lerp_i16(prev->h, curr.h, t);
      if (h != 0) {
        oled.drawFastVLine(x, y, h, col);
      }
    } break;
    case PbShape::Ellipse:
    case PbShape::EllipseFill: {
      int x = lerp_i16(prev->x, curr.x, t);
      int y = lerp_i16(prev->y, curr.y, t);
      int rw = lerp_i16(prev->w, curr.w, t);
      int rh = lerp_i16(prev->h, curr.h, t);
      if (rw < 1) {
        rw = 1;
      }
      if (rh < 1) {
        rh = 1;
      }
      if (curr.shape == PbShape::EllipseFill) {
        oled.fillEllipse(x, y, rw, rh, col);
      } else {
        oled.drawEllipse(x, y, rw, rh, col);
      }
    } break;
    case PbShape::Triangle:
    case PbShape::TriangleFill: {
      int x0 = lerp_i16(prev->x, curr.x, t);
      int y0 = lerp_i16(prev->y, curr.y, t);
      int x1 = lerp_i16(prev->x1, curr.x1, t);
      int y1 = lerp_i16(prev->y1, curr.y1, t);
      int x2 = lerp_i16(prev->x2, curr.x2, t);
      int y2 = lerp_i16(prev->y2, curr.y2, t);
      if (curr.shape == PbShape::TriangleFill) {
        oled.fillTriangle(x0, y0, x1, y1, x2, y2, col);
      } else {
        oled.drawTriangle(x0, y0, x1, y1, x2, y2, col);
      }
    } break;
    case PbShape::RoundRect:
    case PbShape::RoundRectOutline: {
      int x = lerp_i16(prev->x, curr.x, t);
      int y = lerp_i16(prev->y, curr.y, t);
      int w = lerp_i16(prev->w, curr.w, t);
      int h = lerp_i16(prev->h, curr.h, t);
      int rad = lerp_i16(prev->r, curr.r, t);
      if (w < 1) {
        w = 1;
      }
      if (h < 1) {
        h = 1;
      }
      if (rad < 1) {
        rad = 1;
      }
      if (curr.shape == PbShape::RoundRect) {
        oled.fillRoundRect(x, y, w, h, rad, col);
      } else {
        oled.drawRoundRect(x, y, w, h, rad, col);
      }
    } break;
    case PbShape::RotatedRectOutline:
    case PbShape::RotatedRectFill: {
      int cx = lerp_i16(prev->x, curr.x, t);
      int cy = lerp_i16(prev->y, curr.y, t);
      int w = lerp_i16(prev->w, curr.w, t);
      int h = lerp_i16(prev->h, curr.h, t);
      int ang = lerp_i16(prev->angle, curr.angle, t);
      if (w < 1) {
        w = 1;
      }
      if (h < 1) {
        h = 1;
      }
      if (curr.shape == PbShape::RotatedRectFill) {
        oled.fillRotatedRect(cx, cy, w, h, ang, col);
      } else {
        oled.drawRotatedRect(cx, cy, w, h, ang, col);
      }
    } break;
    case PbShape::Text: {
      if (strcmp(prev->text, curr.text) != 0 || prev->text_size != curr.text_size) {
        draw_prim(curr);
        break;
      }
      int x = lerp_i16(prev->x, curr.x, t);
      int y = lerp_i16(prev->y, curr.y, t);
      uint8_t sz = curr.text_size ? curr.text_size : 1;
      if (sz > 3) {
        sz = 3;
      }
      oled.setTextSize(sz);
      oled.setTextColor(col);
      oled.setCursor(x, y);
      oled.print(curr.text);
      oled.setTextSize(1);
    } break;
    case PbShape::None:
    default:
      break;
  }
}

static void draw_layer_lerp(const StoredLayer* prev, const StoredLayer& curr, float t) {
  const uint8_t ncurr = curr.count;
  if (ncurr == 0) {
    return;
  }
  const uint8_t nprev = prev ? prev->count : 0;
  for (uint8_t i = 0; i < ncurr; i++) {
    const StoredPrim& c = curr.prims[i];
    if (i < nprev) {
      draw_prim_lerp(&prev->prims[i], c, t);
    } else {
      draw_prim(c);
    }
  }
}

static void draw_stored_interpolated(const StoredLayer* pn, const StoredLayer* pm, const StoredLayer* pel,
                                     const StoredLayer* per, const StoredLayer* pex, const StoredLayer& cn,
                                     const StoredLayer& cm, const StoredLayer& cel, const StoredLayer& cer,
                                     const StoredLayer& cex, float t) {
  draw_layer_lerp(pn, cn, t);
  draw_layer_lerp(pm, cm, t);
  draw_layer_lerp(pel, cel, t);
  draw_layer_lerp(per, cer, t);
  draw_layer_lerp(pex, cex, t);
}

static void pb_extract_elements_v(JsonDocument& doc, JsonVariantConst* out_elements) {
  JsonVariantConst v = doc.as<JsonVariantConst>();
  if (v.is<JsonObjectConst>() && !v["anim"].isNull()) {
    *out_elements = v["anim"]["elements"];
  } else if (v.is<JsonObjectConst>() && !v["elements"].isNull()) {
    *out_elements = v["elements"];
  } else {
    *out_elements = v;
  }
}

static void pb_commit_prev(const StoredLayer& nose, const StoredLayer& mouth, const StoredLayer& eye_l,
                           const StoredLayer& eye_r, const StoredLayer& extra) {
  memcpy(&s_prev_nose, &nose, sizeof(nose));
  memcpy(&s_prev_mouth, &mouth, sizeof(mouth));
  memcpy(&s_prev_eye_l, &eye_l, sizeof(eye_l));
  memcpy(&s_prev_eye_r, &eye_r, sizeof(eye_r));
  memcpy(&s_prev_extra, &extra, sizeof(extra));
  s_have_prev = true;
}

/* JSON 与旧版一致：{"anim":{"elements":...}} / {"elements":...} / 直接 elements 对象。 */
static void pb_render_vector_json_timed(const char* json, size_t json_len, uint32_t chunk_ms) {
  if (!json || json_len == 0) {
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json, json_len);
  if (err) {
    log_warn("[OLED] pb vector json parse failed: %s", err.c_str());
    return;
  }
  JsonVariantConst elements_v;
  pb_extract_elements_v(doc, &elements_v);

  StoredLayer& cn = s_pb_curr_nose;
  StoredLayer& cm = s_pb_curr_mouth;
  StoredLayer& cel = s_pb_curr_eye_l;
  StoredLayer& cer = s_pb_curr_eye_r;
  StoredLayer& cex = s_pb_curr_extra;
  stored_from_elements_v(elements_v, &cn, &cm, &cel, &cer, &cex);

  const StoredLayer* pn = s_have_prev ? &s_prev_nose : nullptr;
  const StoredLayer* pm = s_have_prev ? &s_prev_mouth : nullptr;
  const StoredLayer* pl = s_have_prev ? &s_prev_eye_l : nullptr;
  const StoredLayer* pr = s_have_prev ? &s_prev_eye_r : nullptr;
  const StoredLayer* px = s_have_prev ? &s_prev_extra : nullptr;

  if (chunk_ms == 0) {
    oled.clearDisplay();
    draw_stored_interpolated(pn, pm, pl, pr, px, cn, cm, cel, cer, cex, 1.f);
    oled.display();
    pb_commit_prev(cn, cm, cel, cer, cex);
    return;
  }

  uint32_t budget = chunk_ms;
  if (budget > 300000u) {
    budget = 300000u;
  }

  const uint32_t t0 = millis();

  while (true) {
    const uint32_t now = millis();
    const uint32_t elapsed = now - t0;
    if (elapsed >= budget) {
      break;
    }
    float t = (float)elapsed / (float)budget;
    if (t > 1.f) {
      t = 1.f;
    }

    oled.clearDisplay();
    draw_stored_interpolated(pn, pm, pl, pr, px, cn, cm, cel, cer, cex, t);

    const uint32_t after_draw = millis();
    uint32_t       remain     = (t0 + budget) - after_draw;
    if (remain < kPbOledDisplayBudgetMs) {
      if (remain > 0) {
        vTaskDelay(pdMS_TO_TICKS(remain));
      }
      break;
    }
    oled.display();
    const uint32_t after_disp = millis();
    remain = (t0 + budget) - after_disp;
    if (remain == 0) {
      break;
    }
    if (remain > 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }

  while ((int32_t)(millis() - t0) < (int32_t)budget) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  pb_commit_prev(cn, cm, cel, cer, cex);
}

struct OledRequest {
  OledScene scene;
  int32_t arg;
  char* json_payload; /* 仅 OLED_SCENE_PB_VECTOR_JSON 使用；malloc 分配，任务内 free。 */
  size_t json_len;
  SemaphoreHandle_t notify_sem;
};

QueueHandle_t     s_queue       = nullptr;
TaskHandle_t      s_task        = nullptr;
SemaphoreHandle_t s_done_sem    = nullptr;
SemaphoreHandle_t s_caller_lock = nullptr;

void oled_render_task(void* /*arg*/) {
  OledRequest req{};
  for (;;) {
    if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    if (req.scene == OLED_SCENE_PB_VECTOR_JSON) {
      const uint32_t chunk_ms = req.arg > 0 ? (uint32_t)req.arg : 0u;
      pb_render_vector_json_timed(req.json_payload, req.json_len, chunk_ms);
      if (req.json_payload) {
        ::free(req.json_payload);
      }
    } else if (req.scene == OLED_SCENE_RESET) {
      pb_vector_interp_reset();
    }
    if (req.notify_sem) {
      xSemaphoreGive(req.notify_sem);
    }
  }
}

void ensure_render_task() {
  if (s_queue && s_task && s_done_sem && s_caller_lock) {
    return;
  }
  if (!s_queue) {
    /* 队列容量 32：与音频/舵机队列对齐，可缓冲 ~32 个 chunk_ms（~3.2~6.4s）的口型动画。
     * 满则 oled_render_submit_pb_vector_json 走 drop-oldest，永不阻塞 caller（WS 回调）。 */
    s_queue = xQueueCreate(32, sizeof(OledRequest));
  }
  if (!s_done_sem) {
    s_done_sem = xSemaphoreCreateBinary();
  }
  if (!s_caller_lock) {
    s_caller_lock = xSemaphoreCreateMutex();
  }
  if (!s_task) {
    xTaskCreatePinnedToCore(oled_render_task, "oled_render", 6 * 1024, nullptr, 2, &s_task,
                            APP_CPU_NUM);
  }
}

}  // namespace

void display_task_setup() {
  ensure_render_task();
}

void oled_render_submit_pb_vector_json(const char* json, size_t json_len, uint32_t chunk_ms, bool wait_done) {
  ensure_render_task();
  if (!json || json_len == 0) {
    return;
  }
  char* copy = (char*)::malloc(json_len + 1);
  if (!copy) {
    return;
  }
  memcpy(copy, json, json_len);
  copy[json_len] = '\0';

  OledRequest req{};
  req.scene = OLED_SCENE_PB_VECTOR_JSON;
  if (chunk_ms > 0x7fffffffu) {
    req.arg = (int32_t)0x7fffffffu;
  } else {
    req.arg = (int32_t)chunk_ms;
  }
  req.json_payload = copy;
  req.json_len = json_len;

  if (wait_done) {
    xSemaphoreTake(s_caller_lock, portMAX_DELAY);
    xSemaphoreTake(s_done_sem, 0);
    req.notify_sem = s_done_sem;
    xQueueSend(s_queue, &req, portMAX_DELAY);
    xSemaphoreTake(s_done_sem, portMAX_DELAY);
    xSemaphoreGive(s_caller_lock);
    return;
  }

  req.notify_sem = nullptr;
  if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
    OledRequest dropped{};
    xQueueReceive(s_queue, &dropped, 0);
    /* 若被丢弃的是 pb 帧，释放其 payload，避免泄漏。 */
    if (dropped.scene == OLED_SCENE_PB_VECTOR_JSON && dropped.json_payload) {
      ::free(dropped.json_payload);
    }
    xQueueSend(s_queue, &req, 0);
  }
}

void oled_render_reset() {
  ensure_render_task();
  xSemaphoreTake(s_caller_lock, portMAX_DELAY);

  /* 1. drain 队列里所有未渲染 req：释放 json_payload + 唤醒 sync caller 防永久阻塞。
   *    drain 必须在入队 RESET 之前，否则 RESET 会被同一轮 drain 误吃。 */
  OledRequest dropped{};
  while (xQueueReceive(s_queue, &dropped, 0) == pdTRUE) {
    if (dropped.scene == OLED_SCENE_PB_VECTOR_JSON && dropped.json_payload) {
      ::free(dropped.json_payload);
    }
    if (dropped.notify_sem) {
      xSemaphoreGive(dropped.notify_sem);
    }
  }

  /* 2. 入队 RESET 到队尾：渲染任务完成"当前正在渲染的 req"（含 vTaskDelay）后 receive 到，
   *    再做 noop 收尾（不清屏、保留当前画面）。 */
  OledRequest req{};
  req.scene = OLED_SCENE_RESET;
  req.arg = 0;
  req.json_payload = nullptr;
  req.json_len = 0;
  req.notify_sem = nullptr;
  xQueueSend(s_queue, &req, portMAX_DELAY);

  xSemaphoreGive(s_caller_lock);
}

unsigned oled_render_input_queue_depth(void) {
  ensure_render_task();
  return s_queue ? (unsigned)uxQueueMessagesWaiting(s_queue) : 0u;
}
