#pragma once

#include <Arduino.h>

// Состояние агента — то, что лента показывает цветом и движением.
// Это язык, а не индикатор: человек должен понимать фазу, не глядя в приложение.
enum class AgentState : uint8_t {
  Boot,       // загрузка: ровное тёплое дыхание
  Unpaired,   // не настроена: синий медленный импульс, ждёт кнопку
  Pairing,    // ждём телефон: синяя волна от центра к краям
  Idle,       // слушаю эфир: еле тлеющий янтарь
  Listening,  // записываю: бирюза, яркость по громкости
  Thinking,   // думаю: фиолетовый бегущий огонёк
  Speaking,   // говорю: зелёная волна, повторяющая речь
  Offline,    // нет сервера: янтарь, плавное перетекание без вспышек
  Error,      // ошибка: красное плавное дыхание
  Incoming,   // входящий звонок: стробоскоп
  Intercom,   // разговор: ровный зелёный
};

class Ui {
 public:
  void begin();
  void tick();

  void setState(AgentState state);
  AgentState state() const { return _state; }

  void setLevel(uint8_t level) { _level = level; }
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

  AgentState _state = AgentState::Boot;
  uint8_t _level = 0;
  bool _muted = false;
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
