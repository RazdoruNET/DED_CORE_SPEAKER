#include "app/Beeper.h"

#include "config/AppConfig.h"

namespace beeper {
namespace {

constexpr uint8_t kChannel = 0;
constexpr uint8_t kResolutionBits = 10;
constexpr uint16_t kIdleFrequency = 0;

// Мелодии короткие намеренно: сигнал не должен перебивать разговор.
const Note kBootNotes[] = {{1568, 90}, {0, 50}, {2093, 130}};
const Note kPairedNotes[] = {{1319, 90}, {1760, 90}, {2093, 160}};
const Note kErrorNotes[] = {{330, 180}, {0, 60}, {262, 260}};
const Note kListeningNotes[] = {{2093, 70}};
const Note kConnectedNotes[] = {{1760, 70}, {2093, 70}};
const Note kUnpairedNotes[] = {{880, 100}, {0, 120}, {880, 100}};

const Note* sequence = nullptr;
size_t sequenceLength = 0;
size_t sequenceIndex = 0;
uint32_t noteStartedAt = 0;
bool active = false;

void silence() {
#if defined(SPEAKER_HAS_BUZZER)
  ledcWriteTone(kChannel, kIdleFrequency);
#endif
}

}  // namespace

bool isAvailable() {
#if defined(SPEAKER_HAS_BUZZER)
  return true;
#else
  return false;
#endif
}

void begin() {
#if defined(SPEAKER_HAS_BUZZER)
  ledcSetup(kChannel, 2000, kResolutionBits);
  ledcAttachPin(pins::BUZZER, kChannel);
  silence();
#endif

  active = false;
  sequence = nullptr;
  sequenceLength = 0;
  sequenceIndex = 0;
}

void play(const Note* notes, size_t count) {
#if defined(SPEAKER_BEEPER_SILENT)
  (void)notes;
  (void)count;
  return;
#endif

  if (notes == nullptr || count == 0) {
    return;
  }

  sequence = notes;
  sequenceLength = count;
  sequenceIndex = 0;
  noteStartedAt = millis();
  active = true;

#if defined(SPEAKER_HAS_BUZZER)
  ledcWriteTone(kChannel, sequence[0].frequency);
#endif
}

void tick() {
  if (!active || sequence == nullptr) {
    return;
  }

  const uint32_t elapsed = millis() - noteStartedAt;

  if (elapsed < sequence[sequenceIndex].durationMs) {
    return;
  }

  ++sequenceIndex;

  if (sequenceIndex >= sequenceLength) {
    silence();
    active = false;
    sequence = nullptr;
    return;
  }

#if defined(SPEAKER_HAS_BUZZER)
  ledcWriteTone(kChannel, sequence[sequenceIndex].frequency);
#endif

  noteStartedAt = millis();
}

void boot() { play(kBootNotes, sizeof(kBootNotes) / sizeof(kBootNotes[0])); }

void paired() { play(kPairedNotes, sizeof(kPairedNotes) / sizeof(kPairedNotes[0])); }

void error() { play(kErrorNotes, sizeof(kErrorNotes) / sizeof(kErrorNotes[0])); }

void listening() {
  play(kListeningNotes, sizeof(kListeningNotes) / sizeof(kListeningNotes[0]));
}

void connected() {
  play(kConnectedNotes, sizeof(kConnectedNotes) / sizeof(kConnectedNotes[0]));
}

void unpaired() {
  play(kUnpairedNotes, sizeof(kUnpairedNotes) / sizeof(kUnpairedNotes[0]));
}

}  // namespace beeper
