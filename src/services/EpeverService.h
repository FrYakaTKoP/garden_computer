#pragma once

#include <Arduino.h>
#include "core/Models.h"

namespace gc
{
class ModbusRtuClient;
class RtcService;

class EpeverService
{
public:
    EpeverService(ModbusRtuClient &modbus, RtcService &rtc, uint32_t pollIntervalMs);

    void refreshIfNeeded();
    void updateEnergyCounters();

    bool syncRtcTimeToController();

    const EpeverTracerData &data() const;
    float pvDailyWh() const;
    float pvMonthlyWh() const;
    float pvTotalWh() const;

private:
    ModbusRtuClient &modbus_;
    RtcService &rtc_;
    uint32_t pollIntervalMs_;

    unsigned long lastPollMs_;
    unsigned long lastDataMs_;
    unsigned long nextPollAllowedMs_;
    uint8_t consecutivePollFailures_;

    EpeverTracerData data_;

    bool energyFromController_;
    float pvDailyWh_;
    float pvMonthlyWh_;
    float pvTotalWh_;
    unsigned long lastEnergySampleMs_;
    bool haveEnergySample_;
    uint16_t energyYear_;
    uint8_t energyMonth_;
    uint8_t energyDay_;
    uint32_t fallbackLastDayBucket_;
    uint32_t fallbackLastMonthBucket_;

    static uint32_t modbusU32LowHigh(const uint16_t *registers, uint16_t startIndex);
    bool poll(EpeverTracerData &fresh);
    static uint32_t failureBackoffMs(uint8_t failureCount);
};
}
