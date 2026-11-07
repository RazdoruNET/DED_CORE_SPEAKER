#include "net/PairingCrypto.h"

#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/sha256.h>
#include <string.h>

#include "config/AppConfig.h"

namespace {

constexpr const char* kNvsNamespace = "spk_id";
constexpr const char* kHkdfInfo = "ded-speaker-pair-v1";
constexpr const char* kHkdfInfoV2 = "ded-speaker-pair-v2";
constexpr size_t kNonceSize = 12;
constexpr size_t kTagSize = 16;
constexpr size_t kKeySize = 32;
constexpr size_t PUBLIC_KEY_POINT_SIZE = 65;
constexpr const char* kGrantDomain = "DED-CORE-SPEAKER-PAIR-V1\n";


// HKDF-SHA256. В предсборке mbedTLS от Arduino-ядра mbedtls_hkdf отсутствует,
// поэтому extract+expand собраны на HMAC-SHA256 — это те же 15 строк спецификации
// RFC 5869, зато без своей сборки библиотеки.
bool hkdfSha256(const uint8_t* salt, size_t saltLength,
                const uint8_t* ikm, size_t ikmLength,
                const uint8_t* info, size_t infoLength,
                uint8_t* out, size_t outLength) {
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md == nullptr) {
    return false;
  }

  // Info — не только константа: в кадре версии 2 в неё входит код с наклейки.
  // Слишком длинный info отвергаем, а не обрезаем: тихое усечение дало бы
  // разные ключи на двух сторонах при одинаковом коде.
  constexpr size_t kMaxInfo = 96;
  if (infoLength > kMaxInfo) {
    return false;
  }

  uint8_t prk[32] = {0};
  if (mbedtls_md_hmac(md, salt, saltLength, ikm, ikmLength, prk) != 0) {
    return false;
  }

  uint8_t block[32] = {0};
  size_t blockLength = 0;
  size_t written = 0;
  uint8_t counter = 1;
  uint8_t input[32 + kMaxInfo + 1] = {0};

  while (written < outLength) {
    size_t cursor = 0;
    memcpy(input, block, blockLength);
    cursor += blockLength;
    memcpy(input + cursor, info, infoLength);
    cursor += infoLength;
    input[cursor++] = counter++;

    if (mbedtls_md_hmac(md, prk, sizeof(prk), input, cursor, block) != 0) {
      return false;
    }
    blockLength = sizeof(block);

    const size_t take = (outLength - written) < blockLength ? (outLength - written) : blockLength;
    memcpy(out + written, block, take);
    written += take;
  }

  return true;
}

// Код с наклейки приводится к одной форме и на устройстве, и на сайте:
// заглавные буквы, только A-Z и 0-9. Иначе один и тот же код даст разные ключи
// на двух сторонах, и привязка будет отвергнута без внятной причины.
String normalizePairCode(const String& raw) {
  String out;
  out.reserve(raw.length());

  for (size_t i = 0; i < raw.length(); ++i) {
    char c = raw[i];

    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }

    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      out += c;
    }
  }

  return out;
}

// Метка HKDF зависит от версии кадра: в версии 2 в неё входит код с наклейки.
// Подставное BLE-устройство кода не знает, выведет другой ключ и не прочитает
// настройки Wi-Fi даже при подмене открытого ключа в эфире.
void buildHkdfInfo(const String& pairCode, uint8_t version, String& out) {
  out = version >= 2 ? kHkdfInfoV2 : kHkdfInfo;

  if (version >= 2) {
    out += '\n';
    out += pairCode;
  }
}

