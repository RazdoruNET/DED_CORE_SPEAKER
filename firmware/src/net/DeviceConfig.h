#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "config/AppConfig.h"

// Настройки устройства в NVS.
//
// Часто меняющиеся величины (громкость) пишутся с задержкой: у ESP32 флеш
// изнашивается, а putUShort на каждом шаге убил бы её за месяцы.
class DeviceConfig {
 public:
  bool begin();

  // Сеть и платформа
  String wifiSsid;
  String wifiPass;
  String apiBase;
  String deviceKey;    // долговременный ключ, приходит после привязки
  String pairToken;    // одноразовый токен привязки, живёт до первого HELLO_ACK

  // Кто мы для человека
  String name;
  String room;

  uint8_t volume = 60;
  bool muted = false;
  uint32_t rulesVersion = 0;

  bool isPaired() const { return deviceKey.length() > 0 || pairToken.length() > 0; }
  bool hasNetwork() const { return wifiSsid.length() > 0; }

  void saveNetwork();
  void saveIdentity();
  void saveVolume();
  void clear();
  void tickVolume();

 private:
  Preferences _prefs;
  bool _volumeDirty = false;
  uint32_t _volumeChangedAt = 0;
};

extern DeviceConfig deviceConfig;
