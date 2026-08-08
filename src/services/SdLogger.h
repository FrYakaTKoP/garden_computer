#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "core/Models.h"

class AsyncWebServerRequest;

namespace gc
{
struct LogSnapshot
{
    bool hasRtc = false;
    Ds1307Time rtc;
    uint32_t uptimeSeconds = 0;
    EpeverTracerData epever;
    PumpRuntimeStatus pumps;
    bool apActive = false;
    bool includeEnergy = false;
    float pvDailyWh = 0.0f;
    float pvMonthlyWh = 0.0f;
    float pvTotalWh = 0.0f;
};

struct SdLoggerStatus
{
    bool mounted = false;
    bool writable = false;
    uint32_t intervalMs = 0;
    uint32_t queuedRecords = 0;
    uint32_t droppedRecords = 0;
    uint32_t writeFailures = 0;
    uint32_t lastWriteUptimeSeconds = 0;
    uint64_t totalBytes = 0;
    uint64_t usedBytes = 0;
    char error[48] = "Not initialized";
};

class SdLogger
{
public:
    SdLogger(Preferences &prefs, int sckPin, int mosiPin, int misoPin, int csPin, uint32_t defaultIntervalMs, bool debugSerial);

    void begin();
    bool enqueue(const LogSnapshot &snapshot);
    bool isDue(unsigned long nowMs) const;
    void markLogged(unsigned long nowMs);
    bool shouldIncludeDailyEnergy(const Ds1307Time &now) const;
    void markDailyEnergyLogged(const Ds1307Time &now);
    void setIntervalMs(uint32_t intervalMs);
    SdLoggerStatus status() const;
    String listLogFilesJson();
    void sendLogFile(AsyncWebServerRequest *request, const String &filename);

private:
    static constexpr uint32_t kMinIntervalMs = 10000UL;
    static constexpr uint32_t kMaxIntervalMs = 3600000UL;

    Preferences &prefs_;
    int sckPin_;
    int mosiPin_;
    int misoPin_;
    int csPin_;
    uint32_t intervalMs_;
    bool debugSerial_;
    unsigned long lastEnqueueMs_ = 0;
    uint32_t lastDailyEnergyDate_ = 0;
    void *queue_ = nullptr;
    mutable portMUX_TYPE statusMux_ = portMUX_INITIALIZER_UNLOCKED;
    SdLoggerStatus status_;
    unsigned long lastMountAttemptMs_ = 0;
    bool hasMountAttempted_ = false;

    static void taskEntry(void *parameter);
    void taskLoop();
    bool mountCard();
    bool writeSnapshot(const LogSnapshot &snapshot);
    String filenameFor(const LogSnapshot &snapshot) const;
    void setError(const char *error);
    void updateCapacity();
    bool isValidLogFilename(const String &filename) const;
    bool isLogFilename(const String &filename) const;
    bool resolveLogPath(const String &filename, String &resolvedPath, uint64_t &fileSize) const;
    static uint32_t crc32(const uint8_t *data, size_t length);
    static int32_t scaled(float value, float scale);
};
}