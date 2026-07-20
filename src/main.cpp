#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <HardwareSerial.h>
#include <U8g2lib.h>

#ifndef RTC_ENABLED_DEFAULT
#define RTC_ENABLED_DEFAULT 1
#endif

#ifndef RTC_DEBUG_DEFAULT
#define RTC_DEBUG_DEFAULT 0
#endif

using namespace fs;

// Configuration
static const char *AP_PREFIX = "Tuttli9000-";
static const char *AP_PASSWORD = ""; // open AP
static const IPAddress apIP(192, 168, 4, 1);
static const uint8_t DNS_PORT = 53;
static const uint32_t AP_TIMEOUT_MS = 15UL * 60UL * 1000UL;
static const int RESTART_BUTTON_PIN = 0;
static const int PUMP1_PIN = 37;
static const int PUMP2_PIN = 38; // must not overlap the RS485 UART pins
static const int I2C_SDA_PIN = 8;
static const int I2C_SCL_PIN = 9;
static const int RS485_DE_PIN = 15;
static const int RS485_TX_PIN = 17;
static const int RS485_RX_PIN = 18;
static const int LCD_CS_PIN = 10;   // RS on Reprap 12864 (ST7920)
static const int LCD_MOSI_PIN = 11; // R/W on Reprap 12864 (ST7920)
static const int LCD_SCK_PIN = 12;  // E on Reprap 12864 (ST7920)
static const uint8_t MODBUS_SLAVE_ADDRESS = 0x01;
static const uint32_t MODBUS_BAUD_RATE = 115200;
static const uint16_t REG_PV_VOLTAGE = 0x3100;
static const uint16_t REG_PV_CURRENT = 0x3101;
static const uint16_t REG_BATTERY_VOLTAGE = 0x3104;
static const uint16_t REG_BATTERY_CURRENT = 0x3105;
static const uint16_t REG_LOAD_VOLTAGE = 0x310C;
static const uint16_t REG_LOAD_CURRENT = 0x310D;

static DNSServer dnsServer;
static AsyncWebServer server(80);
static Preferences prefs;
static bool rtcPresent = false;
static bool rtcEnabled = RTC_ENABLED_DEFAULT != 0;
static bool rtcDebug = RTC_DEBUG_DEFAULT != 0;
static bool rtcBusStarted = false;
static U8G2_ST7920_128X64_F_SW_SPI lcd(U8G2_R0, LCD_SCK_PIN, LCD_MOSI_PIN, LCD_CS_PIN, U8X8_PIN_NONE);

// MT50 frame listening
static unsigned long lastMt50FrameMs = 0;

