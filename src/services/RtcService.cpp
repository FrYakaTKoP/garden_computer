#include "services/RtcService.h"

#include "core/TimeUtils.h"

namespace gc
{
RtcService::RtcService(int sdaPin, int sclPin, bool enabledDefault, bool debugDefault)
    : sdaPin_(sdaPin),
      sclPin_(sclPin),
      rtcPresent_(true),
      rtcEnabled_(enabledDefault),
      rtcDebug_(debugDefault),
      rtcBusStarted_(false),
      lastRtcProbeMs_(0),
      rtcI2cMutex_(nullptr)
{
}

void RtcService::begin()
{
    ensureMutex();
}

bool RtcService::enabled() const { return rtcEnabled_; }
bool RtcService::debug() const { return rtcDebug_; }
bool RtcService::present() const { return rtcPresent_; }

void RtcService::setDebug(bool enabled)
{
    rtcDebug_ = enabled;
}

void RtcService::setEnabled(bool enabled)
{
    rtcEnabled_ = enabled;
    if (!rtcEnabled_)
    {
        rtcPresent_ = false;
        if (lockI2C())
        {
            if (rtcBusStarted_)
            {
                Wire.end();
                rtcBusStarted_ = false;
            }
            unlockI2C();
        }
        if (rtcDebug_)
            Serial.println("RTC disabled via API");
        return;
    }

    init();
}

void RtcService::ensureMutex()
{
    if (rtcI2cMutex_ == nullptr)
        rtcI2cMutex_ = xSemaphoreCreateMutex();
}

bool RtcService::lockI2C(TickType_t timeoutTicks)
{
    ensureMutex();
    return rtcI2cMutex_ != nullptr && xSemaphoreTake(rtcI2cMutex_, timeoutTicks) == pdTRUE;
}

void RtcService::unlockI2C()
{
    if (rtcI2cMutex_ != nullptr)
        xSemaphoreGive(rtcI2cMutex_);
}

void RtcService::ensureBusStarted()
{
    if (rtcBusStarted_)
        return;

    Wire.begin(sdaPin_, sclPin_);
    Wire.setClock(100000);
    rtcBusStarted_ = true;
}

void RtcService::resetBus()
{
    if (rtcBusStarted_)
        Wire.end();
    rtcBusStarted_ = false;
    ensureBusStarted();
}

uint8_t RtcService::bcdToDec(uint8_t value)
{
    return (value >> 4) * 10 + (value & 0x0F);
}

uint8_t RtcService::decToBcd(uint8_t value)
{
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

uint8_t RtcService::rtcDayOfWeekFromIndex(uint8_t weekdayIndex)
{
    if (weekdayIndex == 0)
        return 1;
    return weekdayIndex;
}

bool RtcService::ds1307ReadTime(Ds1307Time &time)
{
    uint8_t regs[7] = {0};

    Wire.beginTransmission(0x68);
    Wire.write(0x00);
    if (Wire.endTransmission(static_cast<uint8_t>(1)) != 0)
        return false;
    if (Wire.requestFrom(static_cast<uint8_t>(0x68), static_cast<size_t>(7)) != 7)
        return false;

    for (int i = 0; i < 7; i++)
        regs[i] = Wire.read();

    time.second = bcdToDec(regs[0] & 0x7F);
    time.minute = bcdToDec(regs[1] & 0x7F);
    time.hour = bcdToDec(regs[2] & 0x3F);
    time.day = bcdToDec(regs[4] & 0x3F);
    time.month = bcdToDec(regs[5] & 0x1F);
    time.year = static_cast<uint16_t>(2000 + bcdToDec(regs[6]));
    return true;
}

bool RtcService::ds1307WriteTime(const Ds1307Time &time)
{
    uint8_t weekdayIndex = gc::time::dayOfWeekFromDate(time.year, time.month, time.day);
    uint8_t regs[8];
    regs[0] = static_cast<uint8_t>(decToBcd(time.second) & 0x7F);
    regs[1] = static_cast<uint8_t>(decToBcd(time.minute) & 0x7F);
    regs[2] = static_cast<uint8_t>(decToBcd(time.hour) & 0x3F);
    regs[3] = static_cast<uint8_t>(decToBcd(rtcDayOfWeekFromIndex(weekdayIndex)) & 0x07);
    regs[4] = static_cast<uint8_t>(decToBcd(time.day) & 0x3F);
    regs[5] = static_cast<uint8_t>(decToBcd(time.month) & 0x1F);
    regs[6] = decToBcd(static_cast<uint8_t>(time.year % 100));
    regs[7] = 0x00;

    Wire.beginTransmission(0x68);
    Wire.write(0x00);
    for (int i = 0; i < 7; i++)
        Wire.write(regs[i]);
    return Wire.endTransmission() == 0;
}

bool RtcService::init()
{
    if (!rtcEnabled_)
    {
        rtcPresent_ = false;
        if (rtcDebug_)
            Serial.println("RTC disabled by config");
        return false;
    }

    if (!lockI2C())
    {
        if (rtcDebug_)
            Serial.println("RTC init lock timeout");
        return false;
    }

    ensureBusStarted();
    lastRtcProbeMs_ = millis();

    Wire.beginTransmission(0x68);
    Wire.write(0x00);
    rtcPresent_ = (Wire.endTransmission(static_cast<uint8_t>(1)) == 0);
    if (!rtcPresent_)
    {
        if (rtcDebug_)
            Serial.println("DS1307 not found on I2C");
        unlockI2C();
        return false;
    }

    Ds1307Time now;
    if (!ds1307ReadTime(now))
    {
        resetBus();
        rtcPresent_ = ds1307ReadTime(now);
    }

    if (!rtcPresent_)
    {
        if (rtcDebug_)
            Serial.println("DS1307 probe read failed; RTC will retry later");
        unlockI2C();
        return false;
    }

    if (rtcDebug_)
        Serial.println("DS1307 ready");

    unlockI2C();
    return true;
}

bool RtcService::readDateTime(Ds1307Time &time)
{
    if (!rtcEnabled_)
        return false;

    if (!lockI2C())
    {
        if (rtcDebug_)
            Serial.println("RTC I2C lock timeout");
        return false;
    }

    if (!rtcPresent_)
    {
        if (millis() - lastRtcProbeMs_ < 5000UL)
        {
            unlockI2C();
            return false;
        }
        unlockI2C();
        init();
        if (!rtcPresent_)
            return false;

        if (!lockI2C())
        {
            if (rtcDebug_)
                Serial.println("RTC I2C relock timeout");
            return false;
        }
    }

    ensureBusStarted();
    if (ds1307ReadTime(time))
    {
        unlockI2C();
        return true;
    }

    if (rtcDebug_)
        Serial.println("RTC read failed, resetting I2C bus and retrying");

    resetBus();
    if (ds1307ReadTime(time))
    {
        unlockI2C();
        return true;
    }

    rtcPresent_ = false;
    lastRtcProbeMs_ = millis();
    if (rtcDebug_)
        Serial.println("RTC read retry failed; will reprobe later");
    unlockI2C();
    return false;
}

bool RtcService::writeDateTime(const Ds1307Time &time)
{
    if (!rtcEnabled_)
        return false;

    if (!rtcPresent_)
        init();

    if (!rtcPresent_)
        return false;

    if (!lockI2C())
        return false;

    ensureBusStarted();
    bool ok = ds1307WriteTime(time);
    if (!ok)
    {
        resetBus();
        ok = ds1307WriteTime(time);
        rtcPresent_ = ok;
        if (!ok)
            lastRtcProbeMs_ = millis();
    }

    unlockI2C();
    return ok;
}

bool RtcService::readClock(uint8_t &hour, uint8_t &minute, uint8_t &weekdayIndex, uint32_t &dayIndex, String &display)
{
    Ds1307Time now;
    if (!readDateTime(now))
        return false;

    hour = now.hour;
    minute = now.minute;
    weekdayIndex = gc::time::dayOfWeekFromDate(now.year, now.month, now.day);
    dayIndex = gc::time::daysSince2000(now.year, now.month, now.day);
    display = gc::time::formatRtcDateTime(now);
    return true;
}
}
