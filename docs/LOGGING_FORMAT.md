# ToughTools Data Logging Format

## Overview

This document describes the standardized format for time-series measurements and events logged by the ToughTools application.

## File Structure

### Measurement Logs

**Location:** `/logs/measurements/`

**Naming Convention:** `measurements_YYYY-MM-DD.csv`

- One file per calendar day (UTC+timezone)
- Automatic rollover at midnight
- Clean, date-based naming for easy organization and archival

**Format:**
```
timestamp,measurement_type,value,unit,session_id,source,global_id
2026-05-15T17:37:58Z,temperature,9.6,°C,20260515_173800,device_001,3000
2026-05-15T17:38:58Z,temperature,9.7,°C,20260515_173800,device_001,3001
```

### Field Descriptions

| Field | Type | Description |
|-------|------|-------------|
| `timestamp` | ISO 8601 | UTC timestamp in format `YYYY-MM-DDTHH:MM:SSZ` |
| `measurement_type` | String | Type of measurement (e.g., `temperature`) |
| `value` | Float | Numeric value (precision: 1 decimal place for temperature) |
| `unit` | String | Unit of measurement (e.g., `°C`, `°F`) |
| `session_id` | String | Boot/session identifier in format `YYYYMMDD_HHMMSS` |
| `source` | String | Source/device identifier (e.g., `device_001`) |
| `global_id` | Integer | Global sequential ID for this measurement across all files |

### Event Logs

**Location:** `/logs/events/`

**Naming Convention:** `event_YYYYMMDD_HHMMSS_NNN.csv`

**Format:**
```
timestamp,event,elapsed,temperature,event_id
2026-05-15T17:35:00Z,SESSION_START,00:00:00,0.0,1
2026-05-15T17:45:36Z,TEMP_BELOW_THRESHOLD,00:10:36,68.4,2
```

## Migration Guide

### Old Format → New Format

**Old time log format:**
```
timestamp,temperature,time_id
2026-05-15 17:37:58,9.6,2992
2026-05-15 17:38:58,9.7,2993
```

**New measurement format:**
```
timestamp,measurement_type,value,unit,session_id,source,global_id
2026-05-15T17:37:58Z,temperature,9.6,°C,20260513_152145,device_001,3000
2026-05-15T17:38:58Z,temperature,9.7,°C,20260513_152145,device_001,3001
```

### Migration Tool

Use `tools/migrate_time_logs.py` to convert old `time_*.csv` files to the new format:

```bash
cd ToughTools-main
python3 tools/migrate_time_logs.py
```

This script will:
1. Read all old `time_*.csv` files from `/logs/time/`
2. Convert them to new standardized format
3. Group by date into `measurements_YYYY-MM-DD.csv` files
4. Output to `/logs/measurements/` directory
5. Assign global sequential IDs

## Key Improvements

1. **Self-Documenting Format**: CSV header describes all fields clearly
2. **Date-Based Organization**: One file per calendar day for easy management
3. **Standardized Timestamps**: ISO 8601 UTC format with timezone information
4. **Global Sequencing**: `global_id` provides unique identification across all sessions
5. **Extensible**: Easy to add new measurement types beyond temperature
6. **Source Tracking**: `source` field identifies which device/sensor generated the data
7. **Session Correlation**: `session_id` links measurements to specific boot sessions

## Analysis Examples

### Python - Read measurements from a specific date

```python
import csv
from datetime import datetime

with open('logs/measurements/measurements_2026-05-15.csv', 'r') as f:
    reader = csv.DictReader(f)
    for row in reader:
        ts = datetime.fromisoformat(row['timestamp'].replace('Z', '+00:00'))
        temp = float(row['value'])
        print(f"{ts}: {temp}°C")
```

### SQL - Query measurements across dates

```sql
SELECT 
    timestamp,
    value as temperature,
    session_id
FROM measurements
WHERE measurement_type = 'temperature'
  AND timestamp >= '2026-05-01T00:00:00Z'
  AND timestamp < '2026-06-01T00:00:00Z'
ORDER BY timestamp;
```

## Backward Compatibility

Old logs remain in `/logs/time/` and `/logs/events/` directories. New logging writes to `/logs/measurements/` and `/logs/events/`.

The firmware continues to support both paths during transition period.

## Future Extensions

The new format supports additional measurement types:

- `pressure` - Barometric pressure in hPa
- `humidity` - Relative humidity in %
- `voltage` - Electrical voltage in V
- `current` - Electrical current in A
- etc.

Simply add new rows with appropriate `measurement_type`, `value`, and `unit` values.
