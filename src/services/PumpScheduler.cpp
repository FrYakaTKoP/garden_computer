#include "services/PumpScheduler.h"

#include "core/TimeUtils.h"
#include "services/RtcService.h"

namespace gc
{
PumpScheduler::PumpScheduler(Preferences &prefs, int pump1Pin, int pump2Pin, uint8_t maxSchedules, uint8_t maxActiveRuns)
    : prefs_(prefs),
      pump1Pin_(pump1Pin),
      pump2Pin_(pump2Pin),
      maxSchedules_(maxSchedules),
      maxActiveRuns_(maxActiveRuns),
      scheduleCount_(0),
      nextId_(1)
{
}

void PumpScheduler::begin()
{
    setPumpPins();
    loadSchedules();
}

void PumpScheduler::loop(RtcService &rtc)
{
    pumpTick();
    checkSchedules(rtc);
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
    pinMode(pump1Pin_, OUTPUT);
    pinMode(pump2Pin_, OUTPUT);
    digitalWrite(pump1Pin_, LOW);
    digitalWrite(pump2Pin_, LOW);
}

uint8_t PumpScheduler::activePumpMask() const
{
    uint8_t combined = 0;
    for (uint8_t i = 0; i < maxActiveRuns_; i++)
        if (activeRuns_[i].active)
            combined |= activeRuns_[i].pumpMask;
    return combined;
}

void PumpScheduler::updateOutputs()
{
    const uint8_t combined = activePumpMask();
    digitalWrite(pump1Pin_, (combined & 1) ? HIGH : LOW);
    digitalWrite(pump2Pin_, (combined & 2) ? HIGH : LOW);
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
            uint32_t durationMs = static_cast<uint32_t>(s.duration5min) * 5UL * 60UL * 1000UL;
            activatePumpMask(s.pumpMask, durationMs, s.id);
            Serial.printf("Trigger schedule %u pumpMask %u for %ums\n", s.id, s.pumpMask, static_cast<unsigned>(durationMs));
        }
    }
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
