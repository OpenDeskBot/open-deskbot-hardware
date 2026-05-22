#ifndef Common_h
#define Common_h

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <FFat.h>
#include "logger.h"
#include "led.h"
#include "oled.h"

#define VERSION "2.0.1"
#define PRODUCT_NAME "Deskbot"
#define AP_SSID "Deskbot_Rom"
#define UDP_DISCOVERY_PREFIX "Deskbot"

#ifndef RECORD_TIME
#define RECORD_TIME 10
#endif

extern unsigned long last_time;
extern bool enable_act;
extern bool start_chat;

void setup_FFat();
String get_mac_address();
/** WebSocket device_id；编译时由 flash 脚本注入 DEVICE_ID，否则回退 deskbot_<mac> */
const char* get_device_id();
String get_local_ip();

#endif
