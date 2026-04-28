#include "logger.h"
#include "config.h"
#include "spi_bus_lock.h"
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <cstdio>
#include <ctime>

// SD card library - may vary depending on M5Stack setup
// #include <SD.h>
// #include <FS.h>

/**
 * Logger implementation for SD card logging.
 */

namespace
{
    constexpr uint8_t SD_FAILURE_THRESHOLD = 3;
    constexpr unsigned long SD_REINIT_COOLDOWN_MS = 30000UL;
    // Keep this short so display updates stay responsive while still decoupling SD/MAX traffic.
    constexpr unsigned long SENSOR_POLL_DEFER_AFTER_SD_WRITE_MS = 280UL;

    void ensure_sd_spi_active()
    {
        prepare_sd_bus_locked();
    }

    bool should_emit_level(LogLevel level, LogLevel minimum)
    {
        return static_cast<uint8_t>(level) >= static_cast<uint8_t>(minimum);
    }

    const char *log_level_to_string(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warning:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        }
        return "INFO";
    }

    const char *sd_card_type_to_string(uint8_t card_type)
    {
        switch (card_type)
        {
        case CARD_MMC:
            return "MMC";
        case CARD_SD:
            return "SDSC";
        case CARD_SDHC:
            return "SDHC/SDXC";
        default:
            return "UNKNOWN";
        }
    }

    bool mount_sd_with_fallback_clock_locked(const char *tag)
    {
        ensure_sd_spi_active();
        Serial.printf("[%s] SD.begin(CS=%d) - using SD SPI bus...\n", tag, SD_CARD_CS_PIN);

        // Some cards need a short settle time right after boot/reset.
        for (int attempt = 1; attempt <= 3; ++attempt)
        {
            ensure_sd_spi_active();
            SD.end();
            if (SD.begin(SD_CARD_CS_PIN, SPI, SD_CARD_SPI_HZ_PRIMARY))
            {
                Serial.printf("[%s] SD mount ok (SPI) on attempt %d\n", tag, attempt);
                return true;
            }

            Serial.printf("[%s] SD mount failed on attempt %d, retrying...\n", tag, attempt);
            SD.end();
            delay(120);
        }

        // Fallback: reduced clock.
        Serial.printf("[%s] Trying SD.begin() with fallback clock...\n", tag);
        ensure_sd_spi_active();
        SD.end();
        if (SD.begin(SD_CARD_CS_PIN, SPI, SD_CARD_SPI_HZ_FALLBACK))
        {
            Serial.printf("[%s] SD mount ok (fallback SPI clock)\n", tag);
            return true;
        }

        // Last-resort recovery: very low SPI clock.
        Serial.printf("[%s] Trying SD.begin() with recovery clock...\n", tag);
        ensure_sd_spi_active();
        SD.end();
        delay(40);
        if (SD.begin(SD_CARD_CS_PIN, SPI, SD_CARD_SPI_HZ_RECOVERY))
        {
            Serial.printf("[%s] SD mount ok (recovery SPI clock)\n", tag);
            return true;
        }

        return false;
    }

    bool mount_sd_with_fallback_clock(const char *tag)
    {
        SpiBusLock bus_lock(SpiBusOwner::Sd);
        return mount_sd_with_fallback_clock_locked(tag);
    }

    void print_sd_debug_info(const char *tag)
    {
        SpiBusLock bus_lock(SpiBusOwner::Sd);
        ensure_sd_spi_active();
        const uint8_t card_type = SD.cardType();
        if (card_type == CARD_NONE)
        {
            Serial.printf("[%s] cardType: CARD_NONE\n", tag);
            return;
        }

        Serial.printf("[%s] cardType: %s (%u)\n", tag, sd_card_type_to_string(card_type), static_cast<unsigned int>(card_type));
        Serial.printf("[%s] cardSize: %llu MB\n", tag, SD.cardSize() / (1024ULL * 1024ULL));

        File root = SD.open("/");
        if (!root || !root.isDirectory())
        {
            Serial.printf("[%s] root open failed\n", tag);
        }
        else
        {
            Serial.printf("[%s] root open ok\n", tag);
            int shown = 0;
            while (shown < 10)
            {
                File entry = root.openNextFile();
                if (!entry)
                {
                    break;
                }
                Serial.printf("[%s] root entry: %s (%s)\n", tag, entry.name(), entry.isDirectory() ? "dir" : "file");
                entry.close();
                shown++;
            }
        }
        root.close();

        if (SD.exists("/test.txt"))
        {
            File test_file = SD.open("/test.txt", FILE_READ);
            if (test_file)
            {
                Serial.printf("[%s] /test.txt found, first bytes: ", tag);
                int count = 0;
                while (test_file.available() && count < 80)
                {
                    Serial.write(test_file.read());
                    count++;
                }
                Serial.println();
                test_file.close();
            }
            else
            {
                Serial.printf("[%s] /test.txt exists but open failed\n", tag);
            }
        }
        else
        {
            Serial.printf("[%s] /test.txt not found\n", tag);
        }
    }
}

