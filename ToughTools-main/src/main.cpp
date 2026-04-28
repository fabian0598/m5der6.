#include <Arduino.h>
#include <ctime>
#include <M5Unified.h>
#include <cstdio>
#include <cstring>
#include <WiFi.h>
#include <SD.h>
#include <SPI.h>
#include <esp_system.h>
#include <esp_sntp.h>
#include "config.h"
#include "app_state.h"
#include "modbus_service.h"
#include "display_service.h"
#include "logger.h"
#include "spi_bus_lock.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#define WIFI_SECRETS_AVAILABLE 1
#else
#define WIFI_SECRETS_AVAILABLE 0
#endif

/**
 * M5Stack Tough PT100/MAX31865 Application
 *
 * Main application logic.
 * Orchestrates:
 * - PT100 temperature acquisition via MAX31865 over SPI
 * - Display updates
 * - SD card logging
 * - Countdown timer management
 */

// Global service instances
ModbusService modbus_service;
DisplayService display_service;
Logger logger;
AppState app_state;

// Timing state for main loop
bool was_below_threshold = false;
bool was_above_threshold = false;
unsigned long boot_epoch_seconds = 0;
unsigned long boot_millis = 0;

namespace
{
    enum class NtpSyncState
    {
        Disabled,
        Idle,
        ConnectingWifi,
        WaitingForTime,
        Backoff
    };

    NtpSyncState ntp_sync_state = NtpSyncState::Idle;
    unsigned long ntp_state_started_ms = 0;
    unsigned long ntp_next_attempt_ms = 0;
    bool ntp_sync_verbose = false;
    bool ntp_missing_credentials_reported = false;

    bool ensure_sd_available_for_settings(bool verbose)
    {
        SpiBusLock bus_lock(SpiBusOwner::Sd);

        if (SD.cardType() != CARD_NONE)
        {
            return true;
        }

        if (!SD.begin(SD_CARD_CS_PIN, SPI, SD_CARD_SPI_HZ_PRIMARY))
        {
            SD.end();
            if (!SD.begin(SD_CARD_CS_PIN, SPI, SD_CARD_SPI_HZ_FALLBACK))
            {
                SD.end();
                if (!SD.begin(SD_CARD_CS_PIN, SPI, SD_CARD_SPI_HZ_RECOVERY))
                {
                    if (verbose)
                    {
                        Serial.println("[Settings] SD not available");
                    }
                    return false;
                }
            }
        }

        if (verbose)
        {
            Serial.println("[Settings] SD mounted for settings");
        }
        return true;
    }

    bool verify_sd_root_access(const char *tag)
    {
        SpiBusLock bus_lock(SpiBusOwner::Sd);

        File root = SD.open("/");
        const bool ok = root && root.isDirectory();
        if (root)
        {
            root.close();
        }

        Serial.printf("[%s] root %s\n", tag, ok ? "open ok" : "open failed");
        return ok;
    }

    bool force_sd_reinit_for_test(const char *tag)
    {
        SpiBusLock bus_lock(SpiBusOwner::Sd);

        SD.end();
        if (SD.begin(SD_CARD_CS_PIN, SPI, SD_CARD_SPI_HZ_PRIMARY))
        {
            Serial.printf("[%s] SD reinit OK at primary clock\n", tag);
            return true;
        }

        SD.end();
        if (SD.begin(SD_CARD_CS_PIN, SPI, SD_CARD_SPI_HZ_FALLBACK))
        {
            Serial.printf("[%s] SD reinit OK at fallback clock\n", tag);
            return true;
        }

        SD.end();
        delay(40);
        if (SD.begin(SD_CARD_CS_PIN, SPI, SD_CARD_SPI_HZ_RECOVERY))
        {
            Serial.printf("[%s] SD reinit OK at recovery clock\n", tag);
            return true;
        }

        Serial.printf("[%s] SD reinit failed\n", tag);
        return false;
    }

