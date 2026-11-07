#pragma once

#include <Arduino.h>

// Ключ устройства и расшифровка кадра привязки.
//
// Схема (подробно — docs/07-pairing-security.md):
//   1. при первом включении колонка создаёт пару P-256 и хранит её в NVS;
//   2. сайт читает открытый ключ по BLE и передаёт его серверу в pair/claim;
//      сервер сверяет его с уже закреплённым отпечатком, а на первой привязке
//      закрепляет отпечаток только после того, как колонка подключится по WSS
//      и предъявит тот же ключ в HELLO (см. поле pub). Поэтому подставное
//      BLE-устройство не может ни прочитать настройки, ни занять устройство
//      навсегда: настоящая колонка ключом не совпадёт и от привязки откажется;
//   3. сайт генерирует свою эфемерную пару, считает ECDH, выводит ключ через
//      HKDF и шифрует настройки AES-256-GCM;
//   4. колонка расшифровывает, применяет Wi-Fi и одноразовый токен привязки.
//
// Аутентичность «кто именно говорит с нами» держится не на BLE-спаривании
// (Just Works подделать может любой в радиусе), а на серверном токене привязки:
// его выдаёт сервер по коду с наклейки, и без него колонка не примет настройки.
class PairingCrypto {
 public:
  // Создаёт пару при первом запуске, иначе поднимает сохранённую.
  bool begin();

  // Открытый ключ в несжатом виде: 0x04 || X || Y, 65 байт.
  const uint8_t* publicKey() const { return _publicKey; }
  static constexpr size_t PUBLIC_KEY_SIZE = 65;

  // Тот же ключ в hex (130 символов). Уходит в HELLO как поле pub: сервер
  // сверяет его с отпечатком из записи привязки до выдачи deviceKey.
  String publicKeyHex() const;

  // Код с наклейки, если он прошит в устройство (NVS spk_id/code или
  // SPEAKER_PAIR_CODE). Пока его нет, кадр привязки идёт версией 1, и пароль
  // Wi-Fi защищён только ECDH — его прочитает подставное BLE-устройство.
  bool hasPairCode() const { return _pairCode.length() > 0; }

  // Какую версию кадра принимает устройство. Сайт читает её из BLE info и
  // выбирает тот же вывод ключа; кадр другой версии устройство отвергает.
  uint8_t requiredFrameVersion() const { return hasPairCode() ? 2 : 1; }

  // Расшифровывает кадр привязки.
  //   version — версия кадра (1 — без кода, 2 — с кодом с наклейки),
  //   peerPublic — 65 байт, nonce — 12, ciphertext — данные с тегом в конце (16).
  // Результат — JSON в out (строка с завершающим нулём).
  // Проверка крипто-пути на старте: шифрует так же, как это сделает сайт, и
  // расшифровывает тем же кодом, что принимает кадр привязки. Без неё первая
  // же неудачная привязка на железе означала бы отладку вслепую.
  bool selfTest();

  bool decrypt(
      uint8_t version,
      const uint8_t* peerPublic,
      const uint8_t* nonce,
      const uint8_t* ciphertext,
      size_t ciphertextLength,
      char* out,
      size_t outCapacity);

  bool verifyServerGrant(
      const String& deviceId,
      const String& publicKeySha256,
      const String& challenge,
      const String& userId,
      const String& pairToken,
      const String& apiBase,
      const String& signatureBase64);

 private:
  bool loadKeypair();
  bool createKeypair();
  void loadPairCode();

  uint8_t _privateKey[32] = {0};
  uint8_t _publicKey[PUBLIC_KEY_SIZE] = {0};
  String _pairCode;
  bool _ready = false;
};
