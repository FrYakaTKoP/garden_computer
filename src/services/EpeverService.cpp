#include "services/EpeverService.h"

#include "config/AppConfig.h"
#include "core/TimeUtils.h"
#include "services/ModbusRtuClient.h"
#include "services/RtcService.h"

namespace gc
{
EpeverService::EpeverService(ModbusRtuClient &modbus, RtcService &rtc, uint32_t pollIntervalMs)
    : modbus_(modbus),
      rtc_(rtc),
      pollIntervalMs_(pollIntervalMs),
      lastPollMs_(0),
      lastDataMs_(0),
    nextPollAllowedMs_(0),
    consecutivePollFailures_(0),
      energyFromController_(false),
      pvDailyWh_(0.0f),
      pvMonthlyWh_(0.0f),
      pvTotalWh_(0.0f),
      lastEnergySampleMs_(0),
      haveEnergySample_(false),
      energyYear_(0),
      energyMonth_(0),
      energyDay_(0),
      fallbackLastDayBucket_(0),
      fallbackLastMonthBucket_(0)
{
}

const EpeverTracerData &EpeverService::data() const { return data_; }
float EpeverService::pvDailyWh() const { return pvDailyWh_; }
float EpeverService::pvMonthlyWh() const { return pvMonthlyWh_; }
float EpeverService::pvTotalWh() const { return pvTotalWh_; }

uint32_t EpeverService::modbusU32LowHigh(const uint16_t *registers, uint16_t startIndex)
{
    return static_cast<uint32_t>(registers[startIndex]) | (static_cast<uint32_t>(registers[startIndex + 1]) << 16);
}

uint32_t EpeverService::failureBackoffMs(uint8_t failureCount)
{
    uint8_t shift = failureCount > 6 ? 6 : failureCount;
    return 1000UL << shift;
}

bool EpeverService::poll(EpeverTracerData &fresh)
{
    uint16_t powerRegisters[14] = {0};
    uint16_t temperatureRegister[1] = {0};
    uint16_t socRegister[1] = {0};
    uint16_t energyRegisters[4] = {0};
    uint16_t totalEnergyRegisters[2] = {0};
    uint16_t netBatteryCurrentRegister[1] = {0};

    if (!modbus_.readInputRegisters(config::kRegPvVoltage, 14, powerRegisters))
        return false;
    if (!modbus_.readInputRegisters(config::kRegBatterySoc, 1, socRegister))
        return false;
    if (!modbus_.readInputRegisters(config::kRegBatteryTemperature, 1, temperatureRegister))
        return false;

    fresh.pvVoltage = powerRegisters[config::kRegPvVoltage - config::kRegPvVoltage] / 100.0f;
    fresh.pvCurrent = powerRegisters[config::kRegPvCurrent - config::kRegPvVoltage] / 100.0f;
    fresh.batteryVoltage = powerRegisters[config::kRegBatteryVoltage - config::kRegPvVoltage] / 100.0f;
    fresh.batteryTemperatureC = static_cast<int16_t>(temperatureRegister[0]) / 100.0f;
    fresh.loadVoltage = powerRegisters[config::kRegLoadVoltage - config::kRegPvVoltage] / 100.0f;
    fresh.loadCurrent = powerRegisters[config::kRegLoadCurrent - config::kRegPvVoltage] / 100.0f;
    fresh.batterySoc = socRegister[0];
    fresh.pvPowerWatts = fresh.pvVoltage * fresh.pvCurrent;
    fresh.loadPowerWatts = fresh.loadVoltage * fresh.loadCurrent;

    if (modbus_.readInputRegisters(config::kRegBatteryNetCurrent, 1, netBatteryCurrentRegister))
    {
        fresh.batteryCurrent = static_cast<int16_t>(netBatteryCurrentRegister[0]) / 100.0f;
        fresh.batteryPowerWatts = fresh.batteryVoltage * fresh.batteryCurrent;
    }
    else
    {
        const float netPower = fresh.pvPowerWatts - fresh.loadPowerWatts;
        fresh.batteryPowerWatts = netPower;
        if (fresh.batteryVoltage > 0.01f)
            fresh.batteryCurrent = netPower / fresh.batteryVoltage;
        else
            fresh.batteryCurrent = 0.0f;
    }

    fresh.valid = true;
    if (fresh.batterySoc > 100)
        fresh.batterySoc = 100;

    if (modbus_.readInputRegisters(config::kRegDailyGeneratedEnergy, 4, energyRegisters))
    {
        energyFromController_ = true;
        pvDailyWh_ = modbusU32LowHigh(energyRegisters, 0) * 10.0f;
        pvMonthlyWh_ = modbusU32LowHigh(energyRegisters, 2) * 10.0f;
    }

    if (modbus_.readInputRegisters(config::kRegTotalGeneratedEnergy, 2, totalEnergyRegisters))
        pvTotalWh_ = modbusU32LowHigh(totalEnergyRegisters, 0) * 10.0f;

    return true;
}

void EpeverService::refreshIfNeeded()
{
    unsigned long nowMs = millis();
    if (nowMs < nextPollAllowedMs_)
    {
        if (nowMs - lastDataMs_ > 10000UL)
            data_.valid = false;
        return;
    }

    if (nowMs - lastPollMs_ < pollIntervalMs_)
    {
        if (nowMs - lastDataMs_ > 10000UL)
            data_.valid = false;
        return;
    }

    lastPollMs_ = nowMs;
    EpeverTracerData fresh;
    if (poll(fresh))
    {
        data_ = fresh;
        lastDataMs_ = nowMs;
        consecutivePollFailures_ = 0;
        nextPollAllowedMs_ = nowMs;
        return;
    }

    if (consecutivePollFailures_ < 250)
        consecutivePollFailures_++;
    nextPollAllowedMs_ = nowMs + failureBackoffMs(consecutivePollFailures_);

    if (nowMs - lastDataMs_ > 10000UL)
        data_.valid = false;
}

void EpeverService::updateEnergyCounters()
{
    if (energyFromController_)
        return;

    unsigned long nowMs = millis();
    if (!haveEnergySample_)
    {
        lastEnergySampleMs_ = nowMs;
        haveEnergySample_ = true;
        if (rtc_.enabled())
        {
            Ds1307Time now;
            if (rtc_.readDateTime(now))
            {
                energyYear_ = now.year;
                energyMonth_ = now.month;
                energyDay_ = now.day;
            }
        }
        return;
    }

    float dtHours = static_cast<float>(nowMs - lastEnergySampleMs_) / 3600000.0f;
    lastEnergySampleMs_ = nowMs;
    if (data_.valid && data_.pvPowerWatts > 0.0f)
    {
        float wh = data_.pvPowerWatts * dtHours;
        pvDailyWh_ += wh;
        pvMonthlyWh_ += wh;
    }

    if (rtc_.enabled())
    {
        Ds1307Time now;
        if (rtc_.readDateTime(now))
        {
            if (energyYear_ == 0)
            {
                energyYear_ = now.year;
                energyMonth_ = now.month;
                energyDay_ = now.day;
            }
            if (now.year != energyYear_ || now.month != energyMonth_)
            {
                pvMonthlyWh_ = 0.0f;
                pvDailyWh_ = 0.0f;
            }
            else if (now.day != energyDay_)
            {
                pvDailyWh_ = 0.0f;
            }
            energyYear_ = now.year;
            energyMonth_ = now.month;
            energyDay_ = now.day;
            return;
        }
    }

    uint32_t dayBucket = nowMs / 86400000UL;
    uint32_t monthBucket = nowMs / (30UL * 86400000UL);
    if (dayBucket != fallbackLastDayBucket_)
    {
        pvDailyWh_ = 0.0f;
        fallbackLastDayBucket_ = dayBucket;
    }
    if (monthBucket != fallbackLastMonthBucket_)
    {
        pvMonthlyWh_ = 0.0f;
        fallbackLastMonthBucket_ = monthBucket;
    }
}

bool EpeverService::syncRtcTimeToController()
{
    Ds1307Time now;
    if (!rtc_.readDateTime(now))
        return false;

    uint16_t rtcRegisters[3] = {
        static_cast<uint16_t>(now.second | (static_cast<uint16_t>(now.minute) << 8)),
        static_cast<uint16_t>(now.hour | (static_cast<uint16_t>(now.day) << 8)),
        static_cast<uint16_t>(now.month | (static_cast<uint16_t>(now.year - 2000) << 8))};

    return modbus_.writeMultipleRegisters(config::kEpeverRtcRegisterBase, 3, rtcRegisters);
}
}
