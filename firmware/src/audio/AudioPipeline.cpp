#include "audio/AudioPipeline.h"

#include <cstdio>
#include <driver/i2s.h>

#include "app/Trace.h"
#include "config/AppConfig.h"

AudioPipeline audioPipeline;

namespace {

#if SPEAKER_DAC_OUTPUT
// Встроенный ЦАП ESP32 доступен только на I2S0 — микрофон уступает ему порт.
constexpr i2s_port_t kMicPort = I2S_NUM_1;
constexpr i2s_port_t kSpeakerPort = I2S_NUM_0;
#else
constexpr i2s_port_t kMicPort = I2S_NUM_0;
constexpr i2s_port_t kSpeakerPort = I2S_NUM_1;
#endif

// Очередь воспроизведения: 40 кадров по 20 мс — 800 мс запаса.
//
// Раньше здесь было 8 кадров (160 мс). Сервер шлёт речь ровно в темпе
// реального времени, и любой рывок сети больше 160 мс оставлял усилитель без
// данных: на слух это щелчки и хрип посреди фразы, хотя звук доходил целиком.
constexpr int kPlaybackQueueFrames = 40;

// Столько кадров ждём перед началом фразы: буфер должен наполниться, иначе
// первые слоги рвутся, пока сеть догоняет.
constexpr int kPlaybackPrimeFrames = 8;

int16_t pixelBuffer[AUDIO_FRAME_SAMPLES];

// Кадр MP3 — до 1152 отсчётов на канал; буфер под запись потока в I2S.
constexpr size_t kMediaSamples = 1152 * 2;

// Куски, которыми эфир пишется в I2S. Блок MP3 (1152 кадра на 44.1 кГц — это
// 26 мс) длиннее очереди DMA (1024 кадра, 23 мс): если отдать референс
// компенсатору целиком, его отсчёты зазвучат вразброс на 26 мс, и фильтр с
// постоянной задержкой такое эхо вычесть не сможет. 256 кадров — 6 мс.
constexpr size_t kMediaAecChunk = 256;
int16_t mediaBuffer[kMediaSamples];

// Два микрофона на одной шине: в буфере лежат чередующиеся пары левый/правый.
int32_t micBuffer[AUDIO_FRAME_SAMPLES * MIC_CHANNEL_COUNT];

QueueHandle_t playbackQueue = nullptr;

struct PlaybackChunk {
  int16_t samples[AUDIO_FRAME_SAMPLES];
  uint16_t count;
};

bool installMic() {
  const i2s_config_t config = {
      .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = AUDIO_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
      // Стерео: иначе второй микрофон (пин L/R на питании) просто не читается.
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      // Четыре буфера по 8 мс, а не восемь: кадр, который отдаёт драйвер, к
      // этому моменту уже 32 мс как записан. Чем длиннее эта очередь, тем
      // позже эхо приходит в компенсатор — и тем длиннее должен быть фильтр.
      .dma_buf_count = 4,
      .dma_buf_len = 128,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0,
  };

  if (i2s_driver_install(kMicPort, &config, 0, nullptr) != ESP_OK) {
    return false;
  }

  const i2s_pin_config_t pins = {
      .bck_io_num = pins::MIC_BCLK,
      .ws_io_num = pins::MIC_WS,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = pins::MIC_SD,
  };

  return i2s_set_pin(kMicPort, &pins) == ESP_OK;
}

// Устанавливает драйвер усилителя. Отдельно от installSpeaker(), потому что
// частота приходит от станции: эфир идёт 44.1 или 48 кГц, а речь — 16 кГц.
bool installSpeakerAt(uint32_t sampleRate) {
  const i2s_config_t config = {
      .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = sampleRate,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 128,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0,
  };

  if (i2s_driver_install(kSpeakerPort, &config, 0, nullptr) != ESP_OK) {
    return false;
  }

  const i2s_pin_config_t pins = {
      .bck_io_num = pins::SPK_BCLK,
      .ws_io_num = pins::SPK_LRC,
      .data_out_num = pins::SPK_DIN,
      .data_in_num = I2S_PIN_NO_CHANGE,
  };

  return i2s_set_pin(kSpeakerPort, &pins) == ESP_OK;
}

bool installSpeaker() {
#if SPEAKER_DAC_OUTPUT
  const i2s_config_t config = {
      .mode = static_cast<i2s_mode_t>(
          I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
      .sample_rate = AUDIO_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 128,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0,
  };

  if (i2s_driver_install(kSpeakerPort, &config, 0, nullptr) != ESP_OK) {
    return false;
  }

  // Встроенный ЦАП берёт 8 старших бит 16-битного отсчёта, поэтому отдельная
  // перекодировка не нужна. DAC1 = GPIO25, DAC2 = GPIO26.
  if (i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN) != ESP_OK) {
    return false;
  }

  // У режима ЦАП фиксированные выводы: свои пины назначать нельзя.
  return i2s_set_pin(kSpeakerPort, nullptr) == ESP_OK;
#else
  const i2s_config_t config = {
      .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = AUDIO_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 128,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0,
  };

  if (i2s_driver_install(kSpeakerPort, &config, 0, nullptr) != ESP_OK) {
    return false;
  }

  const i2s_pin_config_t pins = {
      .bck_io_num = pins::SPK_BCLK,
      .ws_io_num = pins::SPK_LRC,
      .data_out_num = pins::SPK_DIN,
      .data_in_num = I2S_PIN_NO_CHANGE,
  };

  return i2s_set_pin(kSpeakerPort, &pins) == ESP_OK;
#endif
}

// Звено фильтра высокой частоты (транспонированная прямая форма II).
struct Biquad {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
  float z1 = 0.0f;
  float z2 = 0.0f;

