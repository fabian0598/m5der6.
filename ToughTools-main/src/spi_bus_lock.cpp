#include "spi_bus_lock.h"
#include "config.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace
{
    SemaphoreHandle_t g_spi_bus_mutex = nullptr;
    portMUX_TYPE g_spi_bus_mutex_init_guard = portMUX_INITIALIZER_UNLOCKED;

    const char *owner_to_string(SpiBusOwner owner)
    {
        return owner == SpiBusOwner::Sd ? "SD" : "MAX31865";
    }

    void ensure_mutex_created()
    {
        if (g_spi_bus_mutex != nullptr)
        {
            return;
        }

        taskENTER_CRITICAL(&g_spi_bus_mutex_init_guard);
        if (g_spi_bus_mutex == nullptr)
        {
            g_spi_bus_mutex = xSemaphoreCreateRecursiveMutex();
        }
        taskEXIT_CRITICAL(&g_spi_bus_mutex_init_guard);
    }

    void set_all_chip_selects_high()
    {
        pinMode(SD_CARD_CS_PIN, OUTPUT);
        digitalWrite(SD_CARD_CS_PIN, HIGH);

        pinMode(MAX31865_CS, OUTPUT);
        digitalWrite(MAX31865_CS, HIGH);
    }
}

void prepare_sd_bus_locked()
{
    set_all_chip_selects_high();
    Serial.println("[SPI] CS states: SD=HIGH, MAX=HIGH");
}

void prepare_max_bus_locked()
{
    set_all_chip_selects_high();
    Serial.println("[SPI] CS states: SD=HIGH, MAX=HIGH");
}

SpiBusLock::SpiBusLock(SpiBusOwner owner_in) : owner(owner_in)
{
    ensure_mutex_created();
    Serial.printf("[SPI] Lock acquire requested by %s\n", owner_to_string(owner));
    if (g_spi_bus_mutex != nullptr)
    {
        xSemaphoreTakeRecursive(g_spi_bus_mutex, portMAX_DELAY);
    }

    Serial.printf("[SPI] Lock acquired by %s\n", owner_to_string(owner));

    if (owner == SpiBusOwner::Sd)
    {
        prepare_sd_bus_locked();
    }
    else
    {
        prepare_max_bus_locked();
    }
}

SpiBusLock::~SpiBusLock()
{
    set_all_chip_selects_high();
    Serial.println("[SPI] CS states: SD=HIGH, MAX=HIGH");

    if (g_spi_bus_mutex != nullptr)
    {
        xSemaphoreGiveRecursive(g_spi_bus_mutex);
    }

    Serial.printf("[SPI] Lock released by %s\n", owner_to_string(owner));
}