// Проверка подписи по закреплённому ключу.
//
// Ключ хранится как несжатая точка (65 байт в hex), а не как PEM: разбор PEM
// в предсборке mbedtls может быть выключен, и тогда проверка отказывала бы
// всегда — то есть привязка была бы невозможна без внятной причины.
bool verifySignature(const uint8_t* message, size_t messageLength,
                     const uint8_t* signature, size_t signatureLength) {
  const char* hex = SPEAKER_PAIRING_PUBLIC_KEY_HEX;
  const size_t hexLength = strlen(hex);

  if (hexLength != 2 * PUBLIC_KEY_POINT_SIZE) {
    return false;
  }

  uint8_t point[PUBLIC_KEY_POINT_SIZE] = {0};
  for (size_t i = 0; i < PUBLIC_KEY_POINT_SIZE; ++i) {
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };

    const int high = nibble(hex[i * 2]);
    const int low = nibble(hex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    point[i] = static_cast<uint8_t>((high << 4) | low);
  }

  uint8_t digest[32] = {0};
  if (mbedtls_sha256_ret(message, messageLength, digest, 0) != 0) {
    return false;
  }

  mbedtls_ecdsa_context context;
  mbedtls_ecdsa_init(&context);

  bool valid = false;
  if (mbedtls_ecp_group_load(&context.grp, MBEDTLS_ECP_DP_SECP256R1) == 0
      && mbedtls_ecp_point_read_binary(&context.grp, &context.Q, point, sizeof(point)) == 0
      && mbedtls_ecp_check_pubkey(&context.grp, &context.Q) == 0) {
    // read_signature разбирает DER-подпись и проверяет её по ключу из контекста.
    valid = mbedtls_ecdsa_read_signature(
                &context, digest, sizeof(digest), signature, signatureLength) == 0;
  }

  mbedtls_ecdsa_free(&context);
  memset(digest, 0, sizeof(digest));
  memset(point, 0, sizeof(point));
  return valid;
}

int rngCallback(void*, unsigned char* buffer, size_t length) {
  esp_fill_random(buffer, length);
  return 0;
}

struct EcpGuard {
  mbedtls_ecp_group group;
  mbedtls_mpi privateKey;
  mbedtls_ecp_point point;

  EcpGuard() {
    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&privateKey);
    mbedtls_ecp_point_init(&point);
    mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
  }

  ~EcpGuard() {
    mbedtls_ecp_point_free(&point);
    mbedtls_mpi_free(&privateKey);
    mbedtls_ecp_group_free(&group);
  }
};

}  // namespace

bool PairingCrypto::begin() {
  _ready = loadKeypair() || createKeypair();
  loadPairCode();
  return _ready;
}

void PairingCrypto::loadPairCode() {
  String raw;

  Preferences prefs;
  if (prefs.begin(kNvsNamespace, true)) {
    if (prefs.isKey("code")) {
      raw = prefs.getString("code", "");
    }
    prefs.end();
  }

  // Значение из Secrets.h — для стенда; в серии код пишет производство в NVS.
  if (raw.length() == 0) {
    raw = SPEAKER_PAIR_CODE;
  }

  _pairCode = normalizePairCode(raw);

  if (_pairCode.length() > 0) {
    applog::line("[crypto] код привязки прошит: принимается кадр версии 2");
  } else {
    applog::line("[crypto] код привязки не прошит: кадр версии 1, только ECDH");
  }
}

String PairingCrypto::publicKeyHex() const {
  static const char* digits = "0123456789abcdef";

  String hex;
  hex.reserve(PUBLIC_KEY_SIZE * 2);
  for (size_t i = 0; i < PUBLIC_KEY_SIZE; ++i) {
    hex += digits[_publicKey[i] >> 4];
    hex += digits[_publicKey[i] & 0x0F];
  }

  return hex;
}

bool PairingCrypto::loadKeypair() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, true)) {
    return false;
  }

  const size_t privLen = prefs.getBytesLength("priv");
  const size_t pubLen = prefs.getBytesLength("pub");
  bool ok = privLen == sizeof(_privateKey) && pubLen == sizeof(_publicKey);

  if (ok) {
    prefs.getBytes("priv", _privateKey, sizeof(_privateKey));
    prefs.getBytes("pub", _publicKey, sizeof(_publicKey));
  }

  prefs.end();
  return ok;
}

