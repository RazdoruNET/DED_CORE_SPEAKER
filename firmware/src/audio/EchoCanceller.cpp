#include "audio/EchoCanceller.h"

#include <cmath>
#include <cstring>

namespace {

// Начальная задержка тракта, посчитанная по известным очередям, а не угаданная:
// очередь усилителя (8 буферов по 128 кадров = 23 мс) плюс очередь микрофона
// (4 буфера по 8 мс и ожидание полного кадра — до 40 мс) плюс воздух. Итого
// около 60 мс. Ждать, пока это найдёт корреляция, нельзя: на живой плате она
// не находила эхо вовсе (гашение 1.2 дБ) и оставалась на начальном значении.

// Порог корреляции, ниже которого оценка задержки считается случайной.
// На одном кадре 20 мс пик на живой плате не дотягивал до 0.65 вовсе, и
// задержка не обновлялась никогда. На окне 160 мс пик устойчив выше 0.45,
// а от случайных скачков защищает накопление подтверждений.
constexpr float kDelayMinScore = 0.45f;

// Насколько новая задержка должна быть лучше текущей, чтобы её принять.
constexpr float kDelayJumpMargin = 1.15f;

// Сколько измерений подряд новая задержка должна подтвердиться, прежде чем
// окно фильтра переедет. У ритмичной музыки корреляция даёт несколько почти
// равных пиков (на живой плате — 20, 30, 47 и 60 мс), и одиночный «лучший»
// пик уводил фильтр на чужой путь.
constexpr uint32_t kDelayPersistHits = 5;

// Референс должен быть слышен: по тишине задержку не измерить.
constexpr float kDelayMinRefPower = 1.0e-5f;

// Детектор двойного разговора: пока человек говорит, веса не трогаем, иначе
// фильтр «выучит» голос и начнёт вычитать его из микрофона.
constexpr float kDoubleTalkRms = 0.01f;
constexpr float kDoubleTalkRatio = 2.0f;

// Пока веса нулевые, оценка эха тоже нулевая, и детектор двойного разговора
// запретил бы подстройку навсегда.
constexpr float kEmptyFilterPower = 1.0e-8f;

// Пока фильтр не сошёлся, оценка эха занижена (весов ещё нет), и проверка
// «микрофон громче эха» запретила бы подстройку навсегда: на живой плате
// остаток эха так и застревал на 0.5 дБ. Поэтому учиться фильтр перестаёт
// только после настоящего схождения, а не по таймеру.
constexpr float kLearnErle = 40.0f;

// Уровни внутри фильтра нормированы к единице: так и числа в float остаются в
// удобном диапазоне, и шаг NLMS не зависит от разрядности АЦП.
constexpr float kPcmScale = 1.0f / 32768.0f;

constexpr uint32_t kDelaySeed = 960;  // 60 мс

}  // namespace

void EchoCanceller::begin() {
  _ready = true;
  reset();
}

void EchoCanceller::reset() {
  _count.store(0, std::memory_order_release);
  _delay = kDelaySeed;
  _delayCandidate = kDelaySeed;
  _delayHits = 0;
  _micRingCount = 0;
  _framesSinceDelay = 0;
  _framesSinceReset = 0;
  _adaptNext = true;
  _erleDb = 0.0f;
  _inRate = 0;
  _step = 1.0f;
  _phase = 0.0f;
  _prevSample = 0.0f;
  _aa1 = Biquad();
  _aa2 = Biquad();
  _hp1 = Biquad();
  _hp2 = Biquad();

  for (float& value : _taps) {
    value = 0.0f;
  }
  for (int16_t& value : _history) {
    value = 0;
  }
  for (int16_t& value : _stage) {
    value = 0;
  }
}

