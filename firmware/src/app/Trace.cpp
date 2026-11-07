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
  if (tripped) {
    return;
  }

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
}

}  // namespace trace
