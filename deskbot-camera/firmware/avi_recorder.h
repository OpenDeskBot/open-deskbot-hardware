#ifndef AVI_RECORDER_H
#define AVI_RECORDER_H

#include "esp_camera.h"
#include "FS.h"
#include "SD.h"

// ============================
// Recording configuration
// ============================
#define AVI_RECORD_SECS  300     // max seconds per segment (auto-stop safety limit)
#define AVI_MAX_FRAMES   4500    // max frames per segment
#define REC_BTN_PIN      0       // BOOT button on XIAO ESP32S3

// AVI file header is exactly 224 bytes:
//   12 RIFF + 200 hdrl LIST + 12 movi LIST header
#define AVI_HEADER_SIZE  224

// Byte offsets of fields that get patched after recording
#define OFF_RIFF_SIZE      4
#define OFF_USEC_PER_FRAME 32
#define OFF_MAX_BYTES_SEC  36
#define OFF_TOTAL_FRAMES   48
#define OFF_SUGGEST_BUF    60
#define OFF_WIDTH          64
#define OFF_HEIGHT         68
#define OFF_RATE           132
#define OFF_LENGTH         140
#define OFF_SUGGEST_BUF2   144
#define OFF_RC_RIGHT       160
#define OFF_RC_BOTTOM      162
#define OFF_BI_WIDTH       176
#define OFF_BI_HEIGHT      180
#define OFF_BI_SIZE_IMAGE  192
#define OFF_MOVI_SIZE      216

// ----- internal state -----
struct AviIdxEntry { uint32_t offset; uint32_t size; };

static File           _aviFile;
static uint32_t       _aviFrameCnt;
static uint32_t       _aviMoviSize;
static uint32_t       _aviMaxFrame;
static unsigned long  _aviStartMs;
static int            _aviFileIdx = 0;
static AviIdxEntry*   _aviIdx     = nullptr;

// ----- button state -----
static volatile bool          _btnPressed  = false;
static volatile unsigned long _btnLastMs   = 0;

static void IRAM_ATTR _btnISR() {
  unsigned long now = millis();
  if (now - _btnLastMs > 300) {   // 300 ms debounce
    _btnPressed = true;
    _btnLastMs  = now;
  }
}

// little-endian helpers (safe on any alignment)
static inline void _put32(uint8_t* p, uint32_t v) {
  p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24;
}
static inline void _put16(uint8_t* p, uint16_t v) {
  p[0]=v; p[1]=v>>8;
}

// -----------------------------------------------------------
//  Build and write the 224-byte AVI RIFF / MJPEG header
//
//  Layout:
//    RIFF (size) 'AVI '
//      LIST (192) 'hdrl'
//        'avih' (56)  MainAVIHeader
//        LIST (116) 'strl'
//          'strh' (56) AVIStreamHeader  type=vids handler=MJPG
//          'strf' (40) BITMAPINFOHEADER biCompression=MJPG
//      LIST (movi_size) 'movi'
//        '00dc' ... frame data ...
//      'idx1' ...
// -----------------------------------------------------------
static bool _aviOpen(const char* path) {
  _aviFile = SD.open(path, FILE_WRITE);
  if (!_aviFile) return false;

  uint8_t h[AVI_HEADER_SIZE];
  memset(h, 0, sizeof(h));

  memcpy(h,      "RIFF", 4);
  memcpy(h + 8,  "AVI ", 4);

  memcpy(h + 12, "LIST", 4);
  _put32(h + 16, 192);
  memcpy(h + 20, "hdrl", 4);

  memcpy(h + 24, "avih", 4);
  _put32(h + 28, 56);
  _put32(h + 44, 0x10);          // AVIF_HASINDEX
  _put32(h + 56, 1);             // dwStreams = 1

  memcpy(h + 88,  "LIST", 4);
  _put32(h + 92,  116);
  memcpy(h + 96,  "strl", 4);

  memcpy(h + 100, "strh", 4);
  _put32(h + 104, 56);
  memcpy(h + 108, "vids", 4);    // fccType
  memcpy(h + 112, "MJPG", 4);    // fccHandler
  _put32(h + 128, 1);            // dwScale = 1
  _put32(h + 148, 0xFFFFFFFF);   // dwQuality = default

  memcpy(h + 164, "strf", 4);
  _put32(h + 168, 40);
  _put32(h + 172, 40);           // biSize
  _put16(h + 184, 1);            // biPlanes
  _put16(h + 186, 24);           // biBitCount
  memcpy(h + 188, "MJPG", 4);    // biCompression

  memcpy(h + 212, "LIST", 4);
  memcpy(h + 220, "movi", 4);

  _aviFile.write(h, AVI_HEADER_SIZE);

  _aviFrameCnt  = 0;
  _aviMoviSize  = 4;             // counts from 'movi' fourcc
  _aviMaxFrame  = 0;
  _aviStartMs   = millis();

  if (_aviIdx) { free(_aviIdx); _aviIdx = nullptr; }
  _aviIdx = (AviIdxEntry*)ps_malloc(AVI_MAX_FRAMES * sizeof(AviIdxEntry));
  return _aviIdx != nullptr;
}

