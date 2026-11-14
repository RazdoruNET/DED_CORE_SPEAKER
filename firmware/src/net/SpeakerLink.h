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

  /// В главном цикле ничего не делает: сокет живёт в своей задаче (wsLoop).
  /// Оставлено, чтобы не переписывать вызовы и было видно, что это намеренно.
  void loop();
  void sendHello();
  bool isConnected() const { return _connected; }
  bool isStarted() const { return _started; }
  bool isLinkUp() const { return _started; }

  void onAudio(AudioHandler handler) { _onAudio = std::move(handler); }

  /// Кусок трека в MP3 от сервера: устройство декодирует его само.
  void onMedia(AudioHandler handler) { _onMedia = std::move(handler); }
  void onCommand(CommandHandler handler) { _onCommand = std::move(handler); }
  void onSpeechBegin(SpeechHandler handler) { _onSpeechBegin = std::move(handler); }
  void onSpeechEnd(VoidHandler handler) { _onSpeechEnd = std::move(handler); }

  /// Прозвучало слово-активатор. Приходит с сервера: только он знает, было ли
  /// обращение, — поэтому колонка не «просыпается» от любого звука в комнате.
  void onActivated(VoidHandler handler) { _onActivated = std::move(handler); }

  /// Команда принята и ушла помощнику: можно показать «думаю».
  void onThinking(VoidHandler handler) { _onThinking = std::move(handler); }

  /// Уровень речи агента (0–255). Приходит, когда ответ звучит в браузере:
  /// колонка этот звук не слышит, но рот должен двигаться.
  ///
  /// Второй аргумент — идёт ли речь прямо сейчас. Между слогами уровень падает
  /// в ноль, но ответ ещё не кончился: по этому флагу колонка не выходит из
  /// состояния «говорю» до конца фразы.
  using LevelHandler = std::function<void(uint8_t level, bool active)>;
  void onVoiceLevel(LevelHandler handler) { _onVoiceLevel = std::move(handler); }

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

  /// Сколько звука пришло от сервера: по этим двум числам видно, доехала ли
  /// речь агента до колонки, даже если её не слышно.
  uint32_t vadFrames() const { return _vadFrames; }
  uint32_t sayFrames() const { return _sayFrames; }
  uint32_t sayBytes() const { return _sayBytes; }
  void sendVad(bool start, uint32_t rms);
  void sendEvent(const char* kind);
  void sendState(uint32_t freeHeap, int32_t rssi);

 private:
  static void wsTaskEntry(void* arg);
  void wsLoop();
  void pump();
  void connect();
  void handleText(const uint8_t* payload, size_t length);
  void handleBinary(const uint8_t* payload, size_t length);
  void handleHelloAck(JsonVariantConst doc);

  WebSocketsClient _ws;
  TaskHandle_t _wsTask = nullptr;
  volatile bool _started = false;
  volatile bool _connected = false;
  uint8_t _attempts = 0;
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
  volatile uint32_t _vadFrames = 0;
  volatile uint32_t _sayFrames = 0;
  volatile uint32_t _sayBytes = 0;
  uint32_t _lastMicSendAt = 0;
  volatile bool _needHello = false;

  AudioHandler _onAudio;
  AudioHandler _onMedia;
  CommandHandler _onCommand;
  SpeechHandler _onSpeechBegin;
  VoidHandler _onSpeechEnd;
  VoidHandler _onActivated;
  VoidHandler _onThinking;
  LevelHandler _onVoiceLevel;
};

extern SpeakerLink speakerLink;