bool PairingCrypto::createKeypair() {
  EcpGuard ecp;
  if (mbedtls_ecdh_gen_public(&ecp.group, &ecp.privateKey, &ecp.point, rngCallback, nullptr) != 0) {
    return false;
  }

  size_t written = 0;
  if (mbedtls_ecp_point_write_binary(
          &ecp.group, &ecp.point, MBEDTLS_ECP_PF_UNCOMPRESSED, &written,
          _publicKey, sizeof(_publicKey)) != 0) {
    return false;
  }

  if (written != sizeof(_publicKey)) {
    return false;
  }

  if (mbedtls_mpi_write_binary(&ecp.privateKey, _privateKey, sizeof(_privateKey)) != 0) {
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, false)) {
    return false;
  }
  prefs.putBytes("priv", _privateKey, sizeof(_privateKey));
  prefs.putBytes("pub", _publicKey, sizeof(_publicKey));
  prefs.end();
  return true;
}

bool PairingCrypto::selfTest() {
  if (!_ready && !begin()) {
    applog::line("[crypto] selfTest: нет ключа устройства");
    return false;
  }

  EcpGuard ecp;
  if (mbedtls_ecdh_gen_public(&ecp.group, &ecp.privateKey, &ecp.point, rngCallback, nullptr) != 0) {
    applog::line("[crypto] selfTest: ecdh_gen_public");
    return false;
  }

  uint8_t peerPublic[PUBLIC_KEY_SIZE] = {0};
  size_t written = 0;
  if (mbedtls_ecp_point_write_binary(&ecp.group, &ecp.point, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                     &written, peerPublic, sizeof(peerPublic)) != 0
      || written != sizeof(peerPublic)) {
    applog::line("[crypto] selfTest: write_public");
    return false;
  }

  // Общий секрет со стороны «сайта»: своя эфемерная пара × открытый ключ колонки.
  EcpGuard device;
  mbedtls_ecp_point devicePoint;
  mbedtls_mpi shared;
  mbedtls_ecp_point_init(&devicePoint);
  mbedtls_mpi_init(&shared);
  bool ok = false;
  uint8_t nonce[12] = {0};
  uint8_t ciphertext[64] = {0};
  uint8_t key[kKeySize] = {0};

  // Самопроверка идёт тем же выводом ключа, что и настоящий кадр: с кодом —
  // версия 2, без кода — версия 1.
  const uint8_t version = requiredFrameVersion();
  String info;
  buildHkdfInfo(_pairCode, version, info);

  do {
    if (mbedtls_ecp_point_read_binary(&device.group, &devicePoint, _publicKey, sizeof(_publicKey)) != 0) {
      applog::line("[crypto] selfTest: read_device_public");
      break;
    }
    if (mbedtls_ecdh_compute_shared(&device.group, &shared, &devicePoint, &ecp.privateKey,
                                    rngCallback, nullptr) != 0) {
      applog::line("[crypto] selfTest: compute_shared");
      break;
    }

    uint8_t secret[32] = {0};
    uint8_t salt[32] = {0};
    uint8_t hashInput[2 * PUBLIC_KEY_SIZE] = {0};
    if (mbedtls_mpi_write_binary(&shared, secret, sizeof(secret)) != 0) {
      break;
    }
    memcpy(hashInput, _publicKey, PUBLIC_KEY_SIZE);
    memcpy(hashInput + PUBLIC_KEY_SIZE, peerPublic, PUBLIC_KEY_SIZE);
    if (mbedtls_sha256_ret(hashInput, sizeof(hashInput), salt, 0) != 0) {
      break;
    }
    if (!hkdfSha256(salt, sizeof(salt), secret, sizeof(secret),
                    reinterpret_cast<const uint8_t*>(info.c_str()), info.length(),
                    key, sizeof(key))) {
      applog::line("[crypto] selfTest: hkdf");
      break;
    }

    esp_fill_random(nonce, sizeof(nonce));
    const char* plain = "{\"selfTest\":true}";
    const size_t plainLength = strlen(plain);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, kKeySize * 8) != 0) {
      mbedtls_gcm_free(&gcm);
      applog::line("[crypto] selfTest: gcm_setkey");
      break;
    }
    // Тег AES-GCM дописывается вслед за шифртекстом — как его ждёт decrypt().
    const int encryptResult = mbedtls_gcm_crypt_and_tag(
        &gcm, MBEDTLS_GCM_ENCRYPT, plainLength, nonce, sizeof(nonce), nullptr, 0,
        reinterpret_cast<const unsigned char*>(plain), ciphertext,
        kTagSize, ciphertext + plainLength);
    mbedtls_gcm_free(&gcm);
    if (encryptResult != 0) {
      applog::line("[crypto] selfTest: gcm_encrypt");
      break;
    }

    char out[64] = {0};
    if (!decrypt(version, peerPublic, nonce, ciphertext, plainLength + kTagSize,
                 out, sizeof(out))) {
      applog::line("[crypto] selfTest: decrypt");
      break;
    }
    if (strcmp(out, plain) != 0) {
      applog::line("[crypto] selfTest: mismatch");
      break;
    }

    ok = true;
  } while (false);

  mbedtls_mpi_free(&shared);
  mbedtls_ecp_point_free(&devicePoint);
  return ok;
}