// Write one JPEG frame as a '00dc' chunk inside the movi list
static bool _aviAddFrame(camera_fb_t* fb) {
  if (!_aviFile || _aviFrameCnt >= AVI_MAX_FRAMES) return false;

  uint32_t sz  = fb->len;
  uint32_t psz = (sz + 1) & ~1;               // pad to even boundary

  uint8_t ck[8];
  memcpy(ck, "00dc", 4);
  _put32(ck + 4, sz);

  if (_aviFile.write(ck, 8) != 8)   return false;
  if (_aviFile.write(fb->buf, sz) != sz) return false;
  if (psz > sz) { uint8_t z = 0; _aviFile.write(&z, 1); }

  _aviIdx[_aviFrameCnt].offset = _aviMoviSize; // relative to 'movi'
  _aviIdx[_aviFrameCnt].size   = sz;

  _aviMoviSize += 8 + psz;
  if (sz > _aviMaxFrame) _aviMaxFrame = sz;
  _aviFrameCnt++;
  return true;
}

// Write idx1, then seek back and patch all placeholder fields
static void _aviClose(uint16_t w, uint16_t h) {
  if (!_aviFile || _aviFrameCnt == 0) {
    if (_aviFile) _aviFile.close();
    if (_aviIdx) { free(_aviIdx); _aviIdx = nullptr; }
    return;
  }

  unsigned long durMs = millis() - _aviStartMs;
  uint32_t fps = durMs > 0 ? (_aviFrameCnt * 1000UL / durMs) : 10;
  if (fps < 1) fps = 1;

  // ---- write idx1 ----
  uint8_t ih[8];
  memcpy(ih, "idx1", 4);
  _put32(ih + 4, _aviFrameCnt * 16);
  _aviFile.write(ih, 8);

  for (uint32_t i = 0; i < _aviFrameCnt; i++) {
    uint8_t e[16];
    memcpy(e, "00dc", 4);
    _put32(e + 4,  0x10);               // AVIIF_KEYFRAME
    _put32(e + 8,  _aviIdx[i].offset);
    _put32(e + 12, _aviIdx[i].size);
    _aviFile.write(e, 16);
  }

  uint32_t totalSize = _aviFile.position();

  // ---- patch header ----
  uint8_t b4[4], b2[2];
  auto p32 = [&](uint32_t off, uint32_t v) {
    _aviFile.seek(off); _put32(b4, v); _aviFile.write(b4, 4);
  };
  auto p16 = [&](uint32_t off, uint16_t v) {
    _aviFile.seek(off); _put16(b2, v); _aviFile.write(b2, 2);
  };

  p32(OFF_RIFF_SIZE,      totalSize - 8);
  p32(OFF_USEC_PER_FRAME, 1000000 / fps);
  p32(OFF_MAX_BYTES_SEC,  _aviMaxFrame * fps);
  p32(OFF_TOTAL_FRAMES,   _aviFrameCnt);
  p32(OFF_SUGGEST_BUF,    _aviMaxFrame);
  p32(OFF_WIDTH,           w);
  p32(OFF_HEIGHT,          h);
  p32(OFF_RATE,            fps);
  p32(OFF_LENGTH,          _aviFrameCnt);
  p32(OFF_SUGGEST_BUF2,   _aviMaxFrame);
  p16(OFF_RC_RIGHT,        w);
  p16(OFF_RC_BOTTOM,       h);
  p32(OFF_BI_WIDTH,        w);
  p32(OFF_BI_HEIGHT,       h);
  p32(OFF_BI_SIZE_IMAGE,   w * h * 3);
  p32(OFF_MOVI_SIZE,       _aviMoviSize);

  _aviFile.close();
  free(_aviIdx);
  _aviIdx = nullptr;

  Serial.printf("AVI saved: %u frames, %u fps, %.1f KB\n",
                _aviFrameCnt, fps, totalSize / 1024.0);
}

