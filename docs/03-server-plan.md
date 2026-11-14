# План серверной части

Правки в `src/AssistantService`. Ничего не переписывается — добавляется четвёртый
транспорт к уже существующему агенту (браузер, мобильная обёртка, текст).

## Что уже готово и остаётся как есть

| Механизм | Файл | Роль в колонке |
|---|---|---|
| агент | `Services/AgentRuntime.cs` | выполнение задачи по голосовой фразе |
| поток речи по фразам | `Services/Voice/LiveSpeechStream.cs`, `SpeechStreamChunker.cs` | колонка получает Opus-чанки, не дожидаясь конца ответа |
| уведомления | `Services/AssistantNotifier.cs` | `speechChunk`, `messageDone` для браузера |
| умный дом | `Services/Home/HomeContracts.cs`, `HomeDeviceService.cs` | `home.list` / `home.set` для устройств колонки |
| быстрый разбор | `Services/Intent/FastIntentMatcher.cs` | «включи свет» без модели, ~200 мс |
| аудит | `Entities/AssistantAuditEntries` | каждая команда устройству записана |
| STT | `Services/Voice/VoiceTranscriptionService.cs` | резервный путь (фраза целиком) |
| TTS | `Services/Voice/TextToSpeechService.cs` | синтез и кеш mp3 |

## Новые файлы

```
src/AssistantService/
├── Configuration/SpeakerOptions.cs          настройки: кодеки, тайминги, лимиты
├── Services/Speaker/
│   ├── SpeakerContracts.cs                  ISpeakerGateway, SpeakerSession, SpeakerFrame
│   ├── SpeakerGateway.cs                    бинарный WS: кадры, heartbeat, авторизация
│   ├── SpeakerRegistry.cs                   кто в сети, у кого какой владелец звука
│   ├── SpeakerOpusCache.cs                  mp3 → opus (ffmpeg), кеш по хешу
│   ├── SpeakerHomeProvider.cs               IHomeDeviceProvider, ключ "speaker"
│   └── SpeakerSessionBridge.cs              связка «сессия сайта ↔ колонка»
├── Services/Voice/
│   └── StreamingSpeechRecognizer.cs         стриминговый STT (sherpa-onnx)
├── Services/Speaker/SpeakerPairingService.cs  claim → token → deviceKey (одноразово)
├── Services/Speaker/SpeakerGrantSigner.cs     ECDSA-SHA256 подпись BLE challenge grant
├── Services/Tools/SpeakerTools.cs           speaker.say, speaker.listen, speaker.intercom
├── Controllers/SpeakerController.cs         REST: список, привязка, интерком, fallback
└── Entities/SpeakerPairing.cs + миграция    deviceId, codeHash, userId, tokenHash, expiresAt
```

## Изменения в существующих файлах

### `Program.cs`

```csharp
builder.Services.Configure<SpeakerOptions>(builder.Configuration.GetSection("Assistant:Speaker"));
builder.Services.AddSingleton<ISpeakerRegistry, SpeakerRegistry>();
builder.Services.AddSingleton<ISpeakerGateway, SpeakerGateway>();
builder.Services.AddSingleton<IOpusCache, SpeakerOpusCache>();
builder.Services.AddSingleton<IStreamingSpeechRecognizer, StreamingSpeechRecognizer>();
builder.Services.AddScoped<ISpeakerSessionBridge, SpeakerSessionBridge>();

// рядом с существующей регистрацией UnconfiguredHomeDeviceProvider (строка ~165)
builder.Services.AddSingleton<IHomeDeviceProvider, SpeakerHomeProvider>();

// рядом с остальными инструментами (строки ~172–185)
builder.Services.AddScoped<IAssistantTool, SpeakerSayTool>();
builder.Services.AddScoped<IAssistantTool, SpeakerListenTool>();
builder.Services.AddScoped<IAssistantTool, SpeakerIntercomTool>();
builder.Services.AddScoped<IAssistantTool, SpeakerVolumeTool>();
```

`app.UseWebSockets()` уже вызывается (строка ~288), отдельный маршрут добавляется
после `MapHub`:

```csharp
app.Map("/api/assistant/speaker/stream", SpeakerGateway.HandleAsync);
```

Маршрут намеренно не проходит через SignalR и не требует JWT пользователя:
авторизация — ключ устройства, по нему же находится владелец.

### `AssistantNotifier.cs` / `AgentRuntime.cs`

