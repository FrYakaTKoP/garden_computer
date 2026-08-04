#include "services/PumpScheduler.h"

#include <math.h>

#include "config/AppConfig.h"
#include "core/TimeUtils.h"
#include "services/RtcService.h"

namespace
{
struct SchedulerClock
{
    uint32_t dayIndex = 0;
    uint8_t weekdayIndex = 0;
    uint32_t secondsOfDay = 0;
};

bool readSchedulerClock(gc::RtcService &rtc, SchedulerClock &clock)
{
    gc::Ds1307Time now;
    if (rtc.readDateTime(now))
    {
        clock.dayIndex = gc::time::daysSince2000(now.year, now.month, now.day);
        clock.weekdayIndex = gc::time::dayOfWeekFromDate(now.year, now.month, now.day);
        clock.secondsOfDay = static_cast<uint32_t>(now.hour) * 3600UL + static_cast<uint32_t>(now.minute) * 60UL + now.second;
        return true;
    }

    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t weekday = 0;
    uint32_t dayIndex = 0;
    String display;
    if (rtc.present() && rtc.readClock(hour, minute, weekday, dayIndex, display))
    {
        clock.dayIndex = dayIndex;
        clock.weekdayIndex = weekday;
        clock.secondsOfDay = static_cast<uint32_t>(hour) * 3600UL + static_cast<uint32_t>(minute) * 60UL;
        return true;
    }

    unsigned long uptimeSeconds = millis() / 1000UL;
    clock.dayIndex = uptimeSeconds / 86400UL;
    clock.weekdayIndex = static_cast<uint8_t>((uptimeSeconds / 86400UL) % 7UL);
    clock.secondsOfDay = uptimeSeconds % 86400UL;
    return false;
}

String formatCountdownLabel(uint32_t seconds)
{
    if (seconds >= 3600UL)
    {
        uint32_t hours = seconds / 3600UL;
        return String(hours) + "h";
    }
    if (seconds >= 60UL)
    {
        uint32_t minutes = seconds / 60UL;
        return String(minutes) + "m";
    }
    if (seconds == 0)
        return "now";
    return String(seconds) + "s";
}
}

namespace gc
{
PumpScheduler::PumpScheduler(Preferences &prefs,
                     int leftTankFillPumpPin,
                     int rightTankFillPumpPin,
                     int leftTankWateringPumpPin,
                     int rightTankWateringPumpPin,
                     int topTankFullPin,
                     int leftTankEmptyPin,
                     int leftTankFullPin,
                     int rightTankEmptyPin,
                     int rightTankFullPin,
                     uint8_t maxSchedules,
                     uint8_t maxActiveRuns)
    : prefs_(prefs),
    leftTankFillPumpPin_(leftTankFillPumpPin),
    rightTankFillPumpPin_(rightTankFillPumpPin),
    leftTankWateringPumpPin_(leftTankWateringPumpPin),
    rightTankWateringPumpPin_(rightTankWateringPumpPin),
      topTankFullPin_(topTankFullPin),
      leftTankEmptyPin_(leftTankEmptyPin),
      leftTankFullPin_(leftTankFullPin),
      rightTankEmptyPin_(rightTankEmptyPin),
      rightTankFullPin_(rightTankFullPin),
      maxSchedules_(maxSchedules),
      maxActiveRuns_(maxActiveRuns),
      scheduleCount_(0),
      nextId_(1),
    fillPump1Enabled_(true),
    fillPump2Enabled_(true),
    wateringPump1Enabled_(false),
    wateringPump2Enabled_(false),
    pvVoltageThresholdV_(config::kAutonomousPumpPvVoltageThresholdV),
    autonomousCycleMs_(config::kAutonomousPumpCycleMs),
      topTankFull_(false),
      leftTankEmpty_(false),
      leftTankFull_(false),
      rightTankEmpty_(false),
      rightTankFull_(false),
      autonomousPumpMask_(0),
      scheduledPumpMask_(0),
      lastAutonomousSwitchMs_(0),
      autonomousPumpActive_(false),
      autonomousPumpIndex_(0),
      autonomousPumpEnabledForCurrentCycle_(false)
{
}

void PumpScheduler::begin()
{
    setPumpPins();
    loadSchedules();
    loadSettings();
}

void PumpScheduler::loop(RtcService &rtc, const EpeverTracerData &epeverData)
{
    pumpTick();
    checkSchedules(rtc);
    updateTankInputs();
    updateAutonomousLogic(epeverData);
}

uint8_t PumpScheduler::scheduleCount() const { return scheduleCount_; }

uint8_t PumpScheduler::snapTo5(uint8_t minute)
{
    return static_cast<uint8_t>((minute / 5) * 5);
}

uint8_t PumpScheduler::minutesToSteps(int minutes)
{
    if (minutes <= 0)
        return 1;
    int steps = (minutes + 2) / 5;
    if (steps < 1)
        steps = 1;
    if (steps > 12)
        steps = 12;
    return static_cast<uint8_t>(steps);
}

void PumpScheduler::saveSchedules()
{
    DynamicJsonDocument doc(8192);
    doc["nextId"] = nextId_;
    doc["count"] = scheduleCount_;
    JsonArray arr = doc.createNestedArray("schedules");
    for (uint8_t i = 0; i < scheduleCount_; i++)
    {
        JsonObject o = arr.createNestedObject();
        o["id"] = schedules_[i].id;
        o["hour"] = schedules_[i].hour;
        o["minute"] = schedules_[i].minute;
        o["duration5min"] = schedules_[i].duration5min;
        o["weekdays"] = schedules_[i].weekdays;
        o["repeatEvery"] = schedules_[i].repeatEvery;
        o["pumpMask"] = schedules_[i].pumpMask;
    }

    String out;
    serializeJson(doc, out);
    prefs_.putString("schedules", out);
}

void PumpScheduler::loadSchedules()
{
    String in = prefs_.getString("schedules", "");
    if (in.length() == 0)
    {
        scheduleCount_ = 0;
        nextId_ = 1;
        return;
    }

    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, in);
    if (err)
    {
        scheduleCount_ = 0;
        nextId_ = 1;
        return;
    }

