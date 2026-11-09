#pragma once

#include <Arduino.h>
#include <esp_rom_sys.h>

// Строгая сборка для серии: без включённого flash-шифрования устройство не
// работает вообще. Ключ устройства и пароль Wi-Fi лежат в NVS, и на плате без
// шифрования они читаются по UART. Флаг включается в env esp32s3_prod и
// проверяется в setup(); обычные сборки для макета его не используют.
#if defined(SPEAKER_REQUIRE_SECURE_FLASH)
#include <esp_flash_encrypt.h>
inline bool speakerSecureFlashEnabled() { return esp_flash_encryption_enabled(); }
#else
inline bool speakerSecureFlashEnabled() { return true; }
#endif

#if __has_include("config/Secrets.h")
#include "config/Secrets.h"
#endif

#ifndef SPEAKER_ROOT_CA_PEM
#define SPEAKER_ROOT_CA_PEM ""
#endif

// Открытый ключ подписи привязки: несжатая точка P-256 в hex, 130 символов.
// Именно hex, а не PEM: в предсборке mbedtls от Arduino-ядра разбор PEM может
// быть выключен, и тогда проверка подписи молча отказывала бы всегда.
#ifndef SPEAKER_PAIRING_PUBLIC_KEY_HEX
#define SPEAKER_PAIRING_PUBLIC_KEY_HEX ""
#endif

// Код привязки, прошитый в устройство для стенда. В серии код лежит в NVS
// (namespace spk_id, ключ code) и записывается при производстве; значение
// отсюда — только запасной вариант для макета. Пустая строка означает «кода
// нет»: устройство принимает кадр привязки версии 1, и Wi-Fi пароль в нём
// защищён только ECDH (остаточный риск первой привязки).
#ifndef SPEAKER_PAIR_CODE
#define SPEAKER_PAIR_CODE ""
#endif

// ---------------------------------------------------------------------------
// Идентификация устройства и версия
// ---------------------------------------------------------------------------
#define SPEAKER_FIRMWARE_VERSION "0.2.0"
#define SPEAKER_PRODUCT          "smart-speaker"

#if defined(DEVICE_PROFILE_S3)
#define SPEAKER_PROFILE_NAME "esp32s3"
#define SPEAKER_HAS_AEC       1
#define SPEAKER_HAS_MEDIA     1
#else
#define SPEAKER_PROFILE_NAME "esp32da"
#define SPEAKER_HAS_AEC       0
// Классический ESP32 без PSRAM: одновременные речь и музыка не помещаются
// в 520 КБ SRAM, поэтому медиа выключается, пока идёт разговор.
#define SPEAKER_HAS_MEDIA     0
#endif

// Выходной тракт.
//
//   по умолчанию      — внешний I2S-ЦАП или I2S-усилитель (MAX98357A, PCM5102A):
//                       3 провода, качество 16 бит, микрофон на I2S0;
//   SPEAKER_OUTPUT_INTERNAL_DAC — встроенный 8-битный ЦАП ESP32 (GPIO25).
//                       Нужен, чтобы проверить усилитель и динамик, пока не
//                       приехал внешний ЦАП. Качество 8 бит: для речи слышно,
//                       для музыки — нет. Встроенный ЦАП живёт только на I2S0,
//                       поэтому микрофон в этом режиме переезжает на I2S1.
#if defined(SPEAKER_OUTPUT_INTERNAL_DAC) && defined(DEVICE_PROFILE_S3)
// У S3 встроенного ЦАП нет вовсе (SOC_I2S_SUPPORTS_DAC не определён), поэтому
// такой выход для продуктовой платы — ошибка сборки, а не тихо неработающий звук.
#error "SPEAKER_OUTPUT_INTERNAL_DAC доступен только на классическом ESP32: у S3 нет ЦАП"
#endif

#if defined(SPEAKER_OUTPUT_INTERNAL_DAC)
#define SPEAKER_DAC_OUTPUT 1
#else
#define SPEAKER_DAC_OUTPUT 0
#endif

// ---------------------------------------------------------------------------
// Логи
// ---------------------------------------------------------------------------
// На S3 включён native USB, поэтому Serial — это USB CDC, а UART0 отдельным
// объектом Serial0. Пишем сразу в оба: на девките видно и там, и там, а на
// плате с единственным разъёмом UART всё равно видно.
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
#define SPEAKER_LOG_MIRROR 1
#else
#define SPEAKER_LOG_MIRROR 0
#endif

