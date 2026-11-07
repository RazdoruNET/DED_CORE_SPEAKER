#include "app/Ui.h"

#include <Adafruit_NeoPixel.h>

#include "app/Trace.h"
#include "config/AppConfig.h"

Ui ui;

namespace {

Adafruit_NeoPixel strip(SPEAKER_MAX_LED, pins::LED_STRIP, NEO_GRB + NEO_KHZ800);

// Встроенный светодиод платы повторяет состояние внешней ленты: на макете
// он виден, когда лента ещё не подключена.
#if SPEAKER_HAS_STATUS_LED
Adafruit_NeoPixel statusLed(1, pins::LED_STATUS, NEO_GRB + NEO_KHZ800);

void showAll() {
  strip.show();
  statusLed.setPixelColor(0, strip.getPixelColor(0));
  statusLed.show();
}
#else
void showAll() { strip.show(); }
#endif

// Палитра. Значения подобраны так, чтобы даже на потолке яркости соседние
// состояния различались боковым зрением.
const uint32_t kAmber = Adafruit_NeoPixel::Color(255, 140, 30);
const uint32_t kCyan = Adafruit_NeoPixel::Color(0, 210, 220);
const uint32_t kViolet = Adafruit_NeoPixel::Color(120, 40, 220);
const uint32_t kGreen = Adafruit_NeoPixel::Color(30, 220, 120);
const uint32_t kRed = Adafruit_NeoPixel::Color(240, 30, 30);
const uint32_t kBlue = Adafruit_NeoPixel::Color(30, 80, 240);

uint32_t _phase = 0;

uint32_t scale(uint32_t color, uint8_t amount) {
  const uint8_t r = static_cast<uint8_t>(((color >> 16) & 0xFF) * amount / 255);
  const uint8_t g = static_cast<uint8_t>(((color >> 8) & 0xFF) * amount / 255);
  const uint8_t b = static_cast<uint8_t>((color & 0xFF) * amount / 255);
  return Adafruit_NeoPixel::Color(r, g, b);
}

void fillAll(uint32_t color, uint8_t brightness) {
  const uint32_t value = scale(color, brightness);
  for (uint16_t i = 0; i < SPEAKER_MAX_LED; ++i) {
    strip.setPixelColor(i, value);
  }
}

// Плавное перетекание: яркость идёт волной вдоль ленты, без вспышек и рывков.
// Так состояние видно боковым зрением и не бьёт по глазам в тёмной комнате.
void flowGradient(uint32_t color, uint8_t base, uint8_t amplitude, float speed) {
  for (uint16_t i = 0; i < SPEAKER_MAX_LED; ++i) {
    const float wave = sin((i / 4.5f) + _phase * speed);
    const int amount = static_cast<int>(base + wave * amplitude);
    strip.setPixelColor(i, scale(color, static_cast<uint8_t>(constrain(amount, 0, 255))));
  }
}

// Стробоскоп: короткие яркие вспышки с тёмными паузами. Только для входящего
// звонка — это единственное состояние, которое должно бросаться в глаза.
void strobe(uint32_t color) {
  const uint32_t cycle = _phase % 30;
  const uint8_t amount = cycle < 6 ? 220 : (cycle < 10 ? 70 : 0);
  fillAll(color, amount);
}

// Бегущий огонёк: одна яркая точка и хвост за ней.
void runningDot(uint32_t color, uint16_t head, uint8_t brightness) {
  for (uint16_t i = 0; i < SPEAKER_MAX_LED; ++i) {
    int distance = static_cast<int>(head) - static_cast<int>(i);
    if (distance < 0) {
      distance += SPEAKER_MAX_LED;
    }
    const uint8_t amount = distance < 6 ? static_cast<uint8_t>((6 - distance) * 42) : 0;
    strip.setPixelColor(i, scale(color, amount ? amount : brightness / 6));
  }
}

}  // namespace

void Ui::begin() {
  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS_MAX);
  strip.clear();

#if SPEAKER_HAS_STATUS_LED
  statusLed.begin();
  statusLed.setBrightness(LED_BRIGHTNESS_MAX);
  statusLed.clear();
#endif

  showAll();

  pinMode(pins::BTN_ACTION, INPUT_PULLUP);
  pinMode(pins::BTN_VOL_UP, INPUT_PULLUP);
  pinMode(pins::BTN_VOL_DOWN, INPUT_PULLUP);
  pinMode(pins::SW_MUTE, INPUT_PULLUP);
}

void Ui::setState(AgentState state) {
  if (_state == state) {
    return;
  }
  _state = state;
  _phase = 0;
}

void Ui::tick() {
  pollButtons();
  render();

  // Вывод на ленту — с ограничением частоты. Adafruit_NeoPixel выводит биты
  // с выключенными прерываниями: 60 диодов занимают около 2 мс, и вызов на
  // каждой итерации цикла голодом морит Wi-Fi и Bluetooth. 30 кадров в
  // секунду для индикации состояния более чем достаточно.
  const uint32_t now = millis();
  if (now - _lastShowAt < 33) {
    return;
  }
  _lastShowAt = now;
  trace::mark("led.show");
  showAll();
}

