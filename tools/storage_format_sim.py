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


MEASUREMENTS_DIR = Path("logs/measurements")
EVENT_LOG_DIR = Path("logs/events")
# New standardized format for measurements
MEASUREMENTS_HEADER = ["timestamp", "measurement_type", "value", "unit", "session_id", "source", "global_id"]
EVENT_LOG_HEADER = ["timestamp", "event", "elapsed", "temperature", "event_id"]
SETTINGS_LINE = "{hours},{minutes},{seconds},{threshold:.1f}\n"


@dataclass(frozen=True)
class Settings:
    hours: int
    minutes: int
    seconds: int
    threshold: float


@dataclass(frozen=True)
class MeasurementSample:
    """Standardized measurement record."""
    timestamp: datetime
    measurement_type: str  # e.g., "temperature"
    value: float
    unit: str  # e.g., "°C"
    session_id: str
    source: str  # e.g., "device_001"
    global_id: int


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


def write_measurement_log(path: Path, samples: list[MeasurementSample]) -> None:
    """Write measurement log with standardized format."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(MEASUREMENTS_HEADER)
        for sample in samples:
            writer.writerow([
                sample.timestamp.isoformat() + "Z",  # ISO 8601 UTC
                sample.measurement_type,
                f"{sample.value:.1f}",
                sample.unit,
                sample.session_id,
                sample.source,
                sample.global_id,
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


def build_measurement_samples(start_time: datetime, count: int, interval_minutes: int, session_id: str, source: str = "device_001") -> list[MeasurementSample]:
    """Build measurement samples in standardized format."""
    samples: list[MeasurementSample] = []
    current_time = start_time
    value = 72.5
    for global_id in range(1, count + 1):
        samples.append(MeasurementSample(
            timestamp=current_time,
            measurement_type="temperature",
            value=value,
            unit="°C",
            session_id=session_id,
            source=source,
            global_id=global_id,
        ))
        current_time += timedelta(minutes=interval_minutes)
        value -= 0.2
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
    parser.add_argument("--samples", type=int, default=3, help="Number of measurement samples to generate")
    parser.add_argument("--interval-minutes", type=int, default=1, help="Minutes between generated samples")
    parser.add_argument("--reset", action="store_true", help="Delete existing output files before writing")
    args = parser.parse_args()

    output_dir = Path(args.out)

    if args.reset:
        shutil.rmtree(output_dir, ignore_errors=True)

    output_dir.mkdir(parents=True, exist_ok=True)

    start_time = datetime.now().replace(microsecond=0)
    session_tag = start_time.strftime("%Y%m%d_%H%M%S")
    
    # Build measurement samples with new standardized format
    measurement_samples = build_measurement_samples(
        start_time,
        max(args.samples, 1),
        max(args.interval_minutes, 1),
        session_id=session_tag,
        source="device_001"
    )
    event_samples = build_event_samples(start_time)

    # New naming convention: measurements_YYYY-MM-DD.csv
    date_key = start_time.strftime("%Y-%m-%d")
    measurement_path = output_dir / MEASUREMENTS_DIR / f"measurements_{date_key}.csv"
    event_path = output_dir / EVENT_LOG_DIR / f"event_{session_tag}_001.csv"

    write_measurement_log(measurement_path, measurement_samples)
    write_event_log(event_path, event_samples)
    write_settings(output_dir / "settings.csv", DEFAULT_SETTINGS)

    print(f"Wrote local storage simulation files to: {output_dir.resolve()}")
    print(f"- {measurement_path.relative_to(output_dir)}")
    print(f"- {event_path.relative_to(output_dir)}")
    print("- settings.csv")
    print(f"\nNew measurement format:")
    print(f"  Header: {', '.join(MEASUREMENTS_HEADER)}")
    print(f"  Example: {measurement_samples[0] if measurement_samples else 'N/A'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
