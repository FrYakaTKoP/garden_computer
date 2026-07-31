#include "services/WebPortal.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include "core/TimeUtils.h"
#include "services/EpeverService.h"
#include "services/PumpScheduler.h"
#include "services/RtcService.h"

namespace gc
{
WebPortal::WebPortal(const char *apName, const char *apPassword, const IPAddress &apIp, uint8_t dnsPort, uint32_t apTimeoutMs, int restartButtonPin,
                     RtcService &rtc, EpeverService &epever, PumpScheduler &scheduler)
    : apName_(apName),
      apPassword_(apPassword),
      apIp_(apIp),
      dnsPort_(dnsPort),
      apTimeoutMs_(apTimeoutMs),
      restartButtonPin_(restartButtonPin),
      server_(80),
      apActive_(false),
      apStartMillis_(0),
      rtc_(rtc),
      epever_(epever),
      scheduler_(scheduler)
{
}

void WebPortal::begin()
{
    pinMode(restartButtonPin_, INPUT_PULLUP);
    startAP();
    setupServerRoutes();
    server_.begin();
}

void WebPortal::loop()
{
    dnsServer_.processNextRequest();

    if (apActive_ && (millis() - apStartMillis_ > apTimeoutMs_))
        stopAP();

    static unsigned long lastButton = 0;
    if (digitalRead(restartButtonPin_) == LOW)
    {
        if (millis() - lastButton > 800)
        {
            startAP();
            lastButton = millis();
        }
    }
}

bool WebPortal::isApActive() const
{
    return apActive_;
}

String WebPortal::ssid() const
{
    return String(apName_);
}

void WebPortal::startAP()
{
    if (apActive_)
        return;

    WiFi.mode(WIFI_AP);
    WiFi.softAP(apName_, apPassword_);
    WiFi.softAPConfig(apIp_, apIp_, IPAddress(255, 255, 255, 0));
    dnsServer_.start(dnsPort_, "*", apIp_);
    apStartMillis_ = millis();
    apActive_ = true;
    Serial.printf("AP started: %s\n", apName_);
}

void WebPortal::stopAP()
{
    if (!apActive_)
        return;

    dnsServer_.stop();
    WiFi.softAPdisconnect(true);
    apActive_ = false;
    Serial.println("AP stopped");
}

void WebPortal::initFileServer()
{
    server_.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    server_.on("/generate_204", HTTP_GET, [this](AsyncWebServerRequest *r)
               { r->redirect("/"); });
    server_.on("/hotspot-detect.html", HTTP_GET, [this](AsyncWebServerRequest *r)
               { r->redirect("/"); });
    server_.on("/connecttest.txt", HTTP_GET, [this](AsyncWebServerRequest *r)
               { r->send(200, "text/plain", "success"); });
    server_.onNotFound([this](AsyncWebServerRequest *r)
                       {
        if (r->method() == HTTP_GET)
            r->redirect(String("http://") + apIp_.toString() + "/");
        else
            r->send(404); });
}

void WebPortal::apiStatus(AsyncWebServerRequest *request)
{
    epever_.refreshIfNeeded();
    const EpeverTracerData &data = epever_.data();

    DynamicJsonDocument doc(640);

    String rtcDisplay = "";
    bool rtcReadOk = false;
    Ds1307Time now;
    if (rtc_.enabled() && rtc_.readDateTime(now))
    {
        rtcDisplay = gc::time::formatRtcDateTime(now);
        rtcReadOk = true;
    }

    doc["uptimeMin"] = millis() / 60000UL;
    doc["batteryV"] = data.valid ? data.batteryVoltage : 0.0f;
    doc["solarW"] = data.valid ? (data.pvVoltage * data.pvCurrent) : 0.0f;
    doc["pvVoltage"] = data.valid ? data.pvVoltage : 0.0f;
    doc["pvCurrent"] = data.valid ? data.pvCurrent : 0.0f;
    doc["batteryVoltage"] = data.valid ? data.batteryVoltage : 0.0f;
    doc["batteryCurrent"] = data.valid ? data.batteryCurrent : 0.0f;
    doc["batterySoc"] = data.valid ? data.batterySoc : 0;
    doc["batteryTempC"] = data.valid ? data.batteryTemperatureC : 0.0f;
    doc["loadVoltage"] = data.valid ? data.loadVoltage : 0.0f;
    doc["loadCurrent"] = data.valid ? data.loadCurrent : 0.0f;
    doc["pvDailyWh"] = epever_.pvDailyWh();
    doc["pvMonthlyWh"] = epever_.pvMonthlyWh();
    doc["pvTotalWh"] = epever_.pvTotalWh();
    doc["tracerValid"] = data.valid;

    const uint8_t activeMask = scheduler_.activePumpMask();
    doc["pump1Active"] = (activeMask & 1) != 0;
    doc["pump2Active"] = (activeMask & 2) != 0;

    const PumpRuntimeStatus runtime = scheduler_.runtimeStatus();
    doc["newPumpsEnabled"] = runtime.newPumpsEnabled;
    doc["batteryThresholdPct"] = runtime.batteryFullThresholdPct;
    doc["autonomousCycleMs"] = runtime.autonomousCycleMs;
    doc["topTankFull"] = runtime.topTankFull;
    doc["leftTankEmpty"] = runtime.leftTankEmpty;
    doc["leftTankFull"] = runtime.leftTankFull;
    doc["rightTankEmpty"] = runtime.rightTankEmpty;
    doc["rightTankFull"] = runtime.rightTankFull;
    doc["autonomousPumpMask"] = runtime.autonomousPumpMask;
    doc["scheduledPumpMask"] = runtime.scheduledPumpMask;

    doc["apActive"] = apActive_;
    doc["ssid"] = ssid();

    doc["rtcEnabled"] = rtc_.enabled();
    doc["rtcDebug"] = rtc_.debug();
    doc["rtcPresent"] = rtc_.present();
    doc["rtcDisplay"] = rtcDisplay;

    if (rtcReadOk)
    {
        doc["rtcDateInput"] = gc::time::formatRtcDateInput(now);
        doc["rtcTimeInput"] = gc::time::formatRtcTimeInput(now);
    }

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void WebPortal::apiListSchedules(AsyncWebServerRequest *request)
{
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.createNestedArray("schedules");
    scheduler_.writeSchedulesJson(arr);

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void WebPortal::apiGetSchedule(AsyncWebServerRequest *request)
{
    if (!request->hasArg("id"))
    {
        request->send(400);
        return;
    }

    uint32_t id = request->arg("id").toInt();
    Schedule s;
    if (!scheduler_.getSchedule(id, s))
    {
        request->send(404);
        return;
    }

    DynamicJsonDocument doc(256);
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

void WebPortal::handleCreateSchedule(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
{
    (void)index;
    (void)total;

    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, reinterpret_cast<const char *>(data), len);
    if (err)
    {
        request->send(400);
        return;
    }

    if (!scheduler_.createSchedule(doc.as<JsonVariantConst>()))
    {
        request->send(500, "text/plain", "full");
        return;
    }

    request->send(200, "application/json", "{\"ok\":1}");
}

void WebPortal::handleUpdateSchedule(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
{
    (void)index;
    (void)total;

    if (!request->hasArg("id"))
    {
        request->send(400);
        return;
    }

    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, reinterpret_cast<const char *>(data), len);
    if (err)
    {
        request->send(400);
        return;
    }

    uint32_t id = request->arg("id").toInt();
    if (!scheduler_.updateSchedule(id, doc.as<JsonVariantConst>()))
    {
        request->send(404);
        return;
    }

    request->send(200, "application/json", "{\"ok\":1}");
}

void WebPortal::handleDeleteSchedule(AsyncWebServerRequest *request)
{
    if (!request->hasArg("id"))
    {
        request->send(400);
        return;
    }

    uint32_t id = request->arg("id").toInt();
    if (!scheduler_.deleteSchedule(id))
    {
        request->send(404);
        return;
    }

    request->send(200, "application/json", "{\"ok\":1}");
}

void WebPortal::handleSetRtc(AsyncWebServerRequest *request)
{
    if (!rtc_.enabled())
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

    bool ok = rtc_.writeDateTime(time);
    request->send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":1}" : "{\"ok\":0,\"error\":\"write failed\"}");
}

void WebPortal::handleSyncRtcToEpever(AsyncWebServerRequest *request)
{
    if (!rtc_.enabled() || !rtc_.present())
    {
        request->send(500, "application/json", "{\"ok\":0,\"error\":\"RTC unavailable\"}");
        return;
    }

    bool ok = epever_.syncRtcTimeToController();
    request->send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":1}" : "{\"ok\":0,\"error\":\"sync failed\"}");
}

void WebPortal::handleRtcConfig(AsyncWebServerRequest *request)
{
    if (request->hasArg("enabled"))
        rtc_.setEnabled(request->arg("enabled").toInt() != 0);

    if (request->hasArg("debug"))
        rtc_.setDebug(request->arg("debug").toInt() != 0);

    DynamicJsonDocument doc(256);
    doc["ok"] = 1;
    doc["rtcEnabled"] = rtc_.enabled();
    doc["rtcDebug"] = rtc_.debug();
    doc["rtcPresent"] = rtc_.present();

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void WebPortal::handlePumpConfig(AsyncWebServerRequest *request)
{
    bool changed = false;
    if (request->hasArg("enabled"))
    {
        scheduler_.setNewPumpsEnabled(request->arg("enabled").toInt() != 0);
        changed = true;
    }
    if (request->hasArg("threshold"))
    {
        scheduler_.setBatteryThresholdPct(static_cast<uint8_t>(request->arg("threshold").toInt()));
        changed = true;
    }
    if (request->hasArg("cycleMs"))
    {
        scheduler_.setAutonomousCycleMs(static_cast<uint32_t>(request->arg("cycleMs").toInt()));
        changed = true;
    }

    DynamicJsonDocument doc(256);
    doc["ok"] = 1;
    doc["changed"] = changed;
    const PumpRuntimeStatus runtime = scheduler_.runtimeStatus();
    doc["newPumpsEnabled"] = runtime.newPumpsEnabled;
    doc["batteryThresholdPct"] = runtime.batteryFullThresholdPct;
    doc["autonomousCycleMs"] = runtime.autonomousCycleMs;

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void WebPortal::setupServerRoutes()
{
    server_.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *r)
               { apiStatus(r); });
    server_.on("/api/schedules", HTTP_GET, [this](AsyncWebServerRequest *r)
               { apiListSchedules(r); });
    server_.on("/api/schedules/get", HTTP_GET, [this](AsyncWebServerRequest *r)
               { apiGetSchedule(r); });
    server_.on("/api/schedules", HTTP_POST, [this](AsyncWebServerRequest *r)
               { (void)r; }, nullptr,
               [this](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total)
               { handleCreateSchedule(r, data, len, index, total); });
    server_.on("/api/schedules/update", HTTP_PUT, [this](AsyncWebServerRequest *r)
               { (void)r; }, nullptr,
               [this](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total)
               { handleUpdateSchedule(r, data, len, index, total); });
    server_.on("/api/schedules/delete", HTTP_DELETE, [this](AsyncWebServerRequest *r)
               { handleDeleteSchedule(r); });
    server_.on("/api/rtc/set", HTTP_POST, [this](AsyncWebServerRequest *r)
               { handleSetRtc(r); });
    server_.on("/api/epever/sync-time", HTTP_POST, [this](AsyncWebServerRequest *r)
               { handleSyncRtcToEpever(r); });
    server_.on("/api/rtc/config", HTTP_GET, [this](AsyncWebServerRequest *r)
               { handleRtcConfig(r); });
    server_.on("/api/rtc/config", HTTP_POST, [this](AsyncWebServerRequest *r)
               { handleRtcConfig(r); });
    server_.on("/api/pumps/config", HTTP_GET, [this](AsyncWebServerRequest *r)
               { handlePumpConfig(r); });
    server_.on("/api/pumps/config", HTTP_POST, [this](AsyncWebServerRequest *r)
               { handlePumpConfig(r); });

    initFileServer();
}
}
