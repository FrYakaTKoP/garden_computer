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
    lcd_.setFont(u8g2_font_timR08_tf);
    auto drawLabel = [&](int16_t x, int16_t y, const char *text)
    {
        lcd_.setFont(u8g2_font_timB08_tf);
        lcd_.drawStr(x, y, text);
        lcd_.setFont(u8g2_font_timR08_tf);
    };

    char line[28];
    drawLabel(2, 8, "PV");
    drawBatteryIcon(47, 10, batterySocForIcon(epeverData.valid ? epeverData.batterySoc : 0));
    drawLabel(92, 8, "LOAD");

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
        snprintf(line, sizeof(line), "%.2f\xB0" "C", epeverData.batteryTemperatureC);
    else
        snprintf(line, sizeof(line), "xx.xx\xB0" "C");
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
    else
        lcd_.drawStr(2, 63, "RTC unavailable");
}

void DisplayManager::drawWaterPumpScreen(const PumpRuntimeStatus &status, const EpeverTracerData &epeverData, RtcService &rtc)
{
    lcd_.setFont(u8g2_font_4x6_tf);
    auto drawLabel = [&](int16_t x, int16_t y, const char *text)
    {
        lcd_.setFont(u8g2_font_4x6_mf);
        lcd_.drawStr(x, y, text);
        lcd_.setFont(u8g2_font_4x6_tf);
    };
    auto drawLabelValue = [&](int16_t x, int16_t y, const char *label, const char *value)
    {
        lcd_.setFont(u8g2_font_4x6_mf);
        lcd_.drawStr(x, y, label);
        int16_t labelWidth = lcd_.getStrWidth(label);
        lcd_.setFont(u8g2_font_4x6_tf);
        lcd_.drawStr(x + labelWidth + 1, y, value);
    };

    auto nextLabel = [&](bool fillEnabled, const String &nextValue) -> const char *
    {
        if (!fillEnabled)
            return "DISABLED";
        if (nextValue.length() == 0 || nextValue == "--")
            return "NO SCHEDULE";
        return nextValue.c_str();
    };

    auto drawTankBox = [&](int16_t x, int16_t y, int16_t w, int16_t h, const char *title, const char *stateValue, const char *fillValue, const char *waterValue, const char *nextValue, int16_t textXOffset, int16_t textYOffset)
    {
        lcd_.drawFrame(x, y, w, h);
        drawLabel(x + textXOffset, y + 7 + textYOffset, title);
        drawLabelValue(x + textXOffset, y + 15 + textYOffset, "W.-level:", stateValue);
        if (fillValue && fillValue[0] != '\0')
            drawLabelValue(x + textXOffset, y + 24 + textYOffset, "Fill:", fillValue);
        if (waterValue && waterValue[0] != '\0')
            drawLabelValue(x + textXOffset, y + 32 + textYOffset, "Water:", waterValue);
        if (nextValue && nextValue[0] != '\0')
            drawLabelValue(x + textXOffset, y + 40 + textYOffset, "Next:", nextValue);
    };

    drawTankBox(0, 0, 128, 17, "TOP TANK", status.topTankFull ? "FULL" : "OK", "", "", "", 2, 0);

    const char *leftFillValue = status.fillPump1Active ? "ON" : "OFF";
    const char *leftWaterValue = status.wateringPump1Enabled ? (status.wateringPump1Active ? "ON" : "OFF") : "DISABLED";
    const char *leftNextValue = nextLabel(status.fillPump1Enabled, status.nextFillPump1);
    drawTankBox(0, 16, 64, 46, "LEFT TANK", status.leftTankEmpty ? "EMPTY" : "OK", leftFillValue, leftWaterValue, leftNextValue, 3, 1);

    const char *rightFillValue = status.fillPump2Active ? "ON" : "OFF";
    const char *rightWaterValue = status.wateringPump2Enabled ? (status.wateringPump2Active ? "ON" : "OFF") : "DISABLED";
    const char *rightNextValue = nextLabel(status.fillPump2Enabled, status.nextFillPump2);
    drawTankBox(63, 16, 64, 46, "RIGHT TANK", status.rightTankEmpty ? "EMPTY" : "OK", rightFillValue, rightWaterValue, rightNextValue, 3, 1);
}

void DisplayManager::drawEnergyWifiScreen(bool apActive, float pvDailyWh, float pvMonthlyWh, float pvTotalWh)
{
    char value[24];
    lcd_.setFont(u8g2_font_timR08_tf);
    auto drawLabelValue = [&](int16_t x, int16_t y, const char *label, const char *val)
    {
        lcd_.setFont(u8g2_font_timB08_tf);
        lcd_.drawStr(x, y, label);
        int16_t labelWidth = lcd_.getStrWidth(label);
        lcd_.setFont(u8g2_font_timR08_tf);
        lcd_.drawStr(x + labelWidth + 1, y, val);
    };

    snprintf(value, sizeof(value), "%.2f kWh", pvDailyWh / 1000.0f);
    drawLabelValue(2, 12, "Day:", value);
    snprintf(value, sizeof(value), "%.2f kWh", pvMonthlyWh / 1000.0f);
    drawLabelValue(2, 24, "Month:", value);
    snprintf(value, sizeof(value), "%.2f kWh", pvTotalWh / 1000.0f);
    drawLabelValue(2, 36, "Total:", value);
    drawLabelValue(2, 48, "WiFi AP:", apActive ? "ON" : "OFF");
}

void DisplayManager::update(bool apActive, const EpeverTracerData &epeverData, const PumpRuntimeStatus &pumpRuntime, float pvDailyWh, float pvMonthlyWh, float pvTotalWh, RtcService &rtc)
{
    unsigned long nowMs = millis();

    if (nowMs - lastScreenSwitchMs_ >= 5000UL)
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
        drawEnergyWifiScreen(apActive, pvDailyWh, pvMonthlyWh, pvTotalWh);
    else
        drawWaterPumpScreen(pumpRuntime, epeverData, rtc);
    lcd_.sendBuffer();
}
}