Появляется второй получатель речи. Вместо дублирования кода — `IAssistantNotifier`
получает декоратор: если у пользователя привязана колонка, `speechChunk`
уходит ещё и на неё (кадры `SAY`), а браузер в этот момент глушит звук сам.

```csharp
sealed class SpeakerMirrorNotifier : IAssistantNotifier
{
    // вызывает inner и, если SpeakerRegistry.HasAttached(userId), отдаёт чанк в колонку
}
```

Это единственное место, где проходит вся речь, поэтому и дублирование, и
будущий интерком живут в одном месте.

**Как сделано на самом деле (0.2.4).** Декоратор не понадобился: куски речи
уходят из `LiveSpeechStream` — там же, где событие `speechChunk`, — в
`ISpeakerVoice` для каждой колонки пользователя. Браузер глушится не «сам», а по
заголовку `X-Speaker-Audio: 1` в ответе `/api/assistant/voice/speak`. Причины:

* `SpeechChunkEvent` не знает голоса, а голос нужен для синтеза на устройстве:
  в `LiveSpeechStream` он приходит из снимка задачи (`AssistantUserContext.Voice`);
* очередь реплик живёт в `SpeakerVoice`, и повторный синтез от браузера
  (`voice/speak`) не должен читать ту же фразу второй раз — сервер помнит
  отданные куски минуту.

Полное описание поведения — в `docs/05-firmware.md`, раздел про голос.

### `AssistantUserProfile` / `AssistantClientContext`

Новое поле — куда выводить голос:

```
AssistantUserProfile.OutputDevice  (string?, "auto" | "speaker" | "browser", null = auto)
```

Миграция `AssistantOutputDevice` — одна колонка, `null` по умолчанию, совместима
с остальными клиентами. В снимок задачи поле попадает как
`AssistantUserContext.OutputDevice`: фоновая задача решает, отдавать ли ответ
колонке, уже после того, как человек мог переключить вывод.

Переключатель в интерфейсе — кнопка `fa-tower-broadcast` / `fa-computer` в шапке
панели помощника; она показывается только когда у аккаунта есть колонка.
Музыка и видео пока идут в браузер: для них нужен медиапоток, которого в
прошивке нет.

### `HomeDeviceService`

Ничего. `SpeakerHomeProvider` отдаёт колонку и её датчики как обычные устройства:
`id = speaker:{deviceId}:volume|led|mic|temp|hum|lux|presence`.

## REST-ручки

| Метод | Путь | Назначение |
|---|---|---|
| POST | `/api/assistant/speaker/pair/claim` | код с наклейки → одноразовый `pairToken` (10 мин) |
| POST | `/api/assistant/speaker/{id}/unbind` | отвязать (владелец, сразу) |
| POST | `/api/assistant/speaker/{id}/report-stolen` | блокировка устройства |
| GET | `/api/assistant/speaker/devices` | колонки пользователя: имя, комната, online, прошивка |
| POST | `/api/assistant/speaker/{id}/attach` | сделать колонку выводом звука (`OutputDeviceId`) |
| DELETE | `/api/assistant/speaker/{id}/attach` | отвязать вывод |
| POST | `/api/assistant/speaker/{id}/say` | сказать текст (`{text, voice?}`) |
| POST | `/api/assistant/speaker/{id}/intercom` | начать/завершить вызов |
| GET | `/api/assistant/speaker/{id}/rules` | локальные сценарии для автономной работы |
| POST | `/api/assistant/speaker/{id}/utterance` | резервный путь: Opus-фраза → ответ (фаза 1) |
| GET | `/api/assistant/speaker/{id}/speech/{streamId}/{n}` | готовый Opus-чанк |

Все, кроме `stream`, закрыты `[Authorize]` и проверяют владельца устройства —
как `HomeController` сейчас.

## Первая привязка

Подробно — [`07-pairing-security.md`](./07-pairing-security.md). На сервере это
три вещи:

1. `SpeakerPairingService.ClaimAsync(userId, code)` — находит устройство по хешу
   кода, проверяет, что оно свободно, выдаёт одноразовый токен и **уведомляет
   текущего владельца**, если устройство уже чьё-то;
2. хранение только хешей (`codeHash`, `tokenHash`) — утечка базы не даёт
   привязать чужую колонку;
3. при `HELLO` с токеном — сверка открытого ключа устройства с отпечатком,
   для которого выпущен токен, и атомарный обмен на `deviceKey`:
   `UPDATE speaker_pairings SET used_at = now() WHERE token_hash = ? AND used_at IS NULL`,
   и только при одной затронутой строке выдаётся ключ. Здесь же, а не в момент
   `claim`, закрепляется `SpeakerDevices.PublicKeySha256`: до подключения
   колонки нельзя верить, что ключ из эфира принадлежит ей.

