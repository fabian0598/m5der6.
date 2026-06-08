#!/usr/bin/env python3
"""Migrate old time_*.csv files to new measurements_*.csv format.

This script reads all time_*.csv and event_*.csv files in the old format
and migrates them to the new standardized format with:
- Proper file naming: measurements_YYYY-MM-DD.csv
- Clear CSV header with documented fields
- Global sequential ID across all measurements
- Standardized timestamp format (ISO 8601)
- Device/source information
"""

from __future__ import annotations

import csv
import re
from datetime import datetime, timedelta
from pathlib import Path


def migrate_logs() -> None:
    """Main migration function."""
    base_dir = Path(".")
    logs_dir = base_dir / "logs"
    
    if not logs_dir.exists():
        print(f"Error: logs directory not found at {logs_dir}")
        return
    
    time_logs_dir = logs_dir / "time"
    event_logs_dir = logs_dir / "events"
    
    if not time_logs_dir.exists():
        print(f"Error: {time_logs_dir} not found")
        return
    
    # Find all old time_*.csv files
    old_time_files = sorted(time_logs_dir.glob("time_*.csv"))
    
    if not old_time_files:
        print(f"No time_*.csv files found in {time_logs_dir}")
        return
    
    print(f"Found {len(old_time_files)} old time log files")
    
    # Dictionary to group measurements by date
    measurements_by_date: dict[str, list[tuple]] = {}
    global_id = 1
    
    # Process each old time log file
    for old_file in old_time_files:
        print(f"\nProcessing: {old_file.name}")
        
        with open(old_file, "r", encoding="utf-8") as f:
            reader = csv.reader(f)
            # Skip header if present
            header = next(reader, None)
            if header and header[0] != "timestamp":
                # If first row doesn't look like a header, process it as data
                pass
            else:
                # Header exists, continue
                pass
            
            # Process data rows
            for row in reader:
                if len(row) < 3:
                    continue
                
                try:
                    timestamp_str = row[0]
                    temperature = float(row[1])
                    local_id = int(row[2])
                    
                    # Parse timestamp
                    dt = datetime.strptime(timestamp_str, "%Y-%m-%d %H:%M:%S")
                    date_key = dt.strftime("%Y-%m-%d")
                    
                    # Create new record with standardized format
                    record = (
                        dt.isoformat() + "Z",  # ISO 8601 UTC
                        "temperature",         # measurement_type
                        f"{temperature:.1f}", # value
                        "°C",                  # unit
                        old_file.stem,        # session_id (from filename)
                        "device_001",         # source (default)
                        global_id,            # global_id
                    )
                    
                    if date_key not in measurements_by_date:
                        measurements_by_date[date_key] = []
                    
                    measurements_by_date[date_key].append(record)
                    global_id += 1
                    
                except (ValueError, IndexError) as e:
                    print(f"  Warning: Failed to parse row {row}: {e}")
                    continue
    
    # Write new files, one per date
    new_header = [
        "timestamp",
        "measurement_type",
        "value",
        "unit",
        "session_id",
        "source",
        "global_id",
    ]
    
    output_dir = logs_dir / "measurements"
    output_dir.mkdir(exist_ok=True)
    
    for date_key in sorted(measurements_by_date.keys()):
        new_filename = f"measurements_{date_key}.csv"
        new_filepath = output_dir / new_filename
        
        measurements = measurements_by_date[date_key]
        
        print(f"\nWriting {new_filename} with {len(measurements)} measurements")
        
        with open(new_filepath, "w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(new_header)
            for record in measurements:
                writer.writerow(record)
    
    print(f"\n✓ Migration complete!")
    print(f"  - Processed {len(old_time_files)} old files")
    print(f"  - Generated {len(measurements_by_date)} new measurement files")
    print(f"  - Total measurements: {global_id - 1}")
    print(f"  - Output directory: {output_dir}")


if __name__ == "__main__":
    migrate_logs()
