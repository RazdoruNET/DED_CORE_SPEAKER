#pragma once

#include <Arduino.h>

#include <functional>

// Аудиотракт: микрофоны на I2S0, усилитель на I2S1.
//
// Преролл существует из-за wake word: детектор срабатывает через 200–400 мс
// после начала фразы, и без буфера первые слова («Лира, вклю…») теряются.
class AudioPipeline {
 public:
  using FrameHandler = std::function<void(const uint8_t* pcm, size_t length)>;
  using VadHandler = std::function<void(bool start, uint32_t rms)>;
  using LevelHandler = std::function<void(uint8_t level)>;

  bool begin();
  bool isReady() const { return _ready; }

  void onFrame(FrameHandler handler) { _onFrame = std::move(handler); }
  void onVad(VadHandler handler) { _onVad = std::move(handler); }
  void onLevel(LevelHandler handler) { _onLevel = std::move(handler); }

  /// Чих: короткий резкий всплеск с широким спектром. Правило текстовое не
  /// работает — распознавание часто не выдаёт на чих никакого текста.
  using SneezeHandler = std::function<void()>;
  void onSneeze(SneezeHandler handler) { _onSneeze = std::move(handler); }

  void startCapture();
  void stopCapture();

  /// Пауза прослушивания: колонка не слышит ничего заданное время. Нужна, пока
  /// браузер играет звук — иначе микрофон слышит эти же колонки и «слушает вечно».
  void hush(uint32_t milliseconds);

  /// Досрочно вернуть слух.
  void resume();

  /// Пиковые значения за последнее окно: нужны, чтобы настроить детектор чиха
  /// по живым числам, а не на глаз.
  void takeBurstPeaks(uint32_t& rms, uint32_t& zcr);
  bool isCapturing() const { return _capturing; }

  void playPcm(const uint8_t* data, size_t length);
  void flushPlayback();

  void setVolume(uint8_t value);
  uint8_t volume() const { return _volume; }

  // Уровни по каналам: нужны, чтобы проверить оба микрофона по отдельности.
  // Сколько кадров записи обработано с момента старта: по нему сверяется,
  // идёт ли захват в реальном времени.
  uint32_t framesCaptured() const { return _framesCaptured; }

  uint32_t rms() const { return _lastRms; }
  uint32_t noiseFloor() const { return _noiseFloor; }

  uint32_t rmsLeft() const { return _lastRmsLeft; }
  uint32_t rmsRight() const { return _lastRmsRight; }

  /// Уровень воспроизводимого звука (0–255). По нему двигается рот, когда
  /// говорит агент: микрофон в этот момент слышит динамик, а не человека.
  uint8_t outputLevel() const { return _outputLevel; }

  // Пиковые значения с момента прошлого вызова. Пик важнее среднего: по нему
  // видно, упирается ли тракт в предел, то есть не сдвинуты ли биты.
  void takePeaks(uint32_t& left, uint32_t& right) {
    left = _peakLeft;
    right = _peakRight;
    _peakLeft = 0;
    _peakRight = 0;
  }

 private:
  static void micTaskEntry(void* arg);
  static void playerTaskEntry(void* arg);
  void micLoop();
  void playerLoop();
  void emitFrame(const int16_t* samples);

  bool _ready = false;
  volatile bool _capturing = false;

  /// До какого момента микрофон молчит принудительно (0 — не молчит).
  volatile uint32_t _hushUntil = 0;

  /// Захват останавливали посреди фразы: задаче записи нужно сбросить VAD.
  volatile bool _flushVad = false;

  volatile uint32_t _peakRmsWindow = 0;
  volatile uint32_t _peakZcrWindow = 0;

  int16_t* _preroll = nullptr;
  size_t _prerollFrames = 0;
  size_t _prerollWrite = 0;

  /// Сколько кадров в кольце преролла действительно записано с прошлого сброса.
  size_t _prerollValid = 0;

  uint8_t _volume = 60;
  uint32_t _lastRms = 0;
  uint32_t _lastRmsLeft = 0;
  uint32_t _lastRmsRight = 0;
  volatile uint32_t _framesCaptured = 0;
  volatile uint32_t _noiseFloor = 0;
  volatile uint8_t _outputLevel = 0;
  volatile uint32_t _peakLeft = 0;
  volatile uint32_t _peakRight = 0;

  FrameHandler _onFrame;
  VadHandler _onVad;
  LevelHandler _onLevel;
  SneezeHandler _onSneeze;

  TaskHandle_t _micTask = nullptr;
  TaskHandle_t _playerTask = nullptr;
};

extern AudioPipeline audioPipeline;
