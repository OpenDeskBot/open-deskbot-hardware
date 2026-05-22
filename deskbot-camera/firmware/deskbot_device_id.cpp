#include "deskbot_device_id.h"

#include <WiFi.h>
#include <cstdio>

const char* deskbot_device_id() {
#ifdef DESKBOT_DEVICE_ID
  return DESKBOT_DEVICE_ID;
#else
  static char id[32];
  static bool initialized = false;
  if (!initialized) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(id, sizeof(id), "deskbot_%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    initialized = true;
  }
  return id;
#endif
}
