#include <Arduino.h>

#include "app/Beeper.h"
#include "app/Intents.h"
#include "app/Trace.h"
#include "app/Ui.h"
#include "audio/AudioPipeline.h"
#include "config/AppConfig.h"
#include "net/BlePairing.h"
#include "net/DeviceConfig.h"
#include "net/DeviceIdentity.h"
#include "net/PairingCrypto.h"
#include "net/SpeakerLink.h"
#include "net/WifiLink.h"

namespace {

PairingCrypto pairingCrypto;
String deviceId;
uint32_t lastStateAt = 0;
uint32_t lastHeapLogAt = 0;
uint32_t lastCapturedFrames = 0;

// Строгая сборка запустилась на плате без flash-шифрования. Тогда ничего не
// делаем: иначе ключ устройства и пароль Wi-Fi легли бы в NVS открытым текстом.
bool securityBlocked = false;

constexpr uint32_t kStateIntervalMs = 10000;
constexpr uint32_t kHeapLogIntervalMs = 5000;

void openPairingWindow() {
  if (deviceConfig.isPaired()) {
    applog::line("[ble] повторная привязка запрещена: сначала отвяжите колонку в аккаунте");
    ui.setState(AgentState::Offline);
    return;
  }

  ui.setState(AgentState::Pairing);
  if (blePairing.begin(&pairingCrypto, deviceId, [](const String&, const String&) {
        // Настройки применены целиком — перезапуск, чтобы не оставить
        // полуприменённое состояние.
        delay(500);
        ESP.restart();
      })) {
    applog::printf("[boot] окно привязки открыто для %s\n", deviceId.c_str());
  } else {
    ui.setState(AgentState::Error);
  }
}

void beginSession() {
  if (!deviceConfig.hasNetwork()) {
    applog::line("[boot] нет сети — жду настройки по Bluetooth");
    openPairingWindow();
    return;
  }

  // Только сеть: шифрованное соединение поднимаем позже, когда есть и сеть,
  // и проверенное время (см. loop).
  wifiLink.begin();
  ui.setState(AgentState::Idle);
}

// Физический переключатель микрофона. На профиле DA это GPIO36: вывод только
// на вход и без внутренней подтяжки, поэтому на плате обязателен внешний
// резистор вверх. Три одинаковых отсчёта с шагом 20 мс отсекают дребезг и
// наводки — иначе плавающий вход мог бы «выключить» микрофон сам.
bool muteSwitchClosed() {
  static bool stable = false;
  static bool candidate = false;
  static uint8_t sameCount = 0;
  static uint32_t lastSampleAt = 0;

  const uint32_t now = millis();
  if (now - lastSampleAt < 20) {
    return stable;
  }
  lastSampleAt = now;

  const bool sampled = digitalRead(pins::SW_MUTE) == LOW;

  if (sampled == candidate) {
    if (sameCount < 3) {
      ++sameCount;
    }
  } else {
    candidate = sampled;
    sameCount = 1;
  }

  if (sameCount >= 3) {
    stable = candidate;
  }

  return stable;
}

// Микрофон выключен — это обещание человеку, а не только красная лента.
// Программный mute и физический переключатель равноправны: пока любой из них
// активен, захват остановлен и кадры на сервер не уходят.
bool syncMute() {
  static bool applied = false;
  static bool state = false;

  const bool muted = deviceConfig.muted || muteSwitchClosed();

  if (!applied || muted != state) {
    applied = true;

    if (muted != state) {
      applog::printf("[mute] микрофон %s\n", muted ? "выключен" : "включён");
      speakerLink.sendEvent(muted ? "muted" : "unmuted");
    }

    state = muted;
    ui.setMuted(muted);
    speakerLink.setMuteState(muted);
  }

  if (muted && audioPipeline.isCapturing()) {
    audioPipeline.stopCapture();
  }

  return muted;
}

void wireAudio() {
  audioPipeline.onFrame([](const uint8_t* pcm, size_t length) {
    speakerLink.sendMic(pcm, length);
  });

  audioPipeline.onVad([](bool start, uint32_t rms) {
#if defined(SPEAKER_MIC_DIAG)
    applog::printf("[vad] %s, rms %u\n", start ? "начало речи" : "конец речи", rms);
#endif
    speakerLink.sendVad(start, rms);
    if (start) {
      // Короткий сигнал «услышала»: без усилителя это единственный отклик.
      beeper::listening();
      ui.setState(AgentState::Listening);
    } else {
      ui.setState(AgentState::Thinking);
    }
  });

  audioPipeline.onSneeze([]() {
    applog::line("[sneeze] похоже на чих");
    speakerLink.sendEvent("sneeze");
  });

  audioPipeline.onLevel([](uint8_t level) {
    if (ui.state() == AgentState::Listening) {
      ui.setLevel(level);
    }
  });

  speakerLink.onAudio([](const uint8_t* data, size_t length) {
    audioPipeline.playPcm(data, length);
  });

  speakerLink.onSpeechBegin([](uint16_t, const String&) {
    ui.setState(AgentState::Speaking);
  });

  speakerLink.onSpeechEnd([]() {
    ui.setState(speakerLink.isConnected() ? AgentState::Idle : AgentState::Offline);
  });

  speakerLink.onCommand([](const String& action, JsonVariantConst args) {
    if (action == "volume") {
      const int value = args["value"] | deviceConfig.volume;
      deviceConfig.volume = static_cast<uint8_t>(constrain(value, 0, 100));
      audioPipeline.setVolume(deviceConfig.volume);
      deviceConfig.saveVolume();
    } else if (action == "hush") {
      const int ms = args["ms"] | 90000;
      audioPipeline.hush(static_cast<uint32_t>(constrain(ms, 1000, 1800000)));
      // Пока играет музыка в браузере, колонка не слушает — и подсветка не
      // должна показывать «записываю».
      ui.setState(AgentState::Idle);
    } else if (action == "listen") {
      audioPipeline.resume();
      ui.setState(AgentState::Idle);
    } else if (action == "incoming") {
      ui.setState(AgentState::Incoming);
    } else if (action == "stop") {
      audioPipeline.flushPlayback();
      ui.setState(AgentState::Idle);
    } else if (action == "intercom.start") {
      ui.setState(AgentState::Intercom);
    } else if (action == "intercom.end") {
      ui.setState(AgentState::Idle);
    } else if (action == "reboot") {
      ESP.restart();
    }
  });
}

}  // namespace

