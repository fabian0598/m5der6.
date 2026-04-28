#pragma once

#include "app_state.h"
#include "config.h"
#include <cstdint>
#include <ctime>

/**
 * SD card logging service.
 * Logs temperature readings with timestamp to CSV file on SD card.
 */
class Logger
{
public:
    /**
     * Initialize SD card and prepare logging.
     * Creates or opens the log file.
     */
    void init();

    /**
     * Start a new logging session for this boot.
     * Creates fresh per-boot files in dedicated log folders.
     */
    void start_session(time_t session_start_time);

    /**
     * Log the regular minute sample to the time log.
     * Format: "YYYY-MM-DD HH:MM:SS,temperature,time_id"
     */
    void log_time_sample(time_t timestamp, float temperature, unsigned long time_id);

    /**
     * Log an event to the event log.
     * Format: "YYYY-MM-DD HH:MM:SS,event_name,elapsed_hh:mm:ss,temperature,event_id"
     */
    void log_event(time_t timestamp, const char *event_name, unsigned long elapsed_seconds, float temperature, unsigned long event_id, LogLevel level = LogLevel::Info);

    /**
     * Check if SD card is ready for logging.
     */
    bool is_ready() const;

    /**
     * True while SD writes/recovery are active and for a short settle window after writes.
     * Use this to defer sensor SPI access around logging.
     */
    bool should_defer_sensor_poll() const;

private:
    bool sd_ready = false;
    bool sd_reinit_in_progress = false;
    bool sd_io_in_progress = false;
    unsigned long sensor_poll_block_until_ms = 0;
    uint8_t consecutive_sd_failures = 0;
    unsigned long last_sd_recovery_attempt_ms = 0;
    unsigned long last_sd_reinit_attempt_ms = 0;
    char session_tag[20] = {0};
    char current_time_log_path[96] = {0};
    char current_event_log_path[96] = {0};
    char last_announced_time_log_path[96] = {0};
    char last_announced_event_log_path[96] = {0};
    unsigned long time_log_file_index = 0;
    unsigned long event_log_file_index = 0;
    unsigned long time_log_entries_in_file = 0;
    unsigned long event_log_entries_in_file = 0;

    bool prepare_log_directories();
    bool prepare_log_directories_locked();
    bool ensure_log_file(bool for_time_log);
    bool ensure_log_file_locked(bool for_time_log);
    void start_session_locked(time_t session_start_time);
    void rotate_time_log_file();
    void rotate_event_log_file();
    void try_recover_sd();
    void note_sd_success();
    void note_sd_failure(const char *context);
    void note_sd_failure_locked(const char *context);

    /**
     * Helper: Format timestamp into a readable string.
     */
    const char *format_timestamp(time_t timestamp);
};
