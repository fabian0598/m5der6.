#!/usr/bin/env python3
"""Capture ToughTools serial logs and persist them as local CSV files.

Use this when no SD card is inserted. The firmware still prints log lines to serial,
and this script turns those lines into rotating CSV files on the Mac.
"""

from __future__ import annotations

import argparse
import csv
import re
import shutil
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

TIME_HEADER = ["timestamp", "temperature", "time_id"]
EVENT_HEADER = ["timestamp", "event", "elapsed", "temperature", "event_id"]

TIME_LINE_RE = re.compile(r"^\[TimeLog\]\[(?P<target>[^\]]+)\]\s(?P<payload>.+)$")
EVENT_LINE_RE = re.compile(r"^\[EventLog\]\[(?P<target>[^\]]+)\]\s(?P<payload>.+)$")
SESSION_FROM_FILE_RE = re.compile(r"^(?:time|event)_(?P<tag>\d{8}_\d{6})_\d{3}\.csv$")


@dataclass
class SessionWriter:
    out_dir: Path
    max_entries: int

    def __post_init__(self) -> None:
        self.time_dir = self.out_dir / "logs" / "time"
        self.event_dir = self.out_dir / "logs" / "event"
        self.time_dir.mkdir(parents=True, exist_ok=True)
        self.event_dir.mkdir(parents=True, exist_ok=True)

        self.session_tag = ""
        self.time_index = 0
        self.event_index = 0
        self.time_entries = 0
        self.event_entries = 0
        self.time_path: Path | None = None
        self.event_path: Path | None = None

    def _new_session(self, tag: str) -> None:
        if not tag:
            tag = datetime.now().strftime("%Y%m%d_%H%M%S")

        self.session_tag = tag
        self.time_index = 0
        self.event_index = 0
        self.time_entries = 0
        self.event_entries = 0
        self._rotate_time()
        self._rotate_event()
        print(f"[Capture] New session: {self.session_tag}")

    def _rotate_time(self) -> None:
        self.time_index += 1
        self.time_entries = 0
        self.time_path = self.time_dir / f"time_{self.session_tag}_{self.time_index:03d}.csv"
        self._ensure_header(self.time_path, TIME_HEADER)
        print(f"[Capture] Time file: {self.time_path}")

    def _rotate_event(self) -> None:
        self.event_index += 1
        self.event_entries = 0
        self.event_path = self.event_dir / f"event_{self.session_tag}_{self.event_index:03d}.csv"
        self._ensure_header(self.event_path, EVENT_HEADER)
        print(f"[Capture] Event file: {self.event_path}")

    @staticmethod
    def _ensure_header(path: Path, header: list[str]) -> None:
        if path.exists():
            return
        with path.open("w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(header)

    def ensure_started(self, suggested_tag: str | None = None) -> None:
        if self.session_tag:
            return
        self._new_session(suggested_tag or datetime.now().strftime("%Y%m%d_%H%M%S"))

    def start_new_session_if_boot(self, event_row: list[str], suggested_tag: str | None) -> None:
        if len(event_row) < 5:
            return
        event_name = event_row[1].strip()
        event_id = event_row[4].strip()
        if event_name == "BOOT" and event_id == "1":
            self._new_session(suggested_tag or datetime.now().strftime("%Y%m%d_%H%M%S"))

    def append_time_row(self, row: list[str]) -> None:
        self.ensure_started()
        if self.time_entries >= self.max_entries:
            self._rotate_time()
        assert self.time_path is not None
        with self.time_path.open("a", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(row)
        self.time_entries += 1

    def append_event_row(self, row: list[str]) -> None:
        self.ensure_started()
        if self.event_entries >= self.max_entries:
            self._rotate_event()
        assert self.event_path is not None
        with self.event_path.open("a", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(row)
        self.event_entries += 1


def parse_csv_payload(payload: str, expected_len: int) -> list[str] | None:
    try:
        row = next(csv.reader([payload]))
    except Exception:
        return None
    if len(row) != expected_len:
        return None
    return [cell.strip() for cell in row]


def extract_session_tag_from_target(target: str) -> str | None:
    if target == "SERIAL_ONLY":
        return None
    file_name = Path(target).name
    match = SESSION_FROM_FILE_RE.match(file_name)
    if not match:
        return None
    return match.group("tag")


def archive_existing_live_data(base_out_dir: Path) -> None:
    live_dir = base_out_dir / "live"
    archive_dir = base_out_dir / "archive"
    archive_dir.mkdir(parents=True, exist_ok=True)

    if not live_dir.exists():
        return

    has_entries = any(live_dir.iterdir())
    if not has_entries:
        return

    archive_bucket = archive_dir / datetime.now().strftime("capture_%Y%m%d_%H%M%S")
    archive_bucket.mkdir(parents=True, exist_ok=True)

    for child in live_dir.iterdir():
        shutil.move(str(child), str(archive_bucket / child.name))

    print(f"[Capture] Archived previous live data to: {archive_bucket}")


def run_capture(live_dir: Path, max_entries: int) -> int:
    writer = SessionWriter(out_dir=live_dir, max_entries=max(1, max_entries))
    print("[Capture] Waiting for serial lines... (Ctrl+C to stop)")

    for raw_line in sys.stdin:
        line = raw_line.strip()
        if not line:
            continue

        time_match = TIME_LINE_RE.match(line)
        if time_match:
            payload = time_match.group("payload")
            row = parse_csv_payload(payload, expected_len=3)
            if row is None:
                continue
            tag = extract_session_tag_from_target(time_match.group("target"))
            writer.ensure_started(tag)
            writer.append_time_row(row)
            continue

        event_match = EVENT_LINE_RE.match(line)
        if event_match:
            payload = event_match.group("payload")
            row = parse_csv_payload(payload, expected_len=5)
            if row is None:
                continue
            tag = extract_session_tag_from_target(event_match.group("target"))
            writer.start_new_session_if_boot(row, tag)
            writer.ensure_started(tag)
            writer.append_event_row(row)
            continue

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Read ToughTools serial output from stdin and write rotating local CSV files."
    )
    parser.add_argument(
        "--out",
        default="storage_live_output",
        help="Base output directory for live and archive folders",
    )
    parser.add_argument(
        "--max-entries",
        type=int,
        default=1000,
        help="Rotate each file after this many entries",
    )
    parser.add_argument(
        "--reset",
        action="store_true",
        help="Deprecated: kept for compatibility; logs are archived instead of deleted",
    )
    args = parser.parse_args()

    base_out_dir = Path(args.out)
    base_out_dir.mkdir(parents=True, exist_ok=True)
    archive_existing_live_data(base_out_dir)

    live_dir = base_out_dir / "live"
    live_dir.mkdir(parents=True, exist_ok=True)

    try:
        return run_capture(live_dir=live_dir, max_entries=args.max_entries)
    except KeyboardInterrupt:
        print("\n[Capture] Stopped.")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
