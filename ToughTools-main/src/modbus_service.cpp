#include "modbus_service.h"
#include "spi_bus_lock.h"
#include <Adafruit_MAX31865.h>
#include <Arduino.h>
#include <SPI.h>
#include <cmath>

namespace
{
    // MAX31865 on shared hardware SPI bus (SCK/MOSI/MISO shared with SD), CS is dedicated.
    Adafruit_MAX31865 pt100_sensor(MAX31865_CS);
    // Treat short fault bursts as transient before declaring SENSOR_LOST.
    constexpr unsigned int SENSOR_FAILURE_GRACE_COUNT = 4;
    constexpr unsigned long MAX31865_CS_SETTLE_DELAY_US = 120UL;

    uint8_t max_read_register_raw(uint8_t reg)
    {
        // MAX31865 uses SPI mode 1. Keep SD deselected during direct probe.
        digitalWrite(SD_CARD_CS_PIN, HIGH);
        digitalWrite(MAX31865_CS, LOW);
        delayMicroseconds(4);

        SPI.transfer(reg & 0x7F);
        const uint8_t value = SPI.transfer(0x00);

        delayMicroseconds(2);
        digitalWrite(MAX31865_CS, HIGH);
        return value;
    }
}

void ModbusService::init()
{
    Serial.println("[PT100] Initializing MAX31865 over shared Hardware SPI...");

    bool max_begin_ok = false;
    uint16_t startup_rtd = 0;
    uint8_t startup_fault = 0;

    {
        SpiBusLock bus_lock(SpiBusOwner::Max);

        // Important: configure the MAX31865 explicitly for a 2-wire PT100.
        max_begin_ok = pt100_sensor.begin(MAX31865_2WIRE);
        pt100_sensor.clearFault();

        // Immediate hardware-SPI reachability probe after init.
        startup_rtd = pt100_sensor.readRTD();
        startup_fault = pt100_sensor.readFault();

        SPI.beginTransaction(SPISettings(500000, MSBFIRST, SPI_MODE1));
        const uint8_t cfg_raw = max_read_register_raw(0x00);
        const uint8_t fault_raw = max_read_register_raw(0x07);
        SPI.endTransaction();

        Serial.printf("[PT100] Raw probe registers: CFG=0x%02X FAULT=0x%02X\n",
                      static_cast<unsigned int>(cfg_raw),
                      static_cast<unsigned int>(fault_raw));
        if (cfg_raw == 0xFF && fault_raw == 0xFF)
        {
            Serial.println("[PT100] Raw probe also all-ones -> MAX is not driving MISO (likely CS/SDO/power path issue).");
        }
    }

    Serial.printf("[PT100] MAX begin() returned: %s\n", max_begin_ok ? "ok" : "failed");
    Serial.printf("[PT100] Startup probe: RTD=%u FAULT=0x%02X\n",
                  static_cast<unsigned int>(startup_rtd),
                  static_cast<unsigned int>(startup_fault));
    if (startup_rtd >= 32760 && startup_fault == 0xFF)
    {
        Serial.println("[PT100] Startup probe indicates no active MISO driver (all ones). Check MAX SDO->GPIO38, MAX CS->GPIO26, 3V3/GND, and 2-wire jumper.");
    }

    Serial.printf("[PT100] Hardware SPI shared pins: SCK=%d MISO=%d MOSI=%d CS=%d\n",
                  MAX31865_SCK,
                  MAX31865_MISO,
                  MAX31865_MOSI,
                  MAX31865_CS);
    Serial.printf("[PT100] PT100 setup: 2-wire, Rref=%.1f ohm, Rnom=%.1f ohm\n",
                  static_cast<double>(MAX31865_RREF),
                  static_cast<double>(MAX31865_RTD_NOMINAL));
}

