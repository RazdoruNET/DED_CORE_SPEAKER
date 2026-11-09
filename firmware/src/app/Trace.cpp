#include "app/Trace.h"

#include <esp_heap_caps.h>

#include "config/AppConfig.h"

namespace trace {
namespace {

constexpr uint8_t kSlots = 24;
constexpr uint8_t kLength = 28;

char slots[kSlots][kLength];
volatile uint8_t head = 0;
bool tripped = false;

}  // namespace

void mark(const char* what) {
  if (what == nullptr) {
    return;
  }

  const uint8_t slot = head % kSlots;
  strncpy(slots[slot], what, kLength - 1);
  slots[slot][kLength - 1] = '\0';
  head = static_cast<uint8_t>((head + 1) % kSlots);
}

void checkHeap() {
#if !defined(SPEAKER_HEAP_AUDIT)
  // В рабочей прошивке проверки нет вообще.
  //
  // heap_caps_check_integrity_all() обходит весь кучный список без блокировки,
  // а рядом Wi-Fi, I2S и Bluetooth постоянно что-то выделяют и освобождают.
  // Обход видит половину обновлённого списка и объявляет живую кучу испорченной;
  // в логе это `ЦЕЛОСТНОСТЬ НАРУШЕНА` через секунду после подключения к точке,
  // а на деле — ложное срабатывание, за которым шёл перезапуск платы. Хуже:
  // сам обход иногда падал на середине (tlsf_check → block_size), и паника
  // выглядела как повреждение памяти в драйвере Wi-Fi.
  //
  // Включать только осознанно, для отладки одного конкретного сценария:
  // -DSPEAKER_HEAP_AUDIT=1. И тогда — не чаще раза в 2 секунды.
  return;
#else
  if (tripped) {
    return;
  }

  static uint32_t lastAuditAt = 0;
  const uint32_t now = millis();
  if (now - lastAuditAt < 2000) {
    return;
  }
  lastAuditAt = now;

  if (heap_caps_check_integrity_all(false)) {
    return;
  }

  tripped = true;
  applog::line("[heap] ЦЕЛОСТНОСТЬ НАРУШЕНА, последние действия:");

  for (uint8_t step = 0; step < kSlots; ++step) {
    const uint8_t slot = (head + step) % kSlots;
    if (slots[slot][0] != '\0') {
      applog::printf("  %s\n", slots[slot]);
    }
  }

  applog::printf("[heap] свободно %u внутр\n",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  delay(200);
  ESP.restart();
#endif
}

}  // namespace trace
