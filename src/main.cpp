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

using namespace fs;

// Configuration
static const char *AP_PREFIX = "Tuttli9000-";
static const char *AP_PASSWORD = ""; // open AP
static const IPAddress apIP(192, 168, 4, 1);
static const uint8_t DNS_PORT = 53;
static const uint32_t AP_TIMEOUT_MS = 15UL * 60UL * 1000UL;
static const int RESTART_BUTTON_PIN = 0;
static const int PUMP1_PIN = 16;
static const int PUMP2_PIN = 17;
static const int I2C_SDA_PIN = 8;
static const int I2C_SCL_PIN = 9;
static const int RS485_DE_PIN = 15;
static const int RS485_TX_PIN = 17;
static const int RS485_RX_PIN = 18;
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
static unsigned long lastTracerPollMs = 0;

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

uint16_t modbusCrc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= (uint16_t)data[i];
        for (int bit = 0; bit < 8; ++bit)
        {
            if ((crc & 0x0001) != 0)
            {
                crc = (uint16_t)((crc >> 1) ^ 0xA001);
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}

bool modbusReadRegister(uint16_t address, uint16_t &value, uint32_t timeoutMs = 500)
{
    uint8_t request[8] = {MODBUS_SLAVE_ADDRESS, 0x03,
                          (uint8_t)((address >> 8) & 0xFF),
                          (uint8_t)(address & 0xFF),
                          0x00, 0x01, 0x00, 0x00};
    uint16_t crc = modbusCrc16(request, 6);
    request[6] = (uint8_t)(crc & 0xFF);
    request[7] = (uint8_t)((crc >> 8) & 0xFF);

    while (Serial1.available())
        Serial1.read();

    digitalWrite(RS485_DE_PIN, HIGH);
    Serial1.write(request, sizeof(request));
    Serial1.flush();
    digitalWrite(RS485_DE_PIN, LOW);
    delay(20);

    uint8_t response[64] = {0};
    size_t index = 0;
    unsigned long start = millis();
    while (millis() - start < timeoutMs)
    {
        while (Serial1.available() && index < sizeof(response))
        {
            response[index++] = (uint8_t)Serial1.read();
            if (index >= 8)
                break;
        }
        if (index >= 8)
            break;
        delay(5);
    }

    if (index < 8)
        return false;

    if (response[0] != MODBUS_SLAVE_ADDRESS || response[1] != 0x03)
        return false;

    uint8_t byteCount = response[2];
    if (byteCount != 2)
        return false;

    uint16_t responseCrc = ((uint16_t)response[index - 1] << 8) | response[index - 2];
    uint16_t calcCrc = modbusCrc16(response, index - 2);
    if (responseCrc != calcCrc)
        return false;

    value = ((uint16_t)response[3] << 8) | response[4];
    return true;
}

bool readTracerData(EpeverTracerData &data)
{
    EpeverTracerData fresh;
    uint16_t raw = 0;

    if (!modbusReadRegister(REG_PV_VOLTAGE, raw))
        return false;
    fresh.pvVoltage = raw / 10.0f;

    if (!modbusReadRegister(REG_PV_CURRENT, raw))
        return false;
    fresh.pvCurrent = raw / 10.0f;

    if (!modbusReadRegister(REG_BATTERY_VOLTAGE, raw))
        return false;
    fresh.batteryVoltage = raw / 10.0f;

    if (!modbusReadRegister(REG_BATTERY_CURRENT, raw))
        return false;
    fresh.batteryCurrent = raw / 10.0f;

    if (!modbusReadRegister(REG_LOAD_VOLTAGE, raw))
        return false;
    fresh.loadVoltage = raw / 10.0f;

    if (!modbusReadRegister(REG_LOAD_CURRENT, raw))
        return false;
    fresh.loadCurrent = raw / 10.0f;

    fresh.valid = true;
    data = fresh;
    return true;
}

void refreshTracerDataIfNeeded()
{
    unsigned long now = millis();
    if (now - lastTracerPollMs < 3000UL)
        return;

    lastTracerPollMs = now;
    static uint32_t tracerFailureCount = 0;

    if (!readTracerData(tracerData))
    {
        tracerData.valid = false;
        if (++tracerFailureCount % 10 == 0)
            Serial.println("Tracer poll failed");
    }
    else
    {
        tracerFailureCount = 0;
    }
}

bool initRtc()
{
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    uint8_t dummy = 0;
    Wire.beginTransmission(0x68);
    Wire.write(0x00);
    rtcPresent = (Wire.endTransmission(false) == 0);
    if (!rtcPresent)
    {
        Serial.println("DS1307 not found on I2C");
        return false;
    }

    if (dummy & 0x80)
    {
        Serial.println("DS1307 was halted; restarting it");
        ds1307WriteRegister(0x00, dummy & 0x7F);
    }
    Serial.println("DS1307 ready");
    return true;
}

bool readRtcTime(uint8_t &hour, uint8_t &minute, uint8_t &weekdayIndex, uint32_t &dayIndex, String &display)
{
    if (!rtcPresent)
        return false;

    Ds1307Time now;
    if (!ds1307ReadTime(now))
        return false;

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

    if (rtcPresent)
    {
        if (!readRtcTime(hour, minute, weekday, dayIndex, display))
            return;
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
    if (rtcPresent)
    {
        Ds1307Time now;
        if (ds1307ReadTime(now))
            rtcDisplay = formatRtcDateTime(now);
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
    doc["rtcPresent"] = rtcPresent;
    doc["rtcDisplay"] = rtcDisplay;
    if (rtcPresent)
    {
        Ds1307Time now;
        if (ds1307ReadTime(now))
        {
            doc["rtcDateInput"] = formatRtcDateInput(now);
            doc["rtcTimeInput"] = formatRtcTimeInput(now);
        }
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
    if (!rtcPresent)
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
    request->send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":1}" : "{\"ok\":0,\"error\":\"write failed\"}");
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
    initFileServer();
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    if (!LittleFS.begin(true))
        Serial.println("LittleFS mount failed");
    else
        Serial.println("LittleFS mounted");
    prefs.begin("gc", false);
    loadSchedules();
    initRtc();
    pinMode(RS485_DE_PIN, OUTPUT);
    digitalWrite(RS485_DE_PIN, LOW);
    Serial1.begin(MODBUS_BAUD_RATE, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
    Serial.printf("RS485 UART1 ready on pins TX=%d RX=%d DE=%d\n", RS485_TX_PIN, RS485_RX_PIN, RS485_DE_PIN);
    setPumpPins();
    startAP();
    setupServerRoutes();
    server.begin();
    refreshTracerDataIfNeeded();
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
    pumpTick();
    checkSchedules();
    delay(10);
}