#pragma once

#include <Arduino.h>

#include "config/AppConfig.h"

// Физическая раскладка ленты по низу колонки. Лента — одна цепочка из 17
// диодов буквой U, и роли у неё как у лица: по краям уши (они же щёки),
// в середине рот, между ними погашенные разделители. Матрица глаз появится
// отдельной деталью, поэтому здесь только низ лица.
//
// Нумерация на корпусе и порядок цепочки:
//
//   1 2 3 | 4 5 | 6 7 8 9 10 11 12 | 13 14 | 15 16 17
//    уши    выкл.        рот            выкл.    уши
//
// Внутри прошивки индексы с нуля: уши — 0..2 и 14..16, разделители — 3..4 и
// 12..13, рот — 5..11. Если лента смонтирована в обратную сторону, включите
// kReversed: логические позиции отразятся, а физический порядок останется.
namespace leds {

constexpr uint16_t kCount = LED_STRIP_LEDS;

// Симметричные анимации считаются от общего центра рта.
static_assert(kCount % 2 == 1, "Нужен общий центр: иначе рот несимметричен");

constexpr bool kReversed = false;

// ---------------------------------------------------------------------------
// Уши (щёки): индикация событий и отклик на звук.
// ---------------------------------------------------------------------------
constexpr uint16_t kEarLedCount = LED_EAR_LEDS;                  // 3
constexpr uint16_t kRightEarFirst = kCount - kEarLedCount;       // 14

// ---------------------------------------------------------------------------
// Разделители: стык уха и рта. Погашены всегда, чтобы рот читался отдельно.
// ---------------------------------------------------------------------------
constexpr uint16_t kGapLedCount = LED_GAP_LEDS;                  // 2
constexpr uint16_t kLeftGapFirst = kEarLedCount;                 // 3
constexpr uint16_t kRightGapFirst = kCount - kEarLedCount - kGapLedCount;  // 12

// ---------------------------------------------------------------------------
// Рот: семь диодов — центр и по три кольца в каждую сторону.
// ---------------------------------------------------------------------------
constexpr uint16_t kMouthFirst = kLeftGapFirst + kGapLedCount;   // 5
constexpr uint16_t kMouthCount = LED_MOUTH_LEDS;                 // 7
constexpr uint16_t kMouthCenter = kMouthFirst + kMouthCount / 2; // 8
constexpr uint16_t kMouthLast = kMouthFirst + kMouthCount - 1;   // 11
constexpr uint16_t kMouthHalf = kMouthCount / 2;                 // 3

enum class Part : uint8_t {
  LeftEar,
  Gap,
  Mouth,
  RightEar,
};

// Логическая позиция диода в цепочке: 0 — левый кончик, kCount-1 — правый.
inline uint16_t positionOf(uint16_t chainIndex) {
  return kReversed ? static_cast<uint16_t>(kCount - 1 - chainIndex) : chainIndex;
}

inline Part partOf(uint16_t index) {
  const uint16_t p = positionOf(index);
  if (p < kLeftGapFirst) {
    return Part::LeftEar;
  }
  if (p < kMouthFirst) {
    return Part::Gap;
  }
  if (p <= kMouthLast) {
    return Part::Mouth;
  }
  if (p < kRightEarFirst) {
    return Part::Gap;
  }
  return Part::RightEar;
}

inline bool isEar(uint16_t index) {
  const Part part = partOf(index);
  return part == Part::LeftEar || part == Part::RightEar;
}

inline bool isMouth(uint16_t index) {
  return partOf(index) == Part::Mouth;
}

// Разделитель: горит только в сценах, где лента работает как одна линия.
inline bool isGap(uint16_t index) {
  return partOf(index) == Part::Gap;
}

// Позиция внутри рта: 0 — левый край, kMouthHalf — центр, kMouthCount-1 — правый.
inline uint16_t mouthSlot(uint16_t index) {
  const uint16_t p = positionOf(index);
  if (p < kMouthFirst || p > kMouthLast) {
    return kMouthHalf;
  }
  return static_cast<uint16_t>(p - kMouthFirst);
}

// Расстояние от середины рта: 0 — центр, kMouthHalf — край.
inline uint16_t mouthDistance(uint16_t index) {
  const uint16_t slot = mouthSlot(index);
  return slot > kMouthHalf ? slot - kMouthHalf : kMouthHalf - slot;
}

// Позиция на ухе: 0 — внешний край (у «виска»), kEarLedCount-1 — у рта.
inline uint16_t earSlot(uint16_t index) {
  const uint16_t p = positionOf(index);
  return p < kEarLedCount ? p : static_cast<uint16_t>(kCount - 1 - p);
}

}  // namespace leds