  inline float process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

// Фильтр Баттерворта 4-го порядка = два звена с разной добротностью.
void designHighPass(Biquad& stage, float rate, float cutoff, float q) {
  const float w0 = 2.0f * static_cast<float>(M_PI) * cutoff / rate;
  const float alpha = sinf(w0) / (2.0f * q);
  const float cosW0 = cosf(w0);
  const float a0 = 1.0f + alpha;

  stage.b0 = ((1.0f + cosW0) * 0.5f) / a0;
  stage.b1 = -(1.0f + cosW0) / a0;
  stage.b2 = stage.b0;
  stage.a1 = (-2.0f * cosW0) / a0;
  stage.a2 = (1.0f - alpha) / a0;
}

// Мягкий ограничитель после усиления: до колена — линейно, выше — плавно
// поджимаем, чтобы громкий слог не превращался в прямоугольник.
int16_t clampToInt16(int32_t value) {
  const int32_t magnitude = value < 0 ? -value : value;
  if (magnitude <= MIC_SOFT_KNEE) {
    return static_cast<int16_t>(value);
  }

  const float excess = static_cast<float>(magnitude - MIC_SOFT_KNEE);
  const float room = static_cast<float>(32767 - MIC_SOFT_KNEE);
  const float limited = static_cast<float>(MIC_SOFT_KNEE) + room * (excess / (excess + room));

  return static_cast<int16_t>(value < 0 ? -limited : limited);
}

uint32_t rmsOf(const int16_t* samples, size_t count) {
  if (count == 0) {
    return 0;
  }
  uint64_t sum = 0;
  for (size_t i = 0; i < count; ++i) {
    const int32_t value = samples[i];
    sum += static_cast<uint64_t>(value * value);
  }
  return static_cast<uint32_t>(sqrt(static_cast<double>(sum) / count));
}

}  // namespace

bool AudioPipeline::begin() {
  if (_ready) {
    return true;
  }

  // Замок порта создаём до задач: первая же смена режима «речь ↔ эфир» иначе
  // попадёт в незащищённый i2s_write.
  _speakerLock = xSemaphoreCreateMutex();
  if (_speakerLock == nullptr) {
    applog::line("[audio] нет замка порта усилителя");
  }

  if (!installMic()) {
    applog::line("[audio] микрофон не поднялся — колонка работает без входа");
  }
  _speakerReady = installSpeaker();
  if (!_speakerReady) {
    applog::line("[audio] усилитель не поднялся — колонка работает без выхода");
  }

  _prerollFrames = PREROLL_FRAMES;
  _preroll = static_cast<int16_t*>(malloc(_prerollFrames * AUDIO_FRAME_SAMPLES * sizeof(int16_t)));
  if (_preroll == nullptr) {
    applog::line("[audio] нет памяти под преролл");
    _prerollFrames = 0;
  }

  playbackQueue = xQueueCreate(kPlaybackQueueFrames, sizeof(PlaybackChunk));
  if (playbackQueue == nullptr) {
    applog::line("[audio] нет очереди воспроизведения");
  }

#if SPEAKER_HAS_AEC
  _aec.begin();
#endif

  xTaskCreatePinnedToCore(micTaskEntry, "mic", 6144, this, 10, &_micTask, 1);
  xTaskCreatePinnedToCore(playerTaskEntry, "spk", 4096, this, 7, &_playerTask, 0);

  _ready = true;
  return true;
}

bool AudioPipeline::takeSpeakerPort(uint32_t timeoutMs) {
  if (_speakerLock == nullptr) {
    return true;
  }

  return xSemaphoreTake(_speakerLock, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void AudioPipeline::releaseSpeakerPort() {
  if (_speakerLock != nullptr) {
    xSemaphoreGive(_speakerLock);
  }
}

void AudioPipeline::micTaskEntry(void* arg) {
  static_cast<AudioPipeline*>(arg)->micLoop();
}

void AudioPipeline::playerTaskEntry(void* arg) {
  static_cast<AudioPipeline*>(arg)->playerLoop();
}

void AudioPipeline::hush(uint32_t milliseconds) {
  const uint32_t until = millis() + milliseconds;
  _hushUntil = until > _hushUntil ? until : _hushUntil;
}

void AudioPipeline::resume() { _hushUntil = 0; }

void AudioPipeline::takeBurstPeaks(uint32_t& rms, uint32_t& zcr) {
  rms = _peakRmsWindow;
  zcr = _peakZcrWindow;
  _peakRmsWindow = 0;
  _peakZcrWindow = 0;
}

void AudioPipeline::micLoop() {
  size_t bytesRead = 0;
  uint16_t silenceFrames = 0;
  uint16_t speechFrames = 0;
  // Длина фразы по времени, а не по числу громких кадров: в шумной комнате
  // уровень гуляет ниже порога, speechFrames растёт медленно, и фраза
  // растягивалась на 20–40 с. Сервер такие куски отбрасывал как «музыку».
  uint16_t phraseFrames = 0;
  bool voiceActive = false;
  int warmupFrames = MIC_WARMUP_FRAMES;
  uint16_t micReadProbe = 0;
  int32_t noiseFloor = 0;

  // Состояние детектора чиха.
  bool sneezeActive = false;
  uint16_t sneezeFrames = 0;
  uint32_t _lastSneezeAt = 0;
  bool sneezePending = false;
  uint32_t sneezePendingAt = 0;

  // Отсечка инфразвука по каждому каналу: по два звена на канал.
  Biquad hp1Left;
  Biquad hp2Left;
  Biquad hp1Right;
  Biquad hp2Right;
  designHighPass(hp1Left, AUDIO_RATE, MIC_HIGHPASS_FC, 0.54119610f);
  designHighPass(hp2Left, AUDIO_RATE, MIC_HIGHPASS_FC, 1.30656296f);
  designHighPass(hp1Right, AUDIO_RATE, MIC_HIGHPASS_FC, 0.54119610f);
  designHighPass(hp2Right, AUDIO_RATE, MIC_HIGHPASS_FC, 1.30656296f);

  for (;;) {
    trace::mark("mic.read");
    const esp_err_t result = i2s_read(
        kMicPort, micBuffer, sizeof(micBuffer), &bytesRead, pdMS_TO_TICKS(100));

    if (result != ESP_OK || bytesRead == 0) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    // Диагностика полноты кадра: недобор означает, что хвост буфера остаётся
    // от прошлого чтения и уходит на сервер как «застрявший» звук.
    if (micReadProbe < 8 || bytesRead != sizeof(micBuffer)) {
      if (micReadProbe < 40) {
        applog::line(String("[mic] чтение ") + static_cast<unsigned>(bytesRead) + " из " +
                     static_cast<unsigned>(sizeof(micBuffer)));
      }
      ++micReadProbe;
    }

    // Пока играет звук в браузере, микрофон молчит: иначе колонка слышит
    // собственные колонки компьютера и не может отделить речь от музыки.
    if (_hushUntil != 0) {
      if (millis() < _hushUntil) {
        voiceActive = false;
        speechFrames = 0;
        silenceFrames = 0;
        phraseFrames = 0;
        _capturing = false;
        continue;
      }

      _hushUntil = 0;
    }

    // Захват останавливали посреди фразы (заговорил агент, включился mute):
    // состояние детектора надо сбросить, иначе после возобновления звук уйдёт
    // без события «начало речи» и склеится с прошлой фразой.
    if (_flushVad) {
      _flushVad = false;
      voiceActive = false;
      speechFrames = 0;
      silenceFrames = 0;
      phraseFrames = 0;

      // Преролл тоже забываем: иначе в начало следующей команды попадёт голос
      // агента из динамика или звук, снятый при выключенном микрофоне.
      _prerollWrite = 0;
      _prerollValid = 0;

      sneezeActive = false;
      sneezeFrames = 0;
      sneezePending = false;
    }

    // Буфер чередуется: левый, правый, левый, правый...
    const size_t total = bytesRead / sizeof(int32_t);
    const size_t frames = total / MIC_CHANNEL_COUNT;

    uint64_t sumLeft = 0;
    uint64_t sumRight = 0;

    // Первые полсекунды после установки драйвера — переходный процесс:
    // читаем и выбрасываем, иначе детектор сработает на самом включении.
    if (warmupFrames > 0) {
      --warmupFrames;
      continue;
    }

    // Больше кадра не обрабатываем: остаток уйдёт в следующий вызов.
    const size_t used = frames < AUDIO_FRAME_SAMPLES ? frames : AUDIO_FRAME_SAMPLES;

    for (size_t i = 0; i < used; ++i) {
      // Микрофоны отдают 24 бита в 32-битном слоте, выровнено по старшим.
      const float rawLeft = static_cast<float>(micBuffer[i * 2] >> MIC_SAMPLE_SHIFT);
      const float rawRight = static_cast<float>(micBuffer[i * 2 + 1] >> MIC_SAMPLE_SHIFT);

      const int32_t left = static_cast<int32_t>(hp2Left.process(hp1Left.process(rawLeft)));
      const int32_t right = static_cast<int32_t>(hp2Right.process(hp1Right.process(rawRight)));

      sumLeft += static_cast<uint64_t>(left * left);
      sumRight += static_cast<uint64_t>(right * right);

      const uint32_t absLeft = left < 0 ? static_cast<uint32_t>(-left) : static_cast<uint32_t>(left);
      const uint32_t absRight =
          right < 0 ? static_cast<uint32_t>(-right) : static_cast<uint32_t>(right);
      if (absLeft > _peakLeft) {
        _peakLeft = absLeft;
      }
      if (absRight > _peakRight) {
        _peakRight = absRight;
      }

      // На распознавание идёт один микрофон: сумма двух гасит часть спектра.
      const int32_t primary = MIC_PRIMARY_CHANNEL == 0 ? left : right;
      pixelBuffer[i] = clampToInt16(static_cast<int32_t>(primary * MIC_GAIN));
    }

    if (used == 0) {
      continue;
    }

#if defined(SPEAKER_MIC_SELFTEST)
    // Диагностика тракта без единого звука: подменяем микрофон эталонным тоном
    // известной частоты. Тон уходит только на сервер, в динамик не попадает,
    // поэтому по нему проверяются кадры, темп и разметка частоты на приёме.
    {
      static double phase = 0.0;
      static uint32_t burstFrame = 0;
      const double twoPi = 6.283185307179586;
      const double step = twoPi * static_cast<double>(SPEAKER_MIC_SELFTEST) / AUDIO_RATE;
      // Тон идёт вспышками: 2 с звука, 3 с тишины. Без паузы детектор речи
      // никогда не закончит фразу и запись на сервер не уйдёт.
      const bool sounding = (burstFrame % 250) < 100;
      ++burstFrame;
      for (size_t i = 0; i < used; ++i) {
        const double value = sounding ? 8000.0 * sin(phase) : 0.0;
        pixelBuffer[i] = static_cast<int16_t>(value);
        phase += step;
        if (phase > twoPi) {
          phase -= twoPi;
        }
      }
    }
#endif

#if defined(SPEAKER_AEC_DUMP) && SPEAKER_HAS_AEC
    // Снимок микрофона до компенсатора: по нему вместе с референсом видно,
    // есть ли в референсе эхо и на какой задержке.
    if (_mediaMode) {
      if (_dumpMediaSince == 0) {
        _dumpMediaSince = millis();
      }
      for (size_t i = 0; i < used; ++i) {
        _dumpMic[_dumpMicWrite] = pixelBuffer[i];
        _dumpMicWrite = (_dumpMicWrite + 1) % kDumpMic;
        if (_dumpMicValid < kDumpMic) {
          ++_dumpMicValid;
        }
      }
    }
#endif

#if SPEAKER_HAS_AEC
    // Эхо собственной музыки вычитается до детектора речи. Без этого порог
    // приходится поднимать выше самой музыки, и колонка «глохнет» на всё время
    // эфира: в поток уходит музыка, а голос поверх неё не разобрать.
    if (_mediaMode) {
      _aec.process(pixelBuffer, used);
    }
#endif

#if defined(SPEAKER_AEC_DUMP) && SPEAKER_HAS_AEC
    if (_mediaMode && !_dumpDone && _dumpMediaSince != 0
        && millis() - _dumpMediaSince > 6000 && _dumpMicValid == kDumpMic) {
      _dumpDone = true;
      dumpEchoOnce();
    }
#endif

    ++_framesCaptured;

    _lastRmsLeft = static_cast<uint32_t>(sqrt(static_cast<double>(sumLeft) / used));
    _lastRmsRight = static_cast<uint32_t>(sqrt(static_cast<double>(sumRight) / used));

    // Считаем ровно по заполненной части: раньше длина бралась из прочитанного,
    // и при неполном кадре RMS считался по непроинициализированной памяти.
    //
    // rms — уровень до AGC. По нему работают детекторы (речь, чих, фон): они
    // должны видеть сигнал одинаково при любом текущем усилении, иначе
    // чувствительность «плавала» бы вместе с регулятором.
    const uint32_t rms = rmsOf(pixelBuffer, used);
    _lastRms = rms;

    // Детектор чиха. По живым замерам чих — это один короткий всплеск
    // 80–360 мс с пиком 2600–5000 при фоне 130–500. Шипящие (переходы через
    // ноль) не помогают: фон в комнате уже широкополосный, у шума и у чиха
    // они одинаковые. Поэтому смотрим на громкость и длительность, а от
    // повторных срабатываний спасает пауза в несколько секунд.
    uint16_t crossings = 0;
    for (size_t i = 1; i < used; ++i) {
      if ((pixelBuffer[i - 1] < 0) != (pixelBuffer[i] < 0)) {
        ++crossings;
      }
    }

    const uint32_t zcr = used > 1 ? (crossings * 1000u) / (used - 1) : 0;

    if (rms > _peakRmsWindow) {
      _peakRmsWindow = rms;
    }
    if (zcr > _peakZcrWindow) {
      _peakZcrWindow = zcr;
    }

    const uint32_t nowMs = millis();

    // Чих ищем только в тишине и только при включённом микрофоне: на музыке и
    // шуме всплеск «чиха» ловится постоянно, а сервер на него отвечал целой
    // задачей ассистенту. Порог считаем от фона, а не абсолютным числом.
    const uint32_t sneezeThreshold = max<uint32_t>(
        SNEEZE_RMS_START, noiseFloor > 0 ? static_cast<uint32_t>(noiseFloor) * 8 : 0);

    if (!sneezeActive) {
      if (_capturing && !voiceActive && !_mediaMode && rms >= sneezeThreshold
          && nowMs - _lastSneezeAt >= SNEEZE_COOLDOWN_MS) {
        sneezeActive = true;
        sneezeFrames = 1;
      }
    } else {
      ++sneezeFrames;

      if (rms < SNEEZE_RMS_START / 2 || sneezeFrames > SNEEZE_MAX_FRAMES) {
        if (sneezeFrames >= SNEEZE_MIN_FRAMES && sneezeFrames <= SNEEZE_MAX_FRAMES) {
          // Всплеск закончился — но это ещё не чих: у речи и музыки за ним
          // сразу идёт следующий звук. Настоящий чих обрывается тишиной,
          // поэтому ждём паузу и проверяем её.
          sneezePending = true;
          sneezePendingAt = nowMs;
        }

        sneezeActive = false;
        sneezeFrames = 0;
      }
    }

    if (sneezePending) {
      if (!_capturing) {
        // Микрофон выключили посреди всплеска — «чиха» не было.
        sneezePending = false;
      } else if (rms > 1200) {
        sneezePending = false;
      } else if (nowMs - sneezePendingAt >= SNEEZE_QUIET_MS) {
        sneezePending = false;
        _lastSneezeAt = nowMs;

        if (_onSneeze) {
          trace::mark("sneeze.cb");
          _onSneeze();
        }
      }
    }

    // Кольцо преролла обновляется всегда, пока идёт тишина: иначе на сервер
    // уходил бы один и тот же застарелый кусок с момента включения.
    //
    // Оценка фона: минимум в окне 200 мс, сглаженный за пару секунд.
    //
    // Простое среднее (как было) в шумной комнате поднимает порог на минуты, и
    // тихая речь перестаёт проходить. Абсолютный минимум за всё время —
    // наоборот, слишком оптимистичен: он ловит случайную тишину между шумом
    // вентилятора, порог падает, и колонка начинает срабатывать на сам шум.
    // Окно 10 кадров даёт «типичную тишину», а сглаживание убирает дребезг.
    static uint32_t windowMin = 0xFFFFFFFFu;
    static uint8_t windowFrames = 0;
    if (rms < windowMin) {
      windowMin = rms;
    }
    if (++windowFrames >= 10) {
      // При играющем эфире то же окно минимумов даёт «пол» остатка музыки.
      // Он затухает медленно (1/8 за окно), чтобы тихий такт песни не снял
      // порог, и не обновляется во время фразы — иначе голос поднял бы его.
      if (_mediaMode && !voiceActive) {
        const uint32_t floorValue = windowMin;
        if (_mediaNoiseFloor == 0) {
          _mediaNoiseFloor = floorValue;
        } else {
          _mediaNoiseFloor = (_mediaNoiseFloor * 7 + floorValue) / 8;
        }
      }

      // Пока человек говорит, окно не трогаем: его паузы всё равно громче
      // комнаты, и фон «подполз» бы вверх за одну длинную фразу.
      //
      // При играющем эфире фон тоже не трогаем: это не шум комнаты, а наша
      // собственная музыка, и порог от неё считается отдельно (ниже).
      if (!voiceActive && !_mediaMode) {
        if (noiseFloor <= 0) {
          noiseFloor = static_cast<int32_t>(windowMin);
        } else {
          noiseFloor = static_cast<int32_t>(
              (static_cast<int32_t>(noiseFloor) * 7 + static_cast<int32_t>(windowMin)) / 8);
        }
      }
      windowMin = 0xFFFFFFFFu;
      windowFrames = 0;
    }

    // Запас над шумом должен быть больше, чем разброс шума: днём в комнате
    // уровень гуляет в полтора раза, и меньший запас давал ложные срабатывания
    // на скрип кресла и стук по столу. Отсюда 2.4× на включение и 1.8× на
    // выключение — порог адаптивный, но заметно ниже прежних 5× и 3×.
    const uint32_t noise = static_cast<uint32_t>(noiseFloor > 0 ? noiseFloor : 0);
    uint32_t vadOn = max<uint32_t>(VAD_RMS_ON, noise * VAD_NOISE_ON_X10 / 10);
    uint32_t vadOff = max<uint32_t>(VAD_RMS_OFF, noise * VAD_NOISE_OFF_X10 / 10);

    // При играющем эфире порог считается от остатка эха, а не от «фона
    // комнаты»: комната с работающим динамиком тише динамика не бывает.
    //
    // Важно, что берётся именно ПОЛ остатка, а не его пик. Пик — это самый
    // громкий такт музыки; порог от него уходил на 7–20 тысяч, и голос с
    // двух метров (900–1400) не проходил никогда: колонка полностью глохла
    // на всё время эфира. Пол же близок к типичному остатку, и голос,
    // поднимающий уровень втрое, проходит.
    const bool mediaHold = _mediaMode && nowMs < _mediaVadHoldUntil;

    if (_mediaMode) {
#if SPEAKER_HAS_AEC
      // Микрофон упирается в предел раньше, чем это слышно: в закрытом корпусе
      // музыка даёт в микрофоне пики 22–26 тысяч из 32. На клиппированном
      // сигнале линейный компенсатор бесполезен (замер на плате: гашение
      // 1.2 дБ), а голос поверх музыки распознать нечем. Поэтому эфир
      // приглушается ровно настолько, чтобы микрофон оставался линейным, и
      // возвращается, когда музыка стихает.
      const uint32_t micIn = _aec.inputRms();
      if (micIn > MEDIA_ECHO_CLIP_RMS && _mediaLimitGain > MEDIA_LIMIT_MIN_GAIN) {
        _mediaLimitGain -= MEDIA_LIMIT_STEP;
        if (_mediaLimitGain < MEDIA_LIMIT_MIN_GAIN) {
          _mediaLimitGain = MEDIA_LIMIT_MIN_GAIN;
        }
      } else if (micIn < MEDIA_ECHO_CLIP_RMS / 2 && _mediaLimitGain < 1.0f) {
        _mediaLimitGain += MEDIA_LIMIT_STEP / 4;
        if (_mediaLimitGain > 1.0f) {
          _mediaLimitGain = 1.0f;
        }
      }
#endif

      const uint32_t echoFloor = _mediaNoiseFloor;
      vadOn = max<uint32_t>(vadOn, echoFloor * MEDIA_ECHO_ON_X10 / 10);
      vadOff = max<uint32_t>(vadOff, echoFloor * MEDIA_ECHO_OFF_X10 / 10);
    }

    // Глушить эфир можно только тогда, когда эхо действительно вычтено. Пока
    // компенсатор не сошёлся, фразу открывает сама музыка, и приглушение
    // превращается в качели: заглушили → уровень упал → фраза закрылась →
    // эфир вернулся → через 400 мс снова открылась.
#if SPEAKER_HAS_AEC
    const bool echoSuppressed = _mediaMode && _aec.erleDb() >= MEDIA_DUCK_MIN_ERLE_DB;
#else
    const bool echoSuppressed = false;
#endif

    _noiseFloor = noise;

#if SPEAKER_HAS_AEC
    // Диагностика на живой плате: по задержке видно, что корреляция нашлась, а
    // по гашению — помогает ли компенсатор или порог держится только запасом.
    {
      static uint32_t lastAecLog = 0;
      if (_mediaMode && nowMs - lastAecLog >= 5000) {
        lastAecLog = nowMs;
        applog::printf("[aec] пик %.2f на %u мс, задержка %u мс, микрофон %u -> %u, "
                       "гашение %.1f дБ, порог %u, пол %u\n",
                       _aec.probeScore(), static_cast<unsigned>(_aec.probeLagMs()),
                       static_cast<unsigned>(_aec.delayMs()),
                       static_cast<unsigned>(_aec.inputRms()),
                       static_cast<unsigned>(_aec.outputRms()), _aec.erleDb(),
                       static_cast<unsigned>(vadOn),
                       static_cast<unsigned>(_mediaNoiseFloor));
      }
    }
#endif

    // AGC: и тихую, и громкую речь приводим к целевому уровню. Громкую не
    // только не поднимаем, но и приглушаем: у открытого микрофона близкий
    // голос упирался в ограничитель, срезалось начало слова, и распознаватель
    // получал «ключи радио» вместо «включи радио». Включается только когда
    // сигнал явно выше фона, поэтому тишина остаётся тихой, а пороги VAD
    // (они считаются по сигналу до AGC) не зависят от усиления.
    static float agcBoost = 1.0f;
    float targetBoost = 1.0f;
    if (rms >= MIC_AGC_MIN_RMS && rms > noise * MIC_AGC_NOISE_X) {
      targetBoost = constrain(MIC_AGC_TARGET / static_cast<float>(rms), MIC_AGC_MIN_BOOST,
                              MIC_AGC_MAX_BOOST);
    }
    // Вниз — быстро (перегрузку надо снимать сразу), вверх — плавно, иначе
    // шум между словами раздувается до уровня речи.
    agcBoost += (targetBoost - agcBoost) * (targetBoost < agcBoost ? 0.5f : 0.05f);

    if (agcBoost > 1.02f || agcBoost < 0.98f) {
      for (size_t i = 0; i < used; ++i) {
        pixelBuffer[i] = clampToInt16(
            static_cast<int32_t>(static_cast<float>(pixelBuffer[i]) * agcBoost));
      }
    }

    // Уровень для анимации рта считается уже после AGC: рот должен открываться
    // и на тихой фразе, иначе кажется, что колонка не слушает.
    const uint32_t levelRms = rmsOf(pixelBuffer, used);
    _lastRms = levelRms;

    if (!_capturing) {
      if (_onLevel) {
        _onLevel(static_cast<uint8_t>(min<uint32_t>(255, levelRms / MIC_LEVEL_DIVISOR)));
      }
      continue;
    }

    // Преролл пишем только при включённом микрофоне. Иначе после mute на сервер
    // первым же кадром уходил звук, записанный при выключенном захвате, — это
    // нарушало обещание «пока mute, кадры не уходят».
    if (!voiceActive && _preroll != nullptr && _prerollFrames > 0) {
      memcpy(_preroll + _prerollWrite * AUDIO_FRAME_SAMPLES, pixelBuffer,
             AUDIO_FRAME_SAMPLES * sizeof(int16_t));
      _prerollWrite = (_prerollWrite + 1) % _prerollFrames;

      if (_prerollValid < _prerollFrames) {
        ++_prerollValid;
      }
    }

    if (!voiceActive) {
      if (rms >= vadOn && !mediaHold) {
        voiceActive = true;
        silenceFrames = 0;
        speechFrames = 0;
        phraseFrames = 0;

        if (_onVad) {
          _onVad(true, rms);
        }
        // Отдаём только реально записанные кадры: после сброса детектора кольцо
        // может быть пустым, и в начало фразы не должен попасть старый звук.
        if (_preroll != nullptr && _prerollValid > 0) {
          const size_t first =
              (_prerollWrite + _prerollFrames - _prerollValid) % _prerollFrames;

          for (size_t frame = 0; frame < _prerollValid; ++frame) {
            const size_t index = (first + frame) % _prerollFrames;
            if (_onFrame) {
              _onFrame(reinterpret_cast<const uint8_t*>(_preroll + index * AUDIO_FRAME_SAMPLES),
                       AUDIO_FRAME_SAMPLES * sizeof(int16_t));
            }
          }
        }

        _prerollValid = 0;
      } else if (_onLevel) {
        _onLevel(static_cast<uint8_t>(min<uint32_t>(255, levelRms / MIC_LEVEL_DIVISOR)));
      }
    }

    if (!voiceActive) {
      continue;
    }

    if (_onFrame) {
      trace::mark("mic.frame->net");
      _onFrame(reinterpret_cast<const uint8_t*>(pixelBuffer),
               AUDIO_FRAME_SAMPLES * sizeof(int16_t));
    }

    // Уровень обновляется и во время фразы: иначе рот замирал бы на первом
    // кадре и речь выглядела как ровное свечение.
    if (_onLevel) {
      _onLevel(static_cast<uint8_t>(min<uint32_t>(255, levelRms / MIC_LEVEL_DIVISOR)));
    }

    ++phraseFrames;

    // Эфир уходит в тишину не в момент срабатывания, а когда фраза уже
    // подтвердилась: иначе стук посуды выключал музыку на доли секунды.
    // Дальше распознаватель получает голос без музыки, а не микс из динамика
    // и человека.
    //
    // И только если компенсатор эха действительно гасит эхо. Пока он не сошёлся,
    // фразу открывает сама музыка: заглушив её, мы сами роняем уровень ниже
    // порога, фраза закрывается, эфир возвращается — и через 400 мс всё
    // повторяется. Снаружи это слышно как «музыка икает».
#if !defined(SPEAKER_MEDIA_NO_DUCK)
    if (_mediaMode && phraseFrames == MEDIA_DUCK_AFTER_FRAMES && echoSuppressed) {
      _mediaDuckTarget = 0.0f;
    }
#endif

    if (rms >= vadOff) {
      silenceFrames = 0;
      ++speechFrames;
    } else {
      ++silenceFrames;
    }

    // Музыка и гул не заканчиваются сами: без верхнего предела фраза не
    // закрывается никогда, детектор «слушает вечно», а новые команды уже не
    // принимаются. Обрываем затянувшийся фрагмент принудительно — по полной
    // длине фразы, чтобы кусок не превысил предел распознавания.
    const bool tooLong = phraseFrames >= MAX_PHRASE_FRAMES;

    if (silenceFrames >= END_SILENCE_FRAMES || tooLong) {
      voiceActive = false;
      if (speechFrames >= MIN_SPEECH_FRAMES) {
        if (_onVad) {
          _onVad(false, rms);
        }
      } else if (_onVad) {
        // Хлопок или кашель: фразу не отправляем, но и не считаем концом речи.
        _onVad(false, rms);
      }
      _capturing = false;

      if (_mediaMode) {
        // Эфир возвращается сам, но не мгновенно: пока он поднимается, порог
        // уже не должен ловить собственный динамик.
        _mediaDuckTarget = 1.0f;
        _mediaVadHoldUntil = millis() + MEDIA_VAD_RESTORE_MS;
      }
    }
  }
}

void AudioPipeline::speakerSelfTest() {
  if (!_speakerReady) {
    applog::line("[spk] проверка не пройдена: драйвер I2S не поднялся");
    return;
  }

  // Тоны пишутся прямо в I2S, минуя очередь воспроизведения: она держит
  // 160 мс, и длинный сигнал из неё вытеснился бы.
  static const uint16_t kFrequencies[] = {440, 1000, 2000};
  constexpr uint16_t kToneMs = 400;
  constexpr size_t kFadeSamples = AUDIO_RATE / 100;  // 10 мс
  const size_t toneSamples = static_cast<size_t>(AUDIO_RATE) * kToneMs / 1000;
  int16_t stereo[AUDIO_FRAME_SAMPLES * 2];

  applog::line("[spk] проверка выхода: 440, 1000, 2000 Гц");

  for (uint16_t frequency : kFrequencies) {
    size_t offset = 0;

    while (offset < toneSamples) {
      const size_t take = min(toneSamples - offset, static_cast<size_t>(AUDIO_FRAME_SAMPLES));

      for (size_t i = 0; i < take; ++i) {
        const size_t index = offset + i;
        float gain = 1.0f;

        // Плавный вход и выход: без них на краях тона слышен щелчок, и
        // «проверка прошла» путается с «усилитель хрипит».
        if (index < kFadeSamples) {
          gain = static_cast<float>(index) / kFadeSamples;
        } else if (index + kFadeSamples >= toneSamples) {
          gain = static_cast<float>(toneSamples - index) / kFadeSamples;
        }

        const int16_t value = static_cast<int16_t>(
            sinf(2.0f * PI * frequency * index / AUDIO_RATE) * 9000.0f * gain);
        stereo[i * 2] = value;
        stereo[i * 2 + 1] = value;
      }

      size_t written = 0;
      i2s_write(kSpeakerPort, stereo, take * 2 * sizeof(int16_t), &written,
                pdMS_TO_TICKS(200));
      offset += take;
    }
  }

  applog::line("[spk] проверка выхода закончена");
}

#if defined(SPEAKER_AEC_DUMP) && SPEAKER_HAS_AEC
void AudioPipeline::dumpEchoOnce() {
  static int16_t reference[kDumpRef];
  static char line[132];

  const size_t refCount = _aec.copyReference(reference, kDumpRef);
  applog::printf("[aecdump] rate %u, mic %u, ref %u, задержка %u мс, гашение %.1f дБ\n",
                 static_cast<unsigned>(_mediaRate), static_cast<unsigned>(kDumpMic),
                 static_cast<unsigned>(refCount),
                 static_cast<unsigned>(_aec.delayMs()), _aec.erleDb());

  // Оба снимка заканчиваются одним моментом. Микрофон лежит в кольце от самого
  // старого отсчёта к самому новому, референс уже развёрнут так же.
  for (size_t pass = 0; pass < 2; ++pass) {
    const int16_t* data = pass == 0 ? _dumpMic : reference;
    const size_t count = pass == 0 ? kDumpMic : refCount;
    const size_t ring = pass == 0 ? kDumpMic : kDumpRef;
    const size_t start = pass == 0 ? _dumpMicWrite : 0;
    const char* tag = pass == 0 ? "m" : "r";

    for (size_t offset = 0; offset < count; offset += 24) {
      const size_t take = count - offset < 24 ? count - offset : 24;
      size_t at = 0;
      for (size_t i = 0; i < take; ++i) {
        const uint16_t value = static_cast<uint16_t>(data[(start + offset + i) % ring]);
        at += snprintf(line + at, sizeof(line) - at, "%04x", value);
      }
      applog::printf("[aecdump] %s %s\n", tag, line);
    }
  }

  applog::line("[aecdump] done");
}
#endif

void AudioPipeline::leaveMediaMode() {
  if (!_mediaMode) {
    return;
  }

  // Драйвер переставляем только когда задача речи не пишет в порт. Флаг режима
  // снимаем последним, уже после установки драйвера речи: иначе задача речи
  // успела бы отдать кадр в удалённый драйвер.
  const bool owned = takeSpeakerPort(2000);
  i2s_driver_uninstall(kSpeakerPort);
  _mediaRate = 0;
  _speakerReady = installSpeaker();
  _mediaMode = false;
  if (owned) {
    releaseSpeakerPort();
  }

  // Эхо кончилось вместе с музыкой: оценку и веса фильтра держать незачем.
  _mediaNoiseFloor = 0;
  _mediaDuckTarget = 1.0f;
  _mediaDuckGain = 1.0f;
  _mediaLimitGain = 1.0f;
  _mediaVadHoldUntil = 0;
#if SPEAKER_HAS_AEC
  _aec.reset();
#endif

  if (!_speakerReady) {
    applog::line("[media] усилитель не вернулся в режим речи");
  }
}

bool AudioPipeline::enterMediaMode(uint32_t sampleRate) {
  const uint32_t rate = sampleRate > 0 ? sampleRate : AUDIO_RATE;

  if (_mediaMode) {
    if (rate != _mediaRate) {
      // Станция сменила частоту посреди эфира: драйвер перестраивается на ходу.
      const bool owned = takeSpeakerPort(2000);
      i2s_set_clk(kSpeakerPort, rate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
      _mediaRate = rate;
      designBassShelf(rate);
      if (owned) {
        releaseSpeakerPort();
      }
#if SPEAKER_HAS_AEC
      // Референс теперь другого темпа: старые веса и задержка к нему не годятся.
      _aec.reset();
      _mediaVadHoldUntil = millis() + MEDIA_VAD_HOLD_MS;
#endif
    }

    return true;
  }

  // Даём доиграть уже полученной речи: сервер закрывает поток раньше, чем
  // колонка отыграла буфер, и прежний сброс резал последний слог ответа.
  drainPlayback(PLAYBACK_DRAIN_MS);

  // То, что не доиграло за отведённое время, в чужой частоте звучать не должно.
  flushPlayback();

  // Флаг поднимаем до захвата порта: задача речи, которая уже пишет кадр,
  // спокойно его допишет, а следующую запись не начнёт.
  _mediaMode = true;

  if (!takeSpeakerPort(2000)) {
    _mediaMode = false;
    return false;
  }

  if (_speakerReady) {
    i2s_driver_uninstall(kSpeakerPort);
    _speakerReady = false;
  }

  if (!installSpeakerAt(rate)) {
    applog::printf("[media] не удалось открыть выход на %u Гц\n", static_cast<unsigned>(rate));
    _speakerReady = installSpeaker();
    _mediaMode = false;
    releaseSpeakerPort();

    return false;
  }

  _mediaRate = rate;
  _mediaLastLeft = 0;
  _mediaLastRight = 0;
  designBassShelf(rate);

  // Музыка начинается с полной громкости, а эхо ещё не измерено: пока
  // компенсатор не сошёлся, детектор речи закрыт, иначе первые кадры эфира
  // уходят на сервер как «фраза».
  _mediaNoiseFloor = 0;
  _mediaDuckTarget = 1.0f;
  _mediaDuckGain = 1.0f;
  _mediaLimitGain = 1.0f;
  _mediaVadHoldUntil = millis() + MEDIA_VAD_HOLD_MS;
#if SPEAKER_HAS_AEC
  _aec.reset();
#endif

  releaseSpeakerPort();

  return true;
}

bool AudioPipeline::playbackIdle() const {
  if (playbackQueue != nullptr && uxQueueMessagesWaiting(playbackQueue) > 0) {
    return false;
  }

  return static_cast<int32_t>(millis() - _playbackQuietUntil) >= 0;
}

void AudioPipeline::drainPlayback(uint32_t milliseconds) {
  const uint32_t startedAt = millis();

  while (!playbackIdle() && millis() - startedAt < milliseconds) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void AudioPipeline::designBassShelf(uint32_t rate) {
  constexpr float kFrequency = 120.0f;
  constexpr float kGainDb = 8.0f;
  constexpr float kQ = 0.707f;

  const float fs = rate > 0 ? static_cast<float>(rate) : static_cast<float>(AUDIO_RATE);
  const float amp = powf(10.0f, kGainDb / 40.0f);
  const float w0 = 2.0f * 3.14159265f * kFrequency / fs;
  const float cosW = cosf(w0);
  const float sinW = sinf(w0);
  const float alpha = sinW / (2.0f * kQ);
  const float root = 2.0f * sqrtf(amp) * alpha;

  const float b0 = amp * ((amp + 1.0f) - (amp - 1.0f) * cosW + root);
  const float b1 = 2.0f * amp * ((amp - 1.0f) - (amp + 1.0f) * cosW);
  const float b2 = amp * ((amp + 1.0f) - (amp - 1.0f) * cosW - root);
  const float a0 = (amp + 1.0f) + (amp - 1.0f) * cosW + root;
  const float a1 = -2.0f * ((amp - 1.0f) + (amp + 1.0f) * cosW);
  const float a2 = (amp + 1.0f) + (amp - 1.0f) * cosW - root;

  _bassLeft = MediaBiquad();
  _bassRight = MediaBiquad();

  _bassLeft.b0 = _bassRight.b0 = b0 / a0;
  _bassLeft.b1 = _bassRight.b1 = b1 / a0;
  _bassLeft.b2 = _bassRight.b2 = b2 / a0;
  _bassLeft.a1 = _bassRight.a1 = a1 / a0;
  _bassLeft.a2 = _bassRight.a2 = a2 / a0;
}

size_t AudioPipeline::writeMediaPcm(const int16_t* interleaved, size_t frames) {
  if (!_mediaMode || interleaved == nullptr || frames == 0) {
    return 0;
  }

  // Громкость та же, что у речи. Это не только удобство: на полной шкале
  // усилитель тянет под ампер, и от USB-порта плата уходит в BROWNOUT_RST —
  // то есть тише здесь ещё и безопаснее для питания.
  //
  // Эфир тише речи: станции сведены с полной шкалой, и на колонке они
  // перекрывали ответы помощника.
  const float baseGain = static_cast<float>(
      static_cast<int32_t>(_volume) * _volume * 128 / 10000 * MEDIA_GAIN_PERCENT / 100)
      / 128.0f;

  // Пока колонка слушает фразу, эфир уходит в тишину: распознавателю нужен
  // голос, а не музыка поверх него. Переход плавный, иначе в динамике щелчок.
  const float duckTarget = _mediaDuckTarget * _mediaLimitGain;
  const float duckStep = _mediaRate > 0
                             ? 1000.0f / (MEDIA_DUCK_MS * static_cast<float>(_mediaRate))
                             : 1.0f;
  float duck = _mediaDuckGain;

  for (size_t i = 0; i + 1 < frames * 2; i += 2) {
    const int32_t boostedLeft = static_cast<int32_t>(_bassLeft.process(interleaved[i]));
    const int32_t boostedRight = static_cast<int32_t>(_bassRight.process(interleaved[i + 1]));

    if (duck < duckTarget) {
      duck += duckStep;
      if (duck > duckTarget) {
        duck = duckTarget;
      }
    } else if (duck > duckTarget) {
      duck -= duckStep;
      if (duck < duckTarget) {
        duck = duckTarget;
      }
    }

    const float gain = baseGain * duck;
    const int32_t left = static_cast<int32_t>(boostedLeft * gain);
    const int32_t right = static_cast<int32_t>(boostedRight * gain);

    mediaBuffer[i] = static_cast<int16_t>(
        left > INT16_MAX ? INT16_MAX : (left < INT16_MIN ? INT16_MIN : left));
    mediaBuffer[i + 1] = static_cast<int16_t>(
        right > INT16_MAX ? INT16_MAX : (right < INT16_MIN ? INT16_MIN : right));
  }

  _mediaDuckGain = duck;

  _mediaLastLeft = mediaBuffer[(frames - 1) * 2];
  _mediaLastRight = mediaBuffer[(frames - 1) * 2 + 1];

  // Каждый кусок отдаём компенсатору прямо перед записью в I2S: так референс
  // и звук в динамике расходятся не больше чем на длину куска.
  size_t written = 0;
  for (size_t offset = 0; offset < frames; offset += kMediaAecChunk) {
    const size_t chunk = min(kMediaAecChunk, frames - offset);

    // Под замком только запись: кусок — 6 мс, дольше порт никто не держит.
    if (!takeSpeakerPort(1000)) {
      continue;
    }

#if SPEAKER_HAS_AEC
    _aec.pushPlayback(mediaBuffer + offset * 2, chunk, _mediaRate);
#endif

    size_t part = 0;
    i2s_write(kSpeakerPort, mediaBuffer + offset * 2, chunk * 2 * sizeof(int16_t), &part,
              pdMS_TO_TICKS(1000));
    written += part;
    releaseSpeakerPort();
  }

  return written / (2 * sizeof(int16_t));
}

void AudioPipeline::fadeOutMedia(uint32_t milliseconds) {
  if (!_mediaMode) {
    return;
  }

  const uint32_t rate = _mediaRate > 0 ? _mediaRate : AUDIO_RATE;
  size_t frames = rate * milliseconds / 1000;

  if (frames == 0) {
    frames = 1;
  }

  // frames — это стереопары, а kMediaSamples считает отдельные отсчёты: на
  // 44.1 кГц 40 мс давали 1764 пары вместо 1152 и затирали соседние буферы.
  // Отсюда были перезагрузки сразу после «одну секунду» на играющем эфире.
  constexpr size_t kMaxFadeFrames = kMediaSamples / 2;

  if (frames > kMaxFadeFrames) {
    frames = kMaxFadeFrames;
  }

  // Линейный спуск от последнего отсчёта к нулю: волна заканчивается там, где
  // оборвалась, поэтому переключение на речь не даёт щелчка и «обрывка музыки».
  for (size_t i = 0; i < frames; ++i) {
    const int32_t factor = static_cast<int32_t>(frames - i);

    mediaBuffer[i * 2] = static_cast<int16_t>(
        static_cast<int32_t>(_mediaLastLeft) * factor / static_cast<int32_t>(frames));
    mediaBuffer[i * 2 + 1] = static_cast<int16_t>(
        static_cast<int32_t>(_mediaLastRight) * factor / static_cast<int32_t>(frames));
  }

  size_t written = 0;
  const bool owned = takeSpeakerPort(1000);
  i2s_write(kSpeakerPort, mediaBuffer, frames * 2 * sizeof(int16_t), &written,
            pdMS_TO_TICKS(200));
  if (owned) {
    releaseSpeakerPort();
  }

  _mediaLastLeft = 0;
  _mediaLastRight = 0;
}

void AudioPipeline::playerLoop() {
  PlaybackChunk chunk;
  int16_t stereo[AUDIO_FRAME_SAMPLES * 2];
  bool primed = false;

  for (;;) {
    if (playbackQueue == nullptr) {
      _outputLevel = 0;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Эфир забрал порт: речь в это время не пишется, иначе два потока
    // смешались бы в мусор на одной шине.
    if (_mediaMode) {
      _outputLevel = 0;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // Перед фразой копим запас: сеть отдаёт кадры рывками, и без буфера первые
    // слоги рвутся щелчками. Ждём не дольше 400 мс — «промолчать совсем» хуже,
    // чем сыграть с меньшим запасом.
    if (!primed) {
      const uint32_t startedAt = millis();

      while (uxQueueMessagesWaiting(playbackQueue) < kPlaybackPrimeFrames
             && millis() - startedAt < 400) {
        vTaskDelay(pdMS_TO_TICKS(5));
      }

      primed = true;
    }

    // Порт берём до чтения очереди: пока держим замок, задача эфира не может
    // удалить драйвер посреди записи. Ожидание кадра — до 100 мс, поэтому
    // переход в эфир задерживается не дольше этого времени.
    if (!takeSpeakerPort(100)) {
      _outputLevel = 0;
      primed = false;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Режим мог переключиться, пока ждали замок.
    if (_mediaMode) {
      releaseSpeakerPort();
      _outputLevel = 0;
      primed = false;
      continue;
    }

    // Ждём с таймаутом, а не вечно: когда речи нет, уровень должен упасть до
    // нуля, иначе рот останется открытым на последнем слоге.
    if (xQueueReceive(playbackQueue, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) {
      releaseSpeakerPort();
      _outputLevel = 0;
      primed = false;
      continue;
    }

    // Кадр уже вне очереди, но ещё не прозвучал: помечаем хвост сразу, иначе
    // микрофон мог вернуться в этот зазор.
    _playbackQuietUntil = millis() + PLAYBACK_TAIL_MS;

    // Громкость — умножение с округлением, а не сдвиг: сдвиг терял младшие биты
    // и на тихих местах речи был слышен шершавый шум. Кривая квадратичная: ухо
    // слышит громкость нелинейно, и при линейной шкале тихие шаги почти не
    // меняли звук, а верхние — сразу упирались в клип усилителя.
    const int32_t gain = static_cast<int32_t>(_volume) * _volume * 128 / 10000;

    for (size_t i = 0; i < chunk.count; ++i) {
      const int32_t scaled = (static_cast<int32_t>(chunk.samples[i]) * gain + 64) >> 7;
      const int16_t value = static_cast<int16_t>(
          scaled > INT16_MAX ? INT16_MAX : (scaled < INT16_MIN ? INT16_MIN : scaled));
      stereo[i * 2] = value;
      stereo[i * 2 + 1] = value;
    }

    size_t written = 0;
    i2s_write(kSpeakerPort, stereo, chunk.count * 2 * sizeof(int16_t), &written,
              pdMS_TO_TICKS(200));
    releaseSpeakerPort();

    // Хвост отзвучит не сразу: пока он звучит в комнате, микрофон возвращать
    // нельзя, иначе он запишет собственный голос агента.
    _playbackQuietUntil = millis() + PLAYBACK_TAIL_MS;

    // Уровень отданного звука — для анимации рта агента. Считаем по тому, что
    // реально ушло в усилитель: громкость уже применена, и рот открывается
    // ровно настолько, насколько громко звучит речь.
    const uint32_t level = rmsOf(chunk.samples, chunk.count);
    _outputLevel = static_cast<uint8_t>(
        min<uint32_t>(255, level / SPEAKER_VOICE_LEVEL_DIVISOR));
  }
}

void AudioPipeline::startCapture() {
  if (!_ready) {
    return;
  }
  _capturing = true;
}

void AudioPipeline::stopCapture() {
  _capturing = false;
  // Сброс детектора произойдёт в задаче записи: сюда она не должна заглядывать.
  _flushVad = true;

  // Эфир возвращается, если фразу закрыли извне ( mute, речь агента): иначе
  // музыка осталась бы в тишине до следующей команды.
  if (_mediaMode) {
    _mediaDuckTarget = 1.0f;
    _mediaVadHoldUntil = millis() + MEDIA_VAD_RESTORE_MS;
  }
}

void AudioPipeline::playPcm(const uint8_t* data, size_t length) {
  if (playbackQueue == nullptr || data == nullptr) {
    return;
  }

  const size_t samples = length / sizeof(int16_t);
  const int16_t* source = reinterpret_cast<const int16_t*>(data);

  size_t offset = 0;
  while (offset < samples) {
    PlaybackChunk chunk;
    const size_t take = min(samples - offset, static_cast<size_t>(AUDIO_FRAME_SAMPLES));
    chunk.count = static_cast<uint16_t>(take);
    memcpy(chunk.samples, source + offset, take * sizeof(int16_t));
    // Не ждём: если очередь полна, звук лучше потерять, чем заблокировать сеть.
    // Но потерю считаем — это единственный признак, что буфера не хватает.
    if (xQueueSend(playbackQueue, &chunk, 0) != pdTRUE) {
      ++_playbackDrops;
    }
    offset += take;
  }
}

void AudioPipeline::flushPlayback() {
  if (playbackQueue != nullptr) {
    xQueueReset(playbackQueue);
  }

  // В очереди DMA ещё до 64 мс звука: для микрофона это такой же хвост, как
  // и доигранный кадр.
  _playbackQuietUntil = millis() + PLAYBACK_TAIL_MS;
  _outputLevel = 0;
}

void AudioPipeline::setVolume(uint8_t value) {
  _volume = value > 100 ? 100 : value;
}
