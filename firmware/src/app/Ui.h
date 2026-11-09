#pragma once

#include <Arduino.h>

// Состояние агента — то, что лента показывает цветом и движением.
// Это язык, а не индикатор: человек должен понимать фазу, не глядя в приложение.
enum class AgentState : uint8_t {
  Boot,       // загрузка: точки со щёк сходятся к рту, остаётся голубой огонёк
  Unpaired,   // не настроена: синие щёки, тихий рот, ждёт кнопку
  Pairing,    // ждём телефон: синяя искра по щекам
  Idle,       // простой: пастель на щеках, рот спокойно дышит
  Listening,  // слушаю человека: рот повторяет его громкость
  Thinking,   // думаю: фиолетовая искра по щекам
  Speaking,   // говорю: рот раскрывается по громкости речи
  Offline,    // нет сервера: синяя искра по щекам, пока не подключится
  Error,      // ошибка: красное дыхание щёк
  Incoming,   // входящий звонок: стробоскоп
  Intercom,   // разговор: ровные зелёные щёки, рот по звуку
};

class Ui {
 public:
  void begin();
  void tick();

  void setState(AgentState state);
  AgentState state() const { return _state; }

  /// Уровень голоса агента (0–255). Рот двигается только по нему: человек
  /// должен видеть говорящего, а не собственный голос.
  void setVoiceLevel(uint8_t level) { _voiceLevel = level; }

  /// Уровень звука с микрофона (0–255). По нему живут уши — «слышит».
  void setHearingLevel(uint8_t level) { _hearingLevel = level; }

  /// Шкалы микрофонов. Диод 17 → центр — левый микрофон, диод 1 → центр —
  /// правый: видно, с какой стороны звук громче.
  void setMicLevels(uint8_t left, uint8_t right) {
    _micLeft = left;
    _micRight = right;
  }
  /// Вспышка всей ленты поверх любого состояния: короткий отклик на событие.
  /// Нужна, чтобы «услышала обращение» было видно с любого угла комнаты.
  void flash(uint32_t color, uint32_t milliseconds) {
    _flashColor = color;
    _flashUntil = millis() + milliseconds;
  }

  void setMuted(bool muted) { _muted = muted; }
  bool isMuted() const { return _muted; }

  // Кнопки: сначала тикнули, потом спрашиваем, было ли событие.
  bool takeActionShort();
  bool takeActionLong();
  bool takeVolumeUp();
  bool takeVolumeDown();

 private:
  void render();
  void pollButtons();
  void applyState(AgentState state);

  AgentState _state = AgentState::Boot;
  AgentState _pendingState = AgentState::Boot;
  bool _hasPending = false;
  bool _bootDone = false;
  uint8_t _voiceLevel = 0;
  uint8_t _hearingLevel = 0;
  uint8_t _micLeft = 0;
  uint8_t _micRight = 0;
  bool _muted = false;
  uint32_t _flashColor = 0;
  uint32_t _flashUntil = 0;
  uint32_t _lastShowAt = 0;

  bool _actionShort = false;
  bool _actionLong = false;
  bool _volumeUp = false;
  bool _volumeDown = false;

  uint32_t _actionPressedAt = 0;
  bool _actionDown = false;
  bool _actionLongFired = false;
};

extern Ui ui;