// Диагностический выключатель дублирования в UART0.
#if defined(SPEAKER_NO_LOG_MIRROR)
#undef SPEAKER_LOG_MIRROR
#define SPEAKER_LOG_MIRROR 0
#endif

namespace applog {
#if SPEAKER_LOG_MIRROR
// UART0 на S3 не переинициализируем: повторный вызов begin() для того же порта
// роняет ядро (переполнение стека внутри uartBegin). Пишем в ROM-консоль —
// она уже настроена загрузчиком на 115200 и драйвера не требует.
//
// Порциями: у ROM-printf свой внутренний буфер, а строки бывают длиннее
// (например, JSON с открытым ключом), и обрезать их нельзя.
inline void mirror(const char* text, size_t length, bool newline) {
  char chunk[129];

  for (size_t offset = 0; offset < length;) {
    const size_t take = (length - offset) < (sizeof(chunk) - 1)
                            ? (length - offset)
                            : (sizeof(chunk) - 1);
    memcpy(chunk, text + offset, take);
    chunk[take] = '\0';
    esp_rom_printf("%s", chunk);
    offset += take;
  }

  if (newline) {
    esp_rom_printf("\r\n");
  }
}
#else
inline void mirror(const char*, size_t, bool) {}
#endif

// Лог пишут несколько задач (цикл, микрофон, колбэк Bluetooth). Без блокировки
// строки перемешиваются, и настоящая ошибка тонет в мусоре.
inline SemaphoreHandle_t& logMutex() {
  static SemaphoreHandle_t handle = nullptr;
  return handle;
}

inline void write(const char* text, size_t length, bool newline) {
  const SemaphoreHandle_t mutex = logMutex();
  if (mutex != nullptr) {
    xSemaphoreTake(mutex, portMAX_DELAY);
  }

  if (length > 0) {
    Serial.write(reinterpret_cast<const uint8_t*>(text), length);
  }
  if (newline) {
    Serial.write(reinterpret_cast<const uint8_t*>("\r\n"), 2);
  }

  mirror(text, length, newline);

  if (mutex != nullptr) {
    xSemaphoreGive(mutex);
  }
}

inline void begin() {
  logMutex() = xSemaphoreCreateMutex();
  Serial.begin(115200);
}

inline void line(const char* text) { write(text, strlen(text), true); }
inline void line(const String& text) { write(text.c_str(), text.length(), true); }

// Формат уже содержит перевод строки, поэтому двойной не добавляем.
// Буфер 512: строка с открытым ключом устройства занимает около 450 символов,
// и обрезать её нельзя — по ней сайт собирает запрос на привязку.
inline void printf(const char* format, ...) {
  static char buffer[512];

  va_list args;
  va_start(args, format);
  const int length = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  if (length <= 0) {
    return;
  }

  const auto size = static_cast<size_t>(length) < sizeof(buffer) - 1
                        ? static_cast<size_t>(length)
                        : sizeof(buffer) - 1;
  write(buffer, size, false);
}
}  // namespace applog

