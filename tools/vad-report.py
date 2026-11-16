#!/usr/bin/env python3
"""Отчёт по журналу колонки: сколько фраз открыл детектор и сколько из них
были настоящим обращением.

Зачем. Решение «начинать писать фразу» принимает детектор речи по энергии
(`[vad] начало речи`), а решение «это было обращение» — сервер по тексту
(`[voice] слово-активатор распознано`). Разница между этими двумя счётчиками
и есть цена порога: пустые фразы — ложные срабатывания (лишний трафик и
запись не той речи), отсутствие обращений там, где человек говорил — глухота.

Журнал прошивки не содержит времени, поэтому длительность берётся так:
1) метка времени в начале строки, если её добавляет монитор
   (`[  1234.567]` или `[12:34:56.789]`);
2) `--minutes`, если сессию засекли руками;
3) иначе — по числу строк `[state]`: прошивка печатает их раз в 5 секунд,
   пока есть связь (main.cpp, kHeapLogIntervalMs).

Запуск:
    python3 tools/vad-report.py journal.log
    pio device monitor -p /dev/tty.usbserial-0001 > journal.log   # затем отчёт
    python3 tools/vad-report.py --minutes 30 --expect 20 journal.log

Зависимостей нет — только стандартная библиотека.
"""

import argparse
import json
import re
import sys
from statistics import median

STATE_INTERVAL_S = 5.0  # kHeapLogIntervalMs в main.cpp

# Порог приёмки из docs/06-roadmap.md, этап 2.
DEFAULT_MAX_EMPTY_PER_HOUR = 2.0
DEFAULT_MIN_WAKE_RECALL = 0.95  # не больше одного пропуска из двадцати

TIMESTAMP_RE = re.compile(r"^\[\s*(\d+):(\d+):(\d+(?:\.\d+)?)\s*\]\s?")
SECONDS_RE = re.compile(r"^\[\s*(\d+(?:\.\d+)?)\s*\]\s?")

VAD_RE = re.compile(r"\[vad\] (начало|конец) речи, rms (\d+)")
HEAP_RE = re.compile(
    r"\[heap\] (\d+) внутр, кадров (\d+), vad (\d+), темп (\d+)/с, "
    r"шум (\d+), порог (\d+), rms (\d+)"
)
AEC_RE = re.compile(
    r"\[aec\] пик ([\d.]+) на (\d+) мс, задержка (\d+) мс, микрофон (\d+) -> (\d+), "
    r"гашение ([\d.]+) дБ, порог (\d+), пол (\d+)"
)
WAKE_MARK = "[voice] слово-активатор распознано"
AGENT_TALK_MARK = "[voice] агент говорит"
SNEEZE_MARK = "[sneeze]"
MUTE_RE = re.compile(r"\[mute\] микрофон (выключен|включён)")
LIGHT_RE = re.compile(r"\[local\] свет (включён|выключен)")
OFFLINE_MARK = "[local] офлайн-фраза"
HEAP_BROKEN_MARK = "[heap] ЦЕЛОСТНОСТЬ НАРУШЕНА"


class Phrase:
    __slots__ = ("start_t", "end_t", "start_rms", "end_rms", "noise", "threshold", "wake")

    def __init__(self, start_t, start_rms, noise, threshold):
        self.start_t = start_t
        self.end_t = None
        self.start_rms = start_rms
        self.end_rms = None
        self.noise = noise
        self.threshold = threshold
        self.wake = False

    @property
    def duration(self):
        if self.end_t is None or self.start_t is None:
            return None
        return self.end_t - self.start_t


def parse_clock(line):
    """Метка времени из начала строки: секунды от начала записи."""
    match = TIMESTAMP_RE.match(line)
    if match:
        hours, minutes, seconds = match.groups()
        return int(hours) * 3600 + int(minutes) * 60 + float(seconds), match.end()

    match = SECONDS_RE.match(line)
    if match:
        return float(match.group(1)), match.end()

    return None, 0


