#!/usr/bin/env python3
"""Разбор снимка эха: микрофон до компенсатора против референса воспроизведения.

Зачем. Компенсатор вычитает эхо адаптивным фильтром по референсу — теми
отсчётами, что уходят в усилитель. Если на живой плате гашение 1–5 дБ вместо
двадцати, виноват либо фильтр, либо сам референс: не тот темп, не та задержка,
не тот уровень или нелинейность динамика. По журналу этого не видно, поэтому
сборка `esp32s3_aecdump` печатает оба сигнала шестнадцатеричными строками.

Что считает отчёт:
* нормированную взаимную корреляцию микрофона с референсом по задержкам —
  если пик слабый, эха в референсе нет и фильтру нечего вычитать;
* задержку и коэффициент передачи на ней (во сколько раз микрофон тише/громче);
* RMS и долю переходов через ноль обоих сигналов — по ним видно, не перегружен
  ли микрофон и не разошлись ли спектры (срез частоты, наклон);
* пишет mic.wav и ref.wav, чтобы послушать и посмотреть в редакторе.

Формат строк в журнале:
    [aecdump] rate 44100, mic 8192, ref 12288, задержка 60 мс, гашение 1.4 дБ
    [aecdump] m 1a2b...        (24 отсчёта в строке, от старого к новому)
    [aecdump] r 3c4d...
    [aecdump] done

Запуск:
    python3 tools/aec-dump.py dump.log
    pio device monitor -p /dev/tty.usbmodem141201 > dump.log   # затем отчёт

Зависимостей нет — только стандартная библиотека.
"""

import argparse
import struct
import sys
import wave

LINE_PREFIX = "[aecdump]"


def collapse_mirror(lines):
    """Прошивка пишет каждую строку дважды: в Serial и в ROM-консоль."""
    body = [line for line in lines if line.strip()]
    if len(body) < 2:
        return lines, False

    repeats = sum(1 for previous, current in zip(body, body[1:]) if previous == current)
    if repeats / (len(body) - 1) < 0.4:
        return lines, False

    collapsed = []
    for line in lines:
        if collapsed and line.strip() and line == collapsed[-1]:
            continue
        collapsed.append(line)
    return collapsed, True


def parse(lines):
    """Возвращает (заголовок, микрофон, референс, сколько строк потеряно)."""
    header = {}
    mic = []
    ref = []
    broken = 0
    target = None

    for line in lines:
        if LINE_PREFIX not in line:
            continue

        body = line.split(LINE_PREFIX, 1)[1].strip()

        if body.startswith("rate "):
            for part in body.split(","):
                pieces = part.split()
                if len(pieces) == 2 and pieces[0] in ("rate", "mic", "ref"):
                    header[pieces[0]] = int(pieces[1])
            continue

        if body == "done":
            target = None
            continue

        if body.startswith("m ") or body.startswith("r "):
            tag, hex_text = body[0], body[2:].strip()
            try:
                values = [struct.unpack(">h", bytes.fromhex(hex_text[i:i + 4]))[0]
                          for i in range(0, len(hex_text), 4)]
            except ValueError:
                broken += 1
                continue

            if tag == "m":
                mic.extend(values)
                target = "m"
            else:
                ref.extend(values)
                target = "r"
            continue

        if target is not None:
            # Строка продолжения без префикса — обрыв из-за зеркала.
            broken += 1

    return header, mic, ref, broken


def rms(values):
    if not values:
        return 0.0
    return (sum(float(v) * v for v in values) / len(values)) ** 0.5


def zero_crossings(values):
    if len(values) < 2:
        return 0.0
    crossings = sum(1 for a, b in zip(values, values[1:]) if (a < 0) != (b < 0))
    return crossings * 1000.0 / (len(values) - 1)


