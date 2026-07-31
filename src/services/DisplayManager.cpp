#include "services/DisplayManager.h"

#include "core/TimeUtils.h"
#include "services/RtcService.h"

namespace gc
{
DisplayManager::DisplayManager(int sckPin, int mosiPin, int csPin)
    : lcd_(U8G2_R0, sckPin, mosiPin, csPin, U8X8_PIN_NONE),
      activeScreen_(0),
      lastScreenSwitchMs_(0),
      lastLcdRefreshMs_(0),
      lastFlowAnimMs_(0),
      flowAnimState_(0)
{
}

void DisplayManager::begin()
{
    lcd_.begin();
    lcd_.setContrast(180);
    lcd_.clearBuffer();
    lcd_.setFont(u8g2_font_6x12_tf);
    lcd_.drawStr(2, 14, "Garden Computer");
    lcd_.setFont(u8g2_font_5x7_tf);
    lcd_.drawStr(2, 30, "Reprap 12864 ready");
    lcd_.sendBuffer();

    lastScreenSwitchMs_ = millis();
    lastFlowAnimMs_ = millis();
}

uint8_t DisplayManager::batterySocForIcon(uint16_t soc)
{
    if (soc < 5)
        return 3;
    if (soc <= 30)
        return 20;
    if (soc <= 90)
        return 65;
    return 100;
}

void DisplayManager::drawBatteryIcon(int16_t x, int16_t baselineY, uint8_t soc)
{
    int16_t y = baselineY - 12;
    lcd_.drawFrame(x, y + 2, 14, 8);
    lcd_.drawBox(x + 14, y + 4, 2, 4);
    uint8_t clampedSoc = soc > 100 ? 100 : soc;
    uint8_t fill = static_cast<uint8_t>((clampedSoc * 12) / 100);
    if (clampedSoc > 90)
        fill = 12;
    if (fill > 12)
        fill = 12;
    if (fill > 0)
        lcd_.drawBox(x + 1, y + 3, fill, 6);
}

void DisplayManager::drawFlowChevrons(int16_t x, int16_t y, bool enabled)
{
    static const uint8_t sequence[4] = {1, 2, 3, 0};
    uint8_t visible = enabled ? sequence[flowAnimState_] : 0;
    if (visible >= 1)
        lcd_.drawStr(x, y, ">");
    if (visible >= 2)
        lcd_.drawStr(x + 6, y, ">");
    if (visible >= 3)
        lcd_.drawStr(x + 12, y, ">");
}

void DisplayManager::drawPowerFlowScreen(const EpeverTracerData &epeverData, RtcService &rtc)
{
    lcd_.setFont(u8g2_font_5x7_tf);

    char line[28];
    lcd_.drawStr(2, 8, "PV");
    drawBatteryIcon(47, 10, batterySocForIcon(epeverData.valid ? epeverData.batterySoc : 0));
    lcd_.drawStr(92, 8, "LOAD");

    if (epeverData.valid)
        snprintf(line, sizeof(line), "%.2fV", epeverData.pvVoltage);
    else
        snprintf(line, sizeof(line), "xx.xxV");
    lcd_.drawStr(2, 18, line);

    if (epeverData.valid)
        snprintf(line, sizeof(line), "%.2fA", epeverData.pvCurrent);
    else
        snprintf(line, sizeof(line), "xx.xxA");
    lcd_.drawStr(2, 28, line);

    if (epeverData.valid)
        snprintf(line, sizeof(line), "%.2fW", epeverData.pvPowerWatts);
    else
        snprintf(line, sizeof(line), "xx.xxW");
    lcd_.drawStr(2, 38, line);

    if (epeverData.valid)
        snprintf(line, sizeof(line), "%.2fV", epeverData.batteryVoltage);
    else
        snprintf(line, sizeof(line), "xx.xxV");
    lcd_.drawStr(47, 18, line);

    if (epeverData.valid)
        snprintf(line, sizeof(line), "%.2fA", epeverData.batteryCurrent);
    else
        snprintf(line, sizeof(line), "xx.xxA");
    lcd_.drawStr(47, 28, line);

    if (epeverData.valid)
        snprintf(line, sizeof(line), "%.2fW", epeverData.batteryPowerWatts);
    else
        snprintf(line, sizeof(line), "xx.xxW");
    lcd_.drawStr(47, 38, line);

    if (epeverData.valid)
        snprintf(line, sizeof(line), "%.2fC", epeverData.batteryTemperatureC);
    else
        snprintf(line, sizeof(line), "xx.xxC");
    lcd_.drawStr(47, 48, line);

    if (epeverData.valid)
        snprintf(line, sizeof(line), "%.2fV", epeverData.loadVoltage);
    else
        snprintf(line, sizeof(line), "xx.xxV");
    lcd_.drawStr(92, 18, line);

    if (epeverData.valid)
        snprintf(line, sizeof(line), "%.2fA", epeverData.loadCurrent);
    else
        snprintf(line, sizeof(line), "xx.xxA");
    lcd_.drawStr(92, 28, line);

    if (epeverData.valid)
        snprintf(line, sizeof(line), "%.2fW", epeverData.loadPowerWatts);
    else
        snprintf(line, sizeof(line), "xx.xxW");
    lcd_.drawStr(92, 38, line);

    drawFlowChevrons(28, 8, epeverData.valid && epeverData.pvPowerWatts > 0.2f);
    drawFlowChevrons(74, 8, epeverData.valid && epeverData.loadPowerWatts > 0.2f);

    Ds1307Time now;
    if (rtc.readDateTime(now))
        lcd_.drawStr(2, 63, gc::time::formatRtcBottomLine(now).c_str());
}

void DisplayManager::drawWaterPumpScreen(const PumpRuntimeStatus &status, const EpeverTracerData &epeverData, RtcService &rtc)
{
    lcd_.setFont(u8g2_font_5x7_tf);
    char line[40];
    snprintf(line, sizeof(line), "Auto: %s", status.newPumpsEnabled ? "ON" : "OFF");
    lcd_.drawStr(2, 10, line);
    snprintf(line, sizeof(line), "Top: %s", status.topTankFull ? "FULL" : "OPEN");
    lcd_.drawStr(2, 20, line);
    snprintf(line, sizeof(line), "Left: %s/%s", status.leftTankEmpty ? "EMPTY" : "OK", status.leftTankFull ? "FULL" : "OPEN");
    lcd_.drawStr(2, 30, line);
    snprintf(line, sizeof(line), "Right:%s/%s", status.rightTankEmpty ? "EMPTY" : "OK", status.rightTankFull ? "FULL" : "OPEN");
    lcd_.drawStr(2, 40, line);
    snprintf(line, sizeof(line), "Pumps:%u/%u", status.autonomousPumpMask, status.scheduledPumpMask);
    lcd_.drawStr(2, 50, line);
    snprintf(line, sizeof(line), "Bat:%u%%", epeverData.valid ? epeverData.batterySoc : 0);
    lcd_.drawStr(2, 60, line);

    Ds1307Time now;
    if (rtc.readDateTime(now))
        lcd_.drawStr(70, 60, gc::time::formatRtcBottomLine(now).c_str());
}

void DisplayManager::drawEnergyWifiScreen(bool apActive, float pvDailyWh, float pvMonthlyWh, float pvTotalWh)
{
    char line[40];
    lcd_.setFont(u8g2_font_5x7_tf);
    snprintf(line, sizeof(line), "Day:     %.2f kWh", pvDailyWh / 1000.0f);
    lcd_.drawStr(2, 12, line);
    snprintf(line, sizeof(line), "Month:   %.2f kWh", pvMonthlyWh / 1000.0f);
    lcd_.drawStr(2, 24, line);
    snprintf(line, sizeof(line), "Total:   %.2f kWh", pvTotalWh / 1000.0f);
    lcd_.drawStr(2, 36, line);

    snprintf(line, sizeof(line), "WiFi AP: %s", apActive ? "ON" : "OFF");
    lcd_.drawStr(2, 48, line);
}

void DisplayManager::update(bool apActive, const EpeverTracerData &epeverData, const PumpRuntimeStatus &pumpRuntime, float pvDailyWh, float pvMonthlyWh, float pvTotalWh, RtcService &rtc)
{
    unsigned long nowMs = millis();

    if (nowMs - lastScreenSwitchMs_ >= 10000UL)
    {
        activeScreen_ = (activeScreen_ + 1) % 3;
        lastScreenSwitchMs_ = nowMs;
    }

    if (nowMs - lastFlowAnimMs_ >= 350UL)
    {
        flowAnimState_ = (flowAnimState_ + 1) % 4;
        lastFlowAnimMs_ = nowMs;
    }

    if (nowMs - lastLcdRefreshMs_ < 150UL)
        return;
    lastLcdRefreshMs_ = nowMs;

    lcd_.clearBuffer();
    if (activeScreen_ == 0)
        drawPowerFlowScreen(epeverData, rtc);
    else if (activeScreen_ == 1)
        drawWaterPumpScreen(pumpRuntime, epeverData, rtc);
    else
        drawEnergyWifiScreen(apActive, pvDailyWh, pvMonthlyWh, pvTotalWh);
    lcd_.sendBuffer();
}
}