def collapse_mirror(lines):
    """Убирает зеркалирование журнала.

    Прошивка пишет каждую строку дважды: в `Serial` и в ROM-консоль, а на S3 с
    `ARDUINO_USB_CDC_ON_BOOT=1` обе попадают в один и тот же USB-порт. Пары
    идут подряд, поэтому зеркало видно по доле повторов, а не по содержимому:
    у обычного журнала её почти нет.
    """
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
    """Разбор журнала: фразы, снимки эха, память и счётчики событий."""
    phrases = []
    open_phrase = None
    last_noise = 0
    last_threshold = 0
    heaps = []
    aecs = []
    counters = {
        "wake": 0,
        "orphan_wake": 0,
        "agent_turns": 0,
        "sneeze": 0,
        "mute_on": 0,
        "mute_off": 0,
        "light": 0,
        "offline": 0,
        "heap_broken": 0,
        "state_lines": 0,
        "unparsed_vad": 0,
    }
    has_time = False
    first_stamp = None
    last_stamp = None

    for raw in lines:
        line = raw.rstrip("\r\n")
        if not line:
            continue

        stamp, offset = parse_clock(line)
        if stamp is not None:
            has_time = True
            first_stamp = stamp if first_stamp is None else min(first_stamp, stamp)
            last_stamp = stamp if last_stamp is None else max(last_stamp, stamp)
        body = line[offset:]

        if "[state] захват" in body:
            counters["state_lines"] += 1
            continue

        match = HEAP_RE.search(body)
        if match:
            heap_free, noise, threshold = (
                int(match.group(1)),
                int(match.group(5)),
                int(match.group(6)),
            )
            heaps.append({"t": stamp, "free": heap_free, "noise": noise, "threshold": threshold})
            last_noise, last_threshold = noise, threshold
            continue

        if HEAP_BROKEN_MARK in body:
            counters["heap_broken"] += 1
            continue

        match = AEC_RE.search(body)
        if match:
            aecs.append(
                {
                    "probe": float(match.group(1)),
                    "lag_ms": int(match.group(2)),
                    "delay_ms": int(match.group(3)),
                    "mic_in": int(match.group(4)),
                    "mic_out": int(match.group(5)),
                    "erle": float(match.group(6)),
                    "threshold": int(match.group(7)),
                    "floor": int(match.group(8)),
                }
            )
            continue

        match = VAD_RE.search(body)
        if match:
            kind, rms = match.group(1), int(match.group(2))
            if kind == "начало":
                if open_phrase is not None:
                    # Детектор закрывает фразу сам; повторный старт без конца
                    # означает, что «конец речи» потерялся между кадрами.
                    counters["unparsed_vad"] += 1
                    phrases.append(open_phrase)
                open_phrase = Phrase(stamp, rms, last_noise, last_threshold)
            else:
                if open_phrase is None:
                    counters["unparsed_vad"] += 1
                    continue
                open_phrase.end_t = stamp
                open_phrase.end_rms = rms
                phrases.append(open_phrase)
                open_phrase = None
            continue

        if WAKE_MARK in body:
            counters["wake"] += 1
            # Сервер отвечает уже после конца фразы: обращение относится
            # к последней закрытой фразе, а не к текущей.
            target = phrases[-1] if phrases else open_phrase
            if target is not None and not target.wake:
                target.wake = True
            else:
                counters["orphan_wake"] += 1
            continue

        if AGENT_TALK_MARK in body:
            counters["agent_turns"] += 1
            continue

        if SNEEZE_MARK in body:
            counters["sneeze"] += 1
            continue

        if MUTE_RE.search(body):
            if MUTE_RE.search(body).group(1) == "выключен":
                counters["mute_on"] += 1
            else:
                counters["mute_off"] += 1
            continue

        if LIGHT_RE.search(body):
            counters["light"] += 1
            continue

        if OFFLINE_MARK in body:
            counters["offline"] += 1
            continue

    if open_phrase is not None:
        phrases.append(open_phrase)

    return phrases, heaps, aecs, counters, (first_stamp, last_stamp)


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(fraction * (len(ordered) - 1)))))
    return ordered[index]


