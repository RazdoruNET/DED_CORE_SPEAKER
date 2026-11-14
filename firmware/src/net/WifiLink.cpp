#include "net/WifiLink.h"
#include "config/AppConfig.h"

#include <WiFi.h>
#include <time.h>

#include "net/DeviceConfig.h"

WifiLink wifiLink;

namespace {
constexpr uint32_t kConnectTimeoutMs = 12000;
constexpr uint32_t kReconnectMaxMs = 5 * 60 * 1000UL;
}  // namespace

void WifiLink::begin() {
  if (!deviceConfig.hasNetwork()) {
    applog::line("[wifi] сеть не настроена");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // сон Wi-Fi даёт рывки в аудиопотоке

  // Потолок мощности передатчика. Пиковый ток Wi-Fi — самая большая нагрузка
  // на 5 В шине: на слабом порту USB (или длинном тонком кабеле) просадка
  // уходит ниже порога детектора и плата перезагружается с BROWNOUT_RST ровно
  // в момент подключения к сети. Для домашней колонки 11 дБм хватает с запасом,
  // а пиковый ток падает почти вдвое.
  WiFi.setTxPower(SPEAKER_WIFI_TX_POWER);

  WiFi.setAutoReconnect(true);
  _started = true;
  attempt();
}

void WifiLink::attempt() {
  applog::printf("[wifi] подключаюсь к «%s»\n", deviceConfig.wifiSsid.c_str());
  WiFi.begin(deviceConfig.wifiSsid.c_str(), deviceConfig.wifiPass.c_str());
  _lastCheck = millis();
}

void WifiLink::loop() {
  if (!_started) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (_backoffMs != 5000) {
      _backoffMs = 5000;
    }

    // Запускаем синхронизацию один раз после подключения к сети.
    if (!_timeSyncStarted) {
      _timeSyncStarted = true;
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      applog::line("[time] запросил время у NTP");
    }

    return;
  }

  const uint32_t now = millis();

  // Ещё идёт попытка подключения — ждём таймаута.
  if (now - _lastCheck < kConnectTimeoutMs && _nextAttemptAt == 0) {
    return;
  }

  if (_nextAttemptAt == 0) {
    applog::line("[wifi] не удалось, повторю позже");
    _nextAttemptAt = now + _backoffMs;
    return;
  }

  if (now < _nextAttemptAt) {
    return;
  }

  _backoffMs = _backoffMs * 2 > kReconnectMaxMs ? kReconnectMaxMs : _backoffMs * 2;
  _nextAttemptAt = 0;
  attempt();
}

bool WifiLink::isConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

String WifiLink::localAddress() const {
  return WiFi.localIP().toString();
}

bool WifiLink::isTimeSynced() const {
  // 2023-11-14: любое время раньше означает, что часы ещё не выставились.
  return time(nullptr) > 1700000000;
}