struct EpeverTracerData
{
    bool valid = false;
    float pvVoltage = 0.0f;
    float pvCurrent = 0.0f;
    float batteryVoltage = 0.0f;
    float batteryCurrent = 0.0f;
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

static EpeverTracerData tracerData;

static uint8_t activeDisplayScreen = 0;
static unsigned long lastScreenSwitchMs = 0;
static unsigned long lastLcdRefreshMs = 0;
static unsigned long lastFlowAnimMs = 0;
static uint8_t flowAnimState = 0;

static float pvDailyWh = 0.0f;
static float pvMonthlyWh = 0.0f;
static unsigned long lastEnergySampleMs = 0;
static bool haveEnergySample = false;
static uint16_t energyYear = 0;
static uint8_t energyMonth = 0;
static uint8_t energyDay = 0;
static uint32_t fallbackLastDayBucket = 0;
static uint32_t fallbackLastMonthBucket = 0;

struct Ds1307Time
{
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
};

bool isLeapYear(uint16_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

uint8_t daysInMonth(uint16_t year, uint8_t month)
{
    switch (month)
    {
    case 2:
        return isLeapYear(year) ? 29 : 28;
    case 4:
    case 6:
    case 9:
    case 11:
        return 30;
    default:
        return 31;
    }
}

uint8_t dayOfWeekFromDate(uint16_t year, uint8_t month, uint8_t day)
{
    if (month < 3)
        year--;
    uint32_t y = year;
    uint32_t m = month;
    uint32_t d = day;
    uint32_t t = (y + y / 4 - y / 100 + y / 400 + (13 * m + 8) / 5 + d) % 7;
    return (uint8_t)((t + 5) % 7);
}

uint32_t daysSince2000(uint16_t year, uint8_t month, uint8_t day)
{
    uint32_t days = 0;
    for (uint16_t y = 2000; y < year; y++)
        days += (isLeapYear(y) ? 366 : 365);
    for (uint8_t m = 1; m < month; m++)
        days += daysInMonth(year, m);
    days += day - 1;
    return days;
}

static unsigned long apStartMillis = 0;
static bool apActive = false;

// Schedule model
struct Schedule
{
    uint32_t id;
    uint8_t hour;
    uint8_t minute;
    uint8_t duration5min;
    uint8_t weekdays;
    uint8_t repeatEvery;
    uint8_t pumpMask;
};
#define MAX_SCHEDULES 32
static Schedule schedules[MAX_SCHEDULES];
static uint8_t scheduleCount = 0;
static uint32_t nextId = 1;

struct ActiveRun
{
    uint32_t scheduleId;
    uint8_t pumpMask;
    unsigned long endMillis;
    bool active;
};
static ActiveRun activeRuns[4];

// Utilities
String makeApName()
{
    uint8_t mac[6];
    WiFi.softAPmacAddress(mac);
    char buf[32];
    snprintf(buf, sizeof(buf), "%s%02X%02X", AP_PREFIX, mac[4], mac[5]);
    return String(buf);
}
void startAP()
{
    if (apActive)
        return;
    WiFi.mode(WIFI_AP);
    String ssid = makeApName();
    WiFi.softAP(ssid.c_str(), AP_PASSWORD);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    dnsServer.start(DNS_PORT, "*", apIP);
    apStartMillis = millis();
    apActive = true;
    Serial.printf("AP started: %s\n", ssid.c_str());
}
void stopAP()
{
    if (!apActive)
        return;
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    apActive = false;
    Serial.println("AP stopped");
}

uint8_t bcdToDec(uint8_t value)
{
    return (value >> 4) * 10 + (value & 0x0F);
}

uint8_t decToBcd(uint8_t value)
{
    return ((value / 10) << 4) | (value % 10);
}

bool ds1307WriteRegister(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(0x68);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool ds1307ReadTime(Ds1307Time &time)
{
    uint8_t regs[7] = {0};

    Wire.beginTransmission(0x68);
    Wire.write(0x00);
    if (Wire.endTransmission(false) != 0)
        return false;
    if (Wire.requestFrom(0x68, 7) != 7)
        return false;

    for (int i = 0; i < 7; i++)
        regs[i] = Wire.read();

    time.second = bcdToDec(regs[0] & 0x7F);
    time.minute = bcdToDec(regs[1] & 0x7F);
    time.hour = bcdToDec(regs[2] & 0x3F);
    time.day = bcdToDec(regs[4] & 0x3F);
    time.month = bcdToDec(regs[5] & 0x1F);
    time.year = 2000 + bcdToDec(regs[6]);
    return true;
}

uint8_t rtcDayOfWeekFromIndex(uint8_t weekdayIndex)
{
    if (weekdayIndex == 0)
        return 1;
    return weekdayIndex;
}

bool ds1307WriteTime(const Ds1307Time &time)
{
    uint8_t weekdayIndex = dayOfWeekFromDate(time.year, time.month, time.day);
    uint8_t regs[8] = {
        decToBcd(time.second) & 0x7F,
        decToBcd(time.minute) & 0x7F,
        decToBcd(time.hour) & 0x3F,
        decToBcd(rtcDayOfWeekFromIndex(weekdayIndex)) & 0x07,
        decToBcd(time.day) & 0x3F,
        decToBcd(time.month) & 0x1F,
        decToBcd((uint8_t)(time.year % 100)),
        0x00};

    Wire.beginTransmission(0x68);
    Wire.write(0x00);
    for (int i = 0; i < 7; i++)
        Wire.write(regs[i]);
    return Wire.endTransmission() == 0;
}

String formatRtcDateTime(const Ds1307Time &now)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%02u.%02u.%04u %02u:%02u:%02u",
             now.day, now.month, now.year, now.hour, now.minute, now.second);
    return String(buf);
}

String formatRtcDateInput(const Ds1307Time &now)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u", now.year, now.month, now.day);
    return String(buf);
}

String formatRtcTimeInput(const Ds1307Time &now)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%02u:%02u", now.hour, now.minute);
    return String(buf);
}

void drawPvIcon(int16_t x, int16_t baselineY)
{
    int16_t y = baselineY - 12;
    lcd.drawFrame(x + 3, y + 3, 7, 7);
    lcd.drawLine(x + 6, y, x + 6, y + 2);
    lcd.drawLine(x + 6, y + 10, x + 6, y + 12);
    lcd.drawLine(x, y + 6, x + 2, y + 6);
    lcd.drawLine(x + 10, y + 6, x + 12, y + 6);
    lcd.drawLine(x + 1, y + 1, x + 2, y + 2);
    lcd.drawLine(x + 10, y + 10, x + 11, y + 11);
    lcd.drawLine(x + 10, y + 1, x + 11, y + 2);
    lcd.drawLine(x + 1, y + 10, x + 2, y + 11);
}

void drawBatteryIcon(int16_t x, int16_t baselineY, uint8_t soc)
{
    int16_t y = baselineY - 12;
    lcd.drawFrame(x, y + 2, 14, 8);
    lcd.drawBox(x + 14, y + 4, 2, 4);
    uint8_t fill = (uint8_t)((soc > 100 ? 100 : soc) / 10);
    if (fill > 0)
        lcd.drawBox(x + 1, y + 3, fill, 6);
}

void drawLoadIcon(int16_t x, int16_t baselineY)
{
    int16_t y = baselineY - 12;
    lcd.drawCircle(x + 6, y + 6, 4, U8G2_DRAW_ALL);
    lcd.drawLine(x + 6, y + 10, x + 6, y + 12);
    lcd.drawLine(x + 4, y + 12, x + 8, y + 12);
}

void drawFlowChevrons(int16_t x, int16_t y, bool enabled)
{
    static const uint8_t sequence[4] = {1, 2, 3, 0};
    uint8_t visible = enabled ? sequence[flowAnimState] : 0;
    if (visible >= 1)
        lcd.drawStr(x, y, ">");
    if (visible >= 2)
        lcd.drawStr(x + 6, y, ">");
    if (visible >= 3)
        lcd.drawStr(x + 12, y, ">");
}

