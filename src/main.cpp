#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>

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

static DNSServer dnsServer;
static AsyncWebServer server(80);
static Preferences prefs;

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
    unsigned long m = millis() / 60000UL;
    if (m == lastMinute)
        return;
    lastMinute = m;
    unsigned long totalMinutes = m % (24UL * 60UL);
    uint8_t hour = totalMinutes / 60;
    uint8_t minute = totalMinutes % 60;
    uint8_t weekday = (m / (24UL * 60UL)) % 7;
    uint32_t dayIndex = m / (24UL * 60UL);
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
    DynamicJsonDocument doc(256);
    doc["uptimeMin"] = millis() / 60000UL;
    doc["batteryV"] = 12.4;
    doc["solarW"] = 124.5;
    doc["pump1Active"] = (activeRuns[0].active || activeRuns[1].active);
    doc["pump2Active"] = (activeRuns[2].active || activeRuns[3].active);
    doc["apActive"] = apActive;
    doc["ssid"] = makeApName();
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
    setPumpPins();
    startAP();
    setupServerRoutes();
    server.begin();
    Serial.println("Async server started");
}
void loop()
{
    dnsServer.processNextRequest();
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