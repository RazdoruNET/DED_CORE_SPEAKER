// Проверка компенсатора эха без платы: синтетическая комната, известная
// задержка тракта и известное эхо. Собирается обычным компилятором.
//
//   clang++ -std=c++17 -O2 -I firmware/src \
//       tools/aec-selfcheck.cpp firmware/src/audio/EchoCanceller.cpp -o /tmp/aec-check
//   /tmp/aec-check
//
// Проверяется то же, что важно на плате:
//   1. задержка тракта находится корреляцией (ожидание 370 отсчётов);
//   2. эхо гасится не меньше чем на 20 дБ там, где звучит только музыка;
//   3. голос поверх музыки не вычитается: фильтр замирает на двойном разговоре.
//
// Микрофон здесь проходит ту же отсечку 90 Гц, что и в AudioPipeline: иначе
// сравнение с референсом получается нечестным — в референсе низа уже нет.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "audio/EchoCanceller.h"

namespace {

constexpr int kRate = 16000;
constexpr int kFrameSamples = 320;  // 20 мс
constexpr int kSeconds = 12;
constexpr int kTotal = kRate * kSeconds;

// Тракт: задержка DMA 370 отсчётов и короткий «хвост» корпуса.
constexpr int kDelay = 370;
constexpr int kOffsets[3] = {0, 12, 30};
constexpr float kGains[3] = {0.45f, 0.20f, 0.10f};

// Голос поверх музыки: два окна по секунде.
constexpr int kVoiceFrom[2] = {4 * kRate, 7 * kRate};
constexpr int kVoiceTo[2] = {5 * kRate, 8 * kRate};

// Отсечка микрофона: два звена Баттерворта 4-го порядка на 90 Гц.
struct Biquad {
  float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
  float z1 = 0.0f, z2 = 0.0f;

