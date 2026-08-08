#include "services/WebPortal.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include "core/TimeUtils.h"
#include "services/EpeverService.h"
#include "services/PumpScheduler.h"
#include "services/RtcService.h"
#include "services/SdLogger.h"

namespace gc
{
WebPortal::WebPortal(const char *apName, const char *apPassword, const IPAddress &apIp, uint8_t dnsPort, uint32_t apTimeoutMs, int restartButtonPin,
                     RtcService &rtc, EpeverService &epever, PumpScheduler &scheduler, SdLogger &logger)
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
    scheduler_(scheduler),
    logger_(logger)
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
    const EpeverTracerData &data = epever_.data();

    DynamicJsonDocument doc(1152);

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

    const PumpRuntimeStatus runtime = scheduler_.runtimeStatus(rtc_);
    doc["fillPump1Enabled"] = runtime.fillPump1Enabled;
    doc["fillPump2Enabled"] = runtime.fillPump2Enabled;
    doc["wateringPump1Enabled"] = runtime.wateringPump1Enabled;
    doc["wateringPump2Enabled"] = runtime.wateringPump2Enabled;
    doc["fillPump1Active"] = runtime.fillPump1Active;
    doc["fillPump2Active"] = runtime.fillPump2Active;
    doc["wateringPump1Active"] = runtime.wateringPump1Active;
    doc["wateringPump2Active"] = runtime.wateringPump2Active;
    doc["pvVoltageThresholdV"] = runtime.pvVoltageThresholdV;
    doc["autonomousCycleMs"] = runtime.autonomousCycleMs;
    doc["topTankFull"] = runtime.topTankFull;
    doc["leftTankEmpty"] = runtime.leftTankEmpty;
    doc["leftTankFull"] = runtime.leftTankFull;
    doc["rightTankEmpty"] = runtime.rightTankEmpty;
    doc["rightTankFull"] = runtime.rightTankFull;
    doc["fillPumpMask"] = runtime.scheduledPumpMask;
    doc["wateringPumpMask"] = runtime.autonomousPumpMask;
    doc["autonomousPumpMask"] = runtime.autonomousPumpMask;
    doc["scheduledPumpMask"] = runtime.scheduledPumpMask;
    doc["nextFillPump1"] = runtime.nextFillPump1;
    doc["nextFillPump2"] = runtime.nextFillPump2;

    doc["apActive"] = apActive_;
    doc["ssid"] = ssid();

    const SdLoggerStatus sd = logger_.status();
    doc["sdMounted"] = sd.mounted;
    doc["sdWritable"] = sd.writable;
    doc["sdIntervalMs"] = sd.intervalMs;
    doc["sdQueuedRecords"] = sd.queuedRecords;
    doc["sdDroppedRecords"] = sd.droppedRecords;
    doc["sdWriteFailures"] = sd.writeFailures;
    doc["sdLastWriteUptimeSeconds"] = sd.lastWriteUptimeSeconds;
    doc["sdTotalBytes"] = static_cast<double>(sd.totalBytes);
    doc["sdUsedBytes"] = static_cast<double>(sd.usedBytes);
    doc["sdError"] = sd.error;

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

void WebPortal::apiListLogs(AsyncWebServerRequest *request)
{
    request->send(200, "application/json", logger_.listLogFilesJson());
}

void WebPortal::apiGetLog(AsyncWebServerRequest *request)
{
    if (!request->hasArg("name"))
    {
        request->send(400, "application/json", "{\"error\":\"Missing log name\"}");
        return;
    }
    logger_.sendLogFile(request, request->arg("name"));
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
    if (request->hasArg("fillPump1Enabled"))
    {
        scheduler_.setFillPump1Enabled(request->arg("fillPump1Enabled").toInt() != 0);
        changed = true;
    }
    if (request->hasArg("fillPump2Enabled"))
    {
        scheduler_.setFillPump2Enabled(request->arg("fillPump2Enabled").toInt() != 0);
        changed = true;
    }
    if (request->hasArg("enabled"))
    {
        const bool enabled = request->arg("enabled").toInt() != 0;
        scheduler_.setWateringPump1Enabled(enabled);
        scheduler_.setWateringPump2Enabled(enabled);
        changed = true;
    }
    if (request->hasArg("wateringPump1Enabled"))
    {
        scheduler_.setWateringPump1Enabled(request->arg("wateringPump1Enabled").toInt() != 0);
        changed = true;
    }
    if (request->hasArg("wateringPump2Enabled"))
    {
        scheduler_.setWateringPump2Enabled(request->arg("wateringPump2Enabled").toInt() != 0);
        changed = true;
    }
    if (request->hasArg("pvThresholdV"))
    {
        scheduler_.setPvVoltageThresholdV(request->arg("pvThresholdV").toFloat());
        changed = true;
    }
    else if (request->hasArg("threshold"))
    {
        // Backward compatibility with older clients that posted "threshold".
        scheduler_.setPvVoltageThresholdV(request->arg("threshold").toFloat());
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
    const PumpRuntimeStatus runtime = scheduler_.runtimeStatus(rtc_);
    doc["fillPump1Enabled"] = runtime.fillPump1Enabled;
    doc["fillPump2Enabled"] = runtime.fillPump2Enabled;
    doc["wateringPump1Enabled"] = runtime.wateringPump1Enabled;
    doc["wateringPump2Enabled"] = runtime.wateringPump2Enabled;
    doc["fillPump1Active"] = runtime.fillPump1Active;
    doc["fillPump2Active"] = runtime.fillPump2Active;
    doc["wateringPump1Active"] = runtime.wateringPump1Active;
    doc["wateringPump2Active"] = runtime.wateringPump2Active;
    doc["fillPumpMask"] = runtime.scheduledPumpMask;
    doc["wateringPumpMask"] = runtime.autonomousPumpMask;
    doc["pvVoltageThresholdV"] = runtime.pvVoltageThresholdV;
    doc["autonomousCycleMs"] = runtime.autonomousCycleMs;
    doc["nextFillPump1"] = runtime.nextFillPump1;
    doc["nextFillPump2"] = runtime.nextFillPump2;

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void WebPortal::handleLoggingConfig(AsyncWebServerRequest *request)
{
    if (request->hasArg("intervalMs"))
        logger_.setIntervalMs(request->arg("intervalMs").toInt());

    const SdLoggerStatus sd = logger_.status();
    DynamicJsonDocument doc(192);
    doc["ok"] = 1;
    doc["intervalMs"] = sd.intervalMs;
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void WebPortal::setupServerRoutes()
{
    server_.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *r)
               { apiStatus(r); });
    server_.on("/api/logs", HTTP_GET, [this](AsyncWebServerRequest *r)
               { apiListLogs(r); });
    server_.on("/api/log-file", HTTP_GET, [this](AsyncWebServerRequest *r)
               { apiGetLog(r); });
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
    server_.on("/api/logging/config", HTTP_POST, [this](AsyncWebServerRequest *r)
               { handleLoggingConfig(r); });

    initFileServer();
}
}