void Logger::note_sd_success()
{
    consecutive_sd_failures = 0;
}

void Logger::note_sd_failure(const char *context)
{
    SpiBusLock bus_lock(SpiBusOwner::Sd);
    note_sd_failure_locked(context);
}

void Logger::note_sd_failure_locked(const char *context)
{
    if (consecutive_sd_failures < 255)
    {
        consecutive_sd_failures++;
    }

    Serial.printf("[Logger] SD failure (%s), count=%u/%u\n",
                  context,
                  static_cast<unsigned int>(consecutive_sd_failures),
                  static_cast<unsigned int>(SD_FAILURE_THRESHOLD));

    if (consecutive_sd_failures >= SD_FAILURE_THRESHOLD)
    {
        const uint8_t card_type = SD.cardType();
        if (card_type == CARD_NONE)
        {
            Serial.println("[Logger] SD cardType=CARD_NONE after repeated failures -> SERIAL_ONLY");
            sd_ready = false;
        }
        else
        {
            const unsigned long now = millis();
            if (sd_reinit_in_progress)
            {
                return;
            }

            if ((now - last_sd_reinit_attempt_ms) < SD_REINIT_COOLDOWN_MS)
            {
                Serial.println("[Logger] SD card present; reinit cooldown active, keep mounted state");
                consecutive_sd_failures = SD_FAILURE_THRESHOLD;
                return;
            }

            last_sd_reinit_attempt_ms = now;
            sd_reinit_in_progress = true;

            Serial.println("[Logger] SD card present but IO failing -> attempting in-place SD reinit");
            const bool remounted = mount_sd_with_fallback_clock_locked("Logger-Reinit");
            const bool dirs_ok = remounted && prepare_log_directories_locked();

            if (remounted && dirs_ok)
            {
                note_sd_success();
                Serial.println("[Logger] SD in-place reinit successful");
                if (session_tag[0] == '\0')
                {
                    start_session_locked(time(nullptr));
                }
            }
            else
            {
                Serial.println("[Logger] SD in-place reinit failed -> SERIAL_ONLY until periodic recovery");
                sd_ready = false;
            }

            sd_reinit_in_progress = false;
        }
    }
}

bool Logger::prepare_log_directories()
{
    SpiBusLock bus_lock(SpiBusOwner::Sd);
    return prepare_log_directories_locked();
}

bool Logger::prepare_log_directories_locked()
{
    ensure_sd_spi_active();
    constexpr const char *LOG_ROOT_DIR_PATH = "/logs";

    if (!SD.exists(LOG_ROOT_DIR_PATH) && !SD.mkdir(LOG_ROOT_DIR_PATH))
    {
        Serial.printf("[Logger] Failed to create dir: %s\n", LOG_ROOT_DIR_PATH);
        return false;
    }

    if (!SD.exists(TIME_LOG_DIR_PATH) && !SD.mkdir(TIME_LOG_DIR_PATH))
    {
        Serial.printf("[Logger] Failed to create dir: %s\n", TIME_LOG_DIR_PATH);
        return false;
    }

    if (!SD.exists(EVENT_LOG_DIR_PATH) && !SD.mkdir(EVENT_LOG_DIR_PATH))
    {
        Serial.printf("[Logger] Failed to create dir: %s\n", EVENT_LOG_DIR_PATH);
        return false;
    }

    return true;
}

