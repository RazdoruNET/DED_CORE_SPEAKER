#include "net/BlePairing.h"
#include "config/AppConfig.h"

#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_random.h>
#include <string.h>

#include "net/DeviceConfig.h"

BlePairing blePairing;

namespace {

// Случайные, но стабильные UUID: по ним сайт и приложение находят сервис.
constexpr const char* kServiceUuid = "6b1f0001-9a3c-4e6d-8b1a-2c5d7e9f0a11";
constexpr const char* kInfoUuid    = "6b1f0002-9a3c-4e6d-8b1a-2c5d7e9f0a11";
constexpr const char* kPairUuid    = "6b1f0003-9a3c-4e6d-8b1a-2c5d7e9f0a11";
constexpr const char* kStatusUuid  = "6b1f0004-9a3c-4e6d-8b1a-2c5d7e9f0a11";
// Список сетей: браузер не умеет перечислять Wi-Fi, поэтому его отдаёт колонка.
constexpr const char* kWifiUuid    = "6b1f0005-9a3c-4e6d-8b1a-2c5d7e9f0a11";

// Окно привязки: пять минут с момента включения. Дольше — колонка становится
// видимой для чужих телефонов без всякой пользы.
constexpr uint32_t kPairingWindowMs = 5 * 60 * 1000UL;

constexpr size_t kFrameHeader = 4;  // u16 total + u16 offset
constexpr size_t kFramePrefix = 1 + PairingCrypto::PUBLIC_KEY_SIZE + 12;  // ver + ключ + nonce
constexpr size_t kTagSize = 16;                                           // тег AES-GCM
constexpr size_t kChallengeSize = 16;

BlePairing* self = nullptr;
NimBLECharacteristic* statusCharacteristic = nullptr;
NimBLECharacteristic* infoCharacteristic = nullptr;
NimBLECharacteristic* wifiCharacteristic = nullptr;

String toHex(const uint8_t* data, size_t length) {
  static const char* digits = "0123456789abcdef";
  String out;
  out.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 0x0F];
  }
  return out;
}

class PairWriteCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic) override {
    if (self == nullptr) {
      return;
    }
    std::string value = characteristic->getValue();
    if (value.empty()) {
      return;
    }
    self->handleChunk(reinterpret_cast<const uint8_t*>(value.data()), value.size());
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*) override {
    applog::line("[ble] подключился клиент настройки");
  }

  void onDisconnect(NimBLEServer*) override {
    applog::line("[ble] клиент настройки отключился");
    // Окно закрывается только по таймауту или после успешной привязки:
    // обрыв связи посреди ввода — обычное дело.
  }
};

}  // namespace