// ---------------------------------------------------------------------------
// Пины (см. docs/04-hardware-bom.md)
// ---------------------------------------------------------------------------
namespace pins {
#if defined(DEVICE_PROFILE_S3)
// ESP32-S3 N16R8. GPIO 26–32 заняты флешем, 33–37 — восьмибитной PSRAM,
// 19/20 — native USB, 43/44 — UART0, 0/3/45/46 — strapping. Всё выбранное
// ниже свободно и не мешает загрузке.
constexpr int MIC_BCLK = 4;
constexpr int MIC_WS   = 5;
constexpr int MIC_SD   = 6;

constexpr int SPK_BCLK = 15;
constexpr int SPK_LRC  = 16;
constexpr int SPK_DIN  = 17;

constexpr int I2C_SDA = 8;
constexpr int I2C_SCL = 9;

constexpr int BTN_ACTION   = 10;
constexpr int BTN_VOL_UP   = 11;
constexpr int BTN_VOL_DOWN = 12;
constexpr int SW_MUTE      = 13;

// Внешняя лента: на плате этот вывод подписан RGB-OUT, отдельный штырёк.
constexpr int LED_STRIP = 38;
// Встроенный WS2812. Проверено перебором на живой плате — 48, не 38.
constexpr int LED_STATUS = 48;
// Пищалка: пока нет усилителя, колонка подаёт тональные сигналы.
constexpr int BUZZER = 21;
constexpr int IR_TX     = 14;
constexpr int IR_RX     = 18;
#else
constexpr int MIC_BCLK = 32;
constexpr int MIC_WS   = 33;
constexpr int MIC_SD   = 34;

constexpr int SPK_BCLK = 25;
constexpr int SPK_LRC  = 26;
constexpr int SPK_DIN  = 27;

constexpr int I2C_SDA = 21;
constexpr int I2C_SCL = 22;

constexpr int BTN_ACTION   = 0;
constexpr int BTN_VOL_UP   = 39;
constexpr int BTN_VOL_DOWN = 35;
constexpr int SW_MUTE      = 36;

constexpr int LED_STRIP = 16;
// У WROOM-DA встроенного светодиода нет.
constexpr int LED_STATUS = -1;
constexpr int BUZZER = 23;
constexpr int IR_TX     = 4;
constexpr int IR_RX     = 17;
#endif
}  // namespace pins

// Препроцессор не умеет pins::LED_STATUS, поэтому признак наличия встроенного
// светодиода задаётся отдельным макросом.
#if defined(DEVICE_PROFILE_S3)
#define SPEAKER_HAS_STATUS_LED 1
#else
#define SPEAKER_HAS_STATUS_LED 0
#endif

// Пищалка есть на обоих профилях: вывод объявлен в обоих наборах пинов.
// Флаг SPEAKER_BEEPER_SILENT полностью выключает звук пищалки — нужен, когда
// сигналы мешают (например, при отладке ночью).
#define SPEAKER_HAS_BUZZER 1

// ---------------------------------------------------------------------------
// Аудиотракт
// ---------------------------------------------------------------------------
constexpr int AUDIO_RATE          = 16000;
constexpr int AUDIO_FRAME_MS      = 20;
constexpr int AUDIO_FRAME_SAMPLES = AUDIO_RATE * AUDIO_FRAME_MS / 1000;  // 320

// Преролл: wake word срабатывает с задержкой, начало фразы не должно потеряться.
constexpr int PREROLL_MS     = 900;
constexpr int PREROLL_FRAMES = PREROLL_MS / AUDIO_FRAME_MS;  // 45

// Конец фразы. В браузере платформа ждёт 900 мс — для колонки это «тупит».
// Но и 600 мс мало: после «Джарвис,» человек делает естественную паузу, и
// команда приезжает двумя обрывками — «Джарвис.» и «включи радио». Поэтому ждём
// 1.2 с: одна пауза внутри фразы больше не рвёт команду.
constexpr int END_SILENCE_MS     = 1200;
constexpr int END_SILENCE_FRAMES = END_SILENCE_MS / AUDIO_FRAME_MS;

// Короче — это кашель или стук, а не команда. Полсекунды отсекают короткие
// щелчки, на которых распознаватель любит «слышать» музыку.
constexpr int MIN_SPEECH_MS     = 500;

// Предел одной фразы. Музыка из колонок компьютера тянется бесконечно, и без
// предела детектор речи не закрывает фрагмент: колонка «слушает вечно» и новые
// команды не принимает.
constexpr int MAX_PHRASE_MS     = 8000;

// Детектор чиха: один короткий громкий всплеск. Замеры на живой колонке —
// фон 130–500, чих 2600–5000 при длительности 80–360 мс.
constexpr uint32_t SNEEZE_RMS_START   = 4000;
// После всплеска должна наступить тишина: у речи и музыки следующий звук идёт
// сразу, у чиха — пауза.
constexpr uint32_t SNEEZE_QUIET_MS    = 800;
constexpr uint16_t SNEEZE_MIN_FRAMES  = 4;
constexpr uint16_t SNEEZE_MAX_FRAMES  = 18;
constexpr uint32_t SNEEZE_COOLDOWN_MS = 4000;
constexpr int MAX_PHRASE_FRAMES = MAX_PHRASE_MS / AUDIO_FRAME_MS;
constexpr int MIN_SPEECH_FRAMES = MIN_SPEECH_MS / AUDIO_FRAME_MS;

