#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "core/Models.h"

namespace gc
{
class RtcService
{
public:
    RtcService(int sdaPin, int sclPin, bool enabledDefault, bool debugDefault);

    void begin();
    bool init();

    bool readDateTime(Ds1307Time &time);
    bool writeDateTime(const Ds1307Time &time);

    bool readClock(uint8_t &hour, uint8_t &minute, uint8_t &weekdayIndex, uint32_t &dayIndex, String &display);

    bool enabled() const;
    bool debug() const;
    bool present() const;

    void setDebug(bool enabled);
    void setEnabled(bool enabled);

private:
    int sdaPin_;
    int sclPin_;
    bool rtcPresent_;
    bool rtcEnabled_;
    bool rtcDebug_;
    bool rtcBusStarted_;
    unsigned long lastRtcProbeMs_;
    SemaphoreHandle_t rtcI2cMutex_;

    void ensureMutex();
    bool lockI2C(TickType_t timeoutTicks = pdMS_TO_TICKS(100));
    void unlockI2C();
    void ensureBusStarted();
    void resetBus();

    static uint8_t bcdToDec(uint8_t value);
    static uint8_t decToBcd(uint8_t value);
    static uint8_t rtcDayOfWeekFromIndex(uint8_t weekdayIndex);

    bool ds1307ReadTime(Ds1307Time &time);
    bool ds1307WriteTime(const Ds1307Time &time);
};
}
