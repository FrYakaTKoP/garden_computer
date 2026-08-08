#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include "core/Models.h"

namespace gc
{
class RtcService;
class EpeverService;
class PumpScheduler;
class SdLogger;

class WebPortal
{
public:
    WebPortal(const char *apName, const char *apPassword, const IPAddress &apIp, uint8_t dnsPort, uint32_t apTimeoutMs, int restartButtonPin,
              RtcService &rtc, EpeverService &epever, PumpScheduler &scheduler, SdLogger &logger);

    void begin();
    void loop();

    bool isApActive() const;
    String ssid() const;

private:
    const char *apName_;
    const char *apPassword_;
    IPAddress apIp_;
    uint8_t dnsPort_;
    uint32_t apTimeoutMs_;
    int restartButtonPin_;

    DNSServer dnsServer_;
    AsyncWebServer server_;

    bool apActive_;
    unsigned long apStartMillis_;

    RtcService &rtc_;
    EpeverService &epever_;
    PumpScheduler &scheduler_;
    SdLogger &logger_;

    void startAP();
    void stopAP();

    void initFileServer();
    void setupServerRoutes();

    void apiStatus(AsyncWebServerRequest *request);
    void apiListLogs(AsyncWebServerRequest *request);
    void apiGetLog(AsyncWebServerRequest *request);
    void apiListSchedules(AsyncWebServerRequest *request);
    void apiGetSchedule(AsyncWebServerRequest *request);

    void handleCreateSchedule(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
    void handleUpdateSchedule(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
    void handleDeleteSchedule(AsyncWebServerRequest *request);

    void handleSetRtc(AsyncWebServerRequest *request);
    void handleSyncRtcToEpever(AsyncWebServerRequest *request);
    void handleRtcConfig(AsyncWebServerRequest *request);
    void handlePumpConfig(AsyncWebServerRequest *request);
    void handleLoggingConfig(AsyncWebServerRequest *request);
};
}