    nextId_ = doc["nextId"] | 1;
    JsonArray arr = doc["schedules"].as<JsonArray>();
    scheduleCount_ = 0;
    for (JsonObject o : arr)
    {
        if (scheduleCount_ >= maxSchedules_)
            break;
        schedules_[scheduleCount_].id = o["id"].as<uint32_t>();
        schedules_[scheduleCount_].hour = o["hour"].as<uint8_t>();
        schedules_[scheduleCount_].minute = o["minute"].as<uint8_t>();
        schedules_[scheduleCount_].duration5min = o["duration5min"].as<uint8_t>();
        schedules_[scheduleCount_].weekdays = o["weekdays"].as<uint8_t>();
        schedules_[scheduleCount_].repeatEvery = o["repeatEvery"] | 1;
        schedules_[scheduleCount_].pumpMask = o["pumpMask"].as<uint8_t>();
        scheduleCount_++;
    }
}

int PumpScheduler::findScheduleIndex(uint32_t id) const
{
    for (uint8_t i = 0; i < scheduleCount_; i++)
        if (schedules_[i].id == id)
            return i;
    return -1;
}

void PumpScheduler::setPumpPins()
{
    if (leftTankFillPumpPin_ >= 0)
    {
        pinMode(leftTankFillPumpPin_, OUTPUT);
        digitalWrite(leftTankFillPumpPin_, LOW);
    }
    if (rightTankFillPumpPin_ >= 0)
    {
        pinMode(rightTankFillPumpPin_, OUTPUT);
        digitalWrite(rightTankFillPumpPin_, LOW);
    }
    if (leftTankWateringPumpPin_ >= 0)
    {
        pinMode(leftTankWateringPumpPin_, OUTPUT);
        digitalWrite(leftTankWateringPumpPin_, LOW);
    }
    if (rightTankWateringPumpPin_ >= 0)
    {
        pinMode(rightTankWateringPumpPin_, OUTPUT);
        digitalWrite(rightTankWateringPumpPin_, LOW);
    }

    if (topTankFullPin_ >= 0) pinMode(topTankFullPin_, INPUT_PULLUP);
    if (leftTankEmptyPin_ >= 0) pinMode(leftTankEmptyPin_, INPUT_PULLUP);
    if (leftTankFullPin_ >= 0) pinMode(leftTankFullPin_, INPUT_PULLUP);
    if (rightTankEmptyPin_ >= 0) pinMode(rightTankEmptyPin_, INPUT_PULLUP);
    if (rightTankFullPin_ >= 0) pinMode(rightTankFullPin_, INPUT_PULLUP);
}

uint8_t PumpScheduler::activePumpMask() const
{
    return fillActivePumpMask() | wateringActivePumpMask();
}

uint8_t PumpScheduler::fillActivePumpMask() const
{
    uint8_t combined = 0;
    for (uint8_t i = 0; i < maxActiveRuns_; i++)
        if (activeRuns_[i].active)
            combined |= activeRuns_[i].pumpMask;
    return combined;
}

uint8_t PumpScheduler::wateringActivePumpMask() const
{
    return autonomousPumpMask_;
}

String PumpScheduler::nextScheduleLabel(uint8_t pumpMask, RtcService &rtc) const
{
    SchedulerClock clock;
    readSchedulerClock(rtc, clock);

    uint32_t bestDeltaSeconds = 0xFFFFFFFFUL;
    bool found = false;

    for (uint8_t i = 0; i < scheduleCount_; i++)
    {
        const Schedule &s = schedules_[i];
        if ((s.pumpMask & pumpMask) == 0)
            continue;

        const uint32_t scheduleSeconds = static_cast<uint32_t>(s.hour) * 3600UL + static_cast<uint32_t>(s.minute) * 60UL;
        for (uint16_t offsetDays = 0; offsetDays <= 366; offsetDays++)
        {
            const uint32_t candidateDayIndex = clock.dayIndex + offsetDays;
            const uint8_t candidateWeekday = static_cast<uint8_t>((clock.weekdayIndex + offsetDays) % 7U);
            if (!scheduleMatchesToday(s, candidateWeekday, candidateDayIndex))
                continue;

            if (offsetDays == 0 && scheduleSeconds <= clock.secondsOfDay)
                continue;

            const uint32_t deltaSeconds = (offsetDays * 86400UL) + scheduleSeconds - clock.secondsOfDay;
            if (!found || deltaSeconds < bestDeltaSeconds)
            {
                bestDeltaSeconds = deltaSeconds;
                found = true;
            }
            break;
        }
    }

    if (!found)
        return String("--");

    return formatCountdownLabel(bestDeltaSeconds);
}

PumpRuntimeStatus PumpScheduler::runtimeStatus(RtcService &rtc) const
{
    PumpRuntimeStatus status;
    status.fillPump1Enabled = fillPump1Enabled_;
    status.fillPump2Enabled = fillPump2Enabled_;
    status.wateringPump1Enabled = wateringPump1Enabled_;
    status.wateringPump2Enabled = wateringPump2Enabled_;
    const uint8_t fillMask = fillActivePumpMask();
    const uint8_t wateringMask = wateringActivePumpMask();
    status.fillPump1Active = (fillMask & 1) != 0;
    status.fillPump2Active = (fillMask & 2) != 0;
    status.wateringPump1Active = (wateringMask & 1) != 0;
    status.wateringPump2Active = (wateringMask & 2) != 0;
    status.pvVoltageThresholdV = pvVoltageThresholdV_;
    status.autonomousCycleMs = autonomousCycleMs_;
    status.topTankFull = topTankFull_;
    status.leftTankEmpty = leftTankEmpty_;
    status.leftTankFull = leftTankFull_;
    status.rightTankEmpty = rightTankEmpty_;
    status.rightTankFull = rightTankFull_;
    status.autonomousPumpMask = autonomousPumpMask_;
    status.scheduledPumpMask = fillMask;
    status.nextFillPump1 = nextScheduleLabel(1, rtc);
    status.nextFillPump2 = nextScheduleLabel(2, rtc);
    return status;
}

bool PumpScheduler::setFillPump1Enabled(bool enabled)
{
    if (fillPump1Enabled_ == enabled)
        return true;
    fillPump1Enabled_ = enabled;
    saveSettings();
    return true;
}

bool PumpScheduler::setFillPump2Enabled(bool enabled)
{
    if (fillPump2Enabled_ == enabled)
        return true;
    fillPump2Enabled_ = enabled;
    saveSettings();
    return true;
}