void drawPowerFlowScreen()
{
    int16_t baseY = 16;
    int16_t pvX = 8;
    int16_t batX = 56;
    int16_t loadX = 104;

    drawPvIcon(pvX, baseY);
    drawBatteryIcon(batX, baseY, tracerData.batterySoc);
    drawLoadIcon(loadX, baseY);

    bool pvToBat = tracerData.valid && tracerData.pvPowerWatts > 0.5f && tracerData.batteryCurrent > 0.05f;
    bool batToLoad = tracerData.valid && tracerData.loadPowerWatts > 0.5f;
    lcd.setFont(u8g2_font_5x7_tf);
    drawFlowChevrons(38, 14, pvToBat);
    drawFlowChevrons(82, 14, batToLoad);

    char line[24];
    lcd.drawStr(2, 27, "PV");
    snprintf(line, sizeof(line), "%.1fV", tracerData.valid ? tracerData.pvVoltage : 0.0f);
    lcd.drawStr(2, 36, line);
    snprintf(line, sizeof(line), "%.1fA", tracerData.valid ? tracerData.pvCurrent : 0.0f);
    lcd.drawStr(2, 45, line);
    snprintf(line, sizeof(line), "%.0fW", tracerData.valid ? tracerData.pvPowerWatts : 0.0f);
    lcd.drawStr(2, 54, line);

    lcd.drawStr(47, 27, "BAT");
    snprintf(line, sizeof(line), "%.1fV", tracerData.valid ? tracerData.batteryVoltage : 0.0f);
    lcd.drawStr(47, 36, line);
    snprintf(line, sizeof(line), "%.1fA", tracerData.valid ? tracerData.batteryCurrent : 0.0f);
    lcd.drawStr(47, 45, line);
    snprintf(line, sizeof(line), "%.0fW", tracerData.valid ? tracerData.batteryPowerWatts : 0.0f);
    lcd.drawStr(47, 54, line);

    lcd.drawStr(92, 27, "LOAD");
    snprintf(line, sizeof(line), "%.1fV", tracerData.valid ? tracerData.loadVoltage : 0.0f);
    lcd.drawStr(92, 36, line);
    snprintf(line, sizeof(line), "%.1fA", tracerData.valid ? tracerData.loadCurrent : 0.0f);
    lcd.drawStr(92, 45, line);
    snprintf(line, sizeof(line), "%.0fW", tracerData.valid ? tracerData.loadPowerWatts : 0.0f);
    lcd.drawStr(92, 54, line);

    snprintf(line, sizeof(line), "SOC %u%%", tracerData.batterySoc);
    lcd.drawStr(42, 63, line);
}

void drawEnergyWifiScreen()
{
    char line[32];
    lcd.setFont(u8g2_font_6x12_tf);
    lcd.drawStr(2, 13, "PV Production");

    lcd.setFont(u8g2_font_5x7_tf);
    snprintf(line, sizeof(line), "Daily:   %.2f kWh", pvDailyWh / 1000.0f);
    lcd.drawStr(2, 27, line);
    snprintf(line, sizeof(line), "Monthly: %.2f kWh", pvMonthlyWh / 1000.0f);
    lcd.drawStr(2, 38, line);

    snprintf(line, sizeof(line), "WiFi AP: %s", apActive ? "ON" : "OFF");
    lcd.drawStr(2, 52, line);
    lcd.drawStr(2, 62, makeApName().c_str());
}

void updateEnergyCounters()
{
    unsigned long nowMs = millis();
    if (!haveEnergySample)
    {
        lastEnergySampleMs = nowMs;
        haveEnergySample = true;
        if (rtcPresent)
        {
            Ds1307Time now;
            if (ds1307ReadTime(now))
            {
                energyYear = now.year;
                energyMonth = now.month;
                energyDay = now.day;
            }
        }
        return;
    }

    float dtHours = (float)(nowMs - lastEnergySampleMs) / 3600000.0f;
    lastEnergySampleMs = nowMs;
    if (tracerData.valid && tracerData.pvPowerWatts > 0.0f)
    {
        float wh = tracerData.pvPowerWatts * dtHours;
        pvDailyWh += wh;
        pvMonthlyWh += wh;
    }

    if (rtcPresent)
    {
        Ds1307Time now;
        if (ds1307ReadTime(now))
        {
            if (energyYear == 0)
            {
                energyYear = now.year;
                energyMonth = now.month;
                energyDay = now.day;
            }
            if (now.year != energyYear || now.month != energyMonth)
            {
                pvMonthlyWh = 0.0f;
                pvDailyWh = 0.0f;
            }
            else if (now.day != energyDay)
            {
                pvDailyWh = 0.0f;
            }
            energyYear = now.year;
            energyMonth = now.month;
            energyDay = now.day;
            return;
        }
    }

    uint32_t dayBucket = nowMs / 86400000UL;
    uint32_t monthBucket = nowMs / (30UL * 86400000UL);
    if (dayBucket != fallbackLastDayBucket)
    {
        pvDailyWh = 0.0f;
        fallbackLastDayBucket = dayBucket;
    }
    if (monthBucket != fallbackLastMonthBucket)
    {
        pvMonthlyWh = 0.0f;
        fallbackLastMonthBucket = monthBucket;
    }
}

