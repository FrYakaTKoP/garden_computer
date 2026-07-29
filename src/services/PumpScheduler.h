#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "core/Models.h"

namespace gc
{
class RtcService;

class PumpScheduler
{
public:
    PumpScheduler(Preferences &prefs, int pump1Pin, int pump2Pin, uint8_t maxSchedules, uint8_t maxActiveRuns);

    void begin();
    void loop(RtcService &rtc);

    uint8_t scheduleCount() const;
    bool getSchedule(uint32_t id, Schedule &out) const;
    void writeSchedulesJson(JsonArray arr) const;

    bool createSchedule(const JsonVariantConst &json);
    bool updateSchedule(uint32_t id, const JsonVariantConst &json);
    bool deleteSchedule(uint32_t id);

    uint8_t activePumpMask() const;

private:
    Preferences &prefs_;
    int pump1Pin_;
    int pump2Pin_;
    uint8_t maxSchedules_;
    uint8_t maxActiveRuns_;

    Schedule schedules_[32];
    ActiveRun activeRuns_[4];
    uint8_t scheduleCount_;
    uint32_t nextId_;

    static uint8_t snapTo5(uint8_t minute);
    static uint8_t minutesToSteps(int minutes);

    void saveSchedules();
    void loadSchedules();

    int findScheduleIndex(uint32_t id) const;
    void setPumpPins();
    void updateOutputs();
    void activatePumpMask(uint8_t mask, uint32_t durationMs, uint32_t schedId);
    void pumpTick();
    bool scheduleMatchesToday(const Schedule &s, uint8_t weekdayIndex, uint32_t dayIndex) const;
    void checkSchedules(RtcService &rtc);
};
}
