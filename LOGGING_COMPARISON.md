# Vergleich: Alte vs. Neue Time-Logging Struktur

## 📋 Dateibenennungskonvention

### ALT ❌
```
/logs/time/time_20260513_152145_001.csv
/logs/time/time_20260513_152145_002.csv
/logs/time/time_20260513_152145_003.csv
```
**Probleme**:
- Nicht selbsterklärend (was bedeutet `152145`?)
- Session-basiert mit Rotationsnummern
- Mehrere Dateien mit gleichem Zeitstempel

### NEU ✅
```
/logs/measurements/measurements_2026-05-15.csv
/logs/measurements/measurements_2026-06-03.csv
```
**Vorteile**:
- Selbsterklärend (Datum = eine Datei pro Tag)
- Einfache Archivierung nach Datum
- Logische Ordnung

---

## 📊 CSV-Format

### ALT ❌
```csv
timestamp,temperature,time_id
2026-05-15 17:37:58,9.6,2992
2026-05-15 17:38:58,9.7,2993
2026-05-15 17:45:59,9.6,3000
2026-06-03 02:45:36,20.4,2001      ← PROBLEM: time_id resetzt!
2026-06-03 02:53:36,20.4,2009
```

**Probleme**:
- ❌ Keine Spalten-Beschreibung
- ❌ time_id resetzt bei neuer Datei (2001 statt 3001)
- ❌ Zeit springt einen Monat (Lücke unerklärbar)
- ❌ Keine globale eindeutige ID
- ❌ Keine Session-Info

### NEU ✅
```csv
timestamp,measurement_type,value,unit,session_id,source,global_id
2026-05-15T17:37:58Z,temperature,9.6,°C,20260513_152145,device_001,1
2026-05-15T17:38:58Z,temperature,9.7,°C,20260513_152145,device_001,2
2026-05-15T17:45:59Z,temperature,9.6,°C,20260513_152145,device_001,9
2026-06-03T02:45:36Z,temperature,20.4,°C,20260513_152145,device_001,10    ← LÖSUNG: Global ID fortsetzend!
2026-06-03T02:53:36Z,temperature,20.4,°C,20260513_152145,device_001,18
```

**Verbesserungen**:
- ✅ CSV Header erklärt alle Spalten
- ✅ `timestamp`: ISO 8601 UTC Format (international standard)
- ✅ `measurement_type`: Erweiterbar ("temperature", "pressure", "humidity", ...)
- ✅ `value`: Der Messwert
- ✅ `unit`: Einheit (°C, °F, hPa, %, ...)
- ✅ `session_id`: Welche Boot/Session?
- ✅ `source`: Welches Gerät? (device_001, sensor_A, ...)
- ✅ `global_id`: **Eindeutig über ALLE Dateien** (keine Reset mehr!)

---

## 🔍 Datenfluss-Beispiel

### Problem visualisiert
```
Session Start: 2026-05-13 15:21:45
├─ file 001: id 1-XXX
├─ file 002: id 1-XXX      ← Reset!
└─ file 003: id 1-3000     
                ↓ [Monatslücke]
├─ file 003: id 2001-2009  ← Reset nochmal!
```
❌ Wo ist die Lücke? Wie viele Datenpunkte insgesamt?

### Lösung mit global_id
```
Session Start: 2026-05-13 15:21:45
├─ measurements_2026-05-15.csv: global_id 1-9
│  (von alt file 001-003)
                ↓ [Monatslücke identifizierbar]
└─ measurements_2026-06-03.csv: global_id 10-18
   (von alt file 003)
```
✅ Monatslücke ist klar (id 9→10 statt kontinuierlich)
✅ Insgesamt 18 Datenpunkte über alles hinweg
✅ Jede Messung hat eindeutige ID

---

## 🔧 Migrations-Beispiel

### Befehl
```bash
python3 tools/migrate_time_logs.py
```

### Output
```
Found 1 old time log files
Processing: time_20260513_152145_003.csv

Writing measurements_2026-05-15.csv with 9 measurements
Writing measurements_2026-06-03.csv with 9 measurements

✓ Migration complete!
  - Processed 1 old files
  - Generated 2 new measurement files
  - Total measurements: 18
  - Output directory: logs\measurements
```

---

## 📁 Verzeichnis-Struktur

### ALT ❌
```
/logs/
  ├── time/
  │   ├── time_20260417_093820_001.csv
  │   ├── time_20260513_152145_001.csv
  │   ├── time_20260513_152145_002.csv
  │   └── time_20260513_152145_003.csv
  └── events/
      ├── event_20260417_093820_001.csv
      └── event_20260513_152145_001.csv
```

### NEU ✅
```
/logs/
  ├── measurements/
  │   ├── measurements_2026-04-17.csv
  │   ├── measurements_2026-05-15.csv
  │   ├── measurements_2026-05-16.csv
  │   ├── measurements_2026-05-17.csv
  │   └── measurements_2026-06-03.csv
  └── events/
      ├── event_20260417_093820_001.csv
      └── event_20260513_152145_001.csv
```

**Vorteile**:
- Datumsbasiert sortierbar
- Ein Datei pro Tag
- Archivierung nach Datum einfach ("2026-05*" = ganzer Mai)

---

## 📝 Verwendung in Code

### Python
```python
import csv
from datetime import datetime

with open('logs/measurements/measurements_2026-05-15.csv', 'r') as f:
    reader = csv.DictReader(f)  # Header wird automatisch gelesen!
    for row in reader:
        ts = datetime.fromisoformat(row['timestamp'].replace('Z', '+00:00'))
        temp = float(row['value'])
        device = row['source']
        global_id = row['global_id']
        print(f"[{global_id}] {device}: {temp}°C at {ts}")
```

### SQL
```sql
SELECT timestamp, value, source, global_id 
FROM measurements 
WHERE measurement_type = 'temperature'
  AND timestamp >= '2026-05-01'
ORDER BY global_id;
```

---

## ✨ Zusätzliche Features

### Erweiterbar für neue Mestypen
Die neue Struktur unterstützt:
- `temperature` - Temperatur in °C/°F
- `pressure` - Luftdruck in hPa
- `humidity` - Luftfeuchte in %
- `voltage` - Spannung in V
- `current` - Strom in A
- ... beliebig erweiterbar

Beispiel:
```csv
timestamp,measurement_type,value,unit,session_id,source,global_id
2026-06-03T18:33:59Z,temperature,22.5,°C,20260603_183359,device_001,1
2026-06-03T18:34:00Z,humidity,65.0,%,20260603_183359,device_001,2
2026-06-03T18:34:01Z,pressure,1013.25,hPa,20260603_183359,device_001,3
```

### Session Tracking
Mit `session_id` können alle Messungen einer bestimmten Boot/Session gefunden werden:
```sql
SELECT * FROM measurements WHERE session_id = '20260603_183359'
```

### Device Management
Mit `source` können Messungen nach Gerät gefiltert werden:
```sql
SELECT * FROM measurements WHERE source = 'device_001' AND measurement_type = 'temperature'
```