// Порог детекции речи по RMS. Считается уже после отсечки инфразвука, поэтому
// значения ниже, чем для «сырого» сигнала: у INMP441 основной вклад в уровень
// даёт дрейф постоянной составляющей, а не речь.
//
// Пороги намеренно ниже разговорной громкости: раньше тихая речь с 2–3 метров
// давала 500–900, не дотягивала до 1300, и колонка «не слышала», пока в неё
// не говорили громко. Запас над шумом держит адаптивная часть — множитель от
// измеренного фона, поэтому снижение фиксированного порога не будит колонку
// на скрип кресла.
constexpr uint32_t VAD_RMS_ON  = 700;
constexpr uint32_t VAD_RMS_OFF = 380;

// Запас над измеренным фоном: включение и выключение речи. Дробные значения
// хранятся в десятых, чтобы не терять точность на целых.
//
// 2.4× — это около 7.6 дБ над фоном. Живой замер: в комнате с вентилятором
// фон 300–600, спокойная речь с двух метров 900–1400. При прежних 5× порог
// уходил на 2000–3000, и тихая фраза не проходила вообще — отсюда и
// «приходится говорить громко».
constexpr uint32_t VAD_NOISE_ON_X10  = 24;  // 2.4 × фон
constexpr uint32_t VAD_NOISE_OFF_X10 = 18;  // 1.8 × фон

// Джиттер-буфер вниз: 3 кадра (60 мс).
constexpr int JITTER_FRAMES = 3;

// Сдвиг при чтении микрофона.
//
// INMP441 и ICS-43434 отдают 24 бита в 32-битном слоте I2S, выровнено по
// старшим битам. Чтобы получить 16 бит, нужно сдвинуть на 16. Сдвиг на 14
// (встречается в примерах) оставляет 18 значащих бит, и приведение к int16
// искажает громкие звуки.
constexpr int MIC_SAMPLE_SHIFT = 16;

// Прогрев микрофона: при подаче питания INMP441 выдаёт громкий переходный
// процесс (пик упирается в предел), и без отбрасывания первых кадров детектор
// речи срабатывает на каждом включении, отправляя на сервер мусорную фразу.
constexpr int MIC_WARMUP_FRAMES = 25;  // 25 кадров по 20 мс — полсекунды

// Сколько микрофонов на шине. Два: один пишет в левый канал, второй в правый
// (пин L/R на землю и на питание соответственно).
constexpr int MIC_CHANNEL_COUNT = 2;

// Какой канал уходит на распознавание.
//
// Сумма двух микрофонов звучит «гулко» и убивает разборчивость: они разнесены,
// звук приходит с разной задержкой, и при сложении часть спектра гасится
// (гребенчатая фильтрация). Для речи берём один микрофон, второй остаётся для
// диагностики уровней.
constexpr int MIC_PRIMARY_CHANNEL = 0;

// Отсечка инфразвука перед распознаванием. INMP441 даёт заметный дрейф нуля,
// а в комнате живёт гул 40–60 Гц уровня, сравнимого с речью. Одного полюса
// (6 дБ/октава) не хватает: на 40 Гц он гасит всего 7 дБ. Ставим фильтр
// Баттерворта 4-го порядка (24 дБ/октава) на 90 Гц.
constexpr float MIC_HIGHPASS_FC = 90.0f;

// Подъём уровня после фильтра. Речь с двух метров занимает всего 8–9 бит из 16,
// и без усиления теряются тихие согласные. Распознаватель ждёт звук около
// −20 dBFS, поэтому поднимаем на 18 дБ и мягко ограничиваем громкие места.
constexpr float MIC_GAIN = 8.0f;

// Мягкий ограничитель: до колена сигнал идёт как есть, дальше сжимается.
// Жёсткая отсечка на таком усилении превращала громкий слог в прямоугольник.
constexpr int32_t MIC_SOFT_KNEE = 22000;