void Logger::try_recover_sd()
{
    if (sd_ready)
    {
        return;
    }

    const unsigned long now = millis();
    constexpr unsigned long RETRY_INTERVAL_MS = 15000UL;
    if ((now - last_sd_recovery_attempt_ms) < RETRY_INTERVAL_MS)
    {
        return;
    }
    last_sd_recovery_attempt_ms = now;

    sd_reinit_in_progress = true;
    SpiBusLock bus_lock(SpiBusOwner::Sd);

    Serial.println("[Logger] SD recovery attempt...");
    if (!mount_sd_with_fallback_clock_locked("Logger-Recover"))
    {
        Serial.println("[Logger] SD recovery failed, staying in SERIAL_ONLY");
        sd_reinit_in_progress = false;
        return;
    }

    if (!prepare_log_directories_locked())
    {
        Serial.println("[Logger] SD recovery mount ok, but directory prep failed");
        sd_reinit_in_progress = false;
        return;
    }

    sd_ready = true;
    note_sd_success();
    Serial.println("[Logger] SD recovery successful - switching from SERIAL_ONLY to SD");

    // Create a fresh session tag if no session has been created yet.
    if (session_tag[0] == '\0')
    {
        time_t now_ts = time(nullptr);
        start_session_locked(now_ts);
    }

    sd_reinit_in_progress = false;
}

void Logger::rotate_time_log_file()
{
    time_log_file_index++;
    time_log_entries_in_file = 0;
    snprintf(
        current_time_log_path,
        sizeof(current_time_log_path),
        "%s/time_%s_%03lu.csv",
        TIME_LOG_DIR_PATH,
        session_tag,
        time_log_file_index);
}

void Logger::rotate_event_log_file()
{
    event_log_file_index++;
    event_log_entries_in_file = 0;
    snprintf(
        current_event_log_path,
        sizeof(current_event_log_path),
        "%s/event_%s_%03lu.csv",
        EVENT_LOG_DIR_PATH,
        session_tag,
        event_log_file_index);
}

bool Logger::ensure_log_file(bool for_time_log)
{
    SpiBusLock bus_lock(SpiBusOwner::Sd);
    return ensure_log_file_locked(for_time_log);
}

bool Logger::ensure_log_file_locked(bool for_time_log)
{
    ensure_sd_spi_active();
    const char *path = for_time_log ? current_time_log_path : current_event_log_path;
    const char *header = for_time_log ? "timestamp,temperature,time_id" : "timestamp,event,elapsed,temperature,event_id";

    if (path[0] == '\0')
    {
        return false;
    }

    if (SD.exists(path))
    {
        note_sd_success();
        return true;
    }

    File file = SD.open(path, FILE_WRITE);
    if (!file)
    {
        Serial.printf("[Logger] Failed to create file: %s\n", path);
        note_sd_failure_locked("create-file");
        return false;
    }

    file.println(header);
    file.close();
    note_sd_success();

    char *last_announced_path = for_time_log ? last_announced_time_log_path : last_announced_event_log_path;
    if (strncmp(last_announced_path, path, sizeof(current_time_log_path)) != 0)
    {
        strlcpy(last_announced_path, path, sizeof(current_time_log_path));
        Serial.printf("[Logger] New %s file: %s\n", for_time_log ? "time" : "event", path);
    }
    return true;
}

void Logger::init()
{
    Serial.println("[Logger] Initializing SD card...");

    if (!mount_sd_with_fallback_clock("Logger"))
    {
        Serial.println("[Logger] No SD card detected - logging to Serial only");
        sd_ready = false;
        return;
    }

    sd_ready = true;
    note_sd_success();
    Serial.println("[Logger] SD card ready");
    print_sd_debug_info("Logger");

    if (!prepare_log_directories())
    {
        note_sd_failure("prepare-dirs-init");
        sd_ready = false;
        return;
    }
}

void Logger::start_session(time_t session_start_time)
{
    if (!sd_ready)
    {
        return;
    }

    SpiBusLock bus_lock(SpiBusOwner::Sd);
    start_session_locked(session_start_time);
}

void Logger::start_session_locked(time_t session_start_time)
{
    tm *timeinfo = localtime(&session_start_time);
    if (timeinfo != nullptr)
    {
        strftime(session_tag, sizeof(session_tag), "%Y%m%d_%H%M%S", timeinfo);
    }
    else
    {
        snprintf(session_tag, sizeof(session_tag), "boot_%lu", millis());
    }

    time_log_file_index = 0;
    event_log_file_index = 0;
    rotate_time_log_file();
    rotate_event_log_file();

    if (!ensure_log_file_locked(true) || !ensure_log_file_locked(false))
    {
        note_sd_failure_locked("start-session-create");
    }
}

