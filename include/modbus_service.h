#pragma once

#include "app_state.h"
#include "config.h"
#include <cstddef>
#include <cstdint>

/**
 * PT100 sensor service backed by an Adafruit MAX31865 module on SPI.
 * Keeps the existing service shape so the application structure stays stable.
 */
class ModbusService
{
public:
    /**
     * Initialize SPI and the MAX31865 for the configured PT100 wire mode.
     */
    void init();

    /**
     * Poll the PT100 sensor periodically and update AppState.
     */
    void poll(AppState &app_state);

    /**
     * Check if a valid reading was produced at least once.
     */
    bool is_data_available() const;

private:
    unsigned long last_poll_time_ms = 0;
    bool data_received = false;

    bool read_temperature(float &temperature_out, float &raw_temperature_out, uint8_t &fault_out, uint16_t &raw_rtd_out);
    void log_fault(uint8_t fault_bits) const;
};
