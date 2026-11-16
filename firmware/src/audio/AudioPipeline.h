#pragma once

#include <Arduino.h>

#include <functional>

#include "config/AppConfig.h"

#if SPEAKER_HAS_AEC
#include "audio/EchoCanceller.h"
#endif

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

  /// Очередь речи пуста и хвост последнего кадра уже отзвучал в воздухе.
  /// По этому признаку микрофон возвращается после SPEECH_END: иначе он
  /// слышит собственный голос агента из буфера воспроизведения.
  bool playbackIdle() const;

  /// Проверка выходного тракта без сервера: три тона прямо в I2S. Нужна, чтобы
  /// убедиться в пайке усилителя и динамика до подключения к сети. Вызывается
  /// из setup() в стендовых сборках *_spk.
  void speakerSelfTest();

  /// Переключить выход на поток (эфир, музыка): частота станции, стерео.
  /// Речь на это время молчит — порт у устройства один.
  bool enterMediaMode(uint32_t sampleRate);
  void leaveMediaMode();
  bool mediaMode() const { return _mediaMode; }

  /// Отдать в усилитель готовый стереопоток. Возвращает число записанных кадров.
  size_t writeMediaPcm(const int16_t* interleaved, size_t frames);

#if defined(SPEAKER_AEC_DUMP_TONE) && SPEAKER_HAS_AEC
  /// Стенд компенсатора без сети и декодера: сама колонка играет известный
  /// шум через тот же путь, что и эфир, и подаёт его в референс. Нужно, чтобы
  /// отделить «компенсатор не работает» от «декор не отдаёт звук».
  void startTone();
#endif

  /// Плавно погасить эфир за указанное время. Нужно перед переключением порта
  /// на речь: без затухания слышен щелчок и обрывок мелодии.
  void fadeOutMedia(uint32_t milliseconds);

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

  /// Кадры, не влезшие в очередь воспроизведения. Растёт — буфера не хватает,
  /// и в речи слышны щелчки: по этому счётчику видно, помогает ли запас.
  uint32_t playbackDrops() const { return _playbackDrops; }

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

  /// Порт усилителя один, а пишут в него две задачи: речь и эфир. Замок берёт
  /// каждая запись и каждая смена драйвера (речь ↔ эфир), иначе одна задача
  /// успевала начать i2s_write, пока другая уже удаляла драйвер.
  bool takeSpeakerPort(uint32_t timeoutMs);
  void releaseSpeakerPort();
  void drainPlayback(uint32_t milliseconds);

  bool _ready = false;
  /// Флаги читаются из трёх задач, поэтому volatile: без него компилятор
  /// вправе держать значение в регистре и не заметить смену режима.
  volatile bool _speakerReady = false;
  volatile bool _mediaMode = false;
  uint32_t _mediaRate = 0;

  /// Низкая полка для эфира: у 4-омного динамика в корпусе нет низа, и
  /// музыка звучит плоско. Фильтр тот же, что в эквалайзерах: +8 дБ ниже
  /// 120 Гц, выше — без изменений.
  struct MediaBiquad {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float x1 = 0.0f;
    float x2 = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;

    float process(float x) {
      const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
      x2 = x1;
      x1 = x;
      y2 = y1;
      y1 = y;

      return y;
    }
  };

  MediaBiquad _bassLeft;
  MediaBiquad _bassRight;
  void designBassShelf(uint32_t rate);

  /// Последние отсчёты эфира: с них начинается плавное затухание, когда колонке
  /// надо заговорить. Без него драйвер гасился на середине волны — это слышно
  /// как щелчок и обрывок музыки перед ответом.
  int16_t _mediaLastLeft = 0;
  int16_t _mediaLastRight = 0;
  volatile bool _capturing = false;

  /// До какого момента микрофон молчит принудительно (0 — не молчит).
  volatile uint32_t _hushUntil = 0;

  /// «Пол» остатка эха: типичный уровень музыки после компенсатора. Порог
  /// речи при играющем эфире считается от него, а не от пика — иначе колонка
  /// глохнет на всё время музыки.
  volatile uint32_t _mediaNoiseFloor = 0;

  /// Приглушение эфира на время фразы. 1.0 — полная громкость, 0.0 — тишина.
  /// Цель ставит задача записи, плавно отрабатывает задача медиа.
  volatile float _mediaDuckTarget = 1.0f;
  volatile float _mediaDuckGain = 1.0f;

  /// Автолимитер эфира: держит музыку настолько тихой, чтобы микрофон не
  /// клиппировал. Без этого компенсатор бесполезен, а голос не разобрать.
  volatile float _mediaLimitGain = 1.0f;

  /// Пока эхо не оценено, детектор речи закрыт: первые кадры музыки иначе
  /// принимаются за начало фразы.
  volatile uint32_t _mediaVadHoldUntil = 0;

  /// Захват останавливали посреди фразы: задаче записи нужно сбросить VAD.
  volatile bool _flushVad = false;

  volatile uint32_t _peakRmsWindow = 0;
  volatile uint32_t _peakZcrWindow = 0;

  int16_t* _preroll = nullptr;
  size_t _prerollFrames = 0;
  size_t _prerollWrite = 0;

  /// Сколько кадров в кольце преролла действительно записано с прошлого сброса.
  size_t _prerollValid = 0;

  /// До какого момента ещё звучит хвост последнего отданного кадра речи.
  volatile uint32_t _playbackQuietUntil = 0;

  SemaphoreHandle_t _speakerLock = nullptr;

  uint8_t _volume = 60;
  uint32_t _lastRms = 0;
  uint32_t _lastRmsLeft = 0;
  uint32_t _lastRmsRight = 0;
  volatile uint32_t _framesCaptured = 0;
  volatile uint32_t _playbackDrops = 0;
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

#if SPEAKER_HAS_AEC
  EchoCanceller _aec;
#endif

#if defined(SPEAKER_AEC_DUMP) && SPEAKER_HAS_AEC
  /// Снимок для разбора эха: 128 мс микрофона до компенсатора и 256 мс
  /// референса — с запасом на задержку тракта.
#if defined(SPEAKER_AEC_DUMP_TONE)
  static constexpr size_t kDumpMic = 8192;
  static constexpr size_t kDumpRef = 16384;
#else
  static constexpr size_t kDumpMic = 2048;
  static constexpr size_t kDumpRef = 4096;
#endif

  int16_t _dumpMic[kDumpMic] = {};
  size_t _dumpMicWrite = 0;
  size_t _dumpMicValid = 0;
  uint32_t _dumpMediaSince = 0;
  /// Сколько снимков уже снято: первый — на старте эфира, второй — когда
  /// компенсатор успел поработать.
  uint8_t _dumpCount = 0;

  void dumpEchoOnce();

#if defined(SPEAKER_AEC_DUMP_TONE)
  static void toneTaskEntry(void* arg);
  void toneLoop();
#endif
#endif
};

extern AudioPipeline audioPipeline;
