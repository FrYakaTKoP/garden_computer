#pragma once

#include <Arduino.h>
#include <U8g2lib.h>
#include "core/Models.h"

namespace gc
{
class RtcService;
class EpeverService;

class DisplayManager
{
public:
    DisplayManager(int sckPin, int mosiPin, int csPin);

    void begin();
    void update(bool apActive, const EpeverTracerData &epeverData, float pvDailyWh, float pvMonthlyWh, float pvTotalWh, RtcService &rtc);

private:
    U8G2_ST7920_128X64_F_SW_SPI lcd_;

    uint8_t activeScreen_;
    unsigned long lastScreenSwitchMs_;
    unsigned long lastLcdRefreshMs_;
    unsigned long lastFlowAnimMs_;
    uint8_t flowAnimState_;

    static uint8_t batterySocForIcon(uint16_t soc);

    void drawBatteryIcon(int16_t x, int16_t baselineY, uint8_t soc);
    void drawEnergyWifiScreen(bool apActive, float pvDailyWh, float pvMonthlyWh, float pvTotalWh);
    void drawPowerFlowScreen(const EpeverTracerData &epeverData, RtcService &rtc);
    void drawFlowChevrons(int16_t x, int16_t y, bool enabled);
};
}
