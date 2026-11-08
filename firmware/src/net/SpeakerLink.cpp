#include "net/SpeakerLink.h"

#include <new>

#include <time.h>

#include "app/Beeper.h"
#include "app/Trace.h"

#include <WiFi.h>

#include "config/AppConfig.h"
#include "net/DeviceConfig.h"
#include "net/PublicRoots.h"
#include "net/DeviceIdentity.h"
#include "proto/Protocol.h"

SpeakerLink speakerLink;

namespace {

// Длительность одного кадра звука: по ней выравнивается темп отправки.
constexpr uint32_t kMicFrameMs = 1000u * AUDIO_FRAME_SAMPLES / AUDIO_RATE;

// Отправка сорвалась: кадр ушёл не целиком, поток потерял синхронизацию.
bool sendFailed = false;

// Цепочка доверия: по умолчанию только CA платформы.
//
// Публичные корни рядом с пином — это не «запасной путь», а его отмена: доверие
// расширяется до любого публичного CA (и до корпоративного MITM-прокси), и подделать
// сервер сможет тот, кто получит у него сертификат на наш домен. В HELLO при этом
// уходит ключ устройства, то есть последствие — захват аккаунта колонки.
//
// Если сервер серии подписан не CA платформы, включите SPEAKER_ALLOW_PUBLIC_ROOTS
// осознанно (см. docs/09-production-security.md) — по умолчанию флага нет.
// Возвращаем ссылку на статическую строку: библиотека хранит указатель, а не копию,
// и копия на стеке к моменту подключения уже мертва.
const String& trustedRoots() {
  static String chain;
  if (chain.isEmpty()) {
#ifdef SPEAKER_ALLOW_PUBLIC_ROOTS
    chain.reserve(strlen(SPEAKER_ROOT_CA_PEM) + strlen(SPEAKER_PUBLIC_ROOTS_PEM) + 2);
    chain += SPEAKER_ROOT_CA_PEM;
    chain += "\n";
    chain += SPEAKER_PUBLIC_ROOTS_PEM;
#else
    chain.reserve(strlen(SPEAKER_ROOT_CA_PEM) + 1);
    chain += SPEAKER_ROOT_CA_PEM;
#endif
  }

  return chain;
}

bool splitBase(const String& base, String& host, uint16_t& port, String& path) {
  if (!base.startsWith("https://")) {
    return false;
  }

  String rest = base.substring(8);
  if (rest.length() == 0) {
    return false;
  }

  const int slash = rest.indexOf('/');
  String authority = slash >= 0 ? rest.substring(0, slash) : rest;
  path = slash >= 0 ? rest.substring(slash) : String("");
  while (path.endsWith("/")) {
    path.remove(path.length() - 1);
  }
  path += "/api/assistant/speaker/stream";

  const int colon = authority.indexOf(':');
  if (colon >= 0) {
    host = authority.substring(0, colon);
    const long parsedPort = authority.substring(colon + 1).toInt();
    if (parsedPort < 1 || parsedPort > 65535) {
      return false;
    }
    port = static_cast<uint16_t>(parsedPort);
  } else {
    host = authority;
    port = 443;
  }

  return host.length() > 0 && host.indexOf('@') < 0 && host.indexOf('?') < 0
         && host.indexOf('#') < 0;
}

bool sendFrame(WebSocketsClient& ws, proto::Type type, uint16_t streamId, uint16_t seq,
               const uint8_t* payload, size_t length) {
  if (length > proto::MAX_PAYLOAD) {
    length = proto::MAX_PAYLOAD;
  }

  // Буфер в куче, а не на стеке: кадр микрофона — 640 байт, и на стеке задачи
  // записи это вместе с остальным уже приводило к переполнению и падению.
  const size_t total = proto::HEADER_SIZE + length;
  auto* buffer = new (std::nothrow) uint8_t[total];
  if (buffer == nullptr) {
    return false;
  }

  proto::writeHeader(buffer, static_cast<uint8_t>(type), 0, streamId, seq);
  if (payload != nullptr && length > 0) {
    memcpy(buffer + proto::HEADER_SIZE, payload, length);
  }
  trace::mark(type == proto::MIC ? "ws.send.mic" : "ws.send.ctl");
  const bool sent = ws.sendBIN(buffer, total);
  delete[] buffer;

  // Неудачная запись рвёт кадр посередине: поток WebSocket теряет
  // синхронизацию, и дальше соединение живёт до первого таймаута на сервере.
  // Честнее признать обрыв и подключиться заново.
  if (!sent) {
    sendFailed = true;
  }

  return sent;
}

void sendJson(WebSocketsClient& ws, proto::Type type, uint16_t streamId, DynamicJsonDocument& doc) {
  String json;
  serializeJson(doc, json);
  sendFrame(ws, type, streamId, 0,
            reinterpret_cast<const uint8_t*>(json.c_str()), json.length());
}

}  // namespace