bool BlePairing::begin(PairingCrypto* crypto, const String& deviceId, PairedHandler onPaired) {
  if (_active) {
    return true;
  }
  if (crypto == nullptr || !crypto->begin()) {
    applog::line("[ble] нет ключа устройства — привязка невозможна");
    return false;
  }

  _crypto = crypto;
  _deviceId = deviceId;
  _onPaired = std::move(onPaired);
  self = this;

  NimBLEDevice::init(std::string("DED-SPEAKER-") + deviceId.c_str());
  NimBLEDevice::setMTU(247);
  // Аутентичность даёт не BLE-спаривание (Just Works подделывается), а
  // серверный токен привязки внутри зашифрованного кадра. Поэтому BLE-защита
  // не требуется и не запрашивается — это убирает половину проблем совместимости
  // с браузерами.
  NimBLEDevice::setSecurityAuth(false, false, false);

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* service = server->createService(kServiceUuid);

  infoCharacteristic = service->createCharacteristic(
      kInfoUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  // 512 байт: открытый ключ в hex — 130 символов, плюс id, challenge и версии.
  DynamicJsonDocument info(512);
  info["id"] = deviceId;
  info["product"] = SPEAKER_PRODUCT;
  info["fw"] = SPEAKER_FIRMWARE_VERSION;
  info["profile"] = SPEAKER_PROFILE_NAME;
  info["pub"] = toHex(crypto->publicKey(), PairingCrypto::PUBLIC_KEY_SIZE);
  info["paired"] = deviceConfig.isPaired();
  // Какую версию кадра принимает устройство: 2 — Wi-Fi пароль шифруется с
  // участием кода с наклейки, 1 — только ECDH. Сайт обязан использовать ту же.
  info["frame"] = crypto->requiredFrameVersion();
  String infoJson;
  serializeJson(info, infoJson);
  infoCharacteristic->setValue(
      reinterpret_cast<const uint8_t*>(infoJson.c_str()), infoJson.length());

  NimBLECharacteristic* pairCharacteristic = service->createCharacteristic(
      kPairUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  pairCharacteristic->setCallbacks(new PairWriteCallbacks());

  wifiCharacteristic = service->createCharacteristic(kWifiUuid, NIMBLE_PROPERTY::READ);

  statusCharacteristic = service->createCharacteristic(
      kStatusUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  const char* waiting = "{\"stage\":\"waiting\"}";
  statusCharacteristic->setValue(reinterpret_cast<const uint8_t*>(waiting), strlen(waiting));

  service->start();

  // Сканируем один раз при открытии окна, а не по чтению: чтение характеристики
  // не должно занимать эфир на две секунды, иначе браузер отваливается по
  // таймауту. Человек в это время ещё держит кнопку.
  scanNetworks();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  // Короткое имя в основном пакете и полное — в scan response: 128-битный UUID
  // съедает 18 из 31 байта, длинное имя туда не влезает.
  const String shortName = String("DED-SPK-") + deviceId.substring(deviceId.length() - 4);
  advertising->setName(std::string(shortName.c_str()));
  advertising->addServiceUUID(kServiceUuid);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  NimBLEDevice::startAdvertising();

  _active = true;
  _busy = false;
  _startedAt = millis();
  _frameLength = 0;
  _frameExpected = 0;
  uint8_t challenge[kChallengeSize] = {0};
  esp_fill_random(challenge, sizeof(challenge));
  const String challengeHex = toHex(challenge, sizeof(challenge));
  challengeHex.toCharArray(_challenge, sizeof(_challenge));
  memset(challenge, 0, sizeof(challenge));

  // Случайный challenge уникален для этого окна привязки. Сервер подписывает
  // его вместе с deviceId и fingerprint ключа; старый grant нельзя переиграть
  // после перезагрузки или нового окна.
  info["challenge"] = _challenge;
  infoJson = "";
  serializeJson(info, infoJson);
  infoCharacteristic->setValue(
      reinterpret_cast<const uint8_t*>(infoJson.c_str()), infoJson.length());

  applog::printf("[ble] окно привязки открыто: %s\n", infoJson.c_str());
  return true;
}

void BlePairing::scanNetworks() {
  if (wifiCharacteristic == nullptr) {
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // Лимит по времени: перебор всех каналов может занять несколько секунд,
  // а окно привязки не должно зависать.
  const int count = WiFi.scanNetworks(false, false, false, 120);

  DynamicJsonDocument doc(2048);
  doc["scanning"] = false;
  JsonArray networks = doc.createNestedArray("networks");

  for (int i = 0; i < count && networks.size() < 12; ++i) {
    const String ssid = WiFi.SSID(i);

    // Скрытые сети пропускаем: выбрать их в списке всё равно нельзя.
    if (ssid.isEmpty()) {
      continue;
    }

    JsonObject entry = networks.createNestedObject();
    entry["ssid"] = ssid;
    entry["rssi"] = WiFi.RSSI(i);
    entry["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }

  WiFi.scanDelete();

  // Wi-Fi и Bluetooth делят радио на одном чипе. Если оставить Wi-Fi
  // включённым (пусть и без подключения), BLE начинает рваться: браузер
  // получает «GATT operation failed» на ровном месте. После сканирования
  // радио выключаем — привязка идёт только по Bluetooth.
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  String json;
  serializeJson(doc, json);
  wifiCharacteristic->setValue(reinterpret_cast<const uint8_t*>(json.c_str()), json.length());

  applog::printf("[ble] сетей найдено: %u\n", (unsigned)networks.size());
}

void BlePairing::stop() {
  if (!_active) {
    return;
  }
  NimBLEDevice::stopAdvertising();
  _active = false;
  _busy = false;
  _frameLength = 0;
  _frameExpected = 0;
  memset(_challenge, 0, sizeof(_challenge));
  applog::line("[ble] окно привязки закрыто");
}

void BlePairing::tick() {
  if (!_active) {
    return;
  }
  if (millis() - _startedAt > kPairingWindowMs) {
    stop();
  }
}

void BlePairing::report(const String& stage, bool ok, const String& error) {
  if (statusCharacteristic == nullptr) {
    return;
  }
  DynamicJsonDocument doc(192);
  doc["stage"] = stage;
  doc["ok"] = ok;
  if (error.length() > 0) {
    doc["error"] = error;
  }
  String json;
  serializeJson(doc, json);
  statusCharacteristic->setValue(
      reinterpret_cast<const uint8_t*>(json.c_str()), json.length());
  statusCharacteristic->notify();
}

void BlePairing::handleChunk(const uint8_t* data, size_t length) {
  if (length <= kFrameHeader) {
    return;
  }

  const uint16_t total = static_cast<uint16_t>(data[0] | (data[1] << 8));
  const uint16_t offset = static_cast<uint16_t>(data[2] | (data[3] << 8));
  const size_t chunk = length - kFrameHeader;

  if (total == 0 || total > kFrameCapacity || offset + chunk > total) {
    report("error", false, "bad_frame");
    return;
  }

  if (offset == 0) {
    _frameLength = 0;
    _frameExpected = total;
    _busy = true;
    report("receiving", true, "");
  }

  if (offset != _frameLength) {
    report("error", false, "gap");
    _busy = false;
    return;
  }

  memcpy(_frame + offset, data + kFrameHeader, chunk);
  _frameLength += chunk;

  if (_frameLength >= _frameExpected) {
    handleFrame();
  }
}

void BlePairing::handleFrame() {
  _busy = false;

  if (_frameLength < kFramePrefix + kTagSize) {
    report("error", false, "short_frame");
    return;
  }

  const uint8_t version = _frame[0];
  if (version != _crypto->requiredFrameVersion()) {
    // Устройство с кодом не принимает кадр версии 1: иначе подменяющее
    // BLE-устройство могло бы откатиться на схему без кода с наклейки.
    report("error", false, "version");
    return;
  }

  const uint8_t* peerPublic = _frame + 1;
  const uint8_t* nonce = peerPublic + PairingCrypto::PUBLIC_KEY_SIZE;
  const uint8_t* ciphertext = nonce + 12;
  const size_t cipherLength = _frameLength - kFramePrefix;

  // Запас под JSON с grant: 640 байт полезной нагрузки плюс завершающий ноль.
  char plain[900] = {0};
  if (!_crypto->decrypt(version, peerPublic, nonce, ciphertext, cipherLength,
                        plain, sizeof(plain))) {
    // Скорее всего открытый ключ в эфире не тот, что у сервера: подмена.
    report("error", false, "decrypt");
    return;
  }

  // 1024 байта: полезная нагрузка с grant — около 640 байт JSON, и при
  // меньшей ёмкости ArduinoJson возвращает NoMemory, а не разобранный документ.
  // ArduinoJson с char* разбирает на месте и хранит указатели в этот буфер:
  // затирать plain до извлечения полей нельзя — все строки станут пустыми.
  DynamicJsonDocument doc(1024);
  const DeserializationError error = deserializeJson(doc, plain);

  if (error) {
    memset(plain, 0, sizeof(plain));
    applog::printf("[ble] разбор кадра привязки: %s\n", error.c_str());
    report("error", false, "json");
    return;
  }

  const JsonObjectConst grant = doc["grant"].as<JsonObjectConst>();
  const String grantDeviceId = grant["deviceId"] | "";
  const String publicKeySha256 = grant["publicKeySha256"] | "";
  const String challenge = grant["challenge"] | "";
  const String userId = grant["userId"] | "";
  const String token = grant["pairToken"] | "";
  const String signedApi = grant["apiBase"] | "";
  const String signature = grant["signature"] | "";
  const String api = doc["api"] | "";

  if (!grant || !grantDeviceId.equalsIgnoreCase(_deviceId)
      || !challenge.equalsIgnoreCase(_challenge)
      || api != signedApi
      || !_crypto->verifyServerGrant(grantDeviceId, publicKeySha256, challenge,
                                     userId, token, signedApi, signature)) {
    report("error", false, "grant_signature");
    return;
  }

  const String ssid = doc["ssid"] | "";
  const String pass = doc["pass"] | "";
  const String name = doc["name"] | "Колонка";
  const String room = doc["room"] | "";

  // Пароль Wi-Fi больше не нужен в открытом виде: стираем буфер и документ.
  memset(plain, 0, sizeof(plain));

  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 63
      || api.length() > 192 || !api.startsWith("https://")
      || name.length() > 64 || room.length() > 64) {
    report("error", false, "incomplete");
    return;
  }

  deviceConfig.wifiSsid = ssid;
  deviceConfig.wifiPass = pass;
  deviceConfig.apiBase = api.length() > 0 ? api : deviceConfig.apiBase;
  deviceConfig.pairToken = token;
  deviceConfig.name = name;
  deviceConfig.room = room;

  deviceConfig.saveNetwork();
  deviceConfig.saveIdentity();

  report("paired", true, "");
  applog::printf("[ble] привязка принята, сеть «%s», сервер %s\n", ssid.c_str(),
                deviceConfig.apiBase.c_str());

  // Колонка уходит в перезагрузку сама: так настройка применяется целиком и
  // не остаётся полуприменённого состояния.
  delay(400);
  stop();
  if (_onPaired) {
    _onPaired(deviceConfig.room, deviceConfig.name);
  }
}
