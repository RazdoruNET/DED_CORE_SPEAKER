#!/usr/bin/env python3
"""Проверка вывода ключа кадра привязки: одна и та же формула на трёх сторонах.

Прошивка (PairingCrypto), сайт (speaker-pairing.service.ts) и стендовый
pair.py обязаны получать одинаковый ключ AES-256-GCM из одинаковых входов.
Расхождение здесь выглядит как «колонка не расшифровала настройки» без
внятной причины, поэтому формула закреплена эталонными векторами.

Формула:
    salt = SHA-256(devicePublic || ephemeralPublic)      # по 65 байт, несжатая точка
    info = "ded-speaker-pair-v1"                          # кадр версии 1
    info = "ded-speaker-pair-v2" || 0x0A || code           # кадр версии 2
    key  = HKDF-SHA256(salt, ikm = ECDH shared, info, 32)

`code` — код с наклейки в канонической форме: заглавные A-Z и 0-9.
Версию кадра определяет сервер по записи устройства (есть ли код с наклейки),
она входит в подписанный grant, и устройство сверяет её со своей. Поле `frame`
в BLE info — только подсказка: характеристика не аутентифицирована, и
подставное устройство сообщило бы `frame: 1`, чтобы откатить схему к чистому
ECDH и прочитать пароль Wi-Fi.

Запуск: python3 tools/pairing-kdf-check.py
Зависимостей нет — только стандартная библиотека.
"""

import hashlib
import hmac
import sys

HKDF_INFO_V1 = b"ded-speaker-pair-v1"
HKDF_INFO_V2 = b"ded-speaker-pair-v2"
CODE = "ABCDEFGHJKLM"

# Тестовые входы: публичные ключи и «общий секрет» здесь — просто байты,
# никакого реального секрета в файле нет.
DEVICE_PUBLIC = bytes(range(0x04, 0x04 + 65))
EPHEMERAL_PUBLIC = bytes(range(0x50, 0x50 + 65))
SHARED_SECRET = bytes(range(0x90, 0x90 + 32))

EXPECTED = {
    "salt": "72876677339c091182463e9f3b8353b7215d62f17e9603d39e7fa9ef0f98322e",
    "v1": "d5eb361c45ddab6e6ba292f0921f76a86f31cd8a1c59714223d6b1df2643020b",
    "v2": "3181be75b8233c7d8d74ae574901634823ccd18168031bc3e414e496f6db9070",
}


def hkdf_sha256(salt: bytes, ikm: bytes, info: bytes, length: int = 32) -> bytes:
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out = b""
    block = b""
    counter = 1

    while len(out) < length:
        block = hmac.new(prk, block + info + bytes([counter]), hashlib.sha256).digest()
        out += block
        counter += 1

    return out[:length]


def normalize_code(raw: str) -> str:
    return "".join(ch for ch in raw.upper() if ch.isascii() and ch.isalnum())


def build_info(version: int, code: str) -> bytes:
    if version == 1:
        return HKDF_INFO_V1

    return HKDF_INFO_V2 + b"\n" + normalize_code(code).encode()


def main() -> int:
    salt = hashlib.sha256(DEVICE_PUBLIC + EPHEMERAL_PUBLIC).digest()
    v1 = hkdf_sha256(salt, SHARED_SECRET, build_info(1, CODE))
    v2 = hkdf_sha256(salt, SHARED_SECRET, build_info(2, CODE))

    actual = {"salt": salt.hex(), "v1": v1.hex(), "v2": v2.hex()}
    failed = False

    for name, expected in EXPECTED.items():
        ok = hmac.compare_digest(actual[name], expected)
        print(f"{name}: {'ок' if ok else 'РАСХОЖДЕНИЕ'}")
        if not ok:
            print(f"  ожидалось {expected}")
            print(f"  получено  {actual[name]}")
            failed = True

    # Код в другой записи (дефисы, нижний регистр) обязан дать тот же ключ.
    messy = hkdf_sha256(salt, SHARED_SECRET, build_info(2, "abcd-efgh-jklm"))
    if messy != v2:
        print("нормализация кода: РАСХОЖДЕНИЕ")
        failed = True
    else:
        print("нормализация кода: ок")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
