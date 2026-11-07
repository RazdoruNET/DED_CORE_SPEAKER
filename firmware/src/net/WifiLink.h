#pragma once

#include <Arduino.h>

// Подключение к домашней сети с откатом: при обрыве колонка не должна
// перезагружаться, она просто повторяет попытки всё реже.
class WifiLink {
 public:
  void begin();
  void loop();
  bool isConnected() const;
  String localAddress() const;

  // Время нужно до TLS: без него часы стоят на 1970 году, и проверка срока
  // действия сертификата отвергает соединение (X509 -9984).
  bool isTimeSynced() const;

 private:
  void attempt();

  bool _started = false;
  bool _timeSyncStarted = false;
  uint32_t _nextAttemptAt = 0;
  uint32_t _backoffMs = 5000;
  uint32_t _lastCheck = 0;
};

extern WifiLink wifiLink;
