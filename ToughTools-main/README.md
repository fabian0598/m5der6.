# M5Stack Tough PT100/MAX31865 Reader

PlatformIO project for M5Stack Tough with PT100 temperature polling via MAX31865 over SPI, timer logic, and CSV logging.

The current implementation uses one shared hardware SPI bus for SD and MAX31865. SD writes and PT100 reads are synchronized so that short SD-write phases do not immediately turn into false sensor faults or NaN values on the display.

## Features

- PT100 temperature readout via MAX31865 over one shared hardware SPI bus
- 3-wire PT100 configuration with Adafruit MAX31865
- Timer logic based on configurable threshold
- SD logging with per-boot sessions and file rotation
- macOS live capture fallback when no SD is inserted
- Shared SPI lock with explicit CS HIGH/HIGH safety outside active transfers
- Short sensor-defer window after SD writes to avoid false MAX31865 faults

## Quick Start

1. Open project in VS Code with PlatformIO extension.
2. Build:

```bash
pio run
```

3. Upload:

```bash
pio run --target upload --environment m5stack-tough
```

4. Monitor:

```bash
pio device monitor -b 115200
```

## Real PT100 Temperature Instead Of Simulation

The current code reads a real PT100 through a MAX31865 module over SPI.
The sensor is configured as a 3-wire PT100 with a 430 ohm reference resistor.

### 0) Shared SPI bus rules

The project uses the existing pins from the codebase:

- SCK = GPIO18
- MISO = GPIO38
- MOSI = GPIO23
- SD_CS = GPIO4
- MAX31865_CS = GPIO26

Important bus rules:

- SPI.begin is executed only once during setup.
- SD_CS and MAX31865_CS are both set to OUTPUT and driven HIGH at startup.
- Before every SD operation, MAX31865_CS stays HIGH.
- Before every MAX31865 operation, SD_CS stays HIGH.
- A shared SPI lock protects SD and MAX31865 access so that both chips never drive the bus at the same time.
- The code keeps the bus idle as SD=HIGH and MAX=HIGH outside active transfers.

Practical effect:

- While SD is writing a CSV line, the PT100 poll is deferred briefly.
- Short SD-related glitches are masked first, so the last valid temperature can stay visible instead of immediately switching to NaN.
- SENSOR_LOST is only raised after repeated failed reads, not after a single short disturbance.

### 1) Hardware wiring

Use the exact MAX31865-to-M5 wiring below. This SPI wiring does not change between 2-wire and 3-wire PT100 sensors:

| MAX31865 signal | M5Stack Tough connection |
|---|---|
| GND | GND |
| VIN | 3V3 preferred |
| SCK / CLK | GPIO18 |
| SDO / MISO | GPIO38 |
| SDI / MOSI | GPIO23 |
| CS | GPIO26 |

The internal SD card uses the same SCK/MISO/MOSI lines and its own CS on GPIO4.


Power the MAX31865 module from 3.3V on the ESP32 side. Use 5V only if your specific MAX31865 board has a regulator and is documented for 5V input.

The PT100 itself must be wired to the MAX31865 in the sensor's 3-wire RTD configuration. On typical MAX31865 breakout boards this means using the board's documented 3-wire terminal wiring and enabling/soldering the 3-wire jumper or bridge if the board requires it.

### 2) PT100 wire-mode configuration

The current branch is configured for 3-wire PT100 sensors in `include/config.h`:

```cpp
constexpr Pt100WireMode PT100_WIRE_MODE = Pt100WireMode::ThreeWire;
```

The active setup code maps this to `MAX31865_3WIRE` in `src/modbus_service.cpp`.

If your MAX31865 board uses a different reference resistor, adapt only the reference value in `config.h`.

### 4) Verify it is really reading live Modbus

1. Start monitor.
2. Confirm values change with real PT100 temperature.
3. Confirm fault logs stay silent during normal operation.

## Storage Modes And Exactly What Happens

### Mode A: SD inserted (device-side storage)

Firmware writes on SD with this structure:

```text
/settings.csv
/logs/time/time_YYYYMMDD_HHMMSS_001.csv
/logs/events/event_YYYYMMDD_HHMMSS_001.csv
/event_log.csv              # fallback only, if session event file cannot be written
```


Rules:

1. New boot creates a new session tag (`YYYYMMDD_HHMMSS`).
2. Files rotate to `_002`, `_003`, ... after `LOG_FILE_MAX_ENTRIES` (default 1000).
3. `time_id` and `event_id` do not create new files by themselves.
4. Existing log files are not auto-deleted by firmware.
5. `settings.csv` is replaced when settings are saved again.
6. SD writes are wrapped in the shared SPI lock and happen with MAX31865_CS held HIGH.
7. When a time/event CSV line is written, the PT100 poll is briefly deferred so the display keeps the last valid value through short bus disturbances.

### Mode B: No SD inserted (Mac live capture)

Firmware prints `SERIAL_ONLY` lines to serial. Persist them on Mac with:

```bash
pio device monitor -b 115200 | python3 tools/serial_live_capture.py
```

Or via VS Code task:

```bash
pio device monitor -b 115200 | python3 tools/serial_live_capture.py
```

Mac output structure:

```text
storage_live_output/live/logs/time/*.csv
storage_live_output/live/logs/events/*.csv
storage_live_output/archive/capture_YYYYMMDD_HHMMSS/
```

On every new capture start, old `live` content is moved to timestamped archive folder.

### Mode C: Format simulation only (offline test)

Use this only to generate example files without device serial stream:

```bash
python3 tools/storage_format_sim.py --reset
```

### Mode D: WLAN backup from inserted SD