def session_seconds(args, heaps, counters, time_span):
    """Длительность записи и источник этой оценки."""
    first_stamp, last_stamp = time_span
    if first_stamp is not None and last_stamp is not None and last_stamp > first_stamp:
        return last_stamp - first_stamp, "метки времени в журнале"

    if args.minutes:
        return args.minutes * 60.0, "--minutes"

    if counters["state_lines"] >= 2:
        return counters["state_lines"] * STATE_INTERVAL_S, "строки [state] × 5 с"

    if len(heaps) >= 2:
        return (len(heaps) - 1) * STATE_INTERVAL_S, "строки [heap] × 5 с"

    return None, "неизвестна"


def build_report(args, phrases, heaps, aecs, counters, time_span):
    seconds, source = session_seconds(args, heaps, counters, time_span)
    hours = seconds / 3600.0 if seconds else None

    with_wake = [p for p in phrases if p.wake]
    empty = [p for p in phrases if not p.wake]
    durations = [p.duration for p in phrases if p.duration is not None]
    start_rms = [p.start_rms for p in phrases]
    margins = [
        p.start_rms / p.threshold
        for p in phrases
        if p.threshold > 0
    ]
    noise_at_start = [p.noise for p in phrases if p.noise > 0]

    erle = [a["erle"] for a in aecs]
    converged = [a for a in aecs if a["erle"] >= 6.0]
    delays = [a["delay_ms"] for a in aecs]

    empty_per_hour = len(empty) / hours if hours else None
    recall = (len(with_wake) / args.expect) if args.expect else None
    missed = max(0, args.expect - len(with_wake)) if args.expect else None

    warnings = []
    violations = []
    if hours is None:
        warnings.append(
            "длительность неизвестна: часовые показатели не считаются "
            "(укажите --minutes или пишите журнал с метками времени)"
        )
    if empty_per_hour is not None and empty_per_hour > args.max_empty_per_hour:
        message = (
            f"пустых фраз {empty_per_hour:.1f}/ч при допуске {args.max_empty_per_hour:.1f}/ч: "
            "порог занижен либо в комнате идёт речь/ТВ"
        )
        warnings.append(message)
        violations.append(message)
    if args.expect and len(with_wake) > args.expect:
        warnings.append(
            f"обращений распознано больше, чем ожидалось ({len(with_wake)} против {args.expect}): "
            "в фоне звучала речь с обращением или --expect задан неверно"
        )
    if recall is not None and recall < args.min_wake_recall:
        message = (
            f"обращений распознано {len(with_wake)} из {args.expect} "
            f"({recall * 100:.0f}%): порог завышен, фраза обрезана или преролл короток"
        )
        warnings.append(message)
        violations.append(message)
    if margins and percentile(margins, 0.1) is not None and percentile(margins, 0.1) < 1.2:
        warnings.append(
            "десятая часть фраз открывается меньше чем в 1.2× порога: "
            "тихая речь на грани, см. [heap] «шум» и «порог»"
        )
    if erle and len(converged) / len(erle) < 0.5:
        warnings.append(
            f"компенсатор эха ниже 6 дБ в {100 - round(100 * len(converged) / len(erle))}% "
            "замеров: порог по остатку эха занижен, музыка может открывать фразу"
        )
    if counters["orphan_wake"]:
        warnings.append(
            f"обращений без своей фразы: {counters['orphan_wake']} — "
            "вероятно, «начало речи» не дошло до сервера"
        )
    if counters["unparsed_vad"]:
        warnings.append(
            f"нарушена парность границ фраз: {counters['unparsed_vad']} — "
            "«конец речи» потерялся или детектор сбросили посреди фразы"
        )
    if counters["heap_broken"]:
        warnings.append("в журнале есть сообщение о нарушении целостности кучи")
    if counters["offline"]:
        warnings.append(
            f"фраз без сервера: {counters['offline']} — они не могут дать обращение, "
            "для замеров нужна связь"
        )

    return {
        "seconds": seconds,
        "duration_source": source,
        "hours": hours,
        "phrases_total": len(phrases),
        "phrases_with_wake": len(with_wake),
        "phrases_empty": len(empty),
        "empty_per_hour": empty_per_hour,
        "expected": args.expect,
        "missed": missed,
        "recall": recall,
        "duration_p50": median(durations) if durations else None,
        "duration_p90": percentile(durations, 0.9),
        "start_rms_p50": median(start_rms) if start_rms else None,
        "start_rms_p10": percentile(start_rms, 0.1),
        "margin_p50": median(margins) if margins else None,
        "margin_p10": percentile(margins, 0.1),
        "noise_p50": median(noise_at_start) if noise_at_start else None,
        "heap_min": min((h["free"] for h in heaps), default=None),
        "aec_samples": len(aecs),
        "erle_p50": median(erle) if erle else None,
        "erle_converged_share": (len(converged) / len(erle)) if erle else None,
        "delay_p50": median(delays) if delays else None,
        "counters": counters,
        "warnings": warnings,
        "violations": violations,
        "max_empty_per_hour": args.max_empty_per_hour,
        "min_wake_recall": args.min_wake_recall,
    }