`SpeakerGrantSigner` подписывает каноническую строку из
[`07-pairing-security.md`](./07-pairing-security.md), кодировкой UTF-8,
`ECDsa.SignData(..., HashAlgorithmName.SHA256, DSASignatureFormat.Rfc3279DerSequence)`.
Закрытый ключ хранится в Vault; публичный сертификат закрепляется в прошивке
как `SPEAKER_PAIRING_PUBLIC_KEY_HEX`. Подписываются device id, SHA-256 ключа
устройства, случайный BLE challenge, user id, одноразовый токен и точный HTTPS
base URL. Срок жизни токена проверяется сервером; challenge не позволяет
переиграть старый grant в новом окне BLE.

Лимиты: 5 попыток в час на пользователя, 10 в час на устройство, 20 в час на
IP (после `UseForwardedHeaders`, иначе за шлюзом это один общий счётчик).
Подключения к `/api/assistant/speaker/stream`: не больше 256 одновременно,
8 с одного адреса и 120 неудачных попыток HELLO в час с адреса (`Assistant:Speaker`).
Успешные подключения не считаются: перезапуск Wi-Fi не должен упираться в лимит.
Каждая попытка привязки — в `AssistantAuditEntries`.

## Авторизация устройства

Клиентская прошивка принимает только HTTPS/WSS и требует проверку сертификата
доверенным CA. `setInsecure()` запрещён; незаданный CA должен завершать
подключение отказом. На колонке доверенный CA задаётся локально в
`firmware/src/config/Secrets.h` по примеру `Secrets.example.h`; не добавлять
этот файл в Git.

Ключ устройства уже есть в `FirmwareService` (`Device.DeviceKey`, миграция
`20261011173535_AddDeviceKeyToDevice`). Рукопожатие:

1. колонка присылает device key или одноразовый pairing token в первом бинарном
   кадре `HELLO` после TLS-рукопожатия;
2. `SpeakerGateway` держит `HttpClient` к внутренней ручке FirmwareService
   (по образцу других внутренних вызовов, с `InternalApiSecret`);
3. ответ — `{deviceId, userId, product, firmwareVersion}`;
4. `product != "smart-speaker"` → `4401`.

Ключ не ходит ни в query, ни в HTTP-заголовках, где его легче оставить в логах.
До проверки `HELLO` сервер держит соединение в ограниченном состоянии: таймаут
5 секунд, максимальный кадр 512 байт, никаких команд устройству и никакой
регистрации в реестре до успешной аутентификации.

## Отсев «услышанного»: почему радио включалось само

Whisper обучен на субтитрах, поэтому на музыке и шуме он выдаёт титры:
«Редактор субтитров О.Голубкина», «Продолжение следует» — они и раньше сыпались
в журнал. Опаснее другое: подсказку распознавания движок возвращает дословно.

Так и произошло 6 ноября: в комнате играл фильм, подсказка начиналась с
«Лира, включи радио, открой спидометр…», и модель отдала «Джарвис, Лира,
включи радио» — с обращением, то есть настоящей командой. Радио включилось само.

Теперь подсказка состоит только из существительных: команд в ней нет по
построению. Плюс два отсева в `SpeakerPhraseQuality`:

* `IsArtefact` — `no_speech_prob` из подробного ответа (`response_format=verbose_json`)
  выше 0.5 значит, что модель речи не слышала; отдельно отсекаются титры и
  «динамичная музыка»;
* `EchoesPrompt` — три слова подряд из подсказки или фраза целиком из её слов.

Настоящая команда («Джарвис, включи радио») содержит глагол, которого в подсказке
нет, поэтому под отсев не попадает.

## Эфир на колонку: почему его тянет сервер

Колонка декодирует MP3 сама, но поток берёт не она. Причина простая: при живом
TLS-канале к платформе на второй сокет не остаётся непрерывной памяти. В логе
это видно прямо — `станция не ответила, код -7, heap 3304`, и сразу перезагрузка:
человек слышал «включил радио» и тишину.

Поэтому станцию читает `SpeakerMediaRelay.StreamRadioAsync` и отдаёт её
устройству кадрами `MEDIA` по уже открытому каналу, по 1 КБ (64 мс звука).
Темп не подгоняется: живой поток и так идёт со скоростью звука, а искусственная
задержка оставляла буфер платы пустым и эфир дёргался.