If `wifi_secrets.h` contains valid WLAN credentials, the firmware can start a small HTTP backup server after WiFi connects. Tap the **WEB** button on the live screen to enable it for 10 minutes. Tap **WEB** again to switch it off earlier. The serial monitor prints the device URL:

```text
[Backup] HTTP server ready: http://192.168.x.x:80/
```

Open the URL in a browser, sign in with username `hermetia` and password `0406`, then click **Download full backup**. The M5 streams a TAR archive containing:

```text
/settings.csv
/logs/time/*.csv
/logs/events/*.csv
```

On macOS, unpack the downloaded archive with:

```bash
tar -xf m5_sd_backup.tar
```

Alternatively, download all exposed SD files into a local folder from the command line:

```bash
python3 tools/download_backup.py http://192.168.x.x ./m5_sd_backup
```

The script reads:

```text
/manifest.json
```

and downloads:

```text
/settings.csv
/logs/time/*.csv
/logs/events/*.csv
```

Individual files are served through:

```text
/download?path=/logs/time/example.csv
```

The single browser archive is served through:

```text
/backup.tar
```

The firmware holds the shared SPI lock while reading SD files, so PT100 polling and normal logging wait until the active HTTP file transfer is done.

Event logs are written to SD from `Info` level upward. `Debug` events are still shown on Serial/display only, so frequent noise such as sensor reconnect chatter does not fill the SD card.

### Manual timer reset

The live screen has an **RST** button in the top button row. Tapping it opens a confirmation dialog. Confirming with **YES** resets the active countdown to `01:00:00` and writes a `TIMER_MANUAL_RESET` event. **NO** or tapping outside the dialog leaves the timer unchanged.

## How To Check If And Where Data Is Saved

1. Watch serial target path in each line.
2. `[TimeLog][/logs/time/...]` or `[EventLog][/logs/events/...]` means SD write target.
3. `[...][SERIAL_ONLY]` means no SD write, serial fallback only.
4. For Mac capture, confirm files appear under `storage_live_output/live/logs/time` and `storage_live_output/live/logs/events`.
5. If capture is restarted, previous files should appear under `storage_live_output/archive`.

## Event Log Semantics (Threshold)

Threshold transition events are now threshold-neutral:

```text
TEMP_BELOW_THRESHOLD
TEMP_ABOVE_THRESHOLD
TIMER_EXPIRED
TIMER_RESTART
TIMER_MANUAL_RESET
SENSOR_OK
SENSOR_LOST
SETTINGS_SAVED
SHUTDOWN
BOOT
SESSION_START
```

This avoids hardcoding a specific value in event names.

## Key Configuration

Main constants in include/config.h:

```cpp
constexpr Pt100WireMode PT100_WIRE_MODE = Pt100WireMode::ThreeWire;
constexpr unsigned long LOG_INTERVAL_MS = 60000;
constexpr float TEMPERATURE_RESTART_THRESHOLD = 70.0f;
constexpr const char *TIME_LOG_DIR_PATH = "/logs/time";
constexpr const char *EVENT_LOG_DIR_PATH = "/logs/events";
constexpr LogLevel SD_EVENT_LOG_MIN_LEVEL = LogLevel::Info;
constexpr unsigned long HTTP_BACKUP_ACTIVE_WINDOW_MS = 10UL * 60UL * 1000UL;
```

## Troubleshooting

### No real PT100 temperature yet

1. Check the MAX31865 SPI wiring and the CS/SCK/MISO/MOSI pins.
2. Verify the PT100 is connected in 3-wire mode on the module and the MAX31865 board's 3-wire jumper/bridge is set correctly.
3. Verify the board is powered from 3.3 V, not 5 V.
4. Check monitor output for `[PT100] MAX31865 fault:`.
5. If the temperature looks off, verify `MAX31865_RREF` matches the actual reference resistor on the module.
6. If the reading is consistently too high or too low, verify `MAX31865_RREF`, the PT100 wiring mode, and the actual reference resistor on the MAX31865 board.

### Not writing to SD

1. Check SD is inserted and readable (FAT32 recommended).
2. Check monitor for `[Logger] SD card ready`.
3. Check monitor target path is not `SERIAL_ONLY`.
4. Make sure the bus logs show the shared SPI lock and both CS lines returning to HIGH after transfers.

### No Mac files in no-SD mode

1. Use piped command or VS Code live-capture task.
2. Ensure no other monitor process is blocking the serial port.
3. Check `storage_live_output/live` and `storage_live_output/archive`.

### MAX31865 faults during SD logging

If you see faults like `0x08` or `0x18` only around SD writes, that usually means the read was disturbed during a shared-bus transfer and not necessarily a permanent hardware fault.

1. Check that the log shows shared SPI lock activity for both SD and MAX31865.
2. Check that both CS lines return to HIGH outside transfers.
3. The code keeps the last valid temperature visible for short disturbances and only marks the sensor lost after repeated failures.
4. The MAX31865 read path retries once after `clearFault()` before giving up.

## RTD Reference Values

The PT100 sensor was checked against reference resistance values at multiple temperatures.

**Hardware Reference:**
- PT100 resistance at 22.1°C: **108.8 Ω** (theoretical: ~108.6 Ω)
- PT100 resistance at 40.0°C: **115.6 Ω** (theoretical: ~115.5 Ω)
- PT100 resistance at 70.0°C: **127.4 Ω** (theoretical: ~127.2 Ω)

No software temperature offset is applied. The firmware uses the raw temperature calculated by the Adafruit MAX31865 library from `MAX31865_RTD_NOMINAL` and `MAX31865_RREF`.

## References