// Общая часть расчёта: фильтр Баттерворта 4-го порядка из двух звеньев, та же
// форма, что у микрофонного тракта.
void EchoCanceller::designPair(Biquad& first, Biquad& second, float rate, float cutoff,
                               bool highPass) {
  const float qualities[2] = {0.54119610f, 1.30656296f};
  Biquad* stages[2] = {&first, &second};

  for (int s = 0; s < 2; ++s) {
    const float w0 = 2.0f * static_cast<float>(M_PI) * cutoff / rate;
    const float alpha = sinf(w0) / (2.0f * qualities[s]);
    const float cosW0 = cosf(w0);
    const float a0 = 1.0f + alpha;

    if (highPass) {
      stages[s]->b0 = ((1.0f + cosW0) * 0.5f) / a0;
      stages[s]->b1 = -(1.0f + cosW0) / a0;
    } else {
      stages[s]->b0 = ((1.0f - cosW0) * 0.5f) / a0;
      stages[s]->b1 = (1.0f - cosW0) / a0;
    }

    stages[s]->b2 = stages[s]->b0;
    stages[s]->a1 = (-2.0f * cosW0) / a0;
    stages[s]->a2 = (1.0f - alpha) / a0;
  }
}

void EchoCanceller::designAntiAlias(float rate) {
  // Перед прореживанием до 16 кГц всё выше 8 кГц надо снять, иначе оно
  // завернётся в полосу и добавит в референс то, чего микрофон не слышит.
  //
  // Если поток уже 16 кГц, прореживать нечего — фильтр выключается, иначе он
  // срезал бы верхнюю полосу, которую микрофон прекрасно слышит, и эта часть
  // эха осталась бы невычтенной.
  constexpr float kCutoff = 0.46f * static_cast<float>(kRate);  // 7360 Гц

  _aa1 = Biquad();
  _aa2 = Biquad();
  designHighPass();

  if (rate <= static_cast<float>(kRate)) {
    return;
  }

  designPair(_aa1, _aa2, rate, kCutoff, false);
}

void EchoCanceller::designHighPass() {
  // Та же отсечка, что в микрофонном тракте: в микрофоне низа всё равно нет,
  // и фильтру не приходится тратить запас на то, чего не слышно.
  constexpr float kCutoff = 90.0f;

  _hp1 = Biquad();
  _hp2 = Biquad();
  designPair(_hp1, _hp2, static_cast<float>(kRate), kCutoff, true);
}

void EchoCanceller::pushSample(float value) {
  const int16_t pcm = static_cast<int16_t>(
      value > 32767.0f ? 32767.0f : (value < -32768.0f ? -32768.0f : value));
  const uint32_t index = _count.load(std::memory_order_relaxed);
  _history[index % kHistory] = pcm;
  _count.store(index + 1, std::memory_order_release);
}

size_t EchoCanceller::copyReference(int16_t* out, size_t count) const {
  if (out == nullptr || count == 0) {
    return 0;
  }

  const uint32_t total = _count.load(std::memory_order_acquire);
  const size_t available = total < kHistory ? total : kHistory;
  const size_t take = count < available ? count : available;
  if (take == 0) {
    return 0;
  }

  const size_t oldest = total - static_cast<uint32_t>(take);
  const size_t base = oldest % kHistory;
  const size_t tail = kHistory - base;

  if (take <= tail) {
    memcpy(out, _history + base, take * sizeof(int16_t));
  } else {
    memcpy(out, _history + base, tail * sizeof(int16_t));
    memcpy(out + tail, _history, (take - tail) * sizeof(int16_t));
  }

  return take;
}

void EchoCanceller::pushPlayback(const int16_t* interleaved, size_t frames, uint32_t rate) {
  if (!_ready || interleaved == nullptr || frames == 0 || rate == 0) {
    return;
  }

  if (rate != _inRate) {
    _inRate = rate;
    designAntiAlias(static_cast<float>(rate));
    _step = static_cast<float>(_inRate) / static_cast<float>(kRate);
    _phase = 0.0f;
    _prevSample = 0.0f;
  }

  // Усилитель MAX98357A моно: он берёт (L+R)/2. Значит и референс — среднее
  // каналов, а не два отдельных фильтра.
  //
  // Интерполяция идёт по строго равномерной сетке: _phase — сколько входных
  // отсчётов осталось до очередного выхода. Неравномерный шаг даёт «дрожащий»
  // референс, а его невозможно вычесть фильтром с постоянными весами.
  for (size_t i = 0; i < frames; ++i) {
    const float mono = 0.5f * (static_cast<float>(interleaved[i * 2])
                               + static_cast<float>(interleaved[i * 2 + 1]));
    const float filtered = _aa2.process(_aa1.process(mono));

    while (_phase <= 0.0f) {
      const float value = _prevSample + (filtered - _prevSample) * (1.0f + _phase);
      pushSample(_hp2.process(_hp1.process(value)));
      _phase += _step;
    }

    _prevSample = filtered;
    _phase -= 1.0f;
  }
}