    void run_post_max_sd_reinit_test()
    {
        Serial.println("[Setup] Post-MAX SD health check...");

        const bool mounted = ensure_sd_available_for_settings(true);
        const bool root_ok_before = mounted && verify_sd_root_access("Setup-SD-Before-Reinit");
        if (root_ok_before)
        {
            Serial.println("[Setup] Post-MAX SD health check passed");
            return;
        }

        Serial.println("[Setup] SD health check failed after MAX init, trying SD reinit test...");
        const bool reinit_ok = force_sd_reinit_for_test("Setup-SD-Reinit-Test");
        const bool root_ok_after = reinit_ok && verify_sd_root_access("Setup-SD-After-Reinit");

        if (root_ok_after)
        {
            Serial.println("[Setup] SD recovered after post-MAX reinit test");
        }
        else
        {
            Serial.println("[Setup] SD still failing after post-MAX reinit test");
        }
    }

    unsigned long get_configured_timer_duration_seconds(const AppState &state)
    {
        return static_cast<unsigned long>(state.set_timer_hours) * 3600UL +
               static_cast<unsigned long>(state.set_timer_minutes) * 60UL +
               static_cast<unsigned long>(state.set_timer_seconds);
    }

    time_t build_time_to_epoch()
    {
        const char month_names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
        char month_str[4] = {0};
        int day = 1;
        int year = 2026;
        int hour = 0;
        int minute = 0;
        int second = 0;

        sscanf(__DATE__, "%3s %d %d", month_str, &day, &year);
        sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);

        const char *pos = strstr(month_names, month_str);
        int month_index = pos ? static_cast<int>((pos - month_names) / 3) : 0;

