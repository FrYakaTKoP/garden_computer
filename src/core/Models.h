#pragma once

#include <Arduino.h>

namespace gc
{
struct Ds1307Time
{
    uint16_t year = 2000;
    uint8_t month = 1;
    uint8_t day = 1;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
};

struct EpeverTracerData
{
    bool valid = false;
    float pvVoltage = 0.0f;
    float pvCurrent = 0.0f;
    float batteryVoltage = 0.0f;
    float batteryCurrent = 0.0f;
    float batteryTemperatureC = 0.0f;
    float loadVoltage = 0.0f;
    float loadCurrent = 0.0f;
    float pvPowerWatts = 0.0f;
    float batteryPowerWatts = 0.0f;
    float loadPowerWatts = 0.0f;
    uint16_t batterySoc = 0;
    uint16_t chargingState = 0;
    uint16_t loadState = 0;
    uint16_t errorCode = 0;
};

struct Schedule
{
    uint32_t id = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t duration5min = 1;
    uint8_t weekdays = 0;
    uint8_t repeatEvery = 1;
    uint8_t pumpMask = 1;
};

struct ActiveRun
{
    uint32_t scheduleId = 0;
    uint8_t pumpMask = 0;
    unsigned long endMillis = 0;
    bool active = false;
};

struct PumpRuntimeStatus
{
    bool fillPump1Enabled = true;
    bool fillPump2Enabled = true;
    bool wateringPump1Enabled = false;
    bool wateringPump2Enabled = false;
    bool fillPump1Active = false;
    bool fillPump2Active = false;
    bool wateringPump1Active = false;
    bool wateringPump2Active = false;
    float pvVoltageThresholdV = 20.0f;
    uint32_t autonomousCycleMs = 60000;
    bool topTankFull = false;
    bool leftTankEmpty = false;
    bool leftTankFull = false;
    bool rightTankEmpty = false;
    bool rightTankFull = false;
    uint8_t autonomousPumpMask = 0;
    uint8_t scheduledPumpMask = 0;
    String nextFillPump1;
    String nextFillPump2;
};
}
