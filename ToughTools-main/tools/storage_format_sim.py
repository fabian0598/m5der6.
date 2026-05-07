#!/usr/bin/env python3
"""Write local CSV files that mirror the firmware storage format.

This is meant for macOS-side testing of file format and field order without
requiring the M5Stack Tough / SD card.
"""

from __future__ import annotations

import argparse
import csv
import shutil
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path


TIME_LOG_DIR = Path("logs/time")
EVENT_LOG_DIR = Path("logs/events")
TIME_LOG_HEADER = ["timestamp", "temperature", "time_id"]
EVENT_LOG_HEADER = ["timestamp", "event", "elapsed", "temperature", "event_id"]
SETTINGS_LINE = "{hours},{minutes},{seconds},{threshold:.1f}\n"


@dataclass(frozen=True)
class Settings:
    hours: int
    minutes: int
    seconds: int
    threshold: float


@dataclass(frozen=True)
class TimeLogSample:
    timestamp: datetime
    temperature: float
    time_id: int


@dataclass(frozen=True)
class EventLogSample:
    timestamp: datetime
    event: str
    elapsed_seconds: int
    temperature: float
    event_id: int


DEFAULT_SETTINGS = Settings(hours=1, minutes=0, seconds=0, threshold=70.0)


def format_timestamp(value: datetime) -> str:
    return value.strftime("%Y-%m-%d %H:%M:%S")


def format_elapsed(total_seconds: int) -> str:
    hours = total_seconds // 3600
    minutes = (total_seconds % 3600) // 60
    seconds = total_seconds % 60
    return f"{hours:02d}:{minutes:02d}:{seconds:02d}"


def write_time_log(path: Path, samples: list[TimeLogSample]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(TIME_LOG_HEADER)
        for sample in samples:
            writer.writerow([
                format_timestamp(sample.timestamp),
                f"{sample.temperature:.1f}",
                sample.time_id,
            ])


def write_event_log(path: Path, samples: list[EventLogSample]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(EVENT_LOG_HEADER)
        for sample in samples:
            writer.writerow([
                format_timestamp(sample.timestamp),
                sample.event,
                format_elapsed(sample.elapsed_seconds),
                f"{sample.temperature:.1f}",
                sample.event_id,
            ])


def write_settings(path: Path, settings: Settings) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        SETTINGS_LINE.format(
            hours=settings.hours,
            minutes=settings.minutes,
            seconds=settings.seconds,
            threshold=settings.threshold,
        ),
        encoding="utf-8",
    )


def build_samples(start_time: datetime, count: int, interval_minutes: int) -> list[TimeLogSample]:
    samples: list[TimeLogSample] = []
    current_time = start_time
    temperature = 72.5
    for time_id in range(1, count + 1):
        samples.append(TimeLogSample(current_time, temperature, time_id))
        current_time += timedelta(minutes=interval_minutes)
        temperature -= 0.2
    return samples


def build_event_samples(start_time: datetime) -> list[EventLogSample]:
    return [
        EventLogSample(start_time, "BOOT", 0, 0.0, 1),
        EventLogSample(start_time + timedelta(minutes=45, seconds=12), "TEMP_BELOW_THRESHOLD", 900, 69.6, 2),
        EventLogSample(start_time + timedelta(minutes=45, seconds=32), "TIMER_EXPIRED", 3600, 69.6, 3),
        EventLogSample(start_time + timedelta(minutes=45, seconds=52), "TIMER_RESTART", 20, 71.1, 4),
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description="Simulate ToughTools storage files locally on macOS.")
    parser.add_argument("--out", default="storage_sim_output", help="Output directory for generated files")
    parser.add_argument("--samples", type=int, default=3, help="Number of time log samples to generate")
    parser.add_argument("--interval-minutes", type=int, default=1, help="Minutes between generated time samples")
    parser.add_argument("--reset", action="store_true", help="Delete existing output files before writing")
    args = parser.parse_args()

    output_dir = Path(args.out)

    if args.reset:
        shutil.rmtree(output_dir, ignore_errors=True)

    output_dir.mkdir(parents=True, exist_ok=True)

    start_time = datetime.now().replace(microsecond=0)
    session_tag = start_time.strftime("%Y%m%d_%H%M%S")
    time_samples = build_samples(start_time, max(args.samples, 1), max(args.interval_minutes, 1))
    event_samples = build_event_samples(start_time)

    time_path = output_dir / TIME_LOG_DIR / f"time_{session_tag}_001.csv"
    event_path = output_dir / EVENT_LOG_DIR / f"event_{session_tag}_001.csv"

    write_time_log(time_path, time_samples)
    write_event_log(event_path, event_samples)
    write_settings(output_dir / "settings.csv", DEFAULT_SETTINGS)

    print(f"Wrote local storage simulation files to: {output_dir.resolve()}")
    print(f"- {time_path.relative_to(output_dir)}")
    print(f"- {event_path.relative_to(output_dir)}")
    print("- settings.csv")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