void SpeakerLink::begin() {
  if (_started) {
    return;
  }

  String host;
  String path;
  uint16_t port = 0;
  if (!splitBase(deviceConfig.apiBase, host, port, path)) {
    applog::line("[ws] нужен корректный HTTPS-адрес сервера");
    return;
  }

  if (SPEAKER_ROOT_CA_PEM[0] == '\0') {
    applog::line("[ws] TLS CA не настроен — отказ от соединения (insecure запрещён)");
    return;
  }

  applog::printf("[ws] %s:%u%s\n", host.c_str(), port, path.c_str());

  _ws.onEvent([this](WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
      case WStype_CONNECTED:
        // Приветствие уходит из главного цикла: колбэк вызывается внутри
        // разбора входящих данных, и запись в сокет оттуда ломает состояние
        // TLS-сессии — сервер получал мусор, а куча повреждалась.
        _connected = true;
        _needHello = true;
        beeper::connected();
        break;
      case WStype_DISCONNECTED:
        if (_connected) {
          applog::line("[ws] соединение потеряно");
        }
        _connected = false;
        break;
      case WStype_TEXT:
        handleText(payload, length);
        break;
      case WStype_BIN:
        handleBinary(payload, length);
        break;
      default:
        break;
    }
  });

  _host = host;
  _port = port;
  _path = path;

  _ws.setReconnectInterval(WS_RECONNECT_MIN);
  _ws.enableHeartbeat(WS_HEARTBEAT_MS, 3000, 2);

  _ws.beginSslWithCA(host.c_str(), port, path.c_str(), trustedRoots().c_str());

  // Очередь служебных кадров: их готовят разные задачи, а отправляет главный цикл.
  if (_controlQueue == nullptr) {
    _controlQueue = xQueueCreate(8, sizeof(ControlPacket));
    if (_controlQueue == nullptr) {
      applog::line("[ws] нет очереди служебных кадров");
    }
  }

  _started = true;
}

void SpeakerLink::sendHello() {
  DynamicJsonDocument hello(384);
  hello["device"] = speakerDeviceId();
  hello["product"] = DEVICE_PRODUCT;
  hello["fw"] = SPEAKER_FIRMWARE_VERSION;
  hello["hw"] = SPEAKER_PROFILE_NAME;
  hello["volume"] = deviceConfig.volume;
  hello["muted"] = _muted;
  hello["codec"] = "pcm16";
  hello["rate"] = AUDIO_RATE;
  if (_publicKeyHex.length() > 0) {
    // Отпечаток ключа сервер сверяет до выдачи deviceKey: токен привязки
    // выпущен именно под этот ключ.
    hello["pub"] = _publicKeyHex;
  }
  if (deviceConfig.deviceKey.length() > 0) {
    hello["key"] = deviceConfig.deviceKey;
  } else {
    hello["token"] = deviceConfig.pairToken;
  }
  sendJson(_ws, proto::HELLO, 0, hello);
  applog::printf("[ws] отправлен HELLO (ключ %u, токен %u)\n",
                 (unsigned)deviceConfig.deviceKey.length(),
                 (unsigned)deviceConfig.pairToken.length());
}