bool PairingCrypto::verifyServerGrant(
    const String& deviceId,
    const String& publicKeySha256,
    const String& challenge,
    const String& userId,
    const String& pairToken,
    const String& apiBase,
    const String& signatureBase64) {
  if (!_ready || deviceId.length() == 0 || challenge.length() != 32
      || userId.length() == 0 || pairToken.length() == 0
      || !apiBase.startsWith("https://") || signatureBase64.length() == 0
      || SPEAKER_PAIRING_PUBLIC_KEY_HEX[0] == '\0') {
    return false;
  }

  uint8_t fingerprint[32] = {0};
  if (mbedtls_sha256_ret(_publicKey, sizeof(_publicKey), fingerprint, 0) != 0) {
    return false;
  }

  static const char* digits = "0123456789abcdef";
  char fingerprintHex[65] = {0};
  for (size_t i = 0; i < sizeof(fingerprint); ++i) {
    fingerprintHex[i * 2] = digits[fingerprint[i] >> 4];
    fingerprintHex[i * 2 + 1] = digits[fingerprint[i] & 0x0F];
  }

  if (!publicKeySha256.equalsIgnoreCase(fingerprintHex)) {
    memset(fingerprint, 0, sizeof(fingerprint));
    return false;
  }

  // Нормализация та же, что на сервере: иначе сайт, переформатировавший JSON,
  // сломает проверку подписи. Правило: deviceId — верхний регистр, hex-значения
  // и GUID — нижний.
  String canonical;
  canonical.reserve(230 + deviceId.length() + userId.length()
                    + pairToken.length() + apiBase.length());
  canonical += kGrantDomain;

  String upperDevice = deviceId;
  upperDevice.toUpperCase();
  canonical += upperDevice;
  canonical += '\n';
  canonical += fingerprintHex;
  canonical += '\n';

  String lowerChallenge = challenge;
  lowerChallenge.toLowerCase();
  canonical += lowerChallenge;
  canonical += '\n';

  String lowerUser = userId;
  lowerUser.toLowerCase();
  canonical += lowerUser;
  canonical += '\n';
  canonical += pairToken;
  canonical += '\n';
  canonical += apiBase;

  uint8_t signature[96] = {0};
  size_t signatureLength = 0;
  const int decoded = mbedtls_base64_decode(
      signature, sizeof(signature), &signatureLength,
      reinterpret_cast<const unsigned char*>(signatureBase64.c_str()),
      signatureBase64.length());
  const bool valid = decoded == 0
      && verifySignature(reinterpret_cast<const uint8_t*>(canonical.c_str()),
                         canonical.length(), signature, signatureLength);

  memset(fingerprint, 0, sizeof(fingerprint));
  memset(signature, 0, sizeof(signature));
  canonical = "";
  return valid;
}

