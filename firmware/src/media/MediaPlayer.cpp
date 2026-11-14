#include "media/MediaPlayer.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <freertos/stream_buffer.h>
#include <WiFiClientSecure.h>

#include "app/Trace.h"
#include "audio/AudioPipeline.h"
#include "config/AppConfig.h"
#include "minimp3.h"
#include "net/PublicRoots.h"

namespace media {
namespace {

// Входной буфер декодера: полсекунды эфира на 128 кбит/с. Кадр MP3 занимает
// не больше 1441 байта, но читаем заранее, чтобы не просить у потока по
// несколько десятков байт на кадр.
constexpr size_t kInputBytes = 8192;

// Столько накапливаем перед первым кадром в эфир: около 380 мс. Меньше —
// и любой рывок Wi-Fi слышен как провал, потому что выход идёт в реальном
// времени и ждать ему негде.
constexpr size_t kPrebufferBytes = 6144;

// Предбуфер обязан помещаться во входной буфер: иначе условие «накопили
// достаточно» не выполнится никогда, декодер будет ждать вечно, а buffer
// потока забьётся и звука не будет вовсе. Наступали на это.
static_assert(kPrebufferBytes < kInputBytes, "предбуфер должен влезать во входной буфер");

// Стеки считаны по факту: загрузчику нужен HTTPClient (он держит заголовки в
// String), декодеру — кадры minimp3. На 10 КБ загрузчик падал с переполнением
// стека. Декодеру сначала давали 24 КБ, потому что рабочий буфер кадра лежал
// в локальной переменной; после переноса в статику ему хватает 8 КБ.
constexpr int kFetcherStack = 14336;
constexpr int kDecoderStack = 8192;
constexpr size_t kStreamBytes = 10240;
constexpr UBaseType_t kTaskPriority = 3;

SemaphoreHandle_t stateLock = nullptr;

// Источник и потребитель разведены общим буфером: писать в него может либо
// HTTP-загрузчик (колонка тянет поток сама), либо сервер кадрами MEDIA
// (перекодирует то, что устройство не умеет: M4A, AAC).
StreamBufferHandle_t buffer = nullptr;

TaskHandle_t decoderTask = nullptr;
TaskHandle_t fetcherTask = nullptr;

String currentUrl;
String currentTitle;
String savedUrl;
String savedTitle;

volatile bool running = false;
volatile bool relay = false;

// Речь важнее эфира, но живой поток обрывать нельзя: после ответа его некому
// вернуть. Поэтому эфир от сервера не останавливается, а замирает — задача
// декодера отпускает порт и ждёт, пока колонка договорит.
volatile bool paused = false;
volatile uint32_t droppedBytes = 0;
volatile uint32_t receivedBytes = 0;

// Корни для потока: публичные ЦС нужны всегда, в отличие от связи с платформой.
const String& mediaRoots() {
  static String chain;

  if (chain.isEmpty()) {
    chain.reserve(strlen(SPEAKER_ROOT_CA_PEM) + strlen(SPEAKER_PUBLIC_ROOTS_PEM)
                  + strlen(SPEAKER_PUBLIC_ROOTS_RSA_PEM) + 4);
    chain += SPEAKER_ROOT_CA_PEM;
    chain += "\n";
    chain += SPEAKER_PUBLIC_ROOTS_PEM;
    chain += "\n";
    chain += SPEAKER_PUBLIC_ROOTS_RSA_PEM;
  }

  return chain;
}

void remember(const String& url, const String& title) {
  if (stateLock != nullptr) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
  }

  currentUrl = url;
  currentTitle = title;

  if (stateLock != nullptr) {
    xSemaphoreGive(stateLock);
  }
}

bool hostOf(const String& url, String& host, uint16_t& port) {
  const int schemeEnd = url.indexOf("://");
  if (schemeEnd < 0) {
    return false;
  }

  port = url.startsWith("https://") ? 443 : 80;

  String rest = url.substring(schemeEnd + 3);
  const int slash = rest.indexOf('/');
  if (slash >= 0) {
    rest = rest.substring(0, slash);
  }

  const int colon = rest.lastIndexOf(':');
  if (colon >= 0) {
    port = static_cast<uint16_t>(rest.substring(colon + 1).toInt());
    rest = rest.substring(0, colon);
  }

  host = rest;
  return host.length() > 0;
}

int connectStream(HTTPClient& http, WiFiClient& net, const String& url) {
  if (url.startsWith("https://")) {
    WiFiClientSecure& secure = static_cast<WiFiClientSecure&>(net);

#if defined(SPEAKER_MEDIA_INSECURE)
    // Только для стенда: понять, что мешает — сертификат или память под TLS.
    secure.setInsecure();
#else
    secure.setCACert(mediaRoots().c_str());
#endif
  }

  net.setTimeout(8000);

  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setConnectTimeout(6000);
  http.setTimeout(8000);

  if (!http.begin(net, url)) {
    return -1;
  }

  return http.GET();
}

/// Загрузчик: тянет поток и складывает байты в общий буфер.
void fetcherEntry(void*) {
  String url;

  if (stateLock != nullptr) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
  }