void SpeakerLink::loop() {
  if (_started) {
    _ws.loop();
  }

  if (sendFailed) {
    sendFailed = false;
    applog::line("[ws] запись не удалась — пересоздаю соединение");
    _connected = false;
    _ws.disconnect();
  }

  // Бывает, что TCP-соединение не поднимается вообще: SYN уходит, ответ не
  // доходит, и библиотека молча повторяет попытки до бесконечности. Лечится
  // только пересозданием клиента, а если не помогло — переподключением Wi-Fi.
  if (_started && !_connected) {
    const uint32_t now = millis();
    if (_downSince == 0) {
      _downSince = now;
    } else if (now - _downSince > WS_RESET_AFTER_MS) {
      _downSince = now;
      ++_resetCount;
      applog::printf("[ws] связи нет %u с — пересоздаю клиент (попытка %u)\n",
                     (unsigned)(WS_RESET_AFTER_MS / 1000), (unsigned)_resetCount);

      _ws.disconnect();

      if (_resetCount % 2 == 0) {
        applog::line("[ws] переподключаю Wi-Fi");
        WiFi.disconnect();
        WiFi.reconnect();
      } else {
        _ws.beginSslWithCA(_host.c_str(), _port, _path.c_str(), trustedRoots().c_str());
      }
    }
  } else if (_connected) {
    _downSince = 0;
    _resetCount = 0;
  }

  if (_connected && _needHello) {
    _needHello = false;
    sendHello();
  }

  // Служебные кадры, накопленные другими задачами: отправляем из главного
  // цикла, в одном потоке с _ws.loop().
  if (_controlQueue != nullptr && _connected) {
    ControlPacket control;
    while (xQueueReceive(_controlQueue, &control, 0) == pdTRUE) {
      sendFrame(_ws, control.type, 0, 0,
                reinterpret_cast<const uint8_t*>(control.json), control.length);
    }
  }

  // Отправка звука — только отсюда, из одного потока с _ws.loop().
  // Не чаще реального времени: кадр — это 20 мс записи. Без этого ограничения
  // преролл выстреливает десятками кадров за один проход, mbedTLS не успевает
  // отправить буфер и отдаёт внутреннюю ошибку, а поток WebSocket теряет
  // синхронизацию (на сервере это видно как обрыв).
  if (_micQueue != nullptr && _connected) {
    const uint32_t now = millis();
    if (now - _lastMicSendAt >= kMicFrameMs) {
      MicPacket packet;
      if (xQueueReceive(_micQueue, &packet, 0) == pdTRUE) {
        if (sendFrame(_ws, proto::MIC, _micStream, _micSeq++, packet.data, packet.length)) {
          ++_micFrames;
        }
        _lastMicSendAt = now;
      }
    }
  }

  // Ключ из HELLO_ACK сохраняем здесь, а не в сетевой задаче: запись в NVS
  // занимает время и не должна идти в колбэке приёма.
  if (_pendingKey.length() > 0) {
    const String key = _pendingKey;
    _pendingKey = "";
    deviceConfig.deviceKey = key;
    deviceConfig.pairToken = "";
    deviceConfig.saveIdentity();
    applog::printf("[ws] ключ устройства получен (%u символов) и сохранён\n",
                   (unsigned)key.length());
    beeper::paired();
  }
}

void SpeakerLink::sendMic(const uint8_t* data, size_t length) {
  if (!_connected || data == nullptr || length == 0) {
    return;
  }

  if (_micQueue == nullptr) {
    _micQueue = xQueueCreate(16, sizeof(MicPacket));
    if (_micQueue == nullptr) {
      return;
    }
  }

  MicPacket packet{};
  packet.length = static_cast<uint16_t>(length < sizeof(packet.data) ? length : sizeof(packet.data));
  memcpy(packet.data, data, packet.length);

  // Если очередь переполнена, кадр теряем: лучше потерять 20 мс звука, чем
  // заблокировать задачу записи.
  xQueueSend(_micQueue, &packet, 0);
}

void SpeakerLink::postControl(proto::Type type, const char* json) {
  if (!_connected || _controlQueue == nullptr || json == nullptr) {
    return;
  }

  ControlPacket packet;
  packet.type = type;
  const size_t length = strlen(json);
  packet.length = static_cast<uint16_t>(length < sizeof(packet.json) - 1 ? length : sizeof(packet.json) - 1);
  memcpy(packet.json, json, packet.length);
  packet.json[packet.length] = '\0';

  // Переполнение очереди — не беда: служебные кадры повторяются.
  xQueueSend(_controlQueue, &packet, 0);
}

void SpeakerLink::sendVad(bool start, uint32_t rms) {
  DynamicJsonDocument doc(128);
  doc["event"] = start ? "start" : "end";
  doc["rms"] = rms;

  String json;
  serializeJson(doc, json);
  postControl(proto::VAD, json.c_str());
}

void SpeakerLink::sendEvent(const char* kind) {
  DynamicJsonDocument doc(128);
  doc["kind"] = kind;

  String json;
  serializeJson(doc, json);
  postControl(proto::EVENT, json.c_str());
}

