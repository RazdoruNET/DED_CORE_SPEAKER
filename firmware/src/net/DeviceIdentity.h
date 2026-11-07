#pragma once

#include <WiFi.h>

#include "config/AppConfig.h"

// Идентификатор устройства: последние шесть байт MAC.
// Он же печатается на наклейке рядом с кодом привязки и по нему сервер находит
// запись устройства — поэтому формат менять нельзя без миграции.
inline String speakerDeviceId() {
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  char buffer[13] = {0};
  snprintf(buffer, sizeof(buffer), "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buffer);
}