Речь важнее эфира: перед `SPEECH_BEGIN` ретранслятор запоминает станцию и
отменяет поток, после `SPEECH_END` присылает её заново. Плата не пытается
«войти обратно» сама: попытка двух задач дёргать один порт I2S вешала декодер —
буфер забивался, звука не было. Явный «стоп» забывает станцию.

Громкость эфира — `MEDIA_GAIN_PERCENT` от речи (50%), низ поднят полкой +8 дБ
ниже 120 Гц: у динамика в корпусе своего низа нет.

## Токен человека в задачах из колонки

Колонка аутентифицируется ключом устройства и токена пользователя не имеет. Но
инструменты платформы (`profiles.story`, переписка, клан) работают только по
нему — без токена они отвечают «нужно войти в аккаунт», и на вопрос «кто такой
дед» сайт рассказывает, а колонка извиняется.

Поэтому `UserTokenCache` запоминает последний Bearer, пришедший с сайта (ставится
в `OnTokenValidated`), и задача из колонки идёт с ним. Живёт запись до `exp`
токена. Чтобы память не остывала, вкладка раз в пять минут делает
`POST /api/assistant/session`; интерцептор сам продлит access-куку по 401.

Токен не пишется в базу и не уходит на устройство — только в памяти сервиса.

## Стриминговый STT — самый важный пункт

Сейчас `VoiceTranscriptionService` отправляет файл и ждёт весь текст: ~3 с на
фразу. Для колонки это неприемлемо.

План: `StreamingSpeechRecognizer` на sherpa-onnx (zipformer-russian или
`gigaam`-экспорт), работает как отдельный контейнер рядом с `whisper`:

* принимает Opus-кадры по WS/gRPC, отдаёт партиалы каждые ~150 мс;
* финальный текст — по концу фразы (VAD уже на устройстве);
* `whisper` остаётся: второй проход для длинных и шумных фраз, а также там, где
  стриминговый движок не уверен (низкая уверенность → пересегментировать);
* при недоступности стримингового движка колонка деградирует к текущему пути
  без изменений кода — просто `StreamingSpeechRecognizer.IsConfigured == false`.

Оптимизация, которая даёт ещё ~300 мс: запуск агента по стабильному партиалу.
Если два партиала подряд совпали и фраза не растёт — начинаем разбор
(`FastIntentMatcher`) заранее; при расхождении результат отбрасывается.

## Настройки (`appsettings.json`, секция `Assistant:Speaker`)

| Ключ | По умолчанию | Смысл |
|---|---|---|
| `Enabled` | `false` | колонки включены |
| `Codec` | `opus` | `opus` или `pcm16` (отладка) |
| `SampleRate` | `16000` | частота речи |
| `FrameMs` | `20` | кадр |
| `UpBitrate` | `24000` | битрейт вверх |
| `DownBitrate` | `24000` | битрейт вниз |
| `JitterFrames` | `3` | буфер вниз |
| `HeartbeatSeconds` | `15` | ping |
| `MaxDeviceSeconds` | `30` | сколько колонка ждёт ответа до «не расслышала» |
| `IdleSleepSeconds` | `20` | через сколько отпускает микрофон |
| `OfflineCacheUrl` | `/voice/offline` | фразы для автономного режима |

## Тесты

В `tests/` уже есть тесты помощника. Добавляются:

* кодек кадра: сборка/разбор заголовка, устойчивость к обрезанному кадру;
* `SpeakerRegistry`: два пользователя, чужая колонка не видна;
* `SpeakerHomeProvider`: `home.list` показывает колонку и датчики, `home.set volume`
  доходит до `CMD`, `home.set` без онлайна возвращает понятную ошибку;
* `SpeakerMirrorNotifier`: при привязанной колонке чанк уходит и в хаб, и в
  устройство, а при отвязанной — только в хаб;
* агент: «скажи в гостиную, что обед готов» выбирает `speaker.say` с нужным id.

## Порядок внедрения

1. `SpeakerGateway` + `HELLO` + `STATE` + `say` (без микрофона) — проверяется
   колонкой сразу, риск минимальный.
2. Резервный HTTP-путь `utterance` — гоняет весь тракт STT→агент→TTS.
3. Стриминговый STT + `MIC`/`VAD` — доходит до целевой задержки.
4. `SpeakerHomeProvider`, датчики, `rules.update`.
5. Сессия сайта (`attach`) и интерком.
