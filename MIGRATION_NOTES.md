# Time-Logging Struktur - Überarbeitungszusammenfassung

## Problem (Ursprünglich)

Der Benutzer meldete große Lücken im Time-Logging mit einem Monatsprung zwischen 2026-05-15 und 2026-06-03. Die Struktur hatte mehrere Probleme:

1. **Unklar Dateibenennungskonvention**: `time_20260513_152145_003` nicht selbsterklärend
2. **Keine CSV-Header**: Format nicht dokumentiert (`timestamp,temperature,time_id`)
3. **Resetting time_id**: Lokale Sequenznummer wurde bei jeder neuen Datei zurückgesetzt (3000 → 2001)
4. **Keine globale eindeutige ID**: Unmöglich, Messungen über Dateien hinweg eindeutig zu identifizieren
5. **Mehrere Dateien pro Session**: Sequenznummern ohne Kontext

## Lösung (Implementiert)

### 1. Neue Dateibenennungskonvention
- **Alt**: `time_YYYYMMDD_HHMMSS_NNN.csv` (sessionbasiert mit Rotationsnummer)
- **Neu**: `measurements_YYYY-MM-DD.csv` (datumsbasiert)

**Vorteile**:
- Selbsterklärend durch ISO 8601 Format
- Eine Datei pro Kalendertag
- Logische Trennung und Archivierung

### 2. Neues CSV-Format mit Header
```
timestamp,measurement_type,value,unit,session_id,source,global_id
2026-05-15T17:37:58Z,temperature,9.6,°C,20260513_152145,device_001,3000
2026-05-15T17:38:58Z,temperature,9.7,°C,20260513_152145,device_001,3001
```

**Felderbeschreibung**:
- `timestamp`: ISO 8601 UTC Format
- `measurement_type`: Art der Messung (z.B. "temperature")
- `value`: Messwert
- `unit`: Einheit (°C, °F, etc.)
- `session_id`: Boot/Session-ID
- `source`: Geräte-ID
- `global_id`: **Globale eindeutige Sequenznummer über alle Dateien**

### 3. Geänderte Dateien

#### C++ Firmware (src/logger.cpp)
- Header aktualisiert: `MEASUREMENTS_LOG_HEADER`
- Dateibenennungskonvention: `measurements_YYYY-MM-DD.csv`
- CSV-Format: Neue Spalten + Header
- Logging-Ausgabe: Neues Format auf Serial-Port

#### C++ Config (include/config.h)
- `TIME_LOG_DIR_PATH` → `MEASUREMENTS_LOG_DIR_PATH`
- Neue Verzeichnis: `/logs/measurements/`

#### Python Tools (tools/storage_format_sim.py)
- `TimeLogSample` → `MeasurementSample` Dataclass
- Neue `write_measurement_log()` Funktion
- Date-basierte Dateibenennungskonvention
- Global ID Support

#### Neue Migration Tool (tools/migrate_time_logs.py)
- Konvertiert alle alten `time_*.csv` Dateien
- Gruppiert nach Datum in neue Dateien
- Assigniert globale IDs
- Erhält Daten + Session-Info

### 4. Dokumentation
- **docs/LOGGING_FORMAT.md**: Vollständige Dokumentation des neuen Formats
  - Feld-Beschreibungen
  - Migration Guide
  - Analyse-Beispiele
  - Backward Compatibility Info

## Migrationsergebnis (Getestet)

### Original Datadatei
```
2026-05-15 17:37:58,9.6,2992
...
2026-05-15 17:45:59,9.6,3000
2026-06-03 02:45:36,20.4,2001      ← Zeit springt + ID resetzt!
...
2026-06-03 02:53:36,20.4,2009
```

### Nach Migration in 2 Dateien
**measurements_2026-05-15.csv**:
```
2026-05-15T17:37:58Z,temperature,9.6,°C,time_20260513_152145_003,device_001,1
...
2026-05-15T17:45:59Z,temperature,9.6,°C,time_20260513_152145_003,device_001,9
```

**measurements_2026-06-03.csv**:
```
2026-06-03T02:45:36Z,temperature,20.4,°C,time_20260513_152145_003,device_001,10   ← Global ID fortsetzend!
...
2026-06-03T02:53:36Z,temperature,20.4,°C,time_20260513_152145_003,device_001,18
```

## Verwendung der Migration

```bash
cd ToughTools-main
python3 tools/migrate_time_logs.py
```

Das Script:
1. Liest alle `time_*.csv` aus `/logs/time/`
2. Konvertiert in neue Struktur
3. Gruppiert nach Datum
4. Schreibt nach `/logs/measurements/`
5. Assigniert globale IDs

## Neue Funktionalität

✅ Selbsterklärende Dateibenennungskonvention
✅ CSV-Header mit dokumentierten Feldern
✅ Globale eindeutige IDs
✅ ISO 8601 Timestamps
✅ Erweiterbar für weitere Mestypen
✅ Session-Tracking
✅ Device-Identifikation

## Backward Compatibility

- Alte Dateien bleiben unter `/logs/time/` und `/logs/events/`
- Neue Firmware schreibt unter `/logs/measurements/`
- Beide Verzeichnisse können parallel existieren
- Migration kann jederzeit durchgeführt werden
