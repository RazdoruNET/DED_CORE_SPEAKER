#include <Arduino.h>
#include <esp_task_wdt.h>

#include "app/Beeper.h"
#include "app/Intents.h"
#include "app/Role.h"
#include "app/Trace.h"
#include "app/Ui.h"
#include "audio/AudioPipeline.h"
#include "media/MediaPlayer.h"
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

// Пока колонка говорит, микрофон не пишет: иначе она слышит саму себя,
// отправляет собственную речь на распознавание и «разговаривает с собой».
bool agentSpeaking = false;

// Когда последний раз приходил звук речи агента. Нужен, чтобы «говорю» не
// залипло навсегда, если SPEECH_END потерялся: иначе колонка замолкает и
// перестаёт слушать до перезагрузки.
uint32_t lastAgentAudioAt = 0;

// Эфир, который попросили включить, пока колонка ещё говорила или пока сервер
// разбирал реплику. Запускать его в этот момент нельзя: порт I2S занят речью, а
// разбор ответа уже занял память, и на буферы эфира её не остаётся — в логе это
// «мало памяти под эфир: 45 КБ». Запускаем, когда речь кончилась и куча
// освободилась, но не позже MEDIA_PENDING_TIMEOUT_MS.
String pendingMediaUrl;
String pendingMediaTitle;
uint32_t pendingMediaSince = 0;

constexpr uint32_t MEDIA_PENDING_TIMEOUT_MS = 15000;

// Сколько свободной кучи нужно, чтобы эфир запустился наверняка. Замерено по
// логу: буферы и две задачи просят около 54 КБ, ещё немного уходит на сокет.
constexpr uint32_t MEDIA_READY_HEAP = 52000;

// Уровень речи агента, который приходит с сервера: так рот двигается и тогда,
// когда ответ звучит в браузере, а не в динамике колонки.
volatile uint8_t serverVoiceLevel = 0;
uint32_t lastServerVoiceAt = 0;
bool serverVoiceActive = false;