// Автоматическая регулировка уровня перед отправкой на сервер.
//
// Фиксированный MIC_GAIN приходится выбирать под громкую речь: тихую он не
// вытягивает. AGC меряет уровень кадра и поднимает тихую фразу до целевого,
// а громкую не трогает, поэтому распознаватель получает одинаковую громкость
// и с двух метров, и с пяти.
//
// AGC работает только когда сигнал заметно выше фона: иначе он поднял бы шум
// и колонка начала бы слышать собственную тишину. Пороги VAD считаются по
// сигналу ДО AGC, поэтому чувствительность от него не «плывёт».
constexpr float MIC_AGC_TARGET = 5200.0f;    // желаемый RMS кадра
constexpr float MIC_AGC_MAX_BOOST = 5.0f;    // не больше +14 дБ сверх MIC_GAIN
constexpr uint32_t MIC_AGC_MIN_RMS = 220;    // ниже — это фон, не усиливаем
constexpr uint32_t MIC_AGC_NOISE_X = 2;      // включать AGC при RMS > 2 × фон

// Сколько RMS приходится на единицу уровня для анимации рта. При делении на
// 32 тихая речь давала уровень 20–30, и рот почти не двигался.
constexpr uint32_t MIC_LEVEL_DIVISOR = 12;

// Уровень голоса агента для рта. Речь из динамика громче микрофонной, поэтому
// делим сильнее: при делителе микрофона шкала упиралась в 255 на каждом слове,
// и рот стоял открытым всё время.
constexpr uint32_t SPEAKER_VOICE_LEVEL_DIVISOR = 40;

// ---------------------------------------------------------------------------
// Связь
// ---------------------------------------------------------------------------
constexpr int WS_HEARTBEAT_MS  = 15000;
constexpr int WS_RECONNECT_MIN = 2000;
// Если связи нет дольше этого, клиент пересоздаётся: при «залипшем» TCP
// библиотека иначе повторяет попытки бесконечно (SYN уходит, ответ не приходит).
constexpr uint32_t WS_RESET_AFTER_MS = 30000;

// Если связи нет дольше этого, колонка перезагружается: после повреждения кучи
// сеть иногда остаётся в состоянии, из которого сама не выходит.
constexpr uint32_t WATCHDOG_REBOOT_MS = 300000;
constexpr int WS_RECONNECT_MAX = 60000;
constexpr int SPEAKER_REPLY_TIMEOUT_MS = 30000;

// Идентификатор продукта в FirmwareService: чужая колонка не должна
// выдавать себя за наше устройство.
constexpr const char* DEVICE_PRODUCT = SPEAKER_PRODUCT;

// ---------------------------------------------------------------------------
// Светодиодная лента
// ---------------------------------------------------------------------------
// Лента идёт по низу колонки буквой U и делится по смыслу лица: по краям —
// уши (они же щёки), в середине — рот, между ними — погашенные разделители.
// Числа — физическая раскладка, а не длина в метрах; менять их вместе с
// монтажом. Раскладку и анимации разбирает app/LedLayout.h.
//
//   1 2 3 | 4 5 | 6 7 8 9 10 11 12 | 13 14 | 15 16 17
//    уши   выкл.        рот           выкл.      уши
constexpr uint16_t LED_EAR_LEDS   = 3;
constexpr uint16_t LED_GAP_LEDS   = 2;
constexpr uint16_t LED_MOUTH_LEDS = 7;
constexpr uint16_t LED_STRIP_LEDS =
    LED_EAR_LEDS * 2 + LED_GAP_LEDS * 2 + LED_MOUTH_LEDS;  // 17
// Без согласователя ленту питают от 3.3 В: уровни совпадают, но зелёный и
// синий каналы близки к порогу, поэтому 220 — практический потолок.
// С 74HCT125 лента питается от 5 В, и потолок можно поднять до 255
// (env esp32s3_dedbox задаёт его отдельно).
#ifndef SPEAKER_LED_BRIGHTNESS
#define SPEAKER_LED_BRIGHTNESS 220
#endif
constexpr uint8_t LED_BRIGHTNESS_MAX = SPEAKER_LED_BRIGHTNESS;

// ---------------------------------------------------------------------------
// Тайминги
// ---------------------------------------------------------------------------
constexpr uint32_t BUTTON_DEBOUNCE_MS = 40;
constexpr uint32_t BUTTON_LONG_MS     = 5000;
constexpr uint32_t VOLUME_SAVE_DELAY  = 5000;