void EchoCanceller::fillStage(size_t span) {
  const uint32_t total = _count.load(std::memory_order_acquire);
  const uint32_t oldest = total - static_cast<uint32_t>(span);
  const size_t base = oldest % kHistory;
  const size_t tail = kHistory - base;

  if (span <= tail) {
    memcpy(_stage, _history + base, span * sizeof(int16_t));
    return;
  }

  memcpy(_stage, _history + base, tail * sizeof(int16_t));
  memcpy(_stage + tail, _history, (span - tail) * sizeof(int16_t));
}

// Мощность референса на окне текущего кадра для заданной задержки. Нужна и
// для проверки «в референсе вообще есть звук», и как знаменатель NLMS.
float EchoCanceller::referencePower(size_t count, uint32_t lag) const {
  float power = 0.0f;
  const size_t offset = kMaxDelay + kTaps - lag;

  for (size_t i = 0; i < count; ++i) {
    const float value = static_cast<float>(_stage[offset + i]) * kPcmScale;
    power += value * value;
  }

  return power;
}

// Нормированная корреляция микрофона с референсом для заданной задержки.
float EchoCanceller::delayScore(size_t count, uint32_t lag, float micPower,
                                const int16_t* mic) const {
  const float refPower = referencePower(count, lag);
  if (refPower <= 0.0f) {
    return 0.0f;
  }

  const size_t offset = kMaxDelay + kTaps - lag;
  float dot = 0.0f;
  for (size_t i = 0; i < count; ++i) {
    dot += static_cast<float>(mic[i]) * static_cast<float>(_stage[offset + i])
           * kPcmScale * kPcmScale;
  }

  return dot / sqrtf(refPower * micPower + 1.0e-20f);
}

void EchoCanceller::shiftTaps(int32_t delta) {
  const int32_t taps = static_cast<int32_t>(kTaps);

  if (delta >= taps || -delta >= taps) {
    for (float& value : _taps) {
      value = 0.0f;
    }
    return;
  }

  // Окно сдвинулось на delta позже: тот же участок сигнала теперь на delta
  // ближе к началу, то есть веса переезжают к младшим индексам.
  if (delta > 0) {
    for (int32_t j = 0; j + delta < taps; ++j) {
      _taps[j] = _taps[j + delta];
    }
    for (int32_t j = taps - delta; j < taps; ++j) {
      _taps[j] = 0.0f;
    }
    return;
  }

  const int32_t back = -delta;
  for (int32_t j = taps - 1; j >= back; --j) {
    _taps[j] = _taps[j - back];
  }
  for (int32_t j = 0; j < back; ++j) {
    _taps[j] = 0.0f;
  }
}