bool PairingCrypto::decrypt(
    uint8_t version,
    const uint8_t* peerPublic,
    const uint8_t* nonce,
    const uint8_t* ciphertext,
    size_t ciphertextLength,
    char* out,
    size_t outCapacity) {
  if (!_ready || ciphertextLength <= kTagSize || outCapacity == 0) {
    return false;
  }

  // Чужую версию кадра не разбираем: без кода с наклейки ключ не совпадёт,
  // но отказ должен быть явным, а не «не расшифровалось».
  if (version != requiredFrameVersion()) {
    return false;
  }

  EcpGuard ecp;
  mbedtls_mpi ownKey;
  mbedtls_mpi secret;
  mbedtls_ecp_point peerPoint;
  mbedtls_mpi_init(&ownKey);
  mbedtls_mpi_init(&secret);
  mbedtls_ecp_point_init(&peerPoint);

  bool ok = false;
  uint8_t key[kKeySize] = {0};
  uint8_t salt[32] = {0};
  uint8_t hashInput[2 * PUBLIC_KEY_SIZE] = {0};
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);

  do {
    if (mbedtls_mpi_read_binary(&ownKey, _privateKey, sizeof(_privateKey)) != 0) {
      break;
    }

    if (mbedtls_ecp_point_read_binary(&ecp.group, &peerPoint, peerPublic, PUBLIC_KEY_SIZE) != 0) {
      break;
    }

    if (mbedtls_ecp_check_pubkey(&ecp.group, &peerPoint) != 0) {
      break;
    }

    if (mbedtls_ecdh_compute_shared(
            &ecp.group, &secret, &peerPoint, &ownKey, rngCallback, nullptr) != 0) {
      break;
    }

    uint8_t shared[32] = {0};
    if (mbedtls_mpi_write_binary(&secret, shared, sizeof(shared)) != 0) {
      break;
    }

    // Соль привязывает ключ к обеим сторонам: подменить открытый ключ в канале
    // и сохранить тот же ключ шифрования нельзя.
    memcpy(hashInput, _publicKey, PUBLIC_KEY_SIZE);
    memcpy(hashInput + PUBLIC_KEY_SIZE, peerPublic, PUBLIC_KEY_SIZE);
    if (mbedtls_sha256_ret(hashInput, sizeof(hashInput), salt, 0) != 0) {
      break;
    }

    String info;
    buildHkdfInfo(_pairCode, version, info);

    if (!hkdfSha256(salt, sizeof(salt), shared, sizeof(shared),
                    reinterpret_cast<const uint8_t*>(info.c_str()), info.length(),
                    key, sizeof(key))) {
      break;
    }

    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, kKeySize * 8) != 0) {
      break;
    }

    const size_t plainLength = ciphertextLength - kTagSize;
    if (plainLength + 1 > outCapacity) {
      break;
    }

    if (mbedtls_gcm_auth_decrypt(
            &gcm, plainLength, nonce, kNonceSize, nullptr, 0,
            ciphertext + plainLength, kTagSize, ciphertext,
            reinterpret_cast<unsigned char*>(out)) != 0) {
      break;
    }

    out[plainLength] = '\0';
    ok = true;
  } while (false);

  mbedtls_gcm_free(&gcm);
  mbedtls_ecp_point_free(&peerPoint);
  mbedtls_mpi_free(&secret);
  mbedtls_mpi_free(&ownKey);

  if (!ok) {
    memset(out, 0, outCapacity);
  }
  return ok;
}