// FreeRTOS task: button-controlled AVI recording
//   BOOT button press  -> start recording
//   BOOT button press  -> stop & save
//   auto-stops at AVI_RECORD_SECS or AVI_MAX_FRAMES
static void _recordingTask(void* param) {
  pinMode(REC_BTN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(REC_BTN_PIN), _btnISR, FALLING);

  Serial.println("Ready. Press BOOT button to start recording.");

  for (;;) {
    // ---- idle: wait for button press to start ----
    while (!_btnPressed) { vTaskDelay(pdMS_TO_TICKS(50)); }
    _btnPressed = false;

    char fname[32];
    sprintf(fname, "/vid%04d.avi", _aviFileIdx);
    Serial.printf(">> REC START: %s\n", fname);

    if (!_aviOpen(fname)) {
      Serial.println("Cannot create AVI file!");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    uint16_t fw = 0, fh = 0;
    unsigned long tStart = millis();

    // ---- recording: capture frames until button or time limit ----
    while (!_btnPressed &&
           (millis() - tStart) < ((unsigned long)AVI_RECORD_SECS * 1000) &&
           _aviFrameCnt < AVI_MAX_FRAMES) {
      camera_fb_t* fb = esp_camera_fb_get();
      if (!fb) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

      if (fw == 0) { fw = fb->width; fh = fb->height; }

      if (!_aviAddFrame(fb)) {
        esp_camera_fb_return(fb);
        Serial.println("Write failed, ending segment");
        break;
      }
      esp_camera_fb_return(fb);
    }

    _btnPressed = false;
    _aviClose(fw, fh);
    _aviFileIdx++;

    Serial.println(">> REC STOP. Press BOOT button to start new recording.");
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// Scan SD root for existing vidXXXX.avi, return next available index
static int _findNextFileIdx() {
  int maxIdx = -1;
  File root = SD.open("/");
  if (!root) return 0;
  File f = root.openNextFile();
  while (f) {
    const char* name = f.name();
    // match "vidXXXX.avi" (name may or may not start with '/')
    const char* p = name;
    if (*p == '/') p++;
    int idx;
    if (sscanf(p, "vid%d.avi", &idx) == 1 && idx > maxIdx) {
      maxIdx = idx;
    }
    f = root.openNextFile();
  }
  root.close();
  return maxIdx + 1;
}

// Call once after SD.begin() succeeds -- sets up button + background task
void startRecording() {
  _aviFileIdx = _findNextFileIdx();
  Serial.printf("Next video index: %d\n", _aviFileIdx);
  xTaskCreatePinnedToCore(_recordingTask, "aviRec", 10240, NULL, 1, NULL, 0);
}

#endif // AVI_RECORDER_H
