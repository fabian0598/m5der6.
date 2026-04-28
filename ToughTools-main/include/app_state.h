#pragma once

#include <cstddef>
#include <ctime>

/**
 * Shared application state.
 * Simple struct to hold current application data.
 * No methods, just data members for clarity.
 */
struct AppState
{
    static constexpr size_t MAX_RECENT_LOGS = 6;
    static constexpr size_t MAX_LOG_LINE_LENGTH = 64;

    // Temperature data
    float current_temperature = 0.0f;
    float last_valid_temperature = 0.0f;
    bool temperature_valid = false;

    // Time tracking
    time_t last_update_time = 0;

    // Countdown timer
    unsigned long countdown_remaining_seconds = 3600; // Starts at 1 hour
    unsigned long last_timer_update_ms = 0;

    // Modbus communication status
    bool modbus_connected = false;
    unsigned int consecutive_failures = 0;

    // Logging state
    bool logging_enabled = true;
    unsigned long last_log_time_ms = 0;
    unsigned long time_sequence_id = 0;

    // Timer control
    bool timer_should_restart = false;
    unsigned long timer_restart_count = 0;
    unsigned long event_sequence_id = 0;
    time_t last_timer_restart_time = 0;
    time_t timer_end_time = 0;
    bool timer_end_time_valid = false;
    bool timer_expired_waiting_restart = false;
    time_t last_timer_expiry_time = 0;
    unsigned long timer_expiry_pause_started_ms = 0;
    unsigned long timer_expiry_pause_until_ms = 0;
    bool timer_expiry_alert_sent = false;
    unsigned int timer_expiry_alert_beeps_remaining = 0;
    unsigned long timer_expiry_alert_next_beep_ms = 0;

    // User-configurable setpoints (via settings page)
    unsigned int set_timer_hours = 1;
    unsigned int set_timer_minutes = 0;
    unsigned int set_timer_seconds = 0;
    float set_temperature_threshold = 70.0f;
    bool settings_dirty = false;

    // Recent log lines for settings screen preview
    char recent_logs[MAX_RECENT_LOGS][MAX_LOG_LINE_LENGTH] = {};
    size_t recent_log_count = 0;
    size_t recent_log_next_index = 0;
};