def number(value, digits=1, unit=""):
    if value is None:
        return "—"
    if isinstance(value, float):
        return f"{value:.{digits}f}{unit}"
    return f"{value}{unit}"


def print_report(report):
    counters = report["counters"]

    print("## Сессия")
    print()
    print("| Параметр | Значение |")
    print("|---|---|")
    print(f"| длительность | {number(report['seconds'], 0, ' с')} |")
    print(f"| откуда время | {report['duration_source']} |")
    print(f"| зеркало UART | {'сведено' if report['mirror_collapsed'] else 'нет'} |")
    print(f"| свободно в куче, минимум | {number(report['heap_min'], 0, ' Б')} |")
    print()

    print("## Фразы")
    print()
    print("| Показатель | Значение |")
    print("|---|---|")
    print(f"| «начало речи» | {report['phrases_total']} |")
    print(f"| из них с обращением | {report['phrases_with_wake']} |")
    print(f"| без обращения (ложные) | {report['phrases_empty']} |")
    print(f"| пустых фраз в час | {number(report['empty_per_hour'], 1)} |")
    print(f"| ожидалось обращений | {number(report['expected'], 0)} |")
    print(f"| пропущено | {number(report['missed'], 0)} |")
    print(f"| доля распознанных обращений | {number(report['recall'] and report['recall'] * 100, 0, ' %')} |")
    print(f"| длина фразы p50 / p90 | {number(report['duration_p50'], 1, ' с')} / {number(report['duration_p90'], 1, ' с')} |")
    print()

    print("## Приёмка")
    print()
    print("| Критерий | Факт | Допуск | Итог |")
    print("|---|---|---|---|")
    if report["empty_per_hour"] is None:
        print(f"| пустых фраз в час | — | ≤ {report['max_empty_per_hour']:.1f} | нет данных |")
    else:
        passed = report["empty_per_hour"] <= report["max_empty_per_hour"]
        print(
            f"| пустых фраз в час | {report['empty_per_hour']:.1f} | "
            f"≤ {report['max_empty_per_hour']:.1f} | {'пройдено' if passed else 'не пройдено'} |"
        )
    if report["recall"] is None:
        print(
            f"| распознанных обращений | — | ≥ {report['min_wake_recall'] * 100:.0f} % | "
            "нет данных, нужен --expect |"
        )
    else:
        passed = report["recall"] >= report["min_wake_recall"]
        print(
            f"| распознанных обращений | {report['phrases_with_wake']} из {report['expected']} "
            f"({report['recall'] * 100:.0f} %) | ≥ {report['min_wake_recall'] * 100:.0f} % | "
            f"{'пройдено' if passed else 'не пройдено'} |"
        )
    print()

    print("## Отступ от порога")
    print()
    print("| Показатель | Значение |")
    print("|---|---|")
    print(f"| rms на старте p50 / p10 | {number(report['start_rms_p50'], 0)} / {number(report['start_rms_p10'], 0)} |")
    print(f"| rms ÷ порог, p50 / p10 | {number(report['margin_p50'], 2, '×')} / {number(report['margin_p10'], 2, '×')} |")
    print(f"| шум в момент старта p50 | {number(report['noise_p50'], 0)} |")
    print()

    if report["aec_samples"]:
        print("## Компенсатор эха")
        print()
        print("| Показатель | Значение |")
        print("|---|---|")
        print(f"| замеров | {report['aec_samples']} |")
        print(f"| гашение p50 | {number(report['erle_p50'], 1, ' дБ')} |")
        print(f"| замеров с гашением ≥ 6 дБ | {number(report['erle_converged_share'] and report['erle_converged_share'] * 100, 0, ' %')} |")
        print(f"| задержка тракта p50 | {number(report['delay_p50'], 0, ' мс')} |")
        print()

    print("## События")
    print()
    print("| Событие | Значение |")
    print("|---|---|")
    print(f"| обращений с сервера | {counters['wake']} |")
    print(f"| ответов агента | {counters['agent_turns']} |")
    print(f"| чихов | {counters['sneeze']} |")
    print(f"| локальных команд | {counters['light']} |")
    print(f"| выключений / включений микрофона | {counters['mute_on']} / {counters['mute_off']} |")
    print(f"| фраз без сервера | {counters['offline']} |")
    print()

    if report["warnings"]:
        print("## На что смотреть")
        print()
        for warning in report["warnings"]:
            print(f"* {warning}")
        print()


