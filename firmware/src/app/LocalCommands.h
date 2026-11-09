#pragma once

#include <stddef.h>
#include <stdint.h>

// Что делает встроенная фраза. Пока это только свет — он ещё заглушка.
enum class LocalAction : uint8_t { None, LightOn, LightOff };

struct LocalCommand {
  LocalAction action;
  const char* phrase;
};

// Таблица встроенных фраз текущей роли. Возвращает указатель на массив и число
// записей; для роли без встроенных команд — nullptr и 0.
const LocalCommand* localCommands(size_t& count);

// Слово-активатор из встроенного набора («Лира», «Джарвис», «Колонка»).
// Строка уже нормализована распознавателем: сравнение идёт без учёта регистра.
bool isBuiltInWakeWord(const char* normalized);
