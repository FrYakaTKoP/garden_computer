#include "services/SdLogger.h"

#include <SD.h>
#include <SPI.h>
#include <ESPAsyncWebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace gc
{
SdLogger::SdLogger(Preferences &prefs, int sckPin, int mosiPin, int misoPin, int csPin, uint32_t defaultIntervalMs, bool debugSerial)
    : prefs_(prefs),
      sckPin_(sckPin),
      mosiPin_(mosiPin),
      misoPin_(misoPin),
      csPin_(csPin),
    intervalMs_(defaultIntervalMs),
    debugSerial_(debugSerial)
{
}

void SdLogger::begin()
{
    intervalMs_ = prefs_.getULong("logInterval", intervalMs_);
    intervalMs_ = constrain(intervalMs_, kMinIntervalMs, kMaxIntervalMs);
    lastEnqueueMs_ = millis() - intervalMs_;
    status_.intervalMs = intervalMs_;
    if (debugSerial_)
        Serial.printf("SD logger: init SCK=%d MOSI=%d MISO=%d CS=%d interval=%lums\n", sckPin_, mosiPin_, misoPin_, csPin_, static_cast<unsigned long>(intervalMs_));
    queue_ = xQueueCreate(12, sizeof(LogSnapshot));
    if (queue_ == nullptr)
    {
        setError("Queue allocation failed");
        if (debugSerial_)
            Serial.println("SD logger: queue allocation failed");
        return;
    }

    mountCard();
    xTaskCreatePinnedToCore(taskEntry, "sd-logger", 6144, this, 0, nullptr, 1);
}

bool SdLogger::enqueue(const LogSnapshot &snapshot)
{
    if (queue_ == nullptr)
        return false;

    if (xQueueSend(static_cast<QueueHandle_t>(queue_), &snapshot, 0) != pdTRUE)
    {
        portENTER_CRITICAL(&statusMux_);
        status_.droppedRecords++;
        portEXIT_CRITICAL(&statusMux_);
        if (debugSerial_)
            Serial.println("SD logger: queue full; record dropped");
        return false;
    }
    return true;
}

bool SdLogger::isDue(unsigned long nowMs) const
{
    return nowMs - lastEnqueueMs_ >= intervalMs_;
}

void SdLogger::markLogged(unsigned long nowMs)
{
    lastEnqueueMs_ = nowMs;
}

bool SdLogger::shouldIncludeDailyEnergy(const Ds1307Time &now) const
{
    const uint32_t dateKey = static_cast<uint32_t>(now.year) * 10000UL + static_cast<uint32_t>(now.month) * 100UL + now.day;
    return now.hour >= 23 && dateKey != lastDailyEnergyDate_;
}

void SdLogger::markDailyEnergyLogged(const Ds1307Time &now)
{
    lastDailyEnergyDate_ = static_cast<uint32_t>(now.year) * 10000UL + static_cast<uint32_t>(now.month) * 100UL + now.day;
}

void SdLogger::setIntervalMs(uint32_t intervalMs)
{
    intervalMs_ = constrain(intervalMs, kMinIntervalMs, kMaxIntervalMs);
    prefs_.putULong("logInterval", intervalMs_);
    portENTER_CRITICAL(&statusMux_);
    status_.intervalMs = intervalMs_;
    portEXIT_CRITICAL(&statusMux_);
}

SdLoggerStatus SdLogger::status() const
{
    SdLoggerStatus copy;
    portENTER_CRITICAL(&statusMux_);
    copy = status_;
    if (queue_ != nullptr)
        copy.queuedRecords = uxQueueMessagesWaiting(static_cast<QueueHandle_t>(queue_));
    portEXIT_CRITICAL(&statusMux_);
    return copy;
}

String SdLogger::listLogFilesJson()
{
    if (!status().mounted)
    {
        if (debugSerial_)
            Serial.println("SD logger: log listing requested while card is not mounted");
        return "{\"files\":[]}";
    }

    File directory = SD.open("/logs");
    if (!directory || !directory.isDirectory())
    {
        if (debugSerial_)
            Serial.println("SD logger: could not open /logs for listing");
        return "{\"files\":[]}";
    }

    String out = "{\"files\":[";
    bool first = true;
    File entry = directory.openNextFile();
    while (entry)
    {
        String name = entry.name();
        const int slash = name.lastIndexOf('/');
        if (slash >= 0)
            name = name.substring(slash + 1);
        if (debugSerial_)
            Serial.printf("SD logger: found %s (%llu bytes)%s\n", name.c_str(), static_cast<unsigned long long>(entry.size()), entry.isDirectory() ? " directory" : "");
        if (!entry.isDirectory() && isLogFilename(name))
        {
            if (!first)
                out += ',';
            out += "{\"name\":\"" + name + "\",\"size\":" + String(entry.size()) + "}";
            first = false;
        }
        entry.close();
        entry = directory.openNextFile();
    }
    directory.close();
    if (debugSerial_)
        Serial.printf("SD logger: returning %s\n", out.c_str());
    return out + "]}";
}

void SdLogger::sendLogFile(AsyncWebServerRequest *request, const String &filename)
{
    if (!status().mounted || !isLogFilename(filename))
    {
        if (debugSerial_)
            Serial.printf("SD logger: rejected log download name=%s mounted=%s\n", filename.c_str(), status().mounted ? "yes" : "no");
        request->send(404, "application/json", "{\"error\":\"Log not found\"}");
        return;
    }

    String path;
    uint64_t fileSize = 0;
    if (!resolveLogPath(filename, path, fileSize))
    {
        if (debugSerial_)
            Serial.printf("SD logger: log download missing name=%s\n", filename.c_str());
        request->send(404, "application/json", "{\"error\":\"Log not found\"}");
        return;
    }
    if (debugSerial_)
    {
        Serial.printf("SD logger: serving path=%s size=%llu bytes\n", path.c_str(), static_cast<unsigned long long>(fileSize));
    }
    request->send(SD, path, "application/x-ndjson", false);
}

void SdLogger::taskEntry(void *parameter)
{
    static_cast<SdLogger *>(parameter)->taskLoop();
}

void SdLogger::taskLoop()
{
    LogSnapshot snapshot;
    while (true)
    {
        if (xQueueReceive(static_cast<QueueHandle_t>(queue_), &snapshot, pdMS_TO_TICKS(250)) != pdTRUE)
            continue;

        if (!mountCard())
            continue;
        writeSnapshot(snapshot);
    }
}

bool SdLogger::mountCard()
{
    if (status().mounted)
        return true;
    if (hasMountAttempted_ && millis() - lastMountAttemptMs_ < 30000UL)
        return false;

    lastMountAttemptMs_ = millis();
    hasMountAttempted_ = true;
    if (debugSerial_)
        Serial.printf("SD logger: mounting at %lu ms with default SPI (SCK=%d MOSI=%d MISO=%d CS=%d)\n", static_cast<unsigned long>(lastMountAttemptMs_), sckPin_, mosiPin_, misoPin_, csPin_);
    SPI.begin(sckPin_, misoPin_, mosiPin_, csPin_);
    if (!SD.begin(csPin_))
    {
        setError("Card unavailable");
        if (debugSerial_)
            Serial.println("SD logger: SD.begin failed; check card power, FAT32 format, CS, and SPI wiring");
        return false;
    }
    if (!SD.exists("/logs") && !SD.mkdir("/logs"))
    {
        SD.end();
        setError("Cannot create logs dir");
        if (debugSerial_)
            Serial.println("SD logger: mounted card but could not create /logs");
        return false;
    }

    portENTER_CRITICAL(&statusMux_);
    status_.mounted = true;
    status_.writable = true;
    strncpy(status_.error, "", sizeof(status_.error));
    portEXIT_CRITICAL(&statusMux_);
    updateCapacity();
    if (debugSerial_)
    {
        const SdLoggerStatus sd = status();
        Serial.printf("SD logger: mounted type=%u total=%llu used=%llu bytes\n", static_cast<unsigned>(SD.cardType()), static_cast<unsigned long long>(sd.totalBytes), static_cast<unsigned long long>(sd.usedBytes));
    }
    return true;
}

bool SdLogger::writeSnapshot(const LogSnapshot &snapshot)
{
    const uint64_t timestamp = snapshot.hasRtc
                                   ? static_cast<uint64_t>(snapshot.rtc.year) * 10000000000ULL + static_cast<uint64_t>(snapshot.rtc.month) * 100000000ULL + static_cast<uint64_t>(snapshot.rtc.day) * 1000000ULL + static_cast<uint64_t>(snapshot.rtc.hour) * 10000ULL + static_cast<uint64_t>(snapshot.rtc.minute) * 100ULL + snapshot.rtc.second
                                   : snapshot.uptimeSeconds;
    const uint8_t tankMask = (snapshot.pumps.topTankFull ? 1 : 0) |
                             (snapshot.pumps.leftTankEmpty ? 2 : 0) |
                             (snapshot.pumps.leftTankFull ? 4 : 0) |
                             (snapshot.pumps.rightTankEmpty ? 8 : 0) |
                             (snapshot.pumps.rightTankFull ? 16 : 0);
    const uint8_t pumpMask = (snapshot.pumps.fillPump1Active ? 1 : 0) |
                             (snapshot.pumps.fillPump2Active ? 2 : 0) |
                             (snapshot.pumps.wateringPump1Active ? 4 : 0) |
                             (snapshot.pumps.wateringPump2Active ? 8 : 0);

    char body[448];
    int length = snprintf(body, sizeof(body),
                          "{\"v\":1,\"t\":%llu,\"tv\":%u,\"p\":[%ld,%ld,%ld],\"b\":[%ld,%ld,%ld,%u,%ld],\"l\":[%ld,%ld,%ld],\"k\":%u,\"u\":%u,\"w\":%u%s%s}",
                          static_cast<unsigned long long>(timestamp), snapshot.hasRtc ? 1 : 0,
                          static_cast<long>(scaled(snapshot.epever.pvVoltage, 100)), static_cast<long>(scaled(snapshot.epever.pvCurrent, 100)), static_cast<long>(scaled(snapshot.epever.pvPowerWatts, 100)),
                          static_cast<long>(scaled(snapshot.epever.batteryVoltage, 100)), static_cast<long>(scaled(snapshot.epever.batteryCurrent, 100)), static_cast<long>(scaled(snapshot.epever.batteryPowerWatts, 100)), snapshot.epever.batterySoc, static_cast<long>(scaled(snapshot.epever.batteryTemperatureC, 10)),
                          static_cast<long>(scaled(snapshot.epever.loadVoltage, 100)), static_cast<long>(scaled(snapshot.epever.loadCurrent, 100)), static_cast<long>(scaled(snapshot.epever.loadPowerWatts, 100)),
                          tankMask, pumpMask, snapshot.apActive ? 1 : 0,
                          snapshot.includeEnergy ? ",\"e\":[" : "",
                          snapshot.includeEnergy ? String(scaled(snapshot.pvDailyWh, 10)) + "," + String(scaled(snapshot.pvMonthlyWh, 10)) + "," + String(scaled(snapshot.pvTotalWh, 10)) + "]" : "");
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(body))
    {
        setError("Record formatting failed");
        if (debugSerial_)
            Serial.printf("SD logger: record body formatting failed (length=%d)\n", length);
        return false;
    }

    const uint32_t checksum = crc32(reinterpret_cast<const uint8_t *>(body), static_cast<size_t>(length));
    char record[480];
    const int recordLength = snprintf(record, sizeof(record), "%.*s,\"crc\":\"%08lX\"}\n", length - 1, body, static_cast<unsigned long>(checksum));
    if (recordLength <= 0 || static_cast<size_t>(recordLength) >= sizeof(record))
    {
        setError("Record too large");
        if (debugSerial_)
            Serial.printf("SD logger: complete record too large (length=%d)\n", recordLength);
        return false;
    }

    const String filename = filenameFor(snapshot);
    File file = SD.open(filename, FILE_APPEND);
    const size_t written = file ? file.write(reinterpret_cast<const uint8_t *>(record), static_cast<size_t>(recordLength)) : 0;
    const bool writeOk = written == static_cast<size_t>(recordLength);
    if (!writeOk)
    {
        if (file)
            file.close();
        SD.end();
        portENTER_CRITICAL(&statusMux_);
        status_.mounted = false;
        status_.writable = false;
        status_.writeFailures++;
        portEXIT_CRITICAL(&statusMux_);
        setError("Write failed");
        if (debugSerial_)
            Serial.printf("SD logger: write failed file=%s wanted=%d wrote=%u\n", filename.c_str(), recordLength, static_cast<unsigned>(written));
        return false;
    }
    file.flush();
    file.close();

    portENTER_CRITICAL(&statusMux_);
    status_.lastWriteUptimeSeconds = millis() / 1000UL;
    status_.writable = true;
    portEXIT_CRITICAL(&statusMux_);
    updateCapacity();
    if (debugSerial_)
        Serial.printf("SD logger: wrote %d bytes to %s\n", recordLength, filename.c_str());
    return true;
}

