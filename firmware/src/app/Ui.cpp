#include "app/Ui.h"

#include <Adafruit_NeoPixel.h>
#include <driver/gpio.h>

#include "app/LedLayout.h"
#include "app/Trace.h"
#include "config/AppConfig.h"

Ui ui;

namespace {

using leds::kCount;

// Тип ленты. По умолчанию — самый распространённый WS2812B (RGB, 800 кГц).
// Флаги ниже нужны только для диагностических сборок: у части лент 4 канала
// (RGBW) или другой тайминг, и тогда поток надо слать иначе.
#if defined(SPEAKER_LED_RGBW)
Adafruit_NeoPixel strip(leds::kCount, pins::LED_STRIP, NEO_RGBW + NEO_KHZ800);
#elif defined(SPEAKER_LED_KHZ400)
Adafruit_NeoPixel strip(leds::kCount, pins::LED_STRIP, NEO_GRB + NEO_KHZ400);
#else
Adafruit_NeoPixel strip(leds::kCount, pins::LED_STRIP, NEO_GRB + NEO_KHZ800);
#endif

// Встроенный светодиод платы повторяет состояние внешней ленты: на макете
// он виден, когда лента ещё не подключена. Берём центр U, а не первый диод:
// первый теперь кончик левой стороны и в половине состояний почти погашен.
#if SPEAKER_HAS_STATUS_LED
Adafruit_NeoPixel statusLed(1, pins::LED_STATUS, NEO_GRB + NEO_KHZ800);

void showAll() {
  strip.show();
  statusLed.setPixelColor(0, strip.getPixelColor(leds::kMouthCenter));
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
const uint32_t kWhite = Adafruit_NeoPixel::Color(255, 255, 255);

// Пастель для состояния «слушаю эфир»: белое медленно перетекает в мягкие
// оттенки, без насыщенных цветов.
const uint32_t kPastelPink = Adafruit_NeoPixel::Color(255, 185, 205);
const uint32_t kPastelMint = Adafruit_NeoPixel::Color(185, 255, 215);
const uint32_t kPastelLavender = Adafruit_NeoPixel::Color(205, 195, 255);
const uint32_t kPastelPeach = Adafruit_NeoPixel::Color(255, 215, 185);

uint32_t _phase = 0;

uint32_t scale(uint32_t color, uint8_t amount) {
  const uint8_t r = static_cast<uint8_t>(((color >> 16) & 0xFF) * amount / 255);
  const uint8_t g = static_cast<uint8_t>(((color >> 8) & 0xFF) * amount / 255);
  const uint8_t b = static_cast<uint8_t>((color & 0xFF) * amount / 255);
  return Adafruit_NeoPixel::Color(r, g, b);
}

uint8_t amountOf(float value) {
  return static_cast<uint8_t>(constrain(static_cast<int>(value), 0, 255));
}

uint8_t lerpChannel(uint8_t from, uint8_t to, uint8_t amount) {
  return static_cast<uint8_t>((from * (255 - amount) + to * amount) / 255);
}

uint32_t lerpColor(uint32_t from, uint32_t to, uint8_t amount) {
  return Adafruit_NeoPixel::Color(
      lerpChannel((from >> 16) & 0xFF, (to >> 16) & 0xFF, amount),
      lerpChannel((from >> 8) & 0xFF, (to >> 8) & 0xFF, amount),
      lerpChannel(from & 0xFF, to & 0xFF, amount));
}

// Заливка всей линии, кроме разделителей: они не зажигаются даже в сценах,
// где лента работает целиком.
void fillLine(uint32_t color, uint8_t brightness) {
  const uint32_t value = scale(color, brightness);
  for (uint16_t i = 0; i < kCount; ++i) {
    strip.setPixelColor(i, leds::isGap(i) ? 0 : value);
  }
}

uint32_t pastelAt(float wheel);

// ---------------------------------------------------------------------------
// Уши: 1–3 и 15–17. Это индикация событий — цвет и характер движения говорят,
// что происходит, ещё до того, как агент заговорит. Разделители (4, 5, 13, 14)
// не зажигаются никогда: они отделяют уши от рта.
// ---------------------------------------------------------------------------
void paintEars(uint32_t color, uint8_t amount) {
  const uint32_t value = scale(color, amount);
  for (uint16_t i = 0; i < leds::kEarLedCount; ++i) {
    strip.setPixelColor(i, value);
    strip.setPixelColor(static_cast<uint16_t>(leds::kRightEarFirst + i), value);
  }
}

// Ровное дыхание ушей: симметрично с обеих сторон, без вспышек.
void earsBreath(uint32_t color, uint8_t base, uint8_t amplitude, float speed) {
  const float wave = sin(_phase * speed);
  paintEars(color, amountOf(base + wave * amplitude));
}

// Порядок обхода ушей: от рта наружу слева, затем от рта наружу справа.
inline uint16_t earChainAt(uint8_t slot) {
  if (slot < leds::kEarLedCount) {
    return static_cast<uint16_t>(leds::kEarLedCount - 1 - slot);
  }
  return static_cast<uint16_t>(leds::kRightEarFirst + (slot - leds::kEarLedCount));
}

// Бегущая искра по ушам: ходит от рта к «виску» и обратно. Так показывается
// ожидание — подключение, поиск, входящий вызов.
void earsSweep(uint32_t color, uint8_t base) {
  constexpr uint8_t kSlots = leds::kEarLedCount * 2;
  paintEars(color, base);
  const uint8_t step = static_cast<uint8_t>((_phase / 4) % (kSlots * 2 - 2));
  const uint8_t slot = step < kSlots ? step : static_cast<uint8_t>(kSlots * 2 - 2 - step);
  for (uint8_t tail = 0; tail < 3; ++tail) {
    const uint8_t s = slot >= tail ? static_cast<uint8_t>(slot - tail) : 0;
    strip.setPixelColor(earChainAt(s), scale(color, amountOf(230 - tail * 70)));
  }
}

// ---------------------------------------------------------------------------
// Рот: 6–12.
//
// Формы двоичные: в устойчивом виде каждый диод либо горит полностью, либо
// погашен. Мягкость даёт только переход между формами: яркость подтягивается к
// цели за 3–4 кадра и «щёлкает» в 0 или 255, когда разница мала. Так движение
// не дёргается, но полусветов в покое нет.
//
// Покой — единственный средний диод: 0001000.
//
// Речь — симметричные формы по центру, по громкости голоса агента:
//   0001000 → 0011100 → 0111110 → 1111111
//
// Простой — изредка, с разными длинными паузами, две «живые» анимации:
//   * по рту проезжает тройка диодов влево и обратно;
//   * тройка в центре пульсирует: 010 → 111 → 010.
// ---------------------------------------------------------------------------
constexpr uint8_t kMouthCount = leds::kMouthCount;

// Текущая яркость диодов рта: нужна для мягких переходов между формами.
uint8_t mouthBright[kMouthCount] = {0};

// События простоя: что показываем и до какого момента.
uint32_t idleKind = 0;        // 0 — только покой, 1 — проезд, 2 — пульс
uint32_t idleStartedAt = 0;
uint32_t idleNextAt = 0;

uint32_t randomInterval() {
  // 6–18 секунд: паузы разные и длинные, чтобы рот не «жил» постоянно.
  return 6000 + esp_random() % 12000;
}

void applyMouth(uint32_t color, const bool* lit) {
  for (uint8_t slot = 0; slot < kMouthCount; ++slot) {
    const uint8_t target = lit[slot] ? 255 : 0;
    int current = mouthBright[slot];
    const int diff = static_cast<int>(target) - current;

    // Близко к цели — ставим ровно 0 или 255: никаких «полусветов» в покое.
    current = (diff > -40 && diff < 40) ? target : current + diff / 3;
    mouthBright[slot] = static_cast<uint8_t>(current);

    const uint16_t index = static_cast<uint16_t>(leds::kMouthFirst + slot);
    strip.setPixelColor(index, current == 0 ? 0 : scale(color, mouthBright[slot]));
  }
}

// Симметричная форма по центру: 1, 3, 5 или 7 диодов.
void mouthCentered(uint32_t color, uint8_t count) {
  bool lit[kMouthCount] = {false};
  const uint8_t half = static_cast<uint8_t>(count / 2);
  for (uint8_t slot = 0; slot < kMouthCount; ++slot) {
    const uint8_t center = kMouthCount / 2;
    const uint8_t distance = slot > center ? slot - center : center - slot;
    lit[slot] = distance <= half;
  }
  applyMouth(color, lit);
}

// Покой: только средний диод. Здесь же изредка запускаются анимации простоя.
void mouthIdle(uint32_t color) {
  const uint32_t now = millis();

  if (idleKind == 0) {
    if (idleNextAt == 0) {
      idleNextAt = now + randomInterval();
    } else if (now >= idleNextAt) {
      idleKind = 1 + esp_random() % 2;
      idleStartedAt = now;
      idleNextAt = 0;
    }

    mouthCentered(color, 1);
    return;
  }

  const uint32_t elapsed = now - idleStartedAt;

  if (idleKind == 1) {
    // Проезд тройки: 5 позиций туда и обратно, шаг 90 мс.
    constexpr uint8_t kBlock = 3;
    constexpr uint8_t kPositions = kMouthCount - kBlock + 1;  // 5
    constexpr uint8_t kSteps = kPositions * 2 - 2;            // 8
    const uint32_t step = elapsed / 90;

    if (step > kSteps) {
      idleKind = 0;
      idleNextAt = now + randomInterval();
      mouthCentered(color, 1);
      return;
    }

    const uint8_t position =
        step < kPositions ? static_cast<uint8_t>(step)
                          : static_cast<uint8_t>(kSteps - step);

    bool lit[kMouthCount] = {false};
    for (uint8_t slot = position; slot < position + kBlock && slot < kMouthCount; ++slot) {
      lit[slot] = true;
    }
    applyMouth(color, lit);
    return;
  }

  // Пульс тройки в центре: 010 → 111 → 010, два раза.
  const uint32_t phase = elapsed / 120;

  if (phase >= 8) {
    idleKind = 0;
    idleNextAt = now + randomInterval();
    mouthCentered(color, 1);
    return;
  }

  const uint8_t scene = static_cast<uint8_t>(phase % 4);
  const uint8_t center = kMouthCount / 2;

  bool lit[kMouthCount] = {false};
  if (scene == 0 || scene == 2) {
    // 010 — горят соседи, середина погашена.
    lit[center - 1] = true;
    lit[center + 1] = true;
  } else {
    // 111 — горит вся тройка.
    lit[center - 1] = true;
    lit[center] = true;
    lit[center + 1] = true;
  }
  applyMouth(color, lit);
}

// Речь: форма по громкости голоса агента, всегда по центру.
void mouthTalk(uint32_t color, uint8_t level) {
  uint8_t count = 1;

  if (level >= 140) {
    count = 7;
  } else if (level >= 90) {
    count = 5;
  } else if (level >= 45) {
    count = 3;
  }

  mouthCentered(color, count);
}

// ---------------------------------------------------------------------------
// Шкалы микрофонов.
//
// Левый микрофон — от диода 17 к центру, правый — от диода 1 к центру. Чем
// громче канал, тем больше диодов загорается. Это фон: всё, что рисуется
// поверх (рот при речи, события, вспышка), эти диоды перекрывает — речь всегда
// важнее индикации микрофона.
// ---------------------------------------------------------------------------
constexpr uint8_t kBarUnits = 8;  // диодов от края до центра

uint8_t barLength(uint8_t level) {
  return static_cast<uint8_t>(static_cast<uint16_t>(level) * kBarUnits / 255);
}

void micBars(uint8_t leftLevel, uint8_t rightLevel, bool attentive = false) {
  const uint32_t color = scale(kCyan, attentive ? 150 : 90);
  const uint32_t tip = scale(kCyan, attentive ? 255 : 200);

  const uint8_t right = barLength(rightLevel);  // от диода 1 к центру
  const uint8_t left = barLength(leftLevel);    // от диода 17 к центру

  for (uint8_t step = 0; step < kBarUnits; ++step) {
    // Правый микрофон: индексы 0..7, к центру (индекс 8).
    if (step < right) {
      const uint16_t index = static_cast<uint16_t>(step);
      strip.setPixelColor(index, step + 1 == right ? tip : color);
    }

    // Левый микрофон: индексы 16..9, к центру.
    if (step < left) {
      const uint16_t index = static_cast<uint16_t>(kCount - 1 - step);
      strip.setPixelColor(index, step + 1 == left ? tip : color);
    }
  }
}

// Одна яркая точка с хвостом. head — логическая позиция (может быть дробной
// для плавного хода), dir = +1 вправо, -1 влево; хвост всегда позади.
void movingDot(uint32_t color, float head, int dir, uint8_t peak) {
  for (uint16_t i = 0; i < kCount; ++i) {
    if (leds::isGap(i)) {
      continue;
    }
    const uint16_t p = leds::positionOf(i);
    const float behind = dir > 0 ? (head - static_cast<float>(p))
                                 : (static_cast<float>(p) - head);
    if (behind < -0.5f || behind > 3.5f) {
      continue;
    }
    const float weight = 1.0f - constrain(behind + 0.5f, 0.0f, 4.0f) / 4.0f;
    const uint8_t amount = amountOf(peak * weight);
    if (amount > 0) {
      strip.setPixelColor(i, scale(color, amount));
    }
  }
}

// Стробоскоп: короткие яркие вспышки с тёмными паузами. Только для входящего
// звонка — это единственное состояние, которое должно бросаться в глаза.
void strobe(uint32_t color) {
  const uint32_t cycle = _phase % 30;
  const uint8_t amount = cycle < 6 ? 240 : (cycle < 10 ? 90 : 0);
  fillLine(color, amount);
}

uint32_t pastelAt(float wheel) {
  static const uint32_t kPastels[4] = {
      kPastelPink, kPastelMint, kPastelLavender, kPastelPeach};
  const float wrapped = wheel - floorf(wheel);
  const int index = static_cast<int>(floorf(wheel)) & 3;
  return lerpColor(kPastels[index], kPastels[(index + 1) & 3],
                   amountOf(wrapped * 255.0f));
}

constexpr uint16_t kBootMoveFrames = 24;   // ~0.8 с
constexpr uint16_t kBootFadeFrames = 15;   // ~0.5 с
constexpr uint16_t kBootBlueFrames = 45;   // ~1.5 с
constexpr uint16_t kBootTotalFrames =
    kBootMoveFrames + kBootFadeFrames + kBootBlueFrames;

// Включение: белые точки с кончиков ушей идут к центру рта, гаснут и
// превращаются в голубой огонёк — лицо «просыпается». Возвращает true, когда
// сцена закончена.
bool bootAnimation() {
  strip.clear();

  if (_phase < kBootMoveFrames) {
    const float pos = 8.0f * static_cast<float>(_phase) / (kBootMoveFrames - 1);
    movingDot(kWhite, pos, +1, 255);
    movingDot(kWhite, 16.0f - pos, -1, 255);
    return false;
  }

  if (_phase < kBootMoveFrames + kBootFadeFrames) {
    const float t = static_cast<float>(_phase - kBootMoveFrames) / kBootFadeFrames;
    // Белое плавно уступает голубому: цвет идёт от белого к синему, яркость
    // садится до уровня спокойного огонька — без рывка на стыке.
    const uint32_t color = lerpColor(kWhite, kBlue, amountOf(t * 255.0f));
    strip.setPixelColor(leds::kMouthCenter, scale(color, amountOf(255.0f - 85.0f * t)));
    return false;
  }

  if (_phase < kBootTotalFrames) {
    const float breath = sin(static_cast<float>(_phase - kBootMoveFrames
                                               - kBootFadeFrames) / 16.0f);
    strip.setPixelColor(leds::kMouthCenter, scale(kBlue, amountOf(170 + breath * 40)));
    return false;
  }

  strip.setPixelColor(leds::kMouthCenter, scale(kBlue, 170));
  return true;
}

#if defined(SPEAKER_LED_SELFTEST)
void paintRange(uint16_t first, uint16_t last, uint32_t color) {
  for (uint16_t i = first; i <= last; ++i) {
    strip.setPixelColor(i, color);
  }
}

// Простая диагностика: лента держит один цвет. Так видно, рассыпается ли
// кадр: если вместо ровного красного диоды мигают вразнобой, проблема в
// данных или питании, а не в анимации.
void renderSelfTest() {
  constexpr uint16_t kStepFrames = 60;  // ~2 с при 30 кадрах в секунду
  constexpr uint16_t kSteps = 5;
  const uint16_t step = static_cast<uint16_t>((_phase / kStepFrames) % kSteps);

  static uint16_t shown = 0xFFFF;
  if (step != shown) {
    shown = step;
    static const char* const names[kSteps] = {
        "выключено",
        "вся лента — красный",
        "вся лента — зелёный",
        "вся лента — синий",
        "вся лента — приглушённый белый",
    };
    applog::printf("[ledtest] %s\n", names[step]);
  }

  strip.clear();
  switch (step) {
    case 1:
      paintRange(0, kCount - 1, kRed);
      break;
    case 2:
      paintRange(0, kCount - 1, kGreen);
      break;
    case 3:
      paintRange(0, kCount - 1, kBlue);
      break;
    case 4:
      paintRange(0, kCount - 1, Adafruit_NeoPixel::Color(120, 120, 120));
      break;
    default:
      break;
  }
}
#endif

}  // namespace

void Ui::begin() {
  // Максимальная сила выхода на линии данных. У 5-вольтовой ленты при
  // питании 4.5 В запас по уровню всего 0.15 В, и слабый фронт рассыпает
  // кадр. Это не заменяет согласователь уровней, но помогает на коротком
  // проводе.
  gpio_set_drive_capability(static_cast<gpio_num_t>(pins::LED_STRIP), GPIO_DRIVE_CAP_3);

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
  // Вступительную сцену не обрываем: новое состояние ждёт её конца.
  if (_state == AgentState::Boot && !_bootDone && state != AgentState::Boot) {
    _pendingState = state;
    _hasPending = true;
    return;
  }
  applyState(state);
}

void Ui::applyState(AgentState state) {
  if (_state == state) {
    return;
  }
  _state = state;
  _phase = 0;
  if (state == AgentState::Boot) {
    _bootDone = false;
    _hasPending = false;
  }
}

void Ui::tick() {
  pollButtons();

  // Вывод на ленту — с ограничением частоты. Adafruit_NeoPixel выводит биты
  // с выключенными прерываниями: 17 диодов занимают меньше миллисекунды, но
  // вызов на каждой итерации цикла всё равно голодом морит Wi-Fi и Bluetooth.
  // 30 кадров в секунду для индикации состояния более чем достаточно; фазу
  // двигаем вместе с кадром, иначе анимации зависят от скорости цикла.
  const uint32_t now = millis();
  if (now - _lastShowAt < 33) {
    return;
  }
  _lastShowAt = now;
  render();
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

#if defined(SPEAKER_LED_SELFTEST)
  renderSelfTest();
  return;
#endif

  // Кадр начинается с чистого листа. Без этого диод, зажжённый в прошлом
  // кадре, остаётся горящим: шкалы микрофонов не спадали в тишине, а цвета
  // разных состояний наслаивались друг на друга.
  strip.clear();

  // Вспышка поверх всего: «услышала обращение». Горит вся лента, включая
  // разделители, — это событие, а не лицо.
  if (_flashUntil != 0) {
    const uint32_t now = millis();
    if (now < _flashUntil) {
      const uint32_t value = scale(_flashColor, 255);
      for (uint16_t i = 0; i < kCount; ++i) {
        strip.setPixelColor(i, value);
      }
      return;
    }
    _flashUntil = 0;
  }

  if (_muted) {
    // Микрофон выключен — это перекрывает любое состояние. Уши красные и
    // дышат, рот почти закрыт: видно с любого угла, но без резкого света.
    earsBreath(kRed, 95, 45, 0.05f);
    mouthIdle(kRed);
    return;
  }

  switch (_state) {
    case AgentState::Boot:
      if (bootAnimation()) {
        _bootDone = true;
        if (_hasPending) {
          const AgentState next = _pendingState;
          _hasPending = false;
          applyState(next);
        }
      }
      break;
    case AgentState::Unpaired:
      // Синие уши и тихий рот: колонка ждёт кнопку.
      earsBreath(kBlue, 60, 38, 0.03f);
      mouthIdle(kBlue);
      break;
    case AgentState::Pairing:
      // Искра по ушам ходит от рта к виску — «ищу телефон».
      earsSweep(kBlue, 45);
      mouthIdle(kBlue);
      break;
    case AgentState::Idle:
      // Простой: шкалы микрофонов от краёв к центру, рот спокойно дышит.
      micBars(_micLeft, _micRight);
      mouthIdle(kCyan);
      break;
    case AgentState::Listening:
      // Слушаю человека: те же шкалы микрофонов, но ярче — видно, что колонка
      // слышит. Рот молчит: он повторяет только голос агента.
      micBars(_micLeft, _micRight, true);
      mouthIdle(kCyan);
      break;
    case AgentState::Thinking: {
      // Думаю: по ушам бежит фиолетовая искра, рот дышит медленнее.
      earsSweep(kViolet, 25);
      mouthIdle(kViolet);
      break;
    }
    case AgentState::Speaking:
      // Говорю: рот открывается по громкости собственной речи агента.
      earsBreath(kGreen, 65, 35, 0.07f);
      mouthTalk(kGreen, _voiceLevel);
      break;
    case AgentState::Offline:
      // Нет связи: синяя искра по ушам, рот тлеет.
      earsSweep(kBlue, 16);
      mouthIdle(kBlue);
      break;
    case AgentState::Error:
      earsBreath(kRed, 85, 45, 0.06f);
      mouthIdle(kRed);
      break;
    case AgentState::Incoming:
      strobe(kGreen);
      break;
    case AgentState::Intercom:
      // Разговор: ровные зелёные уши, рот — по голосу агента.
      earsBreath(kGreen, 85, 20, 0.02f);
      mouthTalk(kGreen, _voiceLevel);
      break;
  }
}