void setup() {
  applog::begin();
  delay(300);
  applog::printf("\n=== DED CORE SPEAKER %s (%s) ===\n", SPEAKER_FIRMWARE_VERSION,
                SPEAKER_PROFILE_NAME);

  securityBlocked = !speakerSecureFlashEnabled();

  deviceConfig.begin();
  deviceId = speakerDeviceId();
  applog::printf("[boot] устройство %s, свободно %u байт\n", deviceId.c_str(),
                ESP.getFreeHeap());

  beeper::begin();
  beeper::boot();

  audioPipeline.begin();
  audioPipeline.setVolume(deviceConfig.volume);
  ui.begin();
  // Физический переключатель читается с подавлением дребезга в syncMute():
  // разовый digitalRead на старте мог поймать наводку и «выключить» микрофон.
  ui.setMuted(deviceConfig.muted);
  ui.setState(AgentState::Boot);
  intents.begin();
  wireAudio();

  // Крипто-путь привязки проверяется на каждом старте: молчаливая поломка
  // ECDH/HKDF/GCM означала бы «сайт не может настроить колонку» без внятной причины.
  applog::printf("[crypto] самопроверка привязки: %s\n",
                pairingCrypto.selfTest() ? "ок" : "ОШИБКА");

  // Открытый ключ уходит в HELLO: по нему сервер при первом подключении
  // закрепляет отпечаток и не даёт чужому устройству забрать токен привязки.
  speakerLink.setPublicKeyHex(pairingCrypto.publicKeyHex());

#if defined(SPEAKER_MIC_DIAG)
  // Диагностика: включаем захват сразу, иначе детектор речи молчит без кнопки.
  audioPipeline.startCapture();
#endif

  // Действующее состояние микрофона нужно до первого подключения: иначе HELLO
  // сообщит «не выключен», хотя переключатель уже стоит в mute. Вызов идёт
  // после диагностических сборок: те включают захват принудительно, а mute
  // должен победить и здесь.
  syncMute();

#if defined(SPEAKER_TONE_TEST)
  // Проверка тракта: эталонный тон с пищалки должен вернуться с микрофона
  // той же частотой. Так проверяются и разрядность, и частота дискретизации.
  static const beeper::Note kTone[] = {{1000, 3000}};
  beeper::play(kTone, 1);
#endif

  const bool paired = deviceConfig.isPaired() && deviceConfig.hasNetwork();

  // Строгая сборка (серия) на плате без flash-шифрования: дальше не идём.
  // Ни привязки, ни сети — иначе секреты окажутся в NVS открытым текстом.
  if (securityBlocked) {
    applog::line("[security] flash encryption выключена — сборка требует её, устройство не запускается");
    ui.setState(AgentState::Error);
    return;
  }

  if (!paired) {
    ui.setState(AgentState::Unpaired);

#if defined(SPEAKER_BENCH_PAIRING)
    // Только для стенда: на макете кнопка недоступна, а окно привязки нужно
    // проверить сквозным тестом. Флаг задаётся вручную при сборке и не входит
    // ни в один релизный профиль.
    applog::line("[bench] окно привязки открыто принудительно");
    openPairingWindow();
#else
    applog::line("[boot] не привязана — удерживайте кнопку 5 секунд для BLE-настройки");
#endif
  } else {
    beginSession();
  }
}