void updateDisplay()
{
    unsigned long nowMs = millis();

    if (nowMs - lastScreenSwitchMs >= 20000UL)
    {
        activeDisplayScreen = (activeDisplayScreen + 1) % 2;
        lastScreenSwitchMs = nowMs;
    }

    if (nowMs - lastFlowAnimMs >= 350UL)
    {
        flowAnimState = (flowAnimState + 1) % 4;
        lastFlowAnimMs = nowMs;
    }

    if (nowMs - lastLcdRefreshMs < 150UL)
        return;
    lastLcdRefreshMs = nowMs;

    lcd.clearBuffer();
    if (activeDisplayScreen == 0)
        drawPowerFlowScreen();
    else
        drawEnergyWifiScreen();
    lcd.sendBuffer();
}

uint16_t crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t pos = 0; pos < len; pos++)
    {
        crc ^= buf[pos];

        for (uint8_t i = 0; i < 8; i++)
        {
            if (crc & 1)
            {
                crc >>= 1;
                crc ^= 0xA001;
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}

void dumpRawFrame(const uint8_t *frame, size_t frameLen, const char *prefix)
{
    Serial.printf("%s len=%u: ", prefix, (unsigned)frameLen);
    for (size_t i = 0; i < frameLen; i++)
    {
        Serial.printf("%02X", frame[i]);
        if (i + 1 < frameLen)
            Serial.print(' ');
    }
    Serial.println();
}

// Decode MT50 live data frame (Function 0x43)
bool decodeMt50Frame(const uint8_t *frame, size_t frameLen, EpeverTracerData &data)
{
    // MT50 0x43 frames are exactly 67 bytes: slave(1) + func(1) + data(63) + crc(2)
    if (frameLen < 67)
        return false;

    if (frame[0] != MODBUS_SLAVE_ADDRESS || frame[1] != 0x43)
        return false;

    // Validate CRC (last 2 bytes, little-endian)
    uint16_t frameCrc = ((uint16_t)frame[frameLen - 1] << 8) | frame[frameLen - 2];
    uint16_t calcCrc = crc16(frame, (uint16_t)(frameLen - 2));
    if (frameCrc != calcCrc)
    {
        Serial.printf("MT50: CRC mismatch at frame, frameCRC=0x%04X calcCRC=0x%04X\n", frameCrc, calcCrc);
        dumpRawFrame(frame, frameLen, "MT50 raw frame");
        return false;
    }

    EpeverTracerData fresh;

    // Extract 16-bit big-endian values from frame
    // Based on working sniffer: offsets are byte positions in the frame
    auto getU16BE = [&](size_t byteOffset) -> uint16_t {
        return ((uint16_t)frame[byteOffset] << 8) | frame[byteOffset + 1];
    };

    // Offsets based on the working example (where bytes are numbered from 0)
    // PV Voltage at byte 18-19
    fresh.pvVoltage = getU16BE(18) / 100.0f;

    // PV Current at byte 20-21
    fresh.pvCurrent = getU16BE(20) / 100.0f;

    // PV Power
    fresh.pvPowerWatts = fresh.pvVoltage * fresh.pvCurrent;

    // Battery Voltage at byte 26-27
    fresh.batteryVoltage = getU16BE(26) / 100.0f;

    // Load Voltage at byte 34-35
    fresh.loadVoltage = getU16BE(34) / 100.0f;

    // Load Current at byte 36-37
    fresh.loadCurrent = getU16BE(36) / 100.0f;

    // Battery Current = PV Current - Load Current (charge controller logic)
    fresh.batteryCurrent = fresh.pvCurrent - fresh.loadCurrent;

    // SOC fallback derived from battery voltage when MT50 SOC is unavailable in the frame.
    float nominal = (fresh.batteryVoltage > 20.0f) ? 24.0f : 12.0f;
    float emptyV = nominal * 0.975f;
    float fullV = nominal * 1.10f;
    float socf = (fresh.batteryVoltage - emptyV) * 100.0f / (fullV - emptyV);
    if (socf < 0.0f)
        socf = 0.0f;
    if (socf > 100.0f)
        socf = 100.0f;
    fresh.batterySoc = (uint16_t)(socf + 0.5f);

    // Power calculations
    fresh.batteryPowerWatts = fresh.batteryVoltage * fresh.batteryCurrent;
    fresh.loadPowerWatts = fresh.loadVoltage * fresh.loadCurrent;

    fresh.valid = true;
    data = fresh;

    Serial.printf("MT50: PV=%.2fV*%.2fA=%.1fW, BAT=%.2fV*%.2fA=%.2fW, LOAD=%.2fV*%.2fA=%.1fW\n",
                  fresh.pvVoltage, fresh.pvCurrent, fresh.pvPowerWatts,
                  fresh.batteryVoltage, fresh.batteryCurrent, fresh.batteryPowerWatts,
                  fresh.loadVoltage, fresh.loadCurrent, fresh.loadPowerWatts);

    return true;}

// Process incoming MT50 frames from Serial1 buffer
void processMt50Stream()
{
    static uint8_t frameBuf[128];
    static size_t frameIdx = 0;
    static unsigned long lastByteTime = 0;

    while (Serial1.available())
    {
        uint8_t byte = (uint8_t)Serial1.read();
        lastByteTime = millis();

        frameBuf[frameIdx++] = byte;
        if (frameIdx >= sizeof(frameBuf))
            frameIdx = 0;  // Overflow protection

        // Look for frame sync pattern: 01 43
        if (frameIdx >= 2)
        {
            for (size_t i = 0; i + 1 < frameIdx; i++)
            {
                if (frameBuf[i] == MODBUS_SLAVE_ADDRESS && frameBuf[i + 1] == 0x43)
                {
                    // Found potential frame start
                    size_t frameStart = i;
                    size_t availableBytes = frameIdx - frameStart;

                    if (availableBytes >= 67)
                    {
                        // Try to decode this frame
                        if (decodeMt50Frame(&frameBuf[frameStart], 67, tracerData))
                        {
                            dumpRawFrame(&frameBuf[frameStart], 67, "MT50 raw frame (valid)");
                            lastMt50FrameMs = millis();
                            // Remove decoded frame from buffer
                            if (frameStart + 67 < frameIdx)
                                memmove(frameBuf, &frameBuf[frameStart + 67], frameIdx - frameStart - 67);
                            frameIdx = frameIdx - frameStart - 67;
                            return;
                        }
                        // If CRC failed, discard one byte and keep searching for the next sync.
                        size_t dropCount = frameStart + 1;
                        if (dropCount < frameIdx)
                        {
                            memmove(frameBuf, &frameBuf[dropCount], frameIdx - dropCount);
                            frameIdx -= dropCount;
                        }
                        else
                        {
                            frameIdx = 0;
                        }
                        return;
                    }
                    break;
                }
            }
        }
    }
}

void refreshTracerDataIfNeeded()
{
    // Passive listener - just process incoming frames
    processMt50Stream();

    // Mark data as invalid if no frame received for 10 seconds
    if (millis() - lastMt50FrameMs > 10000UL)
        tracerData.valid = false;
}

bool initRtc()
{
    if (!rtcEnabled)
    {
        rtcPresent = false;
        if (rtcDebug)
            Serial.println("RTC disabled by config");
        return false;
    }

    if (!rtcBusStarted)
    {
        Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
        rtcBusStarted = true;
    }

    Wire.beginTransmission(0x68);
    Wire.write(0x00);
    rtcPresent = (Wire.endTransmission(false) == 0);
    if (!rtcPresent)
    {
        if (rtcDebug)
            Serial.println("DS1307 not found on I2C");
        return false;
    }

    Ds1307Time now;
    if (!ds1307ReadTime(now))
    {
        rtcPresent = false;
        if (rtcDebug)
            Serial.println("DS1307 probe read failed; RTC disabled");
        return false;
    }

    if (rtcDebug)
        Serial.println("DS1307 ready");
    return true;
}

bool readRtcTime(uint8_t &hour, uint8_t &minute, uint8_t &weekdayIndex, uint32_t &dayIndex, String &display)
{
    if (!rtcEnabled || !rtcPresent)
        return false;

    Ds1307Time now;
    if (!ds1307ReadTime(now))
    {
        rtcPresent = false;
        if (rtcDebug)
            Serial.println("RTC read failed; falling back to uptime clock");
        return false;
    }

    hour = now.hour;
    minute = now.minute;
    weekdayIndex = dayOfWeekFromDate(now.year, now.month, now.day);
    dayIndex = daysSince2000(now.year, now.month, now.day);
    display = formatRtcDateTime(now);
    return true;
}

// Persistence
void saveSchedules()
{
    DynamicJsonDocument doc(8192);
    doc["nextId"] = nextId;
    doc["count"] = scheduleCount;
    JsonArray arr = doc.createNestedArray("schedules");
    for (int i = 0; i < scheduleCount; i++)
    {
        JsonObject o = arr.createNestedObject();
        o["id"] = schedules[i].id;
        o["hour"] = schedules[i].hour;
        o["minute"] = schedules[i].minute;
        o["duration5min"] = schedules[i].duration5min;
        o["weekdays"] = schedules[i].weekdays;
        o["repeatEvery"] = schedules[i].repeatEvery;
        o["pumpMask"] = schedules[i].pumpMask;
    }
    String out;
    serializeJson(doc, out);
    prefs.putString("schedules", out);
}
void loadSchedules()
{
    String in = prefs.getString("schedules", "");
    if (in.length() == 0)
    {
        scheduleCount = 0;
        nextId = 1;
        return;
    }
    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, in);
    if (err)
    {
        scheduleCount = 0;
        nextId = 1;
        return;
    }
    nextId = doc["nextId"] | 1;
    JsonArray arr = doc["schedules"].as<JsonArray>();
    scheduleCount = 0;
    for (JsonObject o : arr)
    {
        if (scheduleCount >= MAX_SCHEDULES)
            break;
        schedules[scheduleCount].id = o["id"].as<uint32_t>();
        schedules[scheduleCount].hour = o["hour"].as<uint8_t>();
        schedules[scheduleCount].minute = o["minute"].as<uint8_t>();
        schedules[scheduleCount].duration5min = o["duration5min"].as<uint8_t>();
        schedules[scheduleCount].weekdays = o["weekdays"].as<uint8_t>();
        schedules[scheduleCount].repeatEvery = o["repeatEvery"].as<uint8_t>();
        schedules[scheduleCount].pumpMask = o["pumpMask"].as<uint8_t>();
        scheduleCount++;
    }
}

uint8_t snapTo5(uint8_t m) { return (m / 5) * 5; }
int findScheduleIndex(uint32_t id)
{
    for (int i = 0; i < scheduleCount; i++)
        if (schedules[i].id == id)
            return i;
    return -1;
}
void setPumpPins()
{
    pinMode(PUMP1_PIN, OUTPUT);
    pinMode(PUMP2_PIN, OUTPUT);
    digitalWrite(PUMP1_PIN, LOW);
    digitalWrite(PUMP2_PIN, LOW);
}
void updateOutputs()
{
    uint8_t combined = 0;
    for (int i = 0; i < 4; i++)
        if (activeRuns[i].active)
            combined |= activeRuns[i].pumpMask;
    digitalWrite(PUMP1_PIN, (combined & 1) ? HIGH : LOW);
    digitalWrite(PUMP2_PIN, (combined & 2) ? HIGH : LOW);
}
void activatePumpMask(uint8_t mask, uint32_t durationMs, uint32_t schedId)
{
    unsigned long now = millis();
    unsigned long endt = now + durationMs;
    for (int i = 0; i < 4; i++)
        if (!activeRuns[i].active)
        {
            activeRuns[i].active = true;
            activeRuns[i].scheduleId = schedId;
            activeRuns[i].pumpMask = mask;
            activeRuns[i].endMillis = endt;
            break;
        }
    updateOutputs();
}
void pumpTick()
{
    unsigned long now = millis();
    bool changed = false;
    for (int i = 0; i < 4; i++)
        if (activeRuns[i].active && now >= activeRuns[i].endMillis)
        {
            activeRuns[i].active = false;
            changed = true;
        }
    if (changed)
        updateOutputs();
}
uint8_t minutesToSteps(int minutes)
{
    if (minutes <= 0)
        return 1;
    int steps = (minutes + 2) / 5;
    if (steps < 1)
        steps = 1;
    if (steps > 12)
        steps = 12;
    return (uint8_t)steps;
}

bool scheduleMatchesToday(const Schedule &s, uint8_t weekdayIndex, uint32_t dayIndex)
{
    int bitIndex = (weekdayIndex == 0) ? 6 : (weekdayIndex - 1);
    bool weekdayMatch = (s.weekdays & (1 << bitIndex)) != 0;
    if (s.repeatEvery <= 1)
        return weekdayMatch;
    if (!weekdayMatch)
        return false;
    return (dayIndex % s.repeatEvery) == 0;
}

void checkSchedules()
{
    static uint32_t lastMinute = 0;
    static uint32_t lastDayIndex = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t weekday = 0;
    uint32_t dayIndex = 0;
    String display;

    if (rtcPresent && readRtcTime(hour, minute, weekday, dayIndex, display))
    {
        uint32_t currentMinute = (uint32_t)hour * 60UL + minute;
        if (currentMinute == lastMinute && dayIndex == lastDayIndex)
            return;
        lastMinute = currentMinute;
        lastDayIndex = dayIndex;
    }
    else
    {
        unsigned long m = millis() / 60000UL;
        if (m == lastMinute)
            return;
        lastMinute = m;
        unsigned long totalMinutes = m % (24UL * 60UL);
        hour = totalMinutes / 60;
        minute = totalMinutes % 60;
        weekday = (m / (24UL * 60UL)) % 7;
        dayIndex = m / (24UL * 60UL);
    }

    for (int i = 0; i < scheduleCount; i++)
    {
        Schedule &s = schedules[i];
        if (s.hour == hour && s.minute == minute && scheduleMatchesToday(s, weekday, dayIndex))
        {
            uint32_t durationMs = s.duration5min * 5UL * 60UL * 1000UL;
            activatePumpMask(s.pumpMask, durationMs, s.id);
            Serial.printf("Trigger schedule %u pumpMask %u for %ums\n", s.id, s.pumpMask, (unsigned)durationMs);
        }
    }
}

void initFileServer()
{
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *r)
              { r->redirect("/"); });
    server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *r)
              { r->redirect("/"); });
    server.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest *r)
              { r->send(200, "text/plain", "success"); });
    server.onNotFound([](AsyncWebServerRequest *r)
                      {
if(r->method()==HTTP_GET) r->redirect(String("http://") + apIP.toString() + "/"); else r->send(404); });
}