bool PumpScheduler::setWateringPump1Enabled(bool enabled)
{
    if (wateringPump1Enabled_ == enabled)
        return true;
    wateringPump1Enabled_ = enabled;
    saveSettings();
    return true;
}

bool PumpScheduler::setWateringPump2Enabled(bool enabled)
{
    if (wateringPump2Enabled_ == enabled)
        return true;
    wateringPump2Enabled_ = enabled;
    saveSettings();
    return true;
}

bool PumpScheduler::setPvVoltageThresholdV(float value)
{
    if (value < 0.0f || value > 100.0f)
        return false;
    pvVoltageThresholdV_ = value;
    saveSettings();
    return true;
}

bool PumpScheduler::setAutonomousCycleMs(uint32_t value)
{
    if (value < 10000UL)
        return false;
    autonomousCycleMs_ = value;
    saveSettings();
    return true;
}

void PumpScheduler::updateOutputs()
{
    const uint8_t fillMask = fillActivePumpMask();
    const uint8_t wateringMask = wateringActivePumpMask();

    if (leftTankFillPumpPin_ >= 0)
        digitalWrite(leftTankFillPumpPin_, (fillMask & 1) ? HIGH : LOW);
    if (rightTankFillPumpPin_ >= 0)
        digitalWrite(rightTankFillPumpPin_, (fillMask & 2) ? HIGH : LOW);
    if (leftTankWateringPumpPin_ >= 0)
        digitalWrite(leftTankWateringPumpPin_, (wateringMask & 1) ? HIGH : LOW);
    if (rightTankWateringPumpPin_ >= 0)
        digitalWrite(rightTankWateringPumpPin_, (wateringMask & 2) ? HIGH : LOW);
}

void PumpScheduler::setAutonomousPumpOutput(bool on, uint8_t pumpMask)
{
    if (on)
    {
        autonomousPumpMask_ = pumpMask;
        autonomousPumpActive_ = true;
    }
    else
    {
        autonomousPumpMask_ = 0;
        autonomousPumpActive_ = false;
    }
    updateOutputs();
}

bool PumpScheduler::readSwitchState(int pin) const
{
    if (pin < 0)
        return false;
    return digitalRead(pin) == LOW;
}

void PumpScheduler::updateTankInputs()
{
    topTankFull_ = readSwitchState(topTankFullPin_);
    leftTankEmpty_ = readSwitchState(leftTankEmptyPin_);
    leftTankFull_ = readSwitchState(leftTankFullPin_);
    rightTankEmpty_ = readSwitchState(rightTankEmptyPin_);
    rightTankFull_ = readSwitchState(rightTankFullPin_);
}

void PumpScheduler::updateAutonomousLogic(const EpeverTracerData &epeverData)
{
    const bool topFull = topTankFull_;
    const bool leftEmpty = leftTankEmpty_;
    const bool leftFull = leftTankFull_;
    const bool rightEmpty = rightTankEmpty_;
    const bool rightFull = rightTankFull_;
    const uint8_t enabledMask = (wateringPump1Enabled_ ? 1 : 0) | (wateringPump2Enabled_ ? 2 : 0);

    if (enabledMask == 0)
    {
        setAutonomousPumpOutput(false, 0);
        return;
    }

    if (topFull || (!leftEmpty && !leftFull && !rightEmpty && !rightFull))
    {
        setAutonomousPumpOutput(false, 0);
        return;
    }

    const uint8_t fillMask = fillActivePumpMask();
    const uint8_t wateringMask = wateringActivePumpMask();
    scheduledPumpMask_ = fillMask;
    if ((fillMask & 0x03) || (wateringMask & 0x03))
        return;

    const bool batteryCurrentIdle = epeverData.valid && fabsf(epeverData.batteryCurrent) <= 0.05f;
    const bool pvVoltageReady = epeverData.valid && epeverData.pvVoltage >= pvVoltageThresholdV_;
    if (!(batteryCurrentIdle && pvVoltageReady))
    {
        setAutonomousPumpOutput(false, 0);
        return;
    }

    const bool usePump1 = (autonomousPumpIndex_ % 2) == 0;
    uint8_t desiredPumpMask = usePump1 ? 1 : 2;
    if ((enabledMask & desiredPumpMask) == 0)
        desiredPumpMask = (enabledMask & 1) ? 1 : 2;

    const bool shouldRun = !topFull && (desiredPumpMask == 1 ? leftEmpty && !leftFull : rightEmpty && !rightFull);
    if (!shouldRun)
    {
        setAutonomousPumpOutput(false, 0);
        return;
    }

    if (millis() - lastAutonomousSwitchMs_ >= autonomousCycleMs_)
    {
        lastAutonomousSwitchMs_ = millis();
        autonomousPumpIndex_ = (autonomousPumpIndex_ + 1) % 2;
    }

    setAutonomousPumpOutput(true, desiredPumpMask);
}

void PumpScheduler::activatePumpMask(uint8_t mask, uint32_t durationMs, uint32_t schedId)
{
    unsigned long endt = millis() + durationMs;
    for (uint8_t i = 0; i < maxActiveRuns_; i++)
    {
        if (!activeRuns_[i].active)
        {
            activeRuns_[i].active = true;
            activeRuns_[i].scheduleId = schedId;
            activeRuns_[i].pumpMask = mask;
            activeRuns_[i].endMillis = endt;
            break;
        }
    }
    updateOutputs();
}

void PumpScheduler::pumpTick()
{
    unsigned long now = millis();
    bool changed = false;
    for (uint8_t i = 0; i < maxActiveRuns_; i++)
    {
        if (activeRuns_[i].active && now >= activeRuns_[i].endMillis)
        {
            activeRuns_[i].active = false;
            changed = true;
        }
    }

    if (changed)
        updateOutputs();
}

bool PumpScheduler::scheduleMatchesToday(const Schedule &s, uint8_t weekdayIndex, uint32_t dayIndex) const
{
    int bitIndex = (weekdayIndex == 0) ? 6 : (weekdayIndex - 1);
    bool weekdayMatch = (s.weekdays & (1 << bitIndex)) != 0;
    if (s.repeatEvery <= 1)
        return weekdayMatch;
    if (!weekdayMatch)
        return false;
    return (dayIndex % s.repeatEvery) == 0;
}

