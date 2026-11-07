#pragma once

#include <Arduino.h>

// Локальные умения: работают без сервера, поэтому живут отдельно от сети.
class Intents {
 public:
  void begin();
  void tick();

  bool startTimer(uint32_t seconds);
  void cancelTimers();
  bool hasTimer() const { return _timerActive || _alarmActive; }

  void volumeUp();
  void volumeDown();
  void toggleMute();

 private:
  void chime(uint16_t frequency, uint16_t durationMs);

  bool _timerActive = false;
  uint32_t _timerDue = 0;
  bool _alarmActive = false;
  uint32_t _alarmDue = 0;
};

extern Intents intents;