void EchoCanceller::estimateDelay(const int16_t* mic, size_t count) {
  float micPower = 0.0f;
  for (size_t i = 0; i < count; ++i) {
    const float value = static_cast<float>(mic[i]) * kPcmScale;
    micPower += value * value;
  }

  if (micPower < kDelayMinRefPower * static_cast<float>(count)) {
    return;
  }

  // Пока эфир приглушён или в референсе тишина, корреляция показывает шум:
  // задержка от таких измерений «прыгала» с 8 до 46 мс.
  if (referencePower(count, _delay) < kDelayMinRefPower * static_cast<float>(count)) {
    return;
  }

  uint32_t best = _delay;
  float bestScore = 0.0f;

  // Шаг 1 по всей шкале. Раньше шли с шагом 4 и уточняли вокруг найденного
  // пика, и это промахивалось: на шумовом сигнале пик корреляции шириной в
  // один отсчёт, поэтому шаг 4 просто не попадал в него, а «лучшим» оказывался
  // случайный всплеск в другом месте. Окно 160 мс даёт 2560 × 1201 операций
  // раз в секунду — для S3 это доли процента загрузки.
  for (uint32_t lag = 0; lag <= kMaxDelay; ++lag) {
    const float score = delayScore(count, lag, micPower, mic);
    if (score > bestScore) {
      bestScore = score;
      best = lag;
    }
  }

  _probeScore = bestScore;
  _probeLag = best;

  if (bestScore < kDelayMinScore) {
    return;
  }


  // Оценка «липкая». Музыка с ритмом даёт несколько почти равных пиков
  // корреляции, и без запаса новая задержка перебивала текущую на каждом
  // измерении: фильтр не успевал ни за одной. Берём самый сильный пик, но
  // двигаем задержку, только если он заметно сильнее текущего места.
  if (best == _delay) {
    _delayHits = 0;
    return;
  }

  const int32_t delta = static_cast<int32_t>(best) - static_cast<int32_t>(_delay);
  const uint32_t distance =
      delta > 0 ? static_cast<uint32_t>(delta) : static_cast<uint32_t>(-delta);

  // Мелкая поправка внутри запаса окна: переносить веса незачем.
  if (distance <= kDelayJumpSamples) {
    _delayHits = 0;
    _delay = best;
    return;
  }

  // Фильтр ещё пуст: терять нечего, и ждать подтверждений незачем — иначе
  // первые секунды после старта эфира уходят на заведомо неверное окно.
  float tapPower = 0.0f;
  for (const float value : _taps) {
    tapPower += value * value;
  }
  if (tapPower < kEmptyFilterPower) {
    _delay = best;
    _delayHits = 0;
    return;
  }

  // Кандидат считается тем же, если он в пределах 4 мс: оценка по короткому
  // окну гуляет на ±3 мс, и с допуском 8 отсчётов подтверждения не набирались
  // никогда — задержка оставалась начальной.
  constexpr uint32_t kCandidateTolerance = 64;
  const bool sameCandidate =
      _delayHits > 0
      && (best > _delayCandidate ? best - _delayCandidate : _delayCandidate - best)
             <= kCandidateTolerance;

  if (!sameCandidate) {
    _delayCandidate = best;
    _delayHits = 1;
    return;
  }

  if (++_delayHits < kDelayPersistHits) {
    return;
  }

  const float current = delayScore(count, _delay, micPower, mic);
  if (bestScore < current * kDelayJumpMargin) {
    _delayHits = 0;
    return;
  }

  // Путь действительно другой. Если фильтр уже сошёлся, веса переносим в новое
  // окно: обнуление выбрасывало всё выученное, и на музыке фильтр начинал с
  // нуля каждые несколько секунд. Если он ещё не сошёлся, переносить нечего —
  // там мусор от неверного выравнивания, и его надо выбросить.
  if (_erleDb >= kLearnErle) {
    shiftTaps(delta);
  } else {
    for (float& value : _taps) {
      value = 0.0f;
    }
  }

  _delay = best;
  _delayHits = 0;
}

