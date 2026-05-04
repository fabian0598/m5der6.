#pragma once

#include <cstdint>
#include <ctime>

/**
 * Global configuration constants for the M5Stack Tough Modbus RTU Master application.
 * Keep this file simple: only constants, no logic.
 */

// PT100 / MAX31865 configuration
// Required pin definitions for the MAX31865 module.
// NOTE: MAX31865 shares the same hardware SPI bus as the internal SD card.
#define MAX31865_CS 26
#define MAX31865_MOSI 23
#define MAX31865_MISO 38
#define MAX31865_SCK 18

constexpr float MAX31865_RREF = 430.0f;             // Reference resistor on the MAX31865 board
constexpr float MAX31865_RTD_NOMINAL = 100.0f;      // PT100 nominal resistance
constexpr float PT100_CALIBRATION_OFFSET_C = -5.5f; // Subtract this offset after hardware calibration

// Polling and timing (in milliseconds)
constexpr unsigned long PT100_POLL_INTERVAL_MS = 1000;      // Poll temperature every 1 second
constexpr unsigned long DISPLAY_REFRESH_INTERVAL_MS = 1000; // Refresh display every 1 second
constexpr unsigned long LOG_INTERVAL_MS = 60000;            // Log to SD card every 60 seconds
constexpr unsigned long TIMER_DECREMENT_INTERVAL_MS = 1000; // Decrement timer every 1 second

// Timer settings (in seconds)
constexpr unsigned long TIMER_DURATION_SECONDS = 3600;    // 1 hour countdown
constexpr float TEMPERATURE_RESTART_THRESHOLD = 70.0f;    // Restart timer if temperature < threshold
constexpr unsigned long TIMER_RESTART_PAUSE_MS = 20000UL; // Pause before restart after expiry
constexpr uint16_t TIMER_EXPIRED_TONE_HZ = 1800;
constexpr unsigned long TIMER_EXPIRED_TONE_MS = 260UL;
constexpr unsigned long TIMER_EXPIRED_TONE_GAP_MS = 160UL;
constexpr unsigned int TIMER_EXPIRED_TONE_COUNT = 4;
constexpr unsigned long TIMER_EXPIRED_FLASH_MS = 250UL;
constexpr unsigned long TIMER_EXPIRY_DISPLAY_REFRESH_MS = 200UL;
constexpr unsigned long SETTINGS_SAVE_DEBOUNCE_MS = 1500UL;

// Legacy RS485 pins kept only for compatibility with older code paths.
// The active sensor path is the MAX31865/PT100 SPI configuration above.
constexpr int RS485_RX_PIN = 16; // TODO: Verify pin assignment from M5Stack Tough schematic
constexpr int RS485_TX_PIN = 17; // TODO: Verify pin assignment from M5Stack Tough schematic
constexpr int RS485_DE_PIN = 18; // Direction Enable pin for RS485 driver

// Storage settings
constexpr const char *TIME_LOG_FILE_PATH = "/time_log.csv";
constexpr const char *EVENT_LOG_FILE_PATH = "/event_log.csv";
constexpr const char *SETTINGS_FILE_PATH = "/settings.csv";
constexpr const char *TIME_LOG_DIR_PATH = "/logs/time";
constexpr const char *EVENT_LOG_DIR_PATH = "/logs/event";
constexpr unsigned long LOG_FILE_MAX_ENTRIES = 1000;
// SD Card Pins - fixed hardware SPI wiring on M5Stack Tough microSD slot
constexpr int SD_CARD_CS_PIN = 4;
constexpr int SD_CARD_SCK_PIN = 18;
constexpr int SD_CARD_MISO_PIN = 38;
constexpr int SD_CARD_MOSI_PIN = 23;
constexpr unsigned long SD_CARD_SPI_HZ_PRIMARY = 10000000UL;
constexpr unsigned long SD_CARD_SPI_HZ_FALLBACK = 4000000UL;
constexpr unsigned long SD_CARD_SPI_HZ_RECOVERY = 400000UL;
constexpr unsigned long SD_CARD_SPI_HZ = SD_CARD_SPI_HZ_PRIMARY;
constexpr bool LOGGING_ENABLED = true;
constexpr bool SPI_BUS_LOCK_DEBUG_LOGS = false;

enum class LogLevel : uint8_t
{
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
};

constexpr LogLevel SERIAL_LOG_MIN_LEVEL = LogLevel::Debug;
constexpr LogLevel DISPLAY_LOG_MIN_LEVEL = LogLevel::Debug;
constexpr LogLevel SD_EVENT_LOG_MIN_LEVEL = LogLevel::Warning;

// Display settings
constexpr int DISPLAY_WIDTH = 320;
constexpr int DISPLAY_HEIGHT = 240;

// Internet time (NTP) settings
constexpr bool ENABLE_NTP_TIME_SYNC = true;
constexpr const char *TIMEZONE_TZ = "CET-1CEST,M3.5.0/2,M10.5.0/3"; // Europe/Berlin
constexpr const char *NTP_SERVER_1 = "pool.ntp.org";
constexpr const char *NTP_SERVER_2 = "time.cloudflare.com";
constexpr const char *NTP_SERVER_3 = "time.google.com";
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000UL;
constexpr unsigned long NTP_SYNC_TIMEOUT_MS = 12000UL;
constexpr unsigned long NTP_RETRY_INTERVAL_MS = 60000UL;                     // retry every 60s until time is valid
constexpr unsigned long NTP_RESYNC_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL; // 6 hours
constexpr time_t VALID_TIME_EPOCH_MIN = 1700000000;                          // around 2023-11

// WLAN backup server
constexpr bool ENABLE_HTTP_BACKUP_SERVER = true;
constexpr uint16_t HTTP_BACKUP_SERVER_PORT = 80;