bool ModbusService::read_temperature(float &temperature_out, float &raw_temperature_out, uint8_t &fault_out, uint16_t &raw_rtd_out)
{
    SpiBusLock bus_lock(SpiBusOwner::Max);

    // Keep SD deselected while talking to MAX on the shared SPI bus.
    digitalWrite(SD_CARD_CS_PIN, HIGH);
    delayMicroseconds(MAX31865_CS_SETTLE_DELAY_US);

    // Start each conversion/read attempt from a clean MAX fault state.
    pt100_sensor.clearFault();
    delayMicroseconds(MAX31865_CS_SETTLE_DELAY_US);

    raw_rtd_out = pt100_sensor.readRTD();
    fault_out = pt100_sensor.readFault();
    if (fault_out != 0)
    {
        // Retry once after fault clear to filter short bus/settle disturbances.
        pt100_sensor.clearFault();
        delayMicroseconds(MAX31865_CS_SETTLE_DELAY_US);
        raw_rtd_out = pt100_sensor.readRTD();
        fault_out = pt100_sensor.readFault();
        if (fault_out != 0)
        {
            pt100_sensor.clearFault();
            return false;
        }
    }

    float raw_temperature = pt100_sensor.temperature(MAX31865_RTD_NOMINAL, MAX31865_RREF);
    if (isnan(raw_temperature))
    {
        // Retry once for NaN without latched hardware fault bits.
        pt100_sensor.clearFault();
        delayMicroseconds(MAX31865_CS_SETTLE_DELAY_US);
        raw_rtd_out = pt100_sensor.readRTD();
        fault_out = pt100_sensor.readFault();
        if (fault_out == 0)
        {
            raw_temperature = pt100_sensor.temperature(MAX31865_RTD_NOMINAL, MAX31865_RREF);
        }

        if (fault_out != 0 || isnan(raw_temperature))
        {
            if (fault_out == 0)
            {
                fault_out = 0xFF;
            }
            return false;
        }
    }

    raw_temperature_out = raw_temperature;
    temperature_out = raw_temperature + PT100_CALIBRATION_OFFSET_C;
    return true;
}

namespace
{
    void print_adafruit_style_diagnostics(uint16_t raw_rtd, float temperature, float rref)
    {
        const float ratio = static_cast<float>(raw_rtd) / 32768.0f;
        const float resistance = rref * ratio;

        Serial.printf("[PT100] RTD value: %u\n", static_cast<unsigned int>(raw_rtd));
        Serial.printf("[PT100] Ratio = %.8f\n", static_cast<double>(ratio));
        Serial.printf("[PT100] Resistance = %.8f\n", static_cast<double>(resistance));
        Serial.printf("[PT100] Temperature = %.2f C\n", static_cast<double>(temperature));
    }
}

void ModbusService::log_fault(uint8_t fault_bits) const
{
    Serial.printf("[PT100] MAX31865 fault: 0x%02X\n", fault_bits);
    if (fault_bits == 0xFF)
    {
        Serial.println("[PT100] Fault 0xFF indicates invalid SPI read (all ones), not a valid decoded MAX fault state.");
        return;
    }

    if (fault_bits & MAX31865_FAULT_HIGHTHRESH)
    {
        Serial.println("[PT100] Fault detail: RTD high threshold");
    }
    if (fault_bits & MAX31865_FAULT_LOWTHRESH)
    {
        Serial.println("[PT100] Fault detail: RTD low threshold");
    }
    if (fault_bits & MAX31865_FAULT_REFINLOW)
    {
        Serial.println("[PT100] Fault detail: REFIN- > 0.85 x Bias");
    }
    if (fault_bits & MAX31865_FAULT_REFINHIGH)
    {
        Serial.println("[PT100] Fault detail: REFIN- < 0.85 x Bias - FORCE- open");
    }
    if (fault_bits & MAX31865_FAULT_RTDINLOW)
    {
        Serial.println("[PT100] Fault detail: RTDIN- < 0.85 x Bias - FORCE- open");
    }
    if (fault_bits & MAX31865_FAULT_OVUV)
    {
        Serial.println("[PT100] Fault detail: Under/over voltage on bias/reference");
    }
}