void PumpScheduler::checkSchedules(RtcService &rtc)
{
    static uint32_t lastMinute = 0;
    static uint32_t lastDayIndex = 0;

    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t weekday = 0;
    uint32_t dayIndex = 0;
    String display;

    if (rtc.present() && rtc.readClock(hour, minute, weekday, dayIndex, display))
    {
        uint32_t currentMinute = static_cast<uint32_t>(hour) * 60UL + minute;
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

    for (uint8_t i = 0; i < scheduleCount_; i++)
    {
        Schedule &s = schedules_[i];
        if (s.hour == hour && s.minute == minute && scheduleMatchesToday(s, weekday, dayIndex))
        {
            const uint8_t fillEnabledMask = (fillPump1Enabled_ ? 1 : 0) | (fillPump2Enabled_ ? 2 : 0);
            const uint8_t effectiveMask = s.pumpMask & fillEnabledMask;
            if (effectiveMask == 0)
                continue;
            uint32_t durationMs = static_cast<uint32_t>(s.duration5min) * 5UL * 60UL * 1000UL;
            activatePumpMask(effectiveMask, durationMs, s.id);
            Serial.printf("Trigger schedule %u pumpMask %u for %ums\n", s.id, effectiveMask, static_cast<unsigned>(durationMs));
        }
    }
}

void PumpScheduler::loadSettings()
{
    const bool legacyEnabled = prefs_.getBool("newPumpsEnabled", false);
    const bool hasFillPump1 = prefs_.isKey("fillPump1Enabled");
    const bool hasFillPump2 = prefs_.isKey("fillPump2Enabled");
    const bool hasPump1 = prefs_.isKey("wateringPump1Enabled");
    const bool hasPump2 = prefs_.isKey("wateringPump2Enabled");
    fillPump1Enabled_ = hasFillPump1 ? prefs_.getBool("fillPump1Enabled", true) : true;
    fillPump2Enabled_ = hasFillPump2 ? prefs_.getBool("fillPump2Enabled", true) : true;
    wateringPump1Enabled_ = hasPump1 ? prefs_.getBool("wateringPump1Enabled", false) : legacyEnabled;
    wateringPump2Enabled_ = hasPump2 ? prefs_.getBool("wateringPump2Enabled", false) : legacyEnabled;
    pvVoltageThresholdV_ = prefs_.getFloat("pvVoltageThresholdV", config::kAutonomousPumpPvVoltageThresholdV);
    autonomousCycleMs_ = prefs_.getULong("autonomousCycleMs", config::kAutonomousPumpCycleMs);
}

void PumpScheduler::saveSettings()
{
    prefs_.putBool("fillPump1Enabled", fillPump1Enabled_);
    prefs_.putBool("fillPump2Enabled", fillPump2Enabled_);
    prefs_.putBool("wateringPump1Enabled", wateringPump1Enabled_);
    prefs_.putBool("wateringPump2Enabled", wateringPump2Enabled_);
    prefs_.putFloat("pvVoltageThresholdV", pvVoltageThresholdV_);
    prefs_.putULong("autonomousCycleMs", autonomousCycleMs_);
}

void PumpScheduler::writeSchedulesJson(JsonArray arr) const
{
    for (uint8_t i = 0; i < scheduleCount_; i++)
    {
        JsonObject o = arr.createNestedObject();
        o["id"] = schedules_[i].id;
        o["hour"] = schedules_[i].hour;
        o["minute"] = schedules_[i].minute;
        o["duration5min"] = schedules_[i].duration5min;
        o["weekdays"] = schedules_[i].weekdays;
        o["pumpMask"] = schedules_[i].pumpMask;
    }
}

bool PumpScheduler::getSchedule(uint32_t id, Schedule &out) const
{
    int idx = findScheduleIndex(id);
    if (idx < 0)
        return false;
    out = schedules_[idx];
    return true;
}

bool PumpScheduler::createSchedule(const JsonVariantConst &json)
{
    if (scheduleCount_ >= maxSchedules_)
        return false;

    Schedule s;
    s.id = nextId_++;
    s.hour = json["hour"] | 0;
    s.minute = snapTo5(json["minute"] | 0);
    if (json.containsKey("duration5min"))
        s.duration5min = static_cast<uint8_t>(max(1, static_cast<int>(json["duration5min"] | 1)));
    else if (json.containsKey("durationMinutes"))
        s.duration5min = minutesToSteps(static_cast<int>(json["durationMinutes"] | 5));
    else
        s.duration5min = 1;

    s.weekdays = json["weekdays"] | 0;
    s.repeatEvery = json["repeatEvery"] | 1;
    s.pumpMask = json["pumpMask"] | 1;

    schedules_[scheduleCount_++] = s;
    saveSchedules();
    return true;
}

bool PumpScheduler::updateSchedule(uint32_t id, const JsonVariantConst &json)
{
    int idx = findScheduleIndex(id);
    if (idx < 0)
        return false;

    schedules_[idx].hour = json["hour"] | schedules_[idx].hour;
    schedules_[idx].minute = snapTo5(json["minute"] | schedules_[idx].minute);

    if (json.containsKey("duration5min"))
        schedules_[idx].duration5min = static_cast<uint8_t>(max(1, static_cast<int>(json["duration5min"] | schedules_[idx].duration5min)));
    else if (json.containsKey("durationMinutes"))
        schedules_[idx].duration5min = minutesToSteps(static_cast<int>(json["durationMinutes"] | schedules_[idx].duration5min));

    schedules_[idx].weekdays = json["weekdays"] | schedules_[idx].weekdays;
    schedules_[idx].pumpMask = json["pumpMask"] | schedules_[idx].pumpMask;
    schedules_[idx].repeatEvery = json["repeatEvery"] | schedules_[idx].repeatEvery;
    saveSchedules();
    return true;
}

bool PumpScheduler::deleteSchedule(uint32_t id)
{
    int idx = findScheduleIndex(id);
    if (idx < 0)
        return false;

    for (uint8_t i = static_cast<uint8_t>(idx); i + 1 < scheduleCount_; i++)
        schedules_[i] = schedules_[i + 1];
    scheduleCount_--;
    saveSchedules();
    return true;
}
}
