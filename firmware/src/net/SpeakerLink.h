#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>

#include <functional>

#include "proto/Protocol.h"

// Один бинарный WebSocket до AssistantService.
//
// Звук идёт только кадрами MIC/SAY, управление — JSON. Формат кадра —
// src/proto/Protocol.h, описание — docs/02-audio-protocol.md.
class SpeakerLink {
 public:
  using AudioHandler = std::function<void(const uint8_t* data, size_t length)>;
  using CommandHandler = std::function<void(const String& action, JsonVariantConst args)>;
  using SpeechHandler = std::function<void(uint16_t streamId, const String& text)>;
  using VoidHandler = std::function<void()>;

  void begin();
  void loop();
  void sendHello();
  bool isConnected() const { return _connected; }
  bool isStarted() const { return _started; }
  bool isLinkUp() const { return _started; }

  void onAudio(AudioHandler handler) { _onAudio = std::move(handler); }
  void onCommand(CommandHandler handler) { _onCommand = std::move(handler); }
  void onSpeechBegin(SpeechHandler handler) { _onSpeechBegin = std::move(handler); }
  void onSpeechEnd(VoidHandler handler) { _onSpeechEnd = std::move(handler); }

  /// Открытый ключ устройства для HELLO. Ставится один раз при старте: по нему
  /// сервер сверяет отпечаток из записи привязки перед выдачей ключа.
  void setPublicKeyHex(const String& hex) { _publicKeyHex = hex; }

  /// Действующее состояние микрофона: программный mute плюс физический
  /// переключатель. Сервер по нему знает, что кадры идти не должны.
  void setMuteState(bool muted) { _muted = muted; }

  void sendMic(const uint8_t* data, size_t length);

  /// Служебный кадр (VAD, событие, состояние) из любой задачи. В сокет он уйдёт
  /// из главного цикла: WebSockets не потокобезопасен, а звук и детекторы живут
  /// в задаче записи — запись оттуда ломала TLS и подвешивала соединение.
  void postControl(proto::Type type, const char* json);

  // Счётчик отправленных кадров звука: по нему видно, идёт ли поток, когда
  // куча начинает разваливаться.
  uint32_t micFrames() const { return _micFrames; }
  void sendVad(bool start, uint32_t rms);
  void sendEvent(const char* kind);
  void sendState(uint32_t freeHeap, int32_t rssi);

 private:
  void connect();
  void handleText(const uint8_t* payload, size_t length);
  void handleBinary(const uint8_t* payload, size_t length);
  void handleHelloAck(JsonVariantConst doc);

  WebSocketsClient _ws;
  bool _started = false;
  bool _connected = false;
  String _pendingKey;
  String _publicKeyHex;
  volatile bool _muted = false;

  // Перезапуск «залипшего» соединения: адрес нужен, чтобы поднять клиент заново.
  String _host;
  uint16_t _port = 443;
  String _path;
  uint32_t _downSince = 0;
  uint8_t _resetCount = 0;

  // Кадры микрофона приходят из другой задачи, а WebSockets не потокобезопасен:
  // отправляем их из главного цикла через очередь.
  struct MicPacket {
    uint16_t length;
    uint8_t data[640];
  };
  QueueHandle_t _micQueue = nullptr;

  struct ControlPacket {
    proto::Type type;
    uint16_t length;
    char json[256];
  };
  QueueHandle_t _controlQueue = nullptr;
  uint16_t _micStream = 1;
  uint16_t _micSeq = 0;
  volatile uint32_t _micFrames = 0;
  uint32_t _lastMicSendAt = 0;
  volatile bool _needHello = false;

  AudioHandler _onAudio;
  CommandHandler _onCommand;
  SpeechHandler _onSpeechBegin;
  VoidHandler _onSpeechEnd;
};

extern SpeakerLink speakerLink;
