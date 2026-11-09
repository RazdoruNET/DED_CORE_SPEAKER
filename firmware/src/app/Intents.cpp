#include "app/Intents.h"

#include "app/Beeper.h"
#include "app/Ui.h"
#include "config/AppConfig.h"

#include "audio/AudioPipeline.h"
#include "net/DeviceConfig.h"
#include "net/SpeakerLink.h"

Intents intents;

namespace {
constexpr uint16_t kToneSamples = 1600;  // 100 мс при 16 кГц
int16_t toneBuffer[kToneSamples];

// Короткие подтверждения света: тише и короче мелодий привязки, чтобы не
// перебивать разговор.
constexpr beeper::Note kLightOnNotes[] = {{1760, 70}, {2093, 90}};
constexpr beeper::Note kLightOffNotes[] = {{1568, 70}, {1046, 90}};
}  // namespace

void Intents::begin() {
  _timerActive = false;
  _alarmActive = false;
}

void Intents::tick() {
  const uint32_t now = millis();

  if (_timerActive && static_cast<int32_t>(now - _timerDue) >= 0) {
    _timerActive = false;
    // Будильник переживает потерю связи: это обещание колонки, а не сервера.
    chime(880, 400);
    speakerLink.sendEvent("timer");
  }

  if (_alarmActive && static_cast<int32_t>(now - _alarmDue) >= 0) {
    _alarmActive = false;
    chime(1046, 800);
    speakerLink.sendEvent("alarm");
  }
}

bool Intents::startTimer(uint32_t seconds) {
  _timerActive = true;
  _timerDue = millis() + seconds * 1000UL;
  return true;
}

void Intents::cancelTimers() {
  _timerActive = false;
  _alarmActive = false;
}

void Intents::volumeUp() {
  deviceConfig.volume = deviceConfig.volume >= 95 ? 100 : deviceConfig.volume + 5;
  audioPipeline.setVolume(deviceConfig.volume);
  deviceConfig.saveVolume();
}

void Intents::volumeDown() {
  deviceConfig.volume = deviceConfig.volume <= 5 ? 0 : deviceConfig.volume - 5;
  audioPipeline.setVolume(deviceConfig.volume);
  deviceConfig.saveVolume();
}

void Intents::toggleMute() {
  deviceConfig.muted = !deviceConfig.muted;
  ui.setMuted(deviceConfig.muted);
  deviceConfig.saveVolume();
  speakerLink.sendEvent(deviceConfig.muted ? "muted" : "unmuted");
}

void Intents::lightOn() {
  applog::line("[local] свет включён");
  // Пищалка подтверждает команду; при SPEAKER_BEEPER_SILENT beeper::play молчит.
  beeper::play(kLightOnNotes, sizeof(kLightOnNotes) / sizeof(kLightOnNotes[0]));
  if (speakerLink.isConnected()) {
    speakerLink.sendEvent("local.light.on");
  }
}

void Intents::lightOff() {
  applog::line("[local] свет выключен");
  beeper::play(kLightOffNotes, sizeof(kLightOffNotes) / sizeof(kLightOffNotes[0]));
  if (speakerLink.isConnected()) {
    speakerLink.sendEvent("local.light.off");
  }
}

void Intents::chime(uint16_t frequency, uint16_t durationMs) {
  const uint16_t samples = min<uint16_t>(kToneSamples, durationMs * AUDIO_RATE / 1000);
  for (uint16_t i = 0; i < samples; ++i) {
    const float envelope = 1.0f - (static_cast<float>(i) / samples);
    toneBuffer[i] = static_cast<int16_t>(sin(2 * PI * frequency * i / AUDIO_RATE) * 9000 * envelope);
  }
  audioPipeline.playPcm(reinterpret_cast<const uint8_t*>(toneBuffer), samples * sizeof(int16_t));
}