// Синий для вспышки «услышала обращение» — тот же, что в палитре Ui.
constexpr uint32_t kUiBlue = (30u << 16) | (80u << 8) | 240u;

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
// резистор вверх.
//
// Отсечка помех длиной 160 мс: восемь одинаковых отсчётов с шагом 20 мс.
// Прежние три (60 мс) пропускали короткие наводки, и красная лента «mute»
// мигала на долю секунды посреди обычной работы — со стороны это выглядело как
// случайные красные вспышки.
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
    if (sameCount < 8) {
      ++sameCount;
    }
  } else {
    candidate = sampled;
    sameCount = 1;
  }

  if (sameCount >= 8) {
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

// Уровень микрофона для шкалы — по логарифму, а не линейно.
//
// Громкость человек слышит логарифмически: разговор с метра и крик в упор
// различаются по RMS в десятки раз, а на слух — втрое. При линейной шкале
// обычная речь занимала одно деление из восьми, а всё остальное упиралось в
// максимум. Логарифм растягивает тихую часть: шёпот — одно деление, разговор —
// половина шкалы, громкий голос — вся шкала.
uint8_t micLevel(uint32_t rms) {
  // 50 — шум комнаты и шёпот, 3000 — речь в упор.
  constexpr float kFloor = 50.0f;
  constexpr float kCeil = 3000.0f;

  if (rms <= static_cast<uint32_t>(kFloor)) {
    return 0;
  }

  const float ratio = logf(static_cast<float>(rms) / kFloor) / logf(kCeil / kFloor);
  return static_cast<uint8_t>(constrain(static_cast<int>(ratio * 255.0f), 0, 255));
}

#if defined(SPEAKER_RADIO_DNS_TEST)
// Диагностика DNS: какие имена колонка разбирает и каким сервером. Нужна,
// когда станция «не отвечает» на адресе, который открывается с компьютера.
void radioDnsProbe() {
  static uint32_t lastProbe = 0;

  if (millis() - lastProbe < 10000) {
    return;
  }

  lastProbe = millis();

  const char* hosts[] = {"dedbox.pro", "ep128.hostingradio.ru", "rusradio.hostingradio.ru",
                         "retro.hostingradio.ru", "online.radiorecord.ru", "yandex.ru"};

  applog::printf("[dns] wifi %d, сервер %s\n", static_cast<int>(WiFi.status()),
                 WiFi.dnsIP().toString().c_str());

  for (const char* host : hosts) {
    IPAddress ip;
    const uint32_t started = millis();
    const bool ok = WiFi.hostByName(host, ip);
    applog::printf("[dns] %s %s %s (%u мс)\n", host, ok ? "ок" : "НЕТ", ip.toString().c_str(),
                   static_cast<unsigned>(millis() - started));
  }
}
#endif

void wireAudio() {
  audioPipeline.onFrame([](const uint8_t* pcm, size_t length) {
    speakerLink.sendMic(pcm, length);
  });

  audioPipeline.onVad([](bool start, uint32_t rms) {
    // Границы фраз печатаем всегда: по ним видно, доходит ли до сервера
    // сигнал «фраза закончилась» — без него распознавание просто не начнётся.
    applog::printf("[vad] %s, rms %u\n", start ? "начало речи" : "конец речи", rms);

    // Повторный колбэк «конец речи» на ту же фразу не должен плодить строки.
    static bool offlinePhraseReported = false;

    if (!speakerLink.isConnected() && kRole == DeviceRole::Home) {
      // Офлайн-ветка: распознавателя в прошивке ещё нет, поэтому фразу только
      // отмечаем в логе и на сервер ничего не шлём. Следующий этап подключит
      // сюда акустический распознаватель: таблица фраз уже готова
      // (app/LocalCommands.h), останется провести LocalAction → intents.
      if (start) {
        offlinePhraseReported = false;
      } else if (!offlinePhraseReported) {
        offlinePhraseReported = true;
        applog::line("[local] офлайн-фраза: распознавание не подключено");
      }
      return;
    }

    // Состояние здесь НЕ меняем. Колонка слышит комнату постоянно, и раньше
    // она «просыпалась» на любой звук: кашель, посуду, разговор не с ней.
    // Активация — только по слову-активатору, а его знает сервер (см.
    // onActivated). Звук всё равно уходит на распознавание: иначе сервер не
    // поймёт, что обращение было.
    speakerLink.sendVad(start, rms);
  });

  // Слово-активатор распознано: вот теперь колонка показывает «слушаю».
  speakerLink.onActivated([]() {
    applog::line("[voice] слово-активатор распознано — слушаю");
    beeper::listening();
    // Вспышка всей ленты синим на две секунды: обращение услышано.
    ui.flash(kUiBlue, 2000);
    ui.setState(AgentState::Listening);
  });

  // Команда принята и ушла помощнику: «думаю».
  speakerLink.onThinking([]() {
    ui.setState(AgentState::Thinking);
  });

  // Ответ агента звучит в браузере: рисуем его речь по уровню с сервера.
  speakerLink.onVoiceLevel([](uint8_t level, bool active) {
    serverVoiceLevel = level;
    serverVoiceActive = active;
    lastServerVoiceAt = millis();

    if (active && ui.state() != AgentState::Speaking) {
      ui.setState(AgentState::Speaking);
    }
  });

  audioPipeline.onSneeze([]() {
    applog::line("[sneeze] похоже на чих");
    speakerLink.sendEvent("sneeze");
  });

  audioPipeline.onLevel([](uint8_t level) {
    // Микрофон двигает только шкалы: рот повторяет голос агента, а не человека.
    ui.setHearingLevel(level);
  });

  speakerLink.onAudio([](const uint8_t* data, size_t length) {
    lastAgentAudioAt = millis();
    audioPipeline.playPcm(data, length);
  });

  // Сервер присылает трек кусками MP3: устройство декодирует само. Так играет
  // «моя музыка» любого формата — M4A и AAC плата не разберёт без сервера.
  speakerLink.onMedia([](const uint8_t* data, size_t length) {
    media::feed(data, length);
  });

  speakerLink.onSpeechBegin([](uint16_t streamId, const String& text) {
    // Лог важен не для красоты: по нему видно, доехала ли речь агента до
    // колонки. Если строка не появляется, значит сервер молчит, а не лента.
    applog::printf("[voice] агент говорит (поток %u, «%s»)\n",
                   static_cast<unsigned>(streamId), text.c_str());
    // Речь важнее эфира: порт I2S один, и говорить поверх музыки некуда.
    // Эфир вернётся сам после SPEECH_END (живой — переподключением).
    media::pauseForSpeech();
    agentSpeaking = true;
    lastAgentAudioAt = millis();
    if (audioPipeline.isCapturing()) {
      audioPipeline.stopCapture();
    }
    ui.setState(AgentState::Speaking);
  });

  speakerLink.onSpeechEnd([]() {
    applog::printf("[voice] агент закончил: кадров %u, байт %u\n",
                   static_cast<unsigned>(speakerLink.sayFrames()),
                   static_cast<unsigned>(speakerLink.sayBytes()));
    agentSpeaking = false;
    ui.setState(speakerLink.isConnected() ? AgentState::Idle : AgentState::Offline);
    media::resumeAfterSpeech();
  });

  speakerLink.onCommand([](const String& action, JsonVariantConst args) {
    // Команды печатаем всегда: без этого не видно, дошла ли до платы команда
    // сервера или колонка просто ничего не получила.
    applog::printf("[cmd] %s\n", action.c_str());

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
      media::pauseForSpeech();
      ui.setState(AgentState::Idle);
    } else if (action == "media.play") {
      // Эфир и треки колонка тянет сама: через сервер шёл бы PCM 16 кГц, то
      // есть «телефон» вместо музыки.
      const String url = args["url"] | "";
      const String title = args["title"] | "";
      applog::printf("[media] команда: «%s» %s\n", title.c_str(), url.c_str());
      if (url.length() == 0) {
        applog::line("[media] команда без ссылки — играть нечего");
      } else if (agentSpeaking || ESP.getFreeHeap() < MEDIA_READY_HEAP) {
        pendingMediaUrl = url;
        pendingMediaTitle = title;
        pendingMediaSince = millis();
        applog::printf("[media] отложен до конца речи: «%s», свободно %u\n", title.c_str(),
                       static_cast<unsigned>(ESP.getFreeHeap()));
      } else if (!media::play(url, title)) {
        applog::line("[media] не удалось начать поток");
      } else {
        ui.setState(AgentState::Speaking);
      }
    } else if (action == "media.stream") {
      // Трек перекодирует сервер и присылает кадрами MEDIA.
      const String title = args["title"] | "";
      applog::printf("[media] поток от сервера: «%s»\n", title.c_str());
      media::beginRelay(title);
      ui.setState(AgentState::Speaking);
    } else if (action == "media.stop") {
      pendingMediaUrl = "";
      pendingMediaTitle = "";
      media::stop();
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
  applog::printf("\n=== DED CORE SPEAKER %s (%s), LED %u/255 ===\n",
                SPEAKER_FIRMWARE_VERSION, SPEAKER_PROFILE_NAME,
                static_cast<unsigned>(LED_BRIGHTNESS_MAX));

#if defined(SPEAKER_LED_ONLY_TEST)
  // Только лента: без микрофона, Wi-Fi и TLS. Так проверка пайки не зависит
  // от состояния сети и не перезагружается вместе с ней.
  ui.begin();
  ui.setState(AgentState::Boot);
  return;
#endif

  // TLS-рукопожатие с сервером занимает 5–7 с, и всё это время главный цикл
  // стоит внутри WebSocketsClient::loop(). Штатный сторожевой таймер IDLE-задачи
  // ядра 1 (5 с) в это время считает, что система зависла, и перезагружает
  // колонку — в логе это `TG1WDT_SYS_RST` сразу после подключения к точке.
  //
  // Свой сторож остаётся и работает по существу: если связи нет дольше
  // WATCHDOG_REBOOT_MS, колонка перезапускается сама (см. loop). Он ловит именно
  // «зависла и молчит», а не «долго устанавливает защищённое соединение».
  //
  // Таймаут меняется только через снятие и повторную установку: штатные 5 с срабатывают на
  // TLS-рукопожатии: оно блокирует главный цикл, колонка перезагружается прямо
  // на подключении (в логе `TG1WDT_SYS_RST` сразу после «подключаюсь»).
  {
    const esp_err_t stopped = esp_task_wdt_deinit();
    const esp_err_t started = esp_task_wdt_init(30, true);
    applog::printf("[boot] сторожевой таймер: снят %d, включён на 30 с %d\n",
                   static_cast<int>(stopped), static_cast<int>(started));
  }

  securityBlocked = !speakerSecureFlashEnabled();

  deviceConfig.begin();
  deviceId = speakerDeviceId();
  applog::printf("[boot] устройство %s, свободно %u байт\n", deviceId.c_str(),
                ESP.getFreeHeap());

  beeper::begin();
  beeper::boot();

  audioPipeline.begin();
  audioPipeline.setVolume(deviceConfig.volume);

  // Память под эфир берём до Wi-Fi и TLS: после защищённого соединения в куче
  // не остаётся цельного куска, и радио не включалось вовсе.
  media::prepare();

#if defined(SPEAKER_SPK_SELFTEST)
  // Стенд: звук проверяется до сети, привязки и TLS. Слышно три тона подряд.
  audioPipeline.speakerSelfTest();
#endif

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

#if defined(SPEAKER_RADIO_TEST_URL)
  // Стенд: эфир включается сам, без сервера и без команды. Нужен, чтобы
  // проверить поток, декодер и I2S на живой станции до интеграции с сервером.
  media::play(SPEAKER_RADIO_TEST_URL, "проверка эфира");
#endif

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
#if defined(SPEAKER_LED_ONLY_TEST)
  ui.tick();
  delay(5);
  return;
#endif

  beeper::tick();

#if defined(SPEAKER_RADIO_DNS_TEST)
  radioDnsProbe();
#endif

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

  // Страховка от застывшей ленты: «слушаю»/«думаю» живут, пока сервер не
  // пришлёт следующий шаг. Если ответ потерялся (обрыв, ошибка помощника),
  // через 45 секунд возвращаемся в простой.
  {
    static uint32_t waitingSince = 0;
    if (ui.state() == AgentState::Listening || ui.state() == AgentState::Thinking) {
      if (waitingSince == 0) {
        waitingSince = millis();
      } else if (millis() - waitingSince > 45000) {
        waitingSince = 0;
        ui.setState(AgentState::Idle);
      }
    } else {
      waitingSince = 0;
    }
  }

  trace::checkHeap();

  // Рот повторяет голос агента: уровень берём из воспроизводимого звука, а не
  // с микрофона. Микрофон в этот момент слышит динамик, и рот дёргался бы от
  // эха и от чужих голосов в комнате.
  const uint8_t playbackLevel = audioPipeline.outputLevel();

  // Уровень с сервера не должен «залипнуть», если ответ оборвался.
  if (millis() - lastServerVoiceAt > 800) {
    serverVoiceLevel = 0;
    serverVoiceActive = false;
  }

  ui.setVoiceLevel(playbackLevel > serverVoiceLevel ? playbackLevel : serverVoiceLevel);

  // Шкалы микрофонов: правый канал растёт от диода 1 к центру, левый — от
  // диода 17.
  ui.setMicLevels(micLevel(audioPipeline.rmsLeft()), micLevel(audioPipeline.rmsRight()));

  // Рот говорит, пока идёт звук — из динамика или из браузера. Когда звук
  // кончился и сервер молчит, возвращаемся в простой.
  if (ui.state() == AgentState::Speaking && playbackLevel == 0 && serverVoiceLevel == 0
      && !agentSpeaking && !serverVoiceActive && millis() - lastServerVoiceAt > 1200) {
    ui.setState(speakerLink.isConnected() ? AgentState::Idle : AgentState::Offline);
  }

  ui.tick();
  intents.tick();
  wifiLink.loop();
  speakerLink.loop();
  blePairing.tick();
  deviceConfig.tickVolume();

  // Связь пропала — отпускаем эфир. Переустановка TLS просит около 30 КБ, а
  // вместе с эфиром их взять негде: рукопожатие падало с
  // «SSL - Memory allocation failed», колонка не могла подключиться и уходила
  // в перезагрузку по сторожу. Музыку в этот момент всё равно никто не слышит.
  {
    static uint32_t mediaReleaseSince = 0;

    if (speakerLink.isConnected() || !media::active()) {
      mediaReleaseSince = 0;
    } else if (mediaReleaseSince == 0) {
      mediaReleaseSince = millis();
    } else if (millis() - mediaReleaseSince > 3000) {
      mediaReleaseSince = 0;
      applog::line("[media] связи нет — освобождаю память под переподключение");
      media::stop();
    }
  }

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

  // Уши включены, пока микрофон не выключен: детектор речи сам решает, когда
  // отправлять реплику. Домашняя колонка слушает и без сервера — иначе
  // встроенные команды не работали бы в офлайне. Без этого колонка молчала бы
  // до нажатия кнопки, которой на макете физически нет.
  // Сторож речи: SPEECH_END может потеряться (обрыв, остановка потока), и тогда
  // «говорю» висело бы вечно — колонка молчала бы и не слушала.
  // Отложенный эфир: ждём, когда колонка договорит и куча освободится. Без
  // таймаута он мог бы ждать вечно, если ответа вообще не будет.
  if (pendingMediaUrl.length() > 0 && !agentSpeaking
      && millis() - lastAgentAudioAt > 1200
      && (ESP.getFreeHeap() >= MEDIA_READY_HEAP
          || millis() - pendingMediaSince > MEDIA_PENDING_TIMEOUT_MS)) {
    const String url = pendingMediaUrl;
    const String title = pendingMediaTitle;
    pendingMediaUrl = "";
    pendingMediaTitle = "";

    if (media::play(url, title)) {
      applog::printf("[media] отложенный эфир пошёл: «%s»\n", title.c_str());
      ui.setState(AgentState::Speaking);
    } else {
      applog::printf("[media] отложенный эфир не запустился, свободно %u\n",
                     static_cast<unsigned>(ESP.getFreeHeap()));
    }
  }

  if (agentSpeaking && millis() - lastAgentAudioAt > 20000) {
    applog::line("[voice] конец речи потерялся — возвращаю слух");
    agentSpeaking = false;
    ui.setState(speakerLink.isConnected() ? AgentState::Idle : AgentState::Offline);
    media::resumeAfterSpeech();
  }

  const bool muted = syncMute();

  if ((speakerLink.isConnected() || (kRole == DeviceRole::Home && deviceConfig.isPaired()))
      && !muted && !agentSpeaking && !audioPipeline.isCapturing()) {
    audioPipeline.startCapture();
  }

  if (speakerLink.isConnected()) {
    const uint32_t now = millis();
    if (now - lastHeapLogAt >= kHeapLogIntervalMs) {
      lastHeapLogAt = now;
      const uint32_t frames = audioPipeline.framesCaptured();
      const uint32_t fps = (frames - lastCapturedFrames) * 1000 / kHeapLogIntervalMs;
      lastCapturedFrames = frames;
      uint32_t burstRms = 0;
      uint32_t burstZcr = 0;
      audioPipeline.takeBurstPeaks(burstRms, burstZcr);

      // Порог печатается рядом с фоном: по этим двум числам видно, почему
      // колонка не слышит тихую фразу — порог выше речи или ниже.
      const uint32_t threshold = max<uint32_t>(
          VAD_RMS_ON, audioPipeline.noiseFloor() * VAD_NOISE_ON_X10 / 10);

      applog::printf("[heap] %u внутр, кадров %u, vad %u, темп %u/с, шум %u, порог %u, rms %u, пик %u, zcr %u, речь %u/%u, потерь %u\n",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)speakerLink.micFrames(),
                     (unsigned)speakerLink.vadFrames(), (unsigned)fps,
                     (unsigned)audioPipeline.noiseFloor(), (unsigned)threshold,
                     (unsigned)audioPipeline.rms(),
                     (unsigned)burstRms, (unsigned)burstZcr,
                     (unsigned)speakerLink.sayFrames(),
                     (unsigned)speakerLink.sayBytes(),
                     (unsigned)audioPipeline.playbackDrops());

      // Диагностика «молчит»: видно, слушает ли колонка и не залип ли флаг речи.
      applog::printf("[state] захват %d, агент говорит %d, эфир %d, приём %u, буфер %u\n",
                     audioPipeline.isCapturing() ? 1 : 0,
                     agentSpeaking ? 1 : 0,
                     media::active() ? 1 : 0,
                     static_cast<unsigned>(media::receivedBytesTotal()),
                     static_cast<unsigned>(media::bufferedBytes()));
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