  inline float process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

void designHighPass(Biquad& stage, float q) {
  constexpr float kCutoff = 90.0f;
  const float w0 = 2.0f * static_cast<float>(M_PI) * kCutoff / kRate;
  const float alpha = sinf(w0) / (2.0f * q);
  const float cosW0 = cosf(w0);
  const float a0 = 1.0f + alpha;

  stage.b0 = ((1.0f + cosW0) * 0.5f) / a0;
  stage.b1 = -(1.0f + cosW0) / a0;
  stage.b2 = stage.b0;
  stage.a1 = (-2.0f * cosW0) / a0;
  stage.a2 = (1.0f - alpha) / a0;
}

std::vector<float>& musicCache() {
  static std::vector<float> cache;
  if (cache.empty()) {
    cache.resize(kTotal + 1);
    uint32_t seed = 0x12345678u;
    float low = 0.0f;
    float bass = 0.0f;
    for (int i = 0; i < kTotal + 1; ++i) {
      seed = seed * 1664525u + 1013904223u;
      const float white = static_cast<float>(static_cast<int32_t>(seed)) / 2147483648.0f;
      low += (white - low) * 0.35f;
      bass += (low - bass) * 0.05f;
      const float envelope =
          1.0f - 0.25f * sinf(2.0f * static_cast<float>(M_PI) * 0.13f * static_cast<float>(i) / kRate);
      cache[i] = 0.45f * envelope * (low + 0.35f * bass);
    }
  }

  return cache;
}

std::vector<float>& voiceCache() {
  static std::vector<float> cache;
  if (cache.empty()) {
    cache.resize(kTotal + 1);
    uint32_t seed = 0x9e3779b9u;
    float low = 0.0f;
    float rumble = 0.0f;
    for (int i = 0; i < kTotal + 1; ++i) {
      seed = seed * 1664525u + 1013904223u;
      const float white = static_cast<float>(static_cast<int32_t>(seed)) / 2147483648.0f;
      low += (white - low) * 0.18f;
      rumble += (low - rumble) * 0.06f;
      cache[i] = 0.9f * (low - rumble);
    }
  }

  return cache;
}

float music(int n) {
  const std::vector<float>& cache = musicCache();
  return n >= 0 && n <= kTotal ? cache[n] : 0.0f;
}

float voice(int n) {
  const std::vector<float>& cache = voiceCache();
  return n >= 0 && n <= kTotal ? cache[n] : 0.0f;
}

bool voiceActive(int n) {
  for (int i = 0; i < 2; ++i) {
    if (n >= kVoiceFrom[i] && n < kVoiceTo[i]) {
      return true;
    }
  }
  return false;
}

float echoAt(int n) {
  float value = 0.0f;
  for (int j = 0; j < 3; ++j) {
    const int index = n - kDelay - kOffsets[j];
    if (index >= 0) {
      value += kGains[j] * music(index);
    }
  }
  return value;
}

float rmsOf(const std::vector<float>& samples, int from, int to) {
  double sum = 0.0;
  for (int i = from; i < to; ++i) {
    sum += static_cast<double>(samples[i]) * samples[i];
  }
  return static_cast<float>(sqrt(sum / (to - from)));
}

}  // namespace

int main() {
  EchoCanceller aec;
  aec.begin();

  std::vector<float> micInput(kTotal, 0.0f);
  std::vector<float> output(kTotal, 0.0f);
  std::vector<float> cleanVoice(kTotal, 0.0f);

  Biquad hp1;
  Biquad hp2;
  designHighPass(hp1, 0.54119610f);
  designHighPass(hp2, 1.30656296f);

  for (int frame = 0; frame * kFrameSamples < kTotal; ++frame) {
    const int start = frame * kFrameSamples;

    // Референс: тот же музыкальный сигнал в формате эфира — 44.1 кГц стерео.
    constexpr int kInputRate = 44100;
    const int inputFrames = kFrameSamples * kInputRate / kRate;  // 882
    std::vector<int16_t> stereo(static_cast<size_t>(inputFrames) * 2);
    for (int i = 0; i < inputFrames; ++i) {
      const double source = static_cast<double>(start) + static_cast<double>(i) * kRate / kInputRate;
      const int index = static_cast<int>(source);
      const float frac = static_cast<float>(source - index);
      const int16_t pcm =
          static_cast<int16_t>((music(index) * (1.0f - frac) + music(index + 1) * frac) * 32767.0f);
      stereo[static_cast<size_t>(i) * 2] = pcm;
      stereo[static_cast<size_t>(i) * 2 + 1] = pcm;
    }
    aec.pushPlayback(stereo.data(), static_cast<size_t>(inputFrames), kInputRate);

    int16_t mic[kFrameSamples];
    for (int i = 0; i < kFrameSamples; ++i) {
      const int n = start + i;
      const float spoken = voiceActive(n) ? voice(n) : 0.0f;
      cleanVoice[n] = spoken;

      const float captured = hp2.process(hp1.process(echoAt(n) + spoken));
      micInput[n] = captured;
      mic[i] = static_cast<int16_t>(captured * 32767.0f);
    }

    aec.process(mic, kFrameSamples);

    for (int i = 0; i < kFrameSamples; ++i) {
      output[start + i] = static_cast<float>(mic[i]) / 32767.0f;
    }
  }

  int failures = 0;

  const int delayError = std::abs(static_cast<int>(aec.delaySamples()) - kDelay);
  std::printf("задержка: найдено %u, ожидалось %d (ошибка %d)\n",
              static_cast<unsigned>(aec.delaySamples()), kDelay, delayError);
  if (delayError > 8) {
    std::printf("ОШИБКА: задержка тракта не найдена\n");
    ++failures;
  }

  const int from = 2 * kRate;
  const int to = 4 * kRate;
  const float echoLevel = rmsOf(micInput, from, to);
  const float residual = rmsOf(output, from, to);
  const float erle = 20.0f * log10f(echoLevel / (residual + 1.0e-9f));
  std::printf("эхо: %.4f, остаток %.4f, гашение %.1f дБ\n", echoLevel, residual, erle);
  if (erle < 20.0f) {
    std::printf("ОШИБКА: эхо гасится слабее 20 дБ\n");
    ++failures;
  }

  const int talkFrom = kVoiceFrom[0];
  const int talkTo = kVoiceTo[0];
  const float voiceLevel = rmsOf(cleanVoice, talkFrom, talkTo);
  const float talkOut = rmsOf(output, talkFrom, talkTo);
  std::printf("двойной разговор: голос %.4f, на выходе %.4f (%.0f%%)\n", voiceLevel, talkOut,
              100.0f * talkOut / (voiceLevel + 1.0e-9f));
  if (talkOut < 0.6f * voiceLevel) {
    std::printf("ОШИБКА: голос поверх музыки подавляется\n");
    ++failures;
  }

  std::printf("%s\n", failures == 0 ? "ПРОВЕРКА ПРОЙДЕНА" : "ПРОВЕРКА НЕ ПРОЙДЕНА");
  return failures == 0 ? 0 : 1;
}