void ModbusService::poll(AppState &app_state)
{
    const unsigned long now = millis();
    if ((now - last_poll_time_ms) < PT100_POLL_INTERVAL_MS)
    {
        return;
    }
    last_poll_time_ms = now;

    float temperature = 0.0f;
    float raw_temperature = 0.0f;
    uint8_t fault_bits = 0;
    uint16_t raw_rtd = 0;
    if (read_temperature(temperature, raw_temperature, fault_bits, raw_rtd))
    {
        Serial.printf("[PT100] Fault register: 0x%02X\n", static_cast<unsigned int>(fault_bits));
        print_adafruit_style_diagnostics(raw_rtd, raw_temperature, MAX31865_RREF);
        if (PT100_CALIBRATION_OFFSET_C != 0.0f)
        {
            Serial.printf("[PT100] Calibration offset applied: %.2f C\n", static_cast<double>(PT100_CALIBRATION_OFFSET_C));
            Serial.printf("[PT100] Calibrated temperature: %.2f C\n", static_cast<double>(temperature));
        }
        app_state.current_temperature = temperature;
        app_state.last_valid_temperature = temperature;
        app_state.temperature_valid = true;
        app_state.modbus_connected = true;
        app_state.consecutive_failures = 0;
        data_received = true;
        return;
    }

    app_state.consecutive_failures++;

    const bool keep_last_value_for_transient_fault =
        data_received && (app_state.consecutive_failures <= SENSOR_FAILURE_GRACE_COUNT);

    if (keep_last_value_for_transient_fault)
    {
        app_state.current_temperature = app_state.last_valid_temperature;
        app_state.temperature_valid = true;
        app_state.modbus_connected = true;
        Serial.printf("[PT100] Transient fault masked (%u/%u), keeping last valid temperature %.2f C\n",
                      static_cast<unsigned int>(app_state.consecutive_failures),
                      static_cast<unsigned int>(SENSOR_FAILURE_GRACE_COUNT),
                      static_cast<double>(app_state.last_valid_temperature));
    }
    else
    {
        app_state.modbus_connected = false;
        app_state.temperature_valid = false;
    }

    if (fault_bits == 0xFF)
    {
        const float approx_resistance = (static_cast<float>(raw_rtd) * MAX31865_RREF) / 32768.0f;
        const uint8_t fault_reg = fault_bits;

        Serial.println("[PT100] Temperature read returned NaN");
        Serial.printf("[PT100] Raw RTD count: %u / 32768\n", static_cast<unsigned int>(raw_rtd));
        Serial.printf("[PT100] Approx RTD resistance: %.2f ohm\n", static_cast<double>(approx_resistance));
        Serial.printf("[PT100] MAX31865 reg snapshot: CFG=0x?? FAULT=0x%02X\n",
                      static_cast<unsigned int>(fault_reg));

        if (fault_reg != 0)
        {
            log_fault(fault_reg);
        }

        if (raw_rtd >= 32760 && fault_reg == 0xFF)
        {
            Serial.println("[PT100] SPI read looks like all-ones (0xFF). Chip likely not reachable on current MISO/CS wiring.");
        }

        print_adafruit_style_diagnostics(raw_rtd, NAN, MAX31865_RREF);

        Serial.println("[PT100] Hint: open wiring, wrong 2-wire jumper, bad reference resistor, or missing PT100 element are the usual causes.");
    }
    else if (fault_bits != 0)
    {
        Serial.printf("[PT100] Fault register: 0x%02X\n", static_cast<unsigned int>(fault_bits));
        log_fault(fault_bits);
        print_adafruit_style_diagnostics(raw_rtd, NAN, MAX31865_RREF);
    }
    else
    {
        Serial.println("[PT100] No fault bits, but no valid reading was produced");
        print_adafruit_style_diagnostics(raw_rtd, NAN, MAX31865_RREF);
    }
}

bool ModbusService::is_data_available() const
{
    return data_received;
}