def main():
    parser = argparse.ArgumentParser(
        description="Отчёт по журналу колонки: ложные фразы, обращения, порог, эхо.",
        epilog="Пример: python3 tools/vad-report.py --minutes 30 --expect 20 journal.log",
    )
    parser.add_argument("logs", nargs="*", help="файлы журнала; без них читается stdin")
    parser.add_argument("--minutes", type=float, help="длительность записи в минутах")
    parser.add_argument("--expect", type=int, help="сколько раз человек намеренно сказал обращение")
    parser.add_argument("--max-empty-per-hour", type=float, default=DEFAULT_MAX_EMPTY_PER_HOUR,
                        help=f"допуск пустых фраз в час (по умолчанию {DEFAULT_MAX_EMPTY_PER_HOUR})")
    parser.add_argument("--min-wake-recall", type=float, default=DEFAULT_MIN_WAKE_RECALL,
                        help=f"допуск доли распознанных обращений (по умолчанию {DEFAULT_MIN_WAKE_RECALL})")
    parser.add_argument("--json", action="store_true", help="машинный вывод вместо таблиц")
    parser.add_argument("--strict", action="store_true", help="код возврата 1 при нарушении приёмки")
    args = parser.parse_args()

    if args.logs:
        lines = []
        for path in args.logs:
            with open(path, encoding="utf-8", errors="replace") as handle:
                lines.extend(handle.readlines())
    else:
        lines = sys.stdin.readlines()

    lines, mirrored = collapse_mirror(lines)
    phrases, heaps, aecs, counters, time_span = parse(lines)
    report = build_report(args, phrases, heaps, aecs, counters, time_span)
    report["mirror_collapsed"] = mirrored

    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print_report(report)

    if args.strict and report["violations"]:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