void SpeakerLink::sendState(uint32_t freeHeap, int32_t rssi) {
  if (!_connected) {
    return;
  }
  DynamicJsonDocument doc(256);
  doc["volume"] = deviceConfig.volume;
  doc["muted"] = _muted;
  doc["heap"] = freeHeap;
  doc["rssi"] = rssi;
  doc["uptime"] = millis() / 1000;
  doc["fw"] = SPEAKER_FIRMWARE_VERSION;

  String json;
  serializeJson(doc, json);
  postControl(proto::STATE, json.c_str());
}

void SpeakerLink::handleBinary(const uint8_t* payload, size_t length) {
  const proto::Frame frame = proto::readFrame(payload, length);
  switch (frame.type) {
    case proto::HELLO_ACK: {
      // Разбор лёгкий и без ArduinoJson: колбэк вызывается из сетевой задачи,
      // а любое выделение памяти здесь уже приводило к повреждению кучи.
      // Ключ копируем строкой, а в NVS сохраняет главный цикл.
      const char* text = reinterpret_cast<const char*>(frame.payload);
      const size_t size = frame.length;
      const char key[] = "\"deviceKey\":\"";

      size_t found = size;
      for (size_t i = 0; i + sizeof(key) - 1 <= size; ++i) {
        if (memcmp(text + i, key, sizeof(key) - 1) == 0) {
          found = i + sizeof(key) - 1;
          break;
        }
      }

      if (found >= size) {
        // Это обычный обмен: колонка уже вошла по ключу, и новый ей не нужен.
        applog::line("[ws] сервер подтвердил ключ");
        break;
      }

      String value;
      value.reserve(64);
      for (size_t i = found; i < size && text[i] != '"' && value.length() < 64; ++i) {
        value += text[i];
      }

      _pendingKey = value;
      break;
    }
    case proto::EVENT: {
      // Сервер возвращает распознанный текст: пока нет усилителя, это
      // единственный видимый результат работы «ушей».
      const char* text = reinterpret_cast<const char*>(frame.payload);
      const size_t size = frame.length;
      const char marker[] = "\"text\":\"";

      for (size_t i = 0; i + sizeof(marker) - 1 <= size; ++i) {
        if (memcmp(text + i, marker, sizeof(marker) - 1) != 0) {
          continue;
        }

        String value;
        value.reserve(160);
        for (size_t j = i + sizeof(marker) - 1; j < size && text[j] != '"' && value.length() < 160; ++j) {
          value += text[j];
        }

        if (value.length() > 0) {
          applog::printf("[ws] услышала: %s\n", value.c_str());
          beeper::listening();
        }
        break;
      }
      break;
    }
    case proto::SAY:
    case proto::INTERCOM:
      if (_onAudio) {
        _onAudio(frame.payload, frame.length);
      }
      break;
    default:
      break;
  }
}

void SpeakerLink::handleText(const uint8_t* payload, size_t length) {
  // Управляющие кадры приходят с заголовком и текстовым JSON внутри.
  const proto::Frame frame = proto::readFrame(payload, length);
  const uint8_t* body = frame.length > 0 ? frame.payload : payload;
  const size_t bodyLength = frame.length > 0 ? frame.length : length;

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, body, bodyLength)) {
    return;
  }

  switch (frame.type) {
    case proto::HELLO_ACK:
      handleHelloAck(doc.as<JsonVariantConst>());
      break;
    case proto::SPEECH_BEGIN:
      if (_onSpeechBegin) {
        _onSpeechBegin(frame.streamId, String(doc["text"] | ""));
      }
      break;
    case proto::SPEECH_END:
      if (_onSpeechEnd) {
        _onSpeechEnd();
      }
      break;
    case proto::CMD:
      if (_onCommand) {
        _onCommand(String(doc["action"] | ""), doc["args"].as<JsonVariantConst>());
      }
      break;
    default:
      break;
  }
}

void SpeakerLink::handleHelloAck(JsonVariantConst doc) {
  const String key = doc["deviceKey"] | "";
  if (key.length() > 0) {
    // Одноразовый токен привязки обменян на долговременный ключ.
    deviceConfig.deviceKey = key;
    deviceConfig.pairToken = "";
    deviceConfig.saveIdentity();
    applog::line("[ws] привязка подтверждена сервером, ключ устройства сохранён");
  }

  const String voice = doc["voice"] | "";
  applog::printf("[ws] HELLO_ACK: голос «%s»\n", voice.c_str());
}