void EchoCanceller::process(int16_t* samples, size_t count) {
  if (!_ready || samples == nullptr || count == 0 || count > kMaxFrame) {
    return;
  }

  const uint32_t total = _count.load(std::memory_order_acquire);

  for (size_t i = 0; i < count; ++i) {
    _micRing[_micRingCount % kDelaySpan] = samples[i];
    ++_micRingCount;
  }

  const size_t micSpan = _micRingCount < kDelaySpan ? _micRingCount : kDelaySpan;
  const size_t span = micSpan + kMaxDelay + kTaps;
  if (total < span + 8) {
    return;
  }

  fillStage(span);

  if (_framesSinceDelay == 0) {
    // Задержка ищется по всему накопленному окну, а не по одному кадру: на
    // кадре 20 мс у ритмичного сигнала несколько почти равных пиков.
    static int16_t micWindow[kDelaySpan];
    const size_t start = _micRingCount - static_cast<uint32_t>(micSpan);

    for (size_t i = 0; i < micSpan; ++i) {
      micWindow[i] = _micRing[(start + i) % kDelaySpan];
    }

    estimateDelay(micWindow, micSpan);
    _framesSinceDelay = kDelayEveryFrames;
  } else {
    --_framesSinceDelay;
  }

  if (_framesSinceReset < 0xFFFFFFFFu) {
    ++_framesSinceReset;
  }

  // Учимся, пока не сошлись: большой шаг для быстрого схождения, малый — когда
  // гашение уже есть и надо не разболтать веса. Шаг меняется плавно, а не
  // ступенькой: на пороге 6 дБ фильтр «замирал» недосведённым и остаток
  // застревал на 15 дБ вместо 25.
  const bool learning = _erleDb < kLearnErle;
  const bool adapt = learning || _adaptNext;
  float learned = _erleDb / kLearnErle;
  if (learned > 1.0f) {
    learned = 1.0f;
  } else if (learned < 0.0f) {
    learned = 0.0f;
  }
  const float mu = kMuWarmup + (kMuSteady - kMuWarmup) * learned;

  // Окно фильтра начинается на kDelayMargin раньше найденной задержки.
  // В _stage кадр микрофона начинается с индекса kMaxDelay + kTaps: перед ним
  // лежит ровно столько отсчётов референса, сколько нужно фильтру.
  const uint32_t windowStart = _delay > kDelayMargin ? _delay - kDelayMargin : 0;
  // Кадр занимает последние count отсчётов окна; ищем его начало.
  const size_t base = span - count - windowStart;

  float micPower = 0.0f;
  float echoPower = 0.0f;
  float residualPower = 0.0f;

  for (size_t i = 0; i < count; ++i) {
    const float mic = static_cast<float>(samples[i]) * kPcmScale;
    const size_t end = base + i;

    float estimate = 0.0f;
    float refPower = 0.0f;
    for (size_t j = 0; j < kTaps; ++j) {
      const float ref = static_cast<float>(_stage[end - j]) * kPcmScale;
      estimate += _taps[j] * ref;
      refPower += ref * ref;
    }

    const float error = mic - estimate;

    if (adapt && refPower > 1.0e-6f) {
      float step = mu * error / (refPower + 1.0e-6f);
      if (step > 0.5f) {
        step = 0.5f;
      } else if (step < -0.5f) {
        step = -0.5f;
      }

      for (size_t j = 0; j < kTaps; ++j) {
        _taps[j] += step * static_cast<float>(_stage[end - j]) * kPcmScale;
      }
    }

    micPower += mic * mic;
    echoPower += estimate * estimate;
    residualPower += error * error;

    const float output = error / kPcmScale;
    samples[i] = static_cast<int16_t>(
        output > 32767.0f ? 32767.0f : (output < -32768.0f ? -32768.0f : output));
  }

  const float invCount = 1.0f / static_cast<float>(count);
  const float micRms = sqrtf(micPower * invCount);
  const float echoRms = sqrtf(echoPower * invCount);

  _inputRms = static_cast<uint32_t>(micRms / kPcmScale);
  _outputRms = static_cast<uint32_t>(sqrtf(residualPower * invCount) / kPcmScale);

  float filterPower = 0.0f;
  for (size_t j = 0; j < kTaps; ++j) {
    filterPower += _taps[j] * _taps[j];
  }

  // Пока человек говорит громче эха, веса не двигаем: иначе фильтр начнёт
  // вычитать голос. Эхо при этом всё равно вычитается текущими весами.
  _adaptNext = filterPower < kEmptyFilterPower
               || micRms < kDoubleTalkRatio * echoRms + kDoubleTalkRms;

  if (adapt && micRms > 0.003f && echoRms > 1.5e-4f) {
    // Сравниваем средние мощности кадра, а не суммы, и без прибавленной
    // единицы: сигнал нормирован к единице, суммы порядка 0.3, и «+1» в
    // числителе и знаменателе превращала любое гашение в ноль.
    const float micMean = micPower * invCount;
    const float residualMean = residualPower * invCount;
    if (micMean > 1.0e-9f) {
      const float instant =
          10.0f * log10f(micMean / (residualMean > 1.0e-9f ? residualMean : 1.0e-9f));
      _erleDb += (instant - _erleDb) * 0.1f;
    }
  }
}