        tm t = {};
        t.tm_year = year - 1900;
        t.tm_mon = month_index;
        t.tm_mday = day;
        t.tm_hour = hour;
        t.tm_min = minute;
        t.tm_sec = second;
        t.tm_isdst = -1;
        return mktime(&t);
    }

    time_t get_current_device_time()
    {
        m5::rtc_datetime_t dt;
        if (M5.Rtc.getDateTime(&dt))
        {
            tm t = {};
            dt.set_tm(&t);
            t.tm_isdst = -1;
            return mktime(&t);
        }

        return time(nullptr);
    }

    time_t get_runtime_clock_time()
    {
        const time_t system_now = time(nullptr);
        if (system_now >= VALID_TIME_EPOCH_MIN)
        {
            return system_now;
        }

        const unsigned long elapsed_sec = (millis() - boot_millis) / 1000UL;
        return static_cast<time_t>(boot_epoch_seconds + elapsed_sec);
    }

    bool has_wifi_credentials()
    {
#if WIFI_SECRETS_AVAILABLE
        return strlen(WIFI_SSID) > 0 && strlen(WIFI_PASSWORD) > 0;
#else
        return false;
#endif
    }

    bool should_emit_level(LogLevel level, LogLevel minimum)
    {
        return static_cast<uint8_t>(level) >= static_cast<uint8_t>(minimum);
    }

    char log_level_to_letter(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Debug:
            return 'D';
        case LogLevel::Info:
            return 'I';
        case LogLevel::Warning:
            return 'W';
        case LogLevel::Error:
            return 'E';
        }
        return 'I';
    }

    bool ntp_sync_in_progress()
    {
        return ntp_sync_state == NtpSyncState::ConnectingWifi ||
               ntp_sync_state == NtpSyncState::WaitingForTime;
    }

    void schedule_ntp_retry(unsigned long now, unsigned long delay_ms)
    {
        ntp_next_attempt_ms = now + delay_ms;
        ntp_sync_state = NtpSyncState::Backoff;
    }

    void begin_waiting_for_ntp(unsigned long now)
    {
        sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
        configTzTime(TIMEZONE_TZ, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
        ntp_state_started_ms = now;
        ntp_sync_state = NtpSyncState::WaitingForTime;
    }

    void request_ntp_sync(bool verbose, unsigned long now, bool force = false)
    {
        if (!ENABLE_NTP_TIME_SYNC)
        {
            ntp_sync_state = NtpSyncState::Disabled;
            return;
        }

        if (!has_wifi_credentials())
        {
            if (verbose && !ntp_missing_credentials_reported)
            {
                Serial.println("[Time] wifi_secrets.h missing or empty, skipping NTP");
                ntp_missing_credentials_reported = true;
            }
            ntp_sync_state = NtpSyncState::Disabled;
            return;
        }

        if (ntp_sync_in_progress())
        {
            ntp_sync_verbose = ntp_sync_verbose || verbose;
            return;
        }

        if (!force && ntp_sync_state == NtpSyncState::Backoff && now < ntp_next_attempt_ms)
        {
            return;
        }

        ntp_sync_verbose = verbose;
        ntp_state_started_ms = now;

#if WIFI_SECRETS_AVAILABLE
        WiFi.mode(WIFI_STA);
        if (WiFi.status() == WL_CONNECTED)
        {
            Serial.printf("[Time] WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
            begin_waiting_for_ntp(now);
            return;
        }

        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        ntp_sync_state = NtpSyncState::ConnectingWifi;
#else
        schedule_ntp_retry(now, NTP_RETRY_INTERVAL_MS);
#endif
    }

    bool process_ntp_sync(unsigned long now)
    {
        if (ntp_sync_state == NtpSyncState::Disabled)
        {
            return false;
        }

        if (ntp_sync_state == NtpSyncState::Backoff)
        {
            if (now >= ntp_next_attempt_ms)
            {
                request_ntp_sync(false, now, true);
            }
            return false;
        }

        if (ntp_sync_state == NtpSyncState::ConnectingWifi)
        {
            if (WiFi.status() == WL_CONNECTED)
            {
                Serial.printf("[Time] WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
                begin_waiting_for_ntp(now);
                return false;
            }

            if (now - ntp_state_started_ms >= WIFI_CONNECT_TIMEOUT_MS)
            {
                if (ntp_sync_verbose)
                {
                    Serial.println("[Time] WiFi connect timeout");
                }
                WiFi.disconnect(false);
                schedule_ntp_retry(now, NTP_RETRY_INTERVAL_MS);
            }
            return false;
        }

        if (ntp_sync_state == NtpSyncState::WaitingForTime)
        {
            const time_t synced_time = time(nullptr);
            if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED && synced_time >= VALID_TIME_EPOCH_MIN)
            {
                struct timeval tv = {.tv_sec = synced_time, .tv_usec = 0};
                settimeofday(&tv, nullptr);
                ntp_next_attempt_ms = now + NTP_RESYNC_INTERVAL_MS;
                ntp_sync_state = NtpSyncState::Idle;
                if (ntp_sync_verbose)
                {
                    Serial.printf("[Time] NTP sync OK: epoch=%ld\n", static_cast<long>(synced_time));
                }
                return true;
            }

            if (now - ntp_state_started_ms >= NTP_SYNC_TIMEOUT_MS)
            {
                if (ntp_sync_verbose)
                {
                    Serial.println("[Time] NTP sync timeout");
                }
                schedule_ntp_retry(now, NTP_RETRY_INTERVAL_MS);
            }
        }

        return false;
    }

    void append_recent_log_line(AppState &state, const char *line)
    {
        strncpy(
            state.recent_logs[state.recent_log_next_index],
            line,
            AppState::MAX_LOG_LINE_LENGTH - 1);
        state.recent_logs[state.recent_log_next_index][AppState::MAX_LOG_LINE_LENGTH - 1] = '\0';

        state.recent_log_next_index = (state.recent_log_next_index + 1) % AppState::MAX_RECENT_LOGS;
        if (state.recent_log_count < AppState::MAX_RECENT_LOGS)
        {
            state.recent_log_count++;
        }
    }

    void append_time_log_preview(AppState &state, time_t ts, float temp, unsigned long time_id)
    {
        tm *timeinfo = localtime(&ts);
        char time_str[16] = "--:--:--";
        if (timeinfo != nullptr)
        {
            strftime(time_str, sizeof(time_str), "%H:%M:%S", timeinfo);
        }

        char line[AppState::MAX_LOG_LINE_LENGTH];
        snprintf(line, sizeof(line), "%s,%.1f,%lu", time_str, temp, time_id);
        append_recent_log_line(state, line);
    }

    void format_temperature_preview(char *buffer, size_t buffer_size, float temp, bool valid)
    {
        if (valid)
        {
            snprintf(buffer, buffer_size, "%.1f", temp);
            return;
        }

        snprintf(buffer, buffer_size, "n/a");
    }

    void append_event_log_preview(AppState &state, time_t ts, const char *event_name, unsigned long elapsed_seconds, float temp, bool temperature_valid, unsigned long event_id, LogLevel level)
    {
        tm *timeinfo = localtime(&ts);
        char time_str[16] = "--:--:--";
        if (timeinfo != nullptr)
        {
            strftime(time_str, sizeof(time_str), "%H:%M:%S", timeinfo);
        }

        const unsigned long hours = elapsed_seconds / 3600UL;
        const unsigned long minutes = (elapsed_seconds % 3600UL) / 60UL;
        const unsigned long seconds = elapsed_seconds % 60UL;

        char line[AppState::MAX_LOG_LINE_LENGTH];
        char temp_text[12];
        format_temperature_preview(temp_text, sizeof(temp_text), temp, temperature_valid);
        snprintf(line, sizeof(line), "%c %s,%s,%02lu:%02lu:%02lu,%s,%lu", log_level_to_letter(level), time_str, event_name, hours, minutes, seconds, temp_text, event_id);
        append_recent_log_line(state, line);
    }

    void log_event_entry(AppState &state, Logger &logger, const char *event_name, unsigned long elapsed_seconds, float temp, LogLevel level = LogLevel::Info)
    {
        state.event_sequence_id++;
        logger.log_event(
            state.last_update_time,
            event_name,
            elapsed_seconds,
            temp,
            state.temperature_valid,
            state.event_sequence_id,
            level);
        if (should_emit_level(level, DISPLAY_LOG_MIN_LEVEL))
        {
            append_event_log_preview(
                state,
                state.last_update_time,
                event_name,
                elapsed_seconds,
                temp,
                state.temperature_valid,
                state.event_sequence_id,
                level);
        }
    }

    bool load_settings_from_sd(AppState &state)
    {
        if (!ensure_sd_available_for_settings(false))
        {
            return false;
        }

        SpiBusLock bus_lock(SpiBusOwner::Sd);
        if (!SD.exists(SETTINGS_FILE_PATH))
        {
            return false;
        }

        File file = SD.open(SETTINGS_FILE_PATH, FILE_READ);
        if (!file)
        {
            return false;
        }

        char line[96] = {0};
        size_t len = file.readBytesUntil('\n', line, sizeof(line) - 1);
        file.close();
        if (len == 0)
        {
            return false;
        }

        unsigned int h = 0;
        unsigned int m = 0;
        unsigned int s = 0;
        float threshold = 70.0f;
        if (sscanf(line, "%u,%u,%u,%f", &h, &m, &s, &threshold) != 4)
        {
            return false;
        }

        state.set_timer_hours = (h > 23U) ? 23U : h;
        state.set_timer_minutes = (m > 59U) ? 59U : m;
        state.set_timer_seconds = (s > 59U) ? 59U : s;
        state.set_temperature_threshold = (threshold < 0.0f) ? 0.0f : ((threshold > 120.0f) ? 120.0f : threshold);
        return true;
    }

    bool save_settings_to_sd(const AppState &state)
    {
        if (!ensure_sd_available_for_settings(true))
        {
            return false;
        }

        SpiBusLock bus_lock(SpiBusOwner::Sd);
        if (SD.exists(SETTINGS_FILE_PATH))
        {
            SD.remove(SETTINGS_FILE_PATH);
        }

        File file = SD.open(SETTINGS_FILE_PATH, FILE_WRITE);
        if (!file)
        {
            return false;
        }

        file.printf("%u,%u,%u,%.1f\n",
                    state.set_timer_hours,
                    state.set_timer_minutes,
                    state.set_timer_seconds,
                    state.set_temperature_threshold);
        file.close();
        return true;
    }
}

/** Initialize all services and prepare the device. */
void setup()
{
    // Initialize serial for debugging
    Serial.begin(115200);
    delay(100);

    Serial.println("\n\n=== M5Stack Tough Modbus RTU Master ===");
    Serial.printf("[Boot] Reset reason code: %d\n", static_cast<int>(esp_reset_reason()));
    Serial.println("Initializing services...\n");

    // Initialize display first (for user feedback)
    display_service.init();
    delay(500);

    // One shared hardware SPI bus for SD + MAX31865.
    pinMode(SD_CARD_CS_PIN, OUTPUT);
    digitalWrite(SD_CARD_CS_PIN, HIGH);
    pinMode(MAX31865_CS, OUTPUT);
    digitalWrite(MAX31865_CS, HIGH);
    SPI.begin(SD_CARD_SCK_PIN, SD_CARD_MISO_PIN, SD_CARD_MOSI_PIN);
    Serial.printf("[SPI] Shared hardware bus initialized: SCK=%d MISO=%d MOSI=%d SD_CS=%d MAX_CS=%d\n",
                  SD_CARD_SCK_PIN,
                  SD_CARD_MISO_PIN,
                  SD_CARD_MOSI_PIN,
                  SD_CARD_CS_PIN,
                  MAX31865_CS);

    // Initialize SD card logging first.
    logger.init();
    delay(500);

    // Initialize sensor communication after SD init so MAX31865 gets final bus state.
    modbus_service.init();
    delay(500);

    // Optional smoke test: verify whether SD still responds right after MAX setup.
    run_post_max_sd_reinit_test();
    delay(100);

    // Initialize application state
    app_state.set_timer_hours = static_cast<unsigned int>(TIMER_DURATION_SECONDS / 3600UL);
    app_state.set_timer_minutes = static_cast<unsigned int>((TIMER_DURATION_SECONDS % 3600UL) / 60UL);
    app_state.set_timer_seconds = static_cast<unsigned int>(TIMER_DURATION_SECONDS % 60UL);
    app_state.set_temperature_threshold = TEMPERATURE_RESTART_THRESHOLD;

    if (load_settings_from_sd(app_state))
    {
        Serial.println("[Settings] Loaded from SD");
    }

    app_state.countdown_remaining_seconds = get_configured_timer_duration_seconds(app_state);

    // Start internet time sync in the background; use RTC/build-time immediately.
    request_ntp_sync(true, millis(), true);
    time_t current_device_time = get_current_device_time();
    const time_t build_epoch = build_time_to_epoch();
    const time_t max_reasonable_drift = 180L * 24L * 3600L;
    const bool rtc_plausible =
        (current_device_time >= VALID_TIME_EPOCH_MIN) &&
        (build_epoch <= 0 || llabs(static_cast<long long>(current_device_time - build_epoch)) <= max_reasonable_drift);

    if (!rtc_plausible)
    {
        if (build_epoch > 0)
        {
            struct timeval tv = {.tv_sec = build_epoch, .tv_usec = 0};
            settimeofday(&tv, nullptr);
            current_device_time = build_epoch;
            Serial.println("[Time] RTC invalid, using build-time seed");
        }
    }
    else
    {
        Serial.println("[Time] Using RTC/system time while NTP sync runs in background");
    }

    boot_epoch_seconds = static_cast<unsigned long>(current_device_time);
    boot_millis = millis();

    logger.start_session(current_device_time);

    app_state.timer_end_time = current_device_time + static_cast<time_t>(app_state.countdown_remaining_seconds);
    app_state.timer_end_time_valid = true;

    app_state.last_update_time = current_device_time;
    log_event_entry(app_state, logger, "BOOT", 0, 0.0f, LogLevel::Warning);

    Serial.println("Setup complete. Device ready.\n");
}

/** Main application loop. */
void loop()
{
    unsigned long now = millis();
    const bool log_due = (now - app_state.last_log_time_ms) >= LOG_INTERVAL_MS;
    const bool time_log_write_due = log_due && app_state.temperature_valid;

    if (process_ntp_sync(now))
    {
        Serial.println("[Time] Background NTP sync OK");
    }

    if (ENABLE_NTP_TIME_SYNC && !ntp_sync_in_progress() && ntp_sync_state == NtpSyncState::Idle && now >= ntp_next_attempt_ms)
    {
        request_ntp_sync(false, now, true);
    }

    // Update current time
    // Prefer system time when valid (NTP/RTC), otherwise fallback runtime base.
    app_state.last_update_time = get_runtime_clock_time();

    if (time_log_write_due)
    {
        app_state.last_log_time_ms = now;
        app_state.time_sequence_id++;
        app_state.last_logged_temperature = app_state.current_temperature;
        app_state.last_logged_temperature_valid = true;
        display_service.updateDisplay(app_state.last_logged_temperature);
        logger.log_time_sample(
            app_state.last_update_time,
            app_state.current_temperature,
            app_state.time_sequence_id);
        append_time_log_preview(
            app_state,
            app_state.last_update_time,
            app_state.current_temperature,
            app_state.time_sequence_id);
    }

    // Do not poll the sensor while logging is active or shortly after SD writes.
    // This keeps the displayed value stable and avoids SPI contention symptoms.
    if (!time_log_write_due && !logger.should_defer_sensor_poll())
    {
        const bool previous_modbus_connected = app_state.modbus_connected;
        modbus_service.poll(app_state);

        if (app_state.modbus_connected != previous_modbus_connected)
        {
            log_event_entry(
                app_state,
                logger,
                app_state.modbus_connected ? "SENSOR_OK" : "SENSOR_LOST",
                0,
                app_state.current_temperature,
                app_state.modbus_connected ? LogLevel::Debug : LogLevel::Warning);
        }
    }

    // Restart timer only on falling edge: temperature moves from >= threshold to below threshold.
    const unsigned long configured_timer_duration = get_configured_timer_duration_seconds(app_state);
    const unsigned long effective_timer_duration = (configured_timer_duration > 0) ? configured_timer_duration : 1UL;
    const bool is_below_threshold =
        app_state.temperature_valid &&
        app_state.current_temperature < app_state.set_temperature_threshold;
    const bool is_above_threshold = app_state.temperature_valid && !is_below_threshold;
    const unsigned long elapsed_until_reset =
        (effective_timer_duration > app_state.countdown_remaining_seconds)
            ? (effective_timer_duration - app_state.countdown_remaining_seconds)
            : 0;

    if (app_state.timer_expired_waiting_restart)
    {
        app_state.countdown_remaining_seconds = 0;

        if (app_state.timer_expiry_alert_beeps_remaining > 0 && now >= app_state.timer_expiry_alert_next_beep_ms)
        {
            M5.Speaker.tone(TIMER_EXPIRED_TONE_HZ, TIMER_EXPIRED_TONE_MS);
            app_state.timer_expiry_alert_beeps_remaining--;
            app_state.timer_expiry_alert_sent = true;
            app_state.timer_expiry_alert_next_beep_ms = now + TIMER_EXPIRED_TONE_MS + TIMER_EXPIRED_TONE_GAP_MS;
        }

        if (now >= app_state.timer_expiry_pause_until_ms && is_above_threshold)
        {
            app_state.countdown_remaining_seconds = effective_timer_duration;
            app_state.timer_end_time = app_state.last_update_time + static_cast<time_t>(effective_timer_duration);
            app_state.timer_end_time_valid = true;
            app_state.timer_expired_waiting_restart = false;
            app_state.timer_expiry_alert_sent = false;
            app_state.timer_expiry_alert_beeps_remaining = 0;
            app_state.timer_expiry_alert_next_beep_ms = 0;
            app_state.timer_should_restart = true;
            app_state.timer_restart_count++;
            app_state.last_timer_restart_time = app_state.last_update_time;

            log_event_entry(
                app_state,
                logger,
                "TIMER_RESTART",
                (now - app_state.timer_expiry_pause_started_ms) / 1000UL,
                app_state.current_temperature,
                LogLevel::Warning);
        }
        else
        {
            app_state.timer_should_restart = false;
        }
    }
    else if (is_below_threshold)
    {
        // Under-threshold state keeps timer at full duration and pauses countdown.
        app_state.timer_end_time = app_state.last_update_time + static_cast<time_t>(effective_timer_duration);
        app_state.timer_end_time_valid = true;
        app_state.countdown_remaining_seconds = effective_timer_duration;
        app_state.timer_should_restart = true;
    }
    else
    {
        app_state.timer_should_restart = false;

        if (app_state.timer_end_time_valid)
        {
            if (app_state.last_update_time >= app_state.timer_end_time)
            {
                app_state.countdown_remaining_seconds = 0;
                if (!app_state.timer_expired_waiting_restart)
                {
                    app_state.timer_expired_waiting_restart = true;
                    app_state.last_timer_expiry_time = app_state.last_update_time;
                    app_state.timer_expiry_pause_started_ms = now;
                    app_state.timer_expiry_pause_until_ms = now + TIMER_RESTART_PAUSE_MS;
                    app_state.timer_expiry_alert_sent = false;
                    app_state.timer_expiry_alert_beeps_remaining = TIMER_EXPIRED_TONE_COUNT;
                    app_state.timer_expiry_alert_next_beep_ms = now;
                    log_event_entry(
                        app_state,
                        logger,
                        "TIMER_EXPIRED",
                        effective_timer_duration,
                        app_state.current_temperature,
                        LogLevel::Error);
                }
            }
            else
            {
                app_state.countdown_remaining_seconds = static_cast<unsigned long>(app_state.timer_end_time - app_state.last_update_time);
            }
        }
    }

    if (is_below_threshold && !was_below_threshold)
    {
        log_event_entry(
            app_state,
            logger,
            "TEMP_BELOW_THRESHOLD",
            elapsed_until_reset,
            app_state.current_temperature,
            LogLevel::Warning);
    }

    if (is_above_threshold && !was_above_threshold)
    {
        log_event_entry(
            app_state,
            logger,
            "TEMP_ABOVE_THRESHOLD",
            elapsed_until_reset,
            app_state.current_temperature,
            LogLevel::Info);
    }

    was_below_threshold = is_below_threshold;
    was_above_threshold = is_above_threshold;

    // Update display (non-blocking, respects interval)
    display_service.update(app_state);

    if (app_state.settings_dirty)
    {
        if (save_settings_to_sd(app_state))
        {
            app_state.settings_dirty = false;
            log_event_entry(app_state, logger, "SETTINGS_SAVED", 0, app_state.current_temperature, LogLevel::Debug);
        }
    }

    // Prepare shutdown event when the power button is pressed.
    if (M5.BtnPWR.wasClicked())
    {
        log_event_entry(app_state, logger, "SHUTDOWN", 0, app_state.current_temperature, LogLevel::Warning);
        delay(50);
        M5.Power.powerOff();
    }

    // Keep loop responsive - brief delay to avoid monopolizing CPU
    delay(10);
}

// Arduino framework requires these functions at the end for some environments
// Uncomment if needed for your specific setup:
// void setup_tasks() { /* Additional setup if needed */ }
// void loop_tasks() { /* Additional background tasks if needed */ }