  url = currentUrl;

  if (stateLock != nullptr) {
    xSemaphoreGive(stateLock);
  }

  // Ждём сеть: стендовая сборка стартует эфир из setup(), когда Wi-Fi ещё
  // поднимается, и первый же connect уходил в DNS-ошибку.
  while (running && WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  uint8_t* scratch = static_cast<uint8_t*>(malloc(2048));

  if (scratch == nullptr) {
    running = false;
    fetcherTask = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  uint32_t failedConnects = 0;
  uint32_t totalBytes = 0;

  while (running) {
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    const bool tls = url.startsWith("https://");
    WiFiClient& net = tls ? static_cast<WiFiClient&>(secure) : plain;

    const int code = connectStream(http, net, url);

    if (code != HTTP_CODE_OK && code != HTTP_CODE_PARTIAL_CONTENT) {
      http.end();
      ++failedConnects;

      if (failedConnects == 1 || (failedConnects % 10) == 0) {
        String host;
        uint16_t port = 0;
        IPAddress ip;

        if (hostOf(url, host, port)) {
          const bool resolved = WiFi.hostByName(host.c_str(), ip);
          applog::printf(
              "[media] станция не ответила, код %d (попыток %u), %s:%u → %s, heap %u\n",
              code,
              static_cast<unsigned>(failedConnects),
              host.c_str(),
              static_cast<unsigned>(port),
              resolved ? ip.toString().c_str() : "нет DNS",
              static_cast<unsigned>(ESP.getFreeHeap()));
        }
      }

      if (failedConnects >= 3 && totalBytes > 0) {
        break;
      }

      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }

    failedConnects = 0;

    WiFiClient* stream = http.getStreamPtr();
    uint32_t idleCycles = 0;

    while (running && stream != nullptr) {
      const int available = stream->available();

      if (available <= 0) {
        if (!stream->connected() && !net.connected()) {
          break;
        }

        if (++idleCycles > 1000) {
          break;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }

      idleCycles = 0;

      const size_t want = min(sizeof(scratch), static_cast<size_t>(available));
      const int got = stream->read(scratch, want);

      if (got > 0) {
        totalBytes += static_cast<uint32_t>(got);
        // Ждём место в буфере: источник быстрее выхода, терять байты нельзя.
        xStreamBufferSend(buffer, scratch, static_cast<size_t>(got), pdMS_TO_TICKS(1000));
      }
    }

    http.end();

    if (running) {
      applog::printf("[media] поток прервался, всего %u байт\n",
                     static_cast<unsigned>(totalBytes));
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  free(scratch);
  fetcherTask = nullptr;
  vTaskDelete(nullptr);
}

// Буферы декодера выделяются один раз и живут до перезагрузки. Выделять их
// при каждом запуске эфира нельзя: после первой сессии TLS в куче не остаётся
// цельного куска на четыре буфера, и радио молча не включалось.
struct DecoderBuffers {
  mp3dec_t* decoder = nullptr;
  uint8_t* input = nullptr;
  int16_t* pcm = nullptr;
};

DecoderBuffers decoderBuffers;

bool ensureDecoderBuffers() {
  if (decoderBuffers.decoder != nullptr) {
    return true;
  }

  DecoderBuffers fresh;
  fresh.decoder = static_cast<mp3dec_t*>(malloc(sizeof(mp3dec_t)));
  fresh.input = static_cast<uint8_t*>(malloc(kInputBytes));
  // Один буфер на кадр: кадр MP3 — не больше 1152 отсчётов на канал, то есть
  // 2304 значения в чередовании. Отдельный буфер под стерео не нужен: моно
  // раскладывается в нём же, с конца.
  fresh.pcm = static_cast<int16_t*>(malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t)));

  if (fresh.decoder == nullptr || fresh.input == nullptr || fresh.pcm == nullptr) {
    applog::printf("[media] нет памяти под декодер: свободно %u, цельным куском %u\n",
                   static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                   static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    free(fresh.decoder);
    free(fresh.input);
    free(fresh.pcm);

    return false;
  }

  decoderBuffers = fresh;

  return true;
}

/// Декодер: берёт байты из буфера и отдаёт PCM в I2S.
void decoderEntry(void*) {
  mp3dec_t* decoder = decoderBuffers.decoder;
  uint8_t* input = decoderBuffers.input;
  int16_t* pcm = decoderBuffers.pcm;

  mp3dec_init(decoder);

  size_t inputLen = 0;
  uint32_t decodedFrames = 0;
  bool announced = false;

  while (running) {
    if (paused) {
      if (announced) {
        // Гасим эфир за 40 мс: без этого драйвер отключался на середине волны,
        // и перед ответом был слышен щелчок с обрывком мелодии.
        audioPipeline.fadeOutMedia(40);
        audioPipeline.leaveMediaMode();
        announced = false;
      }

      // Эфир идёт прямо сейчас: пока колонка говорит, буфер не копим, иначе
      // после ответа зазвучало бы то, что было минуту назад. Сливаем чтением,
      // а не сбросом: сброс из этой задачи дрался с записью из сетевой и валил
      // плату.
      while (xStreamBufferBytesAvailable(buffer) > 0) {
        if (xStreamBufferReceive(buffer, input, kInputBytes, 0) == 0) {
          break;
        }
      }

      inputLen = 0;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    if (inputLen < kInputBytes) {
      // Короткое ожидание: остановку эфира вызывает сетевая задача (пришла речь),
      // и она не должна ждать дольше одного кадра.
      const size_t got = xStreamBufferReceive(buffer, input + inputLen, kInputBytes - inputLen,
                                              pdMS_TO_TICKS(20));
      inputLen += got;
    }

    if (inputLen == 0) {
      continue;
    }

    // До первого кадра копим запас: источник приходит рывками, а I2S отдаёт
    // звук строго в реальном времени.
    if (!announced && inputLen < kPrebufferBytes) {
      continue;
    }

    mp3dec_frame_info_t info = {};
    const int samples = mp3dec_decode_frame(decoder, input, inputLen, pcm, &info);

    if (info.frame_bytes == 0) {
      // Синхронизации нет: в потоке мусор (ID3, реклама) — ищем кадр заново.
      if (inputLen > 64) {
        memmove(input, input + 1, inputLen - 1);
        --inputLen;
      }

      continue;
    }

    if (samples > 0 && info.hz > 0) {
      if (!announced) {
        if (!audioPipeline.enterMediaMode(static_cast<uint32_t>(info.hz))) {
          running = false;
          break;
        }

        applog::printf("[media] эфир %u Гц, %s\n", static_cast<unsigned>(info.hz),
                       info.channels == 1 ? "моно" : "стерео");
        announced = true;
      } else {
        audioPipeline.enterMediaMode(static_cast<uint32_t>(info.hz));
      }

      if (info.channels == 1) {
        // Разложение в том же буфере, с конца: каждый отсчёт переезжает в
        // позицию вдвое дальше, поэтому ещё не прочитанное не затирается.
        for (int i = samples - 1; i >= 0; --i) {
          pcm[i * 2] = pcm[i];
          pcm[i * 2 + 1] = pcm[i];
        }
      }

      audioPipeline.writeMediaPcm(pcm, static_cast<size_t>(samples));

      ++decodedFrames;

      if ((decodedFrames % 1500) == 0) {
        applog::printf("[media] запас стека %u байт, heap %u\n",
                       static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                       static_cast<unsigned>(ESP.getFreeHeap()));
      }
    }

    memmove(input, input + info.frame_bytes, inputLen - info.frame_bytes);
    inputLen -= info.frame_bytes;
  }

  audioPipeline.leaveMediaMode();

  applog::printf("[media] эфир остановлен: кадров %u, потеряно байт %u\n",
                 static_cast<unsigned>(decodedFrames),
                 static_cast<unsigned>(droppedBytes));

  // Буферы остаются в куче до перезагрузки: их выделение после первой сессии
  // TLS уже не находит цельного куска, а так эфир запускается со второй попытки.
  running = false;
  decoderTask = nullptr;
  vTaskDelete(nullptr);
}

bool start(const String& url, const String& title, bool isRelay) {
  if (stateLock == nullptr) {
    stateLock = xSemaphoreCreateMutex();
  }

  // Память считаем до создания буфера: иначе его 12 КБ уже съедены, и проверка
  // видит остаток, которого не хватает на задачи. Замерено по логу: буферы
  // декодера выделены заранее, здесь нужны буфер потока и две задачи.
  const uint32_t need = static_cast<uint32_t>(kDecoderStack)
                        + (buffer == nullptr ? static_cast<uint32_t>(kStreamBytes) : 0u)
                        + (isRelay ? 0u : static_cast<uint32_t>(kFetcherStack));

  if (ESP.getFreeHeap() < need + 8000) {
    // Лучше честно промолчать и сказать об этом, чем упасть на выделении
    // памяти посреди ответа.
    applog::printf("[media] мало памяти под эфир: нужно %u, свободно %u — не запускаю\n",
                   static_cast<unsigned>(need),
                   static_cast<unsigned>(ESP.getFreeHeap()));

    return false;
  }

  if (buffer == nullptr) {
    // 10 КБ — две трети секунды эфира: этого хватает, чтобы пережить паузу
    // Wi-Fi, и всё ещё влезает в память вместе с открытым TLS-каналом.
    buffer = xStreamBufferCreate(kStreamBytes, 1);

    if (buffer == nullptr) {
      applog::line("[media] нет буфера под поток");
      return false;
    }
  }

  if (running) {
    stop();
  }

  xStreamBufferReset(buffer);

  // Декодеру нужны четыре буфера: сам он их выделяет только один раз, до
  // первой сессии TLS, иначе цельного куска в куче уже не находится.
  if (!ensureDecoderBuffers()) {
    return false;
  }

  relay = isRelay;
  paused = false;
  remember(url, title);
  droppedBytes = 0;
  running = true;

  if (xTaskCreatePinnedToCore(decoderEntry, "media-dec", kDecoderStack, nullptr, kTaskPriority,
                              &decoderTask, 0) != pdPASS) {
    running = false;
    decoderTask = nullptr;
    applog::line("[media] не удалось запустить декодер");

    return false;
  }

  if (!isRelay) {
    if (xTaskCreatePinnedToCore(fetcherEntry, "media-net", kFetcherStack, nullptr, kTaskPriority,
                                &fetcherTask, 0) != pdPASS) {
      running = false;
      applog::line("[media] не удалось запустить загрузчик");

      return false;
    }
  }

  return true;
}

}  // namespace

void prepare() {
  if (ensureDecoderBuffers()) {
    applog::printf("[media] память под эфир готова, свободно %u\n",
                   static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  }
}

bool play(const String& url, const String& title) {
  if (url.isEmpty()) {
    return false;
  }

  applog::printf("[media] открываю поток %s\n", url.c_str());

  return start(url, title, false);
}

bool beginRelay(const String& title) {
  applog::printf("[media] эфир от сервера: %s\n", title.c_str());

  return start("", title, true);
}

void feed(const uint8_t* data, size_t length) {
  if (!running || !relay || buffer == nullptr || data == nullptr || length == 0) {
    return;
  }

  // Не ждём: лучше потерять кусок, чем заблокировать сетевую задачу. Потери
  // видны в строке остановки — по ним же понятно, хватает ли буфера.
  const size_t written = xStreamBufferSend(buffer, data, length, 0);

  receivedBytes += static_cast<uint32_t>(written);

  if (written < length) {
    droppedBytes += static_cast<uint32_t>(length - written);
  }
}

void stop() {
  if (!running) {
    return;
  }

  paused = false;
  running = false;

  const uint32_t startedAt = millis();

  while ((decoderTask != nullptr || fetcherTask != nullptr) && millis() - startedAt < 4000) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  const TaskHandle_t zombieDecoder = decoderTask;
  const TaskHandle_t zombieFetcher = fetcherTask;

  decoderTask = nullptr;
  fetcherTask = nullptr;

  if (zombieDecoder != nullptr) {
    vTaskDelete(zombieDecoder);
  }

  if (zombieFetcher != nullptr) {
    vTaskDelete(zombieFetcher);
  }

  audioPipeline.leaveMediaMode();
}

bool active() { return running; }

bool holdsOutput() { return running; }

uint32_t receivedBytesTotal() { return receivedBytes; }

size_t bufferedBytes() {
  return buffer == nullptr ? 0 : xStreamBufferBytesAvailable(buffer);
}

String title() {
  if (stateLock != nullptr) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
  }

  const String value = currentTitle;

  if (stateLock != nullptr) {
    xSemaphoreGive(stateLock);
  }

  return value;
}

void pauseForSpeech() {
  if (!running) {
    return;
  }

  if (relay) {
    paused = true;

    return;
  }

  if (stateLock != nullptr) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
  }

  savedUrl = currentUrl;
  savedTitle = currentTitle;

  if (stateLock != nullptr) {
    xSemaphoreGive(stateLock);
  }

  stop();
}

void resumeAfterSpeech() {
  // Эфир от сервера сам не размораживается: его вернут новой командой
  // media.stream, и задача декодера начнётся с чистого порта. Попытка войти в
  // эфирный режим сразу после речи дралась с задачей речи за I2S и вешала
  // декодер — буфер забивался, звука не было.
  if (relay) {
    return;
  }

  if (stateLock != nullptr) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
  }

  const String url = savedUrl;
  const String title = savedTitle;
  savedUrl = "";
  savedTitle = "";

  if (stateLock != nullptr) {
    xSemaphoreGive(stateLock);
  }

  // Поток от сервера сам не вернётся: его снова пришлют кадрами MEDIA.
  if (url.isEmpty()) {
    return;
  }

  applog::printf("[media] возвращаю эфир «%s»\n", title.c_str());
  play(url, title);
}

}  // namespace media