void Ui::pollButtons() {
  static uint32_t lastChange = 0;
  static bool volumeUpDown = false;
  static bool volumeDownDown = false;
  const uint32_t now = millis();

  const bool action = digitalRead(pins::BTN_ACTION) == LOW;
  if (action && !_actionDown) {
    _actionDown = true;
    _actionPressedAt = now;
    _actionLongFired = false;
  } else if (!action && _actionDown) {
    _actionDown = false;
    if (!_actionLongFired && now - _actionPressedAt >= BUTTON_DEBOUNCE_MS) {
      _actionShort = true;
    }
  } else if (action && _actionDown && !_actionLongFired
             && now - _actionPressedAt >= BUTTON_LONG_MS) {
    _actionLong = true;
    _actionLongFired = true;
  }

  const bool up = digitalRead(pins::BTN_VOL_UP) == LOW;
  if (now - lastChange > BUTTON_DEBOUNCE_MS) {
    if (up && !volumeUpDown) {
      _volumeUp = true;
      volumeUpDown = true;
      lastChange = now;
    } else if (!up) {
      volumeUpDown = false;
    }
  }

  const bool down = digitalRead(pins::BTN_VOL_DOWN) == LOW;
  if (now - lastChange > BUTTON_DEBOUNCE_MS) {
    if (down && !volumeDownDown) {
      _volumeDown = true;
      volumeDownDown = true;
      lastChange = now;
    } else if (!down) {
      volumeDownDown = false;
    }
  }
}

bool Ui::takeActionShort() {
  const bool value = _actionShort;
  _actionShort = false;
  return value;
}

bool Ui::takeActionLong() {
  const bool value = _actionLong;
  _actionLong = false;
  return value;
}

bool Ui::takeVolumeUp() {
  const bool value = _volumeUp;
  _volumeUp = false;
  return value;
}

bool Ui::takeVolumeDown() {
  const bool value = _volumeDown;
  _volumeDown = false;
  return value;
}

void Ui::render() {
  ++_phase;

  if (_muted) {
    // Микрофон выключен — лента говорит об этом всегда, в любом состоянии.
    fillAll(kRed, 40);
    strip.setPixelColor(0, scale(kRed, 120));
    return;
  }

  switch (_state) {
    case AgentState::Boot: {
      const uint8_t amount = 30 + static_cast<uint8_t>((sin(_phase / 40.0f) + 1) * 35);
      fillAll(kAmber, amount);
      break;
    }
    case AgentState::Unpaired: {
      const uint8_t amount = 8 + static_cast<uint8_t>((sin(_phase / 55.0f) + 1) * 22);
      fillAll(kBlue, amount);
      break;
    }
    case AgentState::Pairing: {
      // Волна от центра к краям — «ищу телефон».
      for (uint16_t i = 0; i < SPEAKER_MAX_LED; ++i) {
        const float centerDistance = fabs(i - (SPEAKER_MAX_LED - 1) / 2.0f);
        const float wave = sin((_phase / 12.0f) - centerDistance / 3.0f);
        const uint8_t amount = static_cast<uint8_t>(40 + (wave + 1) * 60);
        strip.setPixelColor(i, scale(kBlue, amount));
      }
      break;
    }
    case AgentState::Idle:
      flowGradient(kAmber, 16, 6, 0.02f);
      break;
    case AgentState::Listening: {
      // Волна вместо бегущей точки: лента дышит вслед за громкостью, без
      // рывков и мигания — так её спокойно видно боковым зрением.
      const uint8_t base = static_cast<uint8_t>(70 + min<uint32_t>(90, _level));
      flowGradient(kCyan, base, 30, 0.06f);
      break;
    }
    case AgentState::Thinking:
      flowGradient(kViolet, 55, 35, 0.05f);
      break;
    case AgentState::Speaking: {
      // Волна, повторяющая речь: движение есть, только пока звучит голос.
      for (uint16_t i = 0; i < SPEAKER_MAX_LED; ++i) {
        const float wave = sin((i / 2.0f) + (_phase / 4.0f));
        const uint8_t amount = static_cast<uint8_t>(60 + wave * (20 + _level / 2));
        strip.setPixelColor(i, scale(kGreen, amount));
      }
      break;
    }
    case AgentState::Offline:
      // Постоянное свечение с плавным перетеканием вместо прежних вспышек.
      flowGradient(kAmber, 26, 12, 0.03f);
      break;
    case AgentState::Error:
      flowGradient(kRed, 40, 24, 0.05f);
      break;
    case AgentState::Incoming:
      strobe(kGreen);
      break;
    case AgentState::Intercom:
      fillAll(kGreen, 90);
      break;
  }
}