// API handlers
void apiStatus(AsyncWebServerRequest *request)
{
    refreshTracerDataIfNeeded();

    DynamicJsonDocument doc(512);
    String rtcDisplay = "";
    bool rtcReadOk = false;
    Ds1307Time now;
    if (rtcEnabled && rtcPresent)
    {
        if (ds1307ReadTime(now))
        {
            rtcDisplay = formatRtcDateTime(now);
            rtcReadOk = true;
        }
        else
        {
            rtcPresent = false;
            if (rtcDebug)
                Serial.println("RTC status read failed; disabling RTC access");
        }
    }
    doc["uptimeMin"] = millis() / 60000UL;
    doc["batteryV"] = tracerData.valid ? tracerData.batteryVoltage : 0.0f;
    doc["solarW"] = tracerData.valid ? (tracerData.pvVoltage * tracerData.pvCurrent) : 0.0f;
    doc["pvVoltage"] = tracerData.valid ? tracerData.pvVoltage : 0.0f;
    doc["pvCurrent"] = tracerData.valid ? tracerData.pvCurrent : 0.0f;
    doc["batteryVoltage"] = tracerData.valid ? tracerData.batteryVoltage : 0.0f;
    doc["batteryCurrent"] = tracerData.valid ? tracerData.batteryCurrent : 0.0f;
    doc["loadVoltage"] = tracerData.valid ? tracerData.loadVoltage : 0.0f;
    doc["loadCurrent"] = tracerData.valid ? tracerData.loadCurrent : 0.0f;
    doc["tracerValid"] = tracerData.valid;
    doc["pump1Active"] = (activeRuns[0].active || activeRuns[1].active);
    doc["pump2Active"] = (activeRuns[2].active || activeRuns[3].active);
    doc["apActive"] = apActive;
    doc["ssid"] = makeApName();
    doc["rtcEnabled"] = rtcEnabled;
    doc["rtcDebug"] = rtcDebug;
    doc["rtcPresent"] = rtcPresent;
    doc["rtcDisplay"] = rtcDisplay;
    if (rtcReadOk)
    {
        doc["rtcDateInput"] = formatRtcDateInput(now);
        doc["rtcTimeInput"] = formatRtcTimeInput(now);
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}
void apiListSchedules(AsyncWebServerRequest *request)
{
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.createNestedArray("schedules");
    for (int i = 0; i < scheduleCount; i++)
    {
        JsonObject o = arr.createNestedObject();
        o["id"] = schedules[i].id;
        o["hour"] = schedules[i].hour;
        o["minute"] = schedules[i].minute;
        o["duration5min"] = schedules[i].duration5min;
        o["weekdays"] = schedules[i].weekdays;
        o["pumpMask"] = schedules[i].pumpMask;
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}
void apiGetSchedule(AsyncWebServerRequest *request)
{
    if (!request->hasArg("id"))
    {
        request->send(400);
        return;
    }
    uint32_t id = request->arg("id").toInt();
    int idx = findScheduleIndex(id);
    if (idx < 0)
    {
        request->send(404);
        return;
    }
    DynamicJsonDocument doc(256);
    Schedule &s = schedules[idx];
    doc["id"] = s.id;
    doc["hour"] = s.hour;
    doc["minute"] = s.minute;
    doc["duration5min"] = s.duration5min;
    doc["weekdays"] = s.weekdays;
    doc["pumpMask"] = s.pumpMask;
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}
void handleCreateSchedule(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
{
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, (const char *)data, len);
    if (err)
    {
        request->send(400);
        return;
    }
    if (scheduleCount >= MAX_SCHEDULES)
    {
        request->send(500, "text/plain", "full");
        return;
    }
    Schedule s;
    s.id = nextId++;
    s.hour = doc["hour"] | 0;
    s.minute = snapTo5(doc["minute"] | 0);
    if (doc.containsKey("duration5min"))
        s.duration5min = (uint8_t)max(1, (int)(doc["duration5min"] | 1));
    else if (doc.containsKey("durationMinutes"))
        s.duration5min = minutesToSteps((int)(doc["durationMinutes"] | 5));
    else
        s.duration5min = 1;
    s.weekdays = doc["weekdays"] | 0;
    s.repeatEvery = doc["repeatEvery"] | 0;
    s.pumpMask = doc["pumpMask"] | 1;
    schedules[scheduleCount++] = s;
    saveSchedules();
    request->send(200, "application/json", "{\"ok\":1}");
}
void handleUpdateSchedule(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
{
    if (!request->hasArg("id"))
    {
        request->send(400);
        return;
    }
    uint32_t id = request->arg("id").toInt();
    int idx = findScheduleIndex(id);
    if (idx < 0)
    {
        request->send(404);
        return;
    }
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, (const char *)data, len);
    if (err)
    {
        request->send(400);
        return;
    }
    schedules[idx].hour = doc["hour"] | schedules[idx].hour;
    schedules[idx].minute = snapTo5(doc["minute"] | schedules[idx].minute);
    if (doc.containsKey("duration5min"))
        schedules[idx].duration5min = (uint8_t)max(1, (int)(doc["duration5min"] | schedules[idx].duration5min));
    else if (doc.containsKey("durationMinutes"))
        schedules[idx].duration5min = minutesToSteps((int)(doc["durationMinutes"] | schedules[idx].duration5min));
    schedules[idx].weekdays = doc["weekdays"] | schedules[idx].weekdays;
    schedules[idx].pumpMask = doc["pumpMask"] | schedules[idx].pumpMask;
    saveSchedules();
    request->send(200, "application/json", "{\"ok\":1}");
}
void handleDeleteSchedule(AsyncWebServerRequest *request)
{
    if (!request->hasArg("id"))
    {
        request->send(400);
        return;
    }
    uint32_t id = request->arg("id").toInt();
    int idx = findScheduleIndex(id);
    if (idx < 0)
    {
        request->send(404);
        return;
    }
    for (int i = idx; i + 1 < scheduleCount; i++)
        schedules[i] = schedules[i + 1];
    scheduleCount--;
    saveSchedules();
    request->send(200, "application/json", "{\"ok\":1}");
}

void handleSetRtc(AsyncWebServerRequest *request)
{
    if (!rtcEnabled || !rtcPresent)
    {
        request->send(500, "application/json", "{\"ok\":0,\"error\":\"RTC unavailable\"}");
        return;
    }

    Ds1307Time time;
    time.year = request->hasArg("year") ? request->arg("year").toInt() : 2000;
    time.month = request->hasArg("month") ? request->arg("month").toInt() : 1;
    time.day = request->hasArg("day") ? request->arg("day").toInt() : 1;
    time.hour = request->hasArg("hour") ? request->arg("hour").toInt() : 0;
    time.minute = request->hasArg("minute") ? request->arg("minute").toInt() : 0;
    time.second = request->hasArg("second") ? request->arg("second").toInt() : 0;

    bool ok = ds1307WriteTime(time);
    if (!ok)
        rtcPresent = false;
    request->send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":1}" : "{\"ok\":0,\"error\":\"write failed\"}");
}

void handleRtcConfig(AsyncWebServerRequest *request)
{
    if (request->hasArg("enabled"))
    {
        rtcEnabled = request->arg("enabled").toInt() != 0;
        if (!rtcEnabled)
        {
            rtcPresent = false;
            if (rtcBusStarted)
            {
                Wire.end();
                rtcBusStarted = false;
            }
            if (rtcDebug)
                Serial.println("RTC disabled via API");
        }
        else
        {
            initRtc();
        }
    }

    if (request->hasArg("debug"))
        rtcDebug = request->arg("debug").toInt() != 0;

    DynamicJsonDocument doc(256);
    doc["ok"] = 1;
    doc["rtcEnabled"] = rtcEnabled;
    doc["rtcDebug"] = rtcDebug;
    doc["rtcPresent"] = rtcPresent;
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void setupServerRoutes()
{
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *r)
              { apiStatus(r); });
    server.on("/api/schedules", HTTP_GET, [](AsyncWebServerRequest *r)
              { apiListSchedules(r); });
    server.on("/api/schedules/get", HTTP_GET, [](AsyncWebServerRequest *r)
              { apiGetSchedule(r); });
    server.on("/api/schedules", HTTP_POST, [](AsyncWebServerRequest *r)
              { r->send(200); }, NULL, handleCreateSchedule);
    server.on("/api/schedules/update", HTTP_PUT, [](AsyncWebServerRequest *r)
              { r->send(200); }, NULL, handleUpdateSchedule);
    server.on("/api/schedules/delete", HTTP_DELETE, [](AsyncWebServerRequest *r)
              { handleDeleteSchedule(r); });
    server.on("/api/rtc/set", HTTP_POST, [](AsyncWebServerRequest *r)
              { handleSetRtc(r); });
    server.on("/api/rtc/config", HTTP_GET, [](AsyncWebServerRequest *r)
              { handleRtcConfig(r); });
    server.on("/api/rtc/config", HTTP_POST, [](AsyncWebServerRequest *r)
              { handleRtcConfig(r); });
    initFileServer();
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    if (PUMP1_PIN == RS485_TX_PIN || PUMP1_PIN == RS485_RX_PIN || PUMP1_PIN == RS485_DE_PIN ||
        PUMP2_PIN == RS485_TX_PIN || PUMP2_PIN == RS485_RX_PIN || PUMP2_PIN == RS485_DE_PIN)
    {
        Serial.println("Pin conflict detected between pump outputs and RS485 pins");
    }
    if (!LittleFS.begin(true))
        Serial.println("LittleFS mount failed");
    else
        Serial.println("LittleFS mounted");
    prefs.begin("gc", false);
    loadSchedules();
    initRtc();
    pinMode(RESTART_BUTTON_PIN, INPUT_PULLUP);
    pinMode(RS485_DE_PIN, OUTPUT);
    digitalWrite(RS485_DE_PIN, LOW);
    Serial1.begin(MODBUS_BAUD_RATE, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
    Serial.printf("RS485 UART1 ready on pins TX=%d RX=%d DE=%d (passive MT50 listener)\n", RS485_TX_PIN, RS485_RX_PIN, RS485_DE_PIN);
    setPumpPins();
    Serial.printf("LCD: ST7920 software SPI SCK=%d MOSI=%d CS=%d\n", LCD_SCK_PIN, LCD_MOSI_PIN, LCD_CS_PIN);
    lcd.begin();
    lcd.setContrast(180);
    lcd.clearBuffer();
    lcd.setFont(u8g2_font_6x12_tf);
    lcd.drawStr(2, 14, "Garden Computer");
    lcd.setFont(u8g2_font_5x7_tf);
    lcd.drawStr(2, 30, "Reprap 12864 ready");
    lcd.sendBuffer();
    startAP();
    setupServerRoutes();
    server.begin();
    refreshTracerDataIfNeeded();
    lastScreenSwitchMs = millis();
    lastFlowAnimMs = millis();
    lastLcdRefreshMs = 0;
    Serial.println("Async server started");
}
void loop()
{
    dnsServer.processNextRequest();
    refreshTracerDataIfNeeded();
    if (apActive && (millis() - apStartMillis > AP_TIMEOUT_MS))
        stopAP();
    static unsigned long lastButton = 0;
    if (digitalRead(RESTART_BUTTON_PIN) == LOW)
    {
        if (millis() - lastButton > 800)
        {
            startAP();
            lastButton = millis();
        }
    }
    updateEnergyCounters();
    updateDisplay();
    pumpTick();
    checkSchedules();
    delay(10);
}