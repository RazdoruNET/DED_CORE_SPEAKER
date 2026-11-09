#include "app/LocalCommands.h"

#include <string.h>

#include "app/Role.h"

namespace {

#if !defined(SPEAKER_ROLE_INTERCOM)
// Домашний набор: 3 имени-активатора × 2 действия. Фразы записаны так, как их
// будет отдавать распознаватель; сравнение с ними — задача следующего этапа.
constexpr LocalCommand kHomeCommands[] = {
    {LocalAction::LightOn, "Лира, включи свет"},
    {LocalAction::LightOff, "Лира, выключи свет"},
    {LocalAction::LightOn, "Джарвис, включи свет"},
    {LocalAction::LightOff, "Джарвис, выключи свет"},
    {LocalAction::LightOn, "Колонка, включи свет"},
    {LocalAction::LightOff, "Колонка, выключи свет"},
};

constexpr const char* kWakeWords[] = {"лира", "джарвис", "колонка"};

// Один символ UTF-8, приведённый к нижнему регистру.
struct FoldedChar {
  char bytes[4];
  size_t length;
};

size_t utf8Length(uint8_t first) {
  if (first < 0x80) {
    return 1;
  }
  if ((first & 0xE0) == 0xC0) {
    return 2;
  }
  if ((first & 0xF0) == 0xE0) {
    return 3;
  }
  if ((first & 0xF8) == 0xF0) {
    return 4;
  }
  return 1;
}

FoldedChar foldChar(const uint8_t* text) {
  FoldedChar result{};
  result.length = utf8Length(text[0]);

  // Обрезанная последовательность не должна утащить сравнение за конец строки.
  for (size_t i = 1; i < result.length; ++i) {
    if (text[i] == 0) {
      result.length = i;
      break;
    }
  }

  for (size_t i = 0; i < result.length; ++i) {
    result.bytes[i] = static_cast<char>(text[i]);
  }

  // ASCII: A-Z.
  if (result.length == 1) {
    if (result.bytes[0] >= 'A' && result.bytes[0] <= 'Z') {
      result.bytes[0] = static_cast<char>(result.bytes[0] + ('a' - 'A'));
    }
    return result;
  }

  // Кириллица в UTF-8 — два байта. А-П: D0 90..9F → D0 B0..BF.
  if (result.length == 2 && static_cast<uint8_t>(result.bytes[0]) == 0xD0) {
    const uint8_t second = static_cast<uint8_t>(result.bytes[1]);

    if (second >= 0x90 && second <= 0x9F) {
      result.bytes[1] = static_cast<char>(second + 0x20);
    } else if (second >= 0xA0 && second <= 0xAF) {
      // Р-Я: D0 A0..AF → D1 80..8F.
      result.bytes[0] = static_cast<char>(0xD1);
      result.bytes[1] = static_cast<char>(second - 0x20);
    } else if (second == 0x81) {
      // Ё: D0 81 → D1 91.
      result.bytes[0] = static_cast<char>(0xD1);
      result.bytes[1] = static_cast<char>(0x91);
    }
  }

  return result;
}

bool equalsIgnoringCase(const char* left, const char* right) {
  const auto* a = reinterpret_cast<const uint8_t*>(left);
  const auto* b = reinterpret_cast<const uint8_t*>(right);

  while (*a != 0 && *b != 0) {
    const FoldedChar fa = foldChar(a);
    const FoldedChar fb = foldChar(b);

    if (fa.length == 0 || fb.length == 0 || fa.length != fb.length
        || memcmp(fa.bytes, fb.bytes, fa.length) != 0) {
      return false;
    }

    a += fa.length;
    b += fb.length;
  }

  return *a == 0 && *b == 0;
}
#endif

}  // namespace

const LocalCommand* localCommands(size_t& count) {
#if defined(SPEAKER_ROLE_INTERCOM)
  // Таблица команд гарнитуры (режимы, карты, зажигание) задаётся в её сборке.
  // В этой базе она пуста — чтобы домашние команды света не попали в гоночную
  // гарнитуру. Поэтому возвращаем nullptr и ноль.
  static_assert(kRole == DeviceRole::Intercom, "набор интеркома только для роли Intercom");
  count = 0;
  return nullptr;
#else
  static_assert(kRole == DeviceRole::Home, "домашний набор только для роли Home");
  count = sizeof(kHomeCommands) / sizeof(kHomeCommands[0]);
  return kHomeCommands;
#endif
}

bool isBuiltInWakeWord(const char* normalized) {
#if defined(SPEAKER_ROLE_INTERCOM)
  // У интеркома свои слова-активаторы: домашние имена в его сборку не попадают.
  (void)normalized;
  return false;
#else
  if (normalized == nullptr) {
    return false;
  }

  for (const char* word : kWakeWords) {
    if (equalsIgnoringCase(normalized, word)) {
      return true;
    }
  }
  return false;
#endif
}
