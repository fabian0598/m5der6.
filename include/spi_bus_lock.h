#pragma once

#include <cstdint>

enum class SpiBusOwner : uint8_t
{
    Sd,
    Max,
    Display
};

class SpiBusLock
{
public:
    explicit SpiBusLock(SpiBusOwner owner);
    ~SpiBusLock();

private:
    SpiBusOwner owner;
};

void prepare_sd_bus_locked();
void prepare_max_bus_locked();
