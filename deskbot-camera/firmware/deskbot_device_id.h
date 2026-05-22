#pragma once

#include <Arduino.h>

/** WebSocket device_id；编译时由 flash 脚本注入 DESKBOT_DEVICE_ID，否则回退 deskbot_<mac> */
const char* deskbot_device_id();