def best_lag(mic, ref, max_lag):
    """Нормированная корреляция: mic[i] против ref[ref_mid + i - lag].

    Оба снимка заканчиваются одним моментом, поэтому микрофон выровнен по
    концу референса, а задержка сдвигает его назад во времени.
    """
    if not mic or not ref:
        return 0, 0.0, 0.0

    mic_len = len(mic)
    ref_start = len(ref) - mic_len  # выравнивание по концу референса

    mic_energy = sum(float(v) * v for v in mic)
    if mic_energy <= 0.0:
        return 0, 0.0, 0.0

    best_score = -1.0
    best = 0
    best_dot = 0.0
    best_ref_energy = 0.0

    for lag in range(0, max_lag + 1):
        dot = 0.0
        energy = 0.0
        for i in range(mic_len):
            j = ref_start + i - lag
            if j < 0:
                continue
            value = float(ref[j])
            dot += mic[i] * value
            energy += value * value

        if energy <= 0.0:
            continue

        score = dot / ((mic_energy * energy) ** 0.5)
        if score > best_score:
            best_score = score
            best = lag
            best_dot = dot
            best_ref_energy = energy

    gain = best_dot / best_ref_energy if best_ref_energy > 0 else 0.0
    return best, max(best_score, -1.0), gain


def write_wav(path, values, rate):
    if not values:
        return
    with wave.open(path, "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        handle.writeframes(b"".join(struct.pack("<h", v) for v in values))


def main():
    parser = argparse.ArgumentParser(description="Разбор снимка эха из журнала колонки.")
    parser.add_argument("logs", nargs="*", help="файлы журнала; без них читается stdin")
    parser.add_argument("--max-lag", type=int, default=2000,
                        help="до какой задержки искать, в отсчётах 16 кГц (по умолчанию 2000 = 125 мс)")
    parser.add_argument("--rate", type=int, default=16000,
                        help="частота снимков для WAV (по умолчанию 16000)")
    parser.add_argument("--out", default=".", help="куда писать WAV")
    args = parser.parse_args()

    if args.logs:
        lines = []
        for path in args.logs:
            with open(path, encoding="utf-8", errors="replace") as handle:
                lines.extend(handle.readlines())
    else:
        lines = sys.stdin.readlines()

    lines, mirrored = collapse_mirror(lines)
    header, mic, ref, broken = parse(lines)

    if not mic or not ref:
        print("в журнале нет снимка: нужна сборка esp32s3_aecdump и включённый эфир")
        return 1

    mic_rms = rms(mic)
    ref_rms = rms(ref)
    lag, score, gain = best_lag(mic, ref, args.max_lag)

    print("## Снимок")
    print()
    print("| Параметр | Значение |")
    print("|---|---|")
    print(f"| зеркало UART | {'сведено' if mirrored else 'нет'} |")
    print(f"| частота эфира на входе | {header.get('rate', '—')} Гц |")
    print(f"| отсчётов микрофона | {len(mic)} ({len(mic) / args.rate:.2f} с) |")
    print(f"| отсчётов референса | {len(ref)} ({len(ref) / args.rate:.2f} с) |")
    if broken:
        print(f"| оборванных строк | {broken} |")
    print()

    print("## Сигналы")
    print()
    print("| Что | Значение |")
    print("|---|---|")
    print(f"| RMS микрофона | {mic_rms:.0f} из 32767 |")
    print(f"| RMS референса | {ref_rms:.0f} из 32767 |")
    print(f"| переходов через ноль, микрофон | {zero_crossings(mic):.0f} на 1000 |")
    print(f"| переходов через ноль, референс | {zero_crossings(ref):.0f} на 1000 |")
    print()

    print("## Эхо в референсе")
    print()
    print("| Что | Значение |")
    print("|---|---|")
    print(f"| лучшая задержка | {lag} отсчётов ({lag * 1000.0 / args.rate:.0f} мс) |")
    print(f"| корреляция на ней | {score:.2f} |")
    print(f"| передача микрофон / референс | {gain:.2f} |")
    print()

    print("## Что это значит")
    print()
    if score >= 0.7:
        print("* эхо в референсе есть и выровнено: дело в фильтре, а не в тракте;")
        print("* сверьте задержку с той, что печатает `[aec]` — окно фильтра должно её накрывать.")
    elif score >= 0.4:
        print("* корреляция средняя: референс и микрофон выровнены частично —")
        print("* проверьте, совпадает ли задержка с `[aec]` и не перегружен ли микрофон (RMS выше ~8000).")
    else:
        print("* корреляции почти нет: референс не совпадает с тем, что слышит микрофон —")
        print("* смотрите темп (частота эфира против 16 кГц), уровень и нелинейность динамика.")
    print()

    mic_path = f"{args.out}/mic.wav"
    ref_path = f"{args.out}/ref.wav"
    write_wav(mic_path, mic, args.rate)
    write_wav(ref_path, ref, args.rate)
    print(f"Записаны {mic_path} и {ref_path} ({args.rate} Гц).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