void loop() {
  beeper::tick();

  // Строгая сборка без flash-шифрования: только лента с ошибкой, никакой сети,
  // привязки и записи секретов.
  if (securityBlocked) {
    ui.tick();
    delay(50);
    return;
  }

#if defined(SPEAKER_MIC_DIAG)
  // Проверка обоих микрофонов по отдельности: говорите или хлопайте и следите,
  // чтобы уровни росли у обоих каналов.
  static uint32_t lastMicDiagAt = 0;
  if (millis() - lastMicDiagAt >= 1000) {
    lastMicDiagAt = millis();
    uint32_t peakLeft = 0;
    uint32_t peakRight = 0;
    audioPipeline.takePeaks(peakLeft, peakRight);
    applog::printf("[mic] rms л=%u п=%u | пик л=%u п=%u\n",
                   audioPipeline.rmsLeft(), audioPipeline.rmsRight(), peakLeft, peakRight);
  }
#endif

  trace::checkHeap();
  ui.tick();
  intents.tick();
  wifiLink.loop();
  speakerLink.loop();
  blePairing.tick();
  deviceConfig.tickVolume();

  // Последняя линия обороны: если связи нет совсем долго, колонка
  // перезагружается сама. Лучше короткий перезапуск, чем «зависла и молчит»:
  // однажды повреждённая куча оставляет сеть в состоянии, из которого нет выхода.
  {
    static uint32_t linkLostSince = 0;
    if (speakerLink.isConnected()) {
      linkLostSince = 0;
    } else if (linkLostSince == 0) {
      linkLostSince = millis();
    } else if (millis() - linkLostSince > WATCHDOG_REBOOT_MS) {
      applog::line("[ws] связи нет слишком долго — перезагрузка");
      delay(100);
      ESP.restart();
    }
  }

  if (!deviceConfig.isPaired() && !blePairing.isActive()) {
    ui.setState(AgentState::Unpaired);
  }

  if (ui.takeActionLong()) {
    applog::line("[ui] долгое удержание — запрос физической привязки");
    openPairingWindow();
  }

  if (ui.takeActionShort()) {
    if (speakerLink.isConnected()) {
      // Кнопка работает как рация: пока держим — говорим, но в прошивке
      // прототипа это одиночное нажатие «слушай сейчас».
      audioPipeline.startCapture();
      speakerLink.sendEvent("button");
    } else {
      ui.setState(AgentState::Offline);
    }
  }

  if (ui.takeVolumeUp()) {
    intents.volumeUp();
  }

  if (ui.takeVolumeDown()) {
    intents.volumeDown();
  }

  // Шифрованное соединение поднимаем, только когда есть сеть и верное время:
  // без времени проверка сертификата всегда падает.
  if (deviceConfig.isPaired() && !speakerLink.isStarted()
      && wifiLink.isConnected() && wifiLink.isTimeSynced()) {
    applog::line("[ws] сеть и время готовы — подключаюсь");
    speakerLink.begin();
  }

  // Уши включены, пока есть связь и микрофон не выключен: детектор речи сам
  // решает, когда отправлять реплику. Без этого колонка молчала бы до нажатия
  // кнопки, которой на макете физически нет.
  const bool muted = syncMute();

  if (speakerLink.isConnected() && !muted && !audioPipeline.isCapturing()) {
    audioPipeline.startCapture();
  }

  if (speakerLink.isConnected()) {
    const uint32_t now = millis();
    if (now - lastHeapLogAt >= kHeapLogIntervalMs) {
      lastHeapLogAt = now;
      const bool heapOk = heap_caps_check_integrity_all(true);
      const uint32_t frames = audioPipeline.framesCaptured();
      const uint32_t fps = (frames - lastCapturedFrames) * 1000 / kHeapLogIntervalMs;
      lastCapturedFrames = frames;
      uint32_t burstRms = 0;
      uint32_t burstZcr = 0;
      audioPipeline.takeBurstPeaks(burstRms, burstZcr);

      applog::printf("[heap] %u внутр, кадров %u, темп %u/с, шум %u, rms %u, пик %u, zcr %u, куча %s\n",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)speakerLink.micFrames(), (unsigned)fps,
                     (unsigned)audioPipeline.noiseFloor(), (unsigned)audioPipeline.rms(),
                     (unsigned)burstRms, (unsigned)burstZcr,
                     heapOk ? "цела" : "ПОВРЕЖДЕНА");
    }
    if (now - lastStateAt >= kStateIntervalMs) {
      lastStateAt = now;
      speakerLink.sendState(ESP.getFreeHeap(), WiFi.RSSI());
    }
    if (ui.state() == AgentState::Offline) {
      ui.setState(AgentState::Idle);
    }
  } else if (deviceConfig.isPaired() && ui.state() != AgentState::Pairing
             && ui.state() != AgentState::Error) {
    ui.setState(AgentState::Offline);
  }
}