void Logger::log_time_sample(time_t timestamp, float temperature, unsigned long time_id)
{
    if (!LOGGING_ENABLED)
    {
        return;
    }

    try_recover_sd();

    const char *time_str = format_timestamp(timestamp);
    const char *target_path = (sd_ready && current_time_log_path[0] != '\0') ? current_time_log_path : "SERIAL_ONLY";

    Serial.printf("[TimeLog][%s] %s,%.1f,%lu\n", target_path, time_str, temperature, time_id);

    if (!sd_ready)
    {
        return;
    }

    sd_io_in_progress = true;

    SpiBusLock bus_lock(SpiBusOwner::Sd);

    if (time_log_entries_in_file >= LOG_FILE_MAX_ENTRIES)
    {
        rotate_time_log_file();
    }

    if (!ensure_log_file_locked(true))
    {
        sd_io_in_progress = false;
        sensor_poll_block_until_ms = millis() + SENSOR_POLL_DEFER_AFTER_SD_WRITE_MS;
        return;
    }

    ensure_sd_spi_active();
    File log_file = SD.open(current_time_log_path, FILE_APPEND);
    if (log_file)
    {
        log_file.printf("%s,%.1f,%lu\n", time_str, temperature, time_id);
        log_file.flush();
        log_file.close();
        time_log_entries_in_file++;
        note_sd_success();
    }
    else
    {
        Serial.printf("[Logger] Failed to open time log file: %s\n", current_time_log_path);
        note_sd_failure_locked("open-time-log");
    }

    sd_io_in_progress = false;
    sensor_poll_block_until_ms = millis() + SENSOR_POLL_DEFER_AFTER_SD_WRITE_MS;
}

void Logger::log_event(time_t timestamp, const char *event_name, unsigned long elapsed_seconds, float temperature, unsigned long event_id, LogLevel level)
{
    if (!LOGGING_ENABLED)
    {
        return;
    }

    const char *time_str = format_timestamp(timestamp);
    const unsigned long hours = elapsed_seconds / 3600UL;
    const unsigned long minutes = (elapsed_seconds % 3600UL) / 60UL;
    const unsigned long seconds = elapsed_seconds % 60UL;
    const bool write_to_sd = should_emit_level(level, SD_EVENT_LOG_MIN_LEVEL);
    const char *target_path = (write_to_sd && sd_ready && current_event_log_path[0] != '\0') ? current_event_log_path : "SCREEN_ONLY";

    if (should_emit_level(level, SERIAL_LOG_MIN_LEVEL))
    {
        Serial.printf("[EventLog][%s][%s] %s,%s,%02lu:%02lu:%02lu,%.1f,%lu\n",
                      target_path,
                      log_level_to_string(level),
                      time_str,
                      event_name,
                      hours,
                      minutes,
                      seconds,
                      temperature,
                      event_id);
    }

    if (!write_to_sd)
    {
        return;
    }

    try_recover_sd();

    if (!sd_ready)
    {
        return;
    }

    sd_io_in_progress = true;

    SpiBusLock bus_lock(SpiBusOwner::Sd);

    if (event_log_entries_in_file >= LOG_FILE_MAX_ENTRIES)
    {
        rotate_event_log_file();
    }

    if (!ensure_log_file_locked(false))
    {
        sd_io_in_progress = false;
        sensor_poll_block_until_ms = millis() + SENSOR_POLL_DEFER_AFTER_SD_WRITE_MS;
        return;
    }

    ensure_sd_spi_active();
    File log_file = SD.open(current_event_log_path, FILE_APPEND);
    if (log_file)
    {
        log_file.printf("%s,%s,%02lu:%02lu:%02lu,%.1f,%lu\n",
                        time_str,
                        event_name,
                        hours,
                        minutes,
                        seconds,
                        temperature,
                        event_id);
        log_file.flush();
        log_file.close();
        event_log_entries_in_file++;
        note_sd_success();
    }
    else
    {
        Serial.printf("[Logger] Failed to open event log file: %s\n", current_event_log_path);
        note_sd_failure_locked("open-event-log");
    }

    sd_io_in_progress = false;
    sensor_poll_block_until_ms = millis() + SENSOR_POLL_DEFER_AFTER_SD_WRITE_MS;
}

bool Logger::is_ready() const
{
    return sd_ready;
}

bool Logger::should_defer_sensor_poll() const
{
    return sd_reinit_in_progress || sd_io_in_progress || (millis() < sensor_poll_block_until_ms);
}

const char *Logger::format_timestamp(time_t timestamp)
{
    static char buffer[32];
    struct tm *timeinfo = localtime(&timestamp);
    if (timeinfo == nullptr)
    {
        strlcpy(buffer, "---- -- -- --:--:--", sizeof(buffer));
        return buffer;
    }

    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
    return buffer;
}