String SdLogger::filenameFor(const LogSnapshot &snapshot) const
{
    if (!snapshot.hasRtc)
        return "/logs/uptime.ndjson";
    char filename[32];
    snprintf(filename, sizeof(filename), "/logs/%04u%02u%02u.ndjson", snapshot.rtc.year, snapshot.rtc.month, snapshot.rtc.day);
    return String(filename);
}

void SdLogger::setError(const char *error)
{
    portENTER_CRITICAL(&statusMux_);
    strncpy(status_.error, error, sizeof(status_.error) - 1);
    status_.error[sizeof(status_.error) - 1] = '\0';
    portEXIT_CRITICAL(&statusMux_);
}

void SdLogger::updateCapacity()
{
    portENTER_CRITICAL(&statusMux_);
    status_.totalBytes = SD.totalBytes();
    status_.usedBytes = SD.usedBytes();
    portEXIT_CRITICAL(&statusMux_);
}

uint32_t SdLogger::crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t index = 0; index < length; ++index)
    {
        crc ^= data[index];
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320UL & (-(static_cast<int32_t>(crc & 1))));
    }
    return ~crc;
}

int32_t SdLogger::scaled(float value, float scale)
{
    return static_cast<int32_t>(roundf(value * scale));
}

bool SdLogger::isValidLogFilename(const String &filename) const
{
    String normalized = filename;
    normalized.toLowerCase();
    if (normalized.length() != 15 || !normalized.endsWith(".ndjson"))
        return false;
    for (uint8_t index = 0; index < 8; ++index)
    {
        if (!isDigit(normalized[index]))
            return false;
    }
    return true;
}

bool SdLogger::isLogFilename(const String &filename) const
{
    String normalized = filename;
    normalized.toLowerCase();
    return normalized == "uptime.ndjson" || isValidLogFilename(normalized);
}

bool SdLogger::resolveLogPath(const String &filename, String &resolvedPath, uint64_t &fileSize) const
{
    resolvedPath = "";
    fileSize = 0;

    File directory = SD.open("/logs");
    if (!directory || !directory.isDirectory())
        return false;

    String requested = filename;
    requested.toLowerCase();

    File entry = directory.openNextFile();
    while (entry)
    {
        String rawName = entry.name();
        String baseName = rawName;
        const int slash = baseName.lastIndexOf('/');
        if (slash >= 0)
            baseName = baseName.substring(slash + 1);

        String normalized = baseName;
        normalized.toLowerCase();
        if (!entry.isDirectory() && normalized == requested)
        {
            resolvedPath = rawName.startsWith("/") ? rawName : ("/logs/" + baseName);
            fileSize = entry.size();
            entry.close();
            directory.close();
            return true;
        }

        entry.close();
        entry = directory.openNextFile();
    }

    directory.close();
    return false;
}
}