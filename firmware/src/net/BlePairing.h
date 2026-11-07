#pragma once

#include <Arduino.h>
#include <functional>

#include "net/PairingCrypto.h"

// Привязка по Bluetooth: единственный способ настроить колонку.
//
// Окно привязки открывается не всегда, а по действию человека (первое
// включение, удержание кнопки, команда из приложения) — иначе колонку можно
// было бы «увести» к себе прямо из подъезда.
//
// GATT:
//   info  (read,   notify) — id, версия, открытый ключ, открыто ли окно
//   pair  (write)          — кадр привязки по частям
//   wifi  (read)           — список сетей вокруг: браузер не умеет их перечислять
//   status(read,   notify) — этап и ошибка
//
// Приватность: открытый ключ здесь нужен сайту только чтобы сверить его с тем,
// что отдал сервер. Доверять ключу из эфира нельзя.
class BlePairing {
 public:
  using PairedHandler = std::function<void(const String& room, const String& name)>;

  bool begin(PairingCrypto* crypto, const String& deviceId, PairedHandler onPaired);
  void stop();
  bool isActive() const { return _active; }
  bool isBusy() const { return _busy; }

  void tick();

  // Вызываются колбэками GATT.
  void handleChunk(const uint8_t* data, size_t length);

 private:
  void handleFrame();
  void scanNetworks();
  void report(const String& stage, bool ok, const String& error);

  uint32_t _startedAt = 0;
  bool _active = false;
  bool _busy = false;
  PairingCrypto* _crypto = nullptr;
  String _deviceId;
  PairedHandler _onPaired;

  // Кадр привязки приходит кусками: 1 версия + 65 ключ + 12 nonce + шифртекст
  // с тегом. JSON с grant занимает ~640 байт, поэтому предел — 1024: кадр
  // больше просто не принимаем, вместо чтения за границей буфера.
  static constexpr size_t kFrameCapacity = 1024;
  uint8_t _frame[kFrameCapacity] = {0};
  size_t _frameLength = 0;
  size_t _frameExpected = 0;
  char _challenge[33] = {0};
};

extern BlePairing blePairing;
