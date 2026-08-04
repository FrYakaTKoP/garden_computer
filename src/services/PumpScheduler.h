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
    PumpScheduler(Preferences &prefs,
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
                  uint8_t maxActiveRuns);

    void begin();
    void loop(RtcService &rtc, const EpeverTracerData &epeverData);

    uint8_t scheduleCount() const;
    bool getSchedule(uint32_t id, Schedule &out) const;
    void writeSchedulesJson(JsonArray arr) const;

    bool createSchedule(const JsonVariantConst &json);
    bool updateSchedule(uint32_t id, const JsonVariantConst &json);
    bool deleteSchedule(uint32_t id);

    uint8_t activePumpMask() const;
    PumpRuntimeStatus runtimeStatus(RtcService &rtc) const;
    bool setFillPump1Enabled(bool enabled);
    bool setFillPump2Enabled(bool enabled);
    bool setWateringPump1Enabled(bool enabled);
    bool setWateringPump2Enabled(bool enabled);
    bool setPvVoltageThresholdV(float value);
    bool setAutonomousCycleMs(uint32_t value);

private:
    Preferences &prefs_;
    int leftTankFillPumpPin_;
    int rightTankFillPumpPin_;
    int leftTankWateringPumpPin_;
    int rightTankWateringPumpPin_;
    int topTankFullPin_;
    int leftTankEmptyPin_;
    int leftTankFullPin_;
    int rightTankEmptyPin_;
    int rightTankFullPin_;
    uint8_t maxSchedules_;
    uint8_t maxActiveRuns_;

    Schedule schedules_[32];
    ActiveRun activeRuns_[4];
    uint8_t scheduleCount_;
    uint32_t nextId_;
    bool fillPump1Enabled_;
    bool fillPump2Enabled_;
    bool wateringPump1Enabled_;
    bool wateringPump2Enabled_;
    float pvVoltageThresholdV_;
    uint32_t autonomousCycleMs_;
    bool topTankFull_;
    bool leftTankEmpty_;
    bool leftTankFull_;
    bool rightTankEmpty_;
    bool rightTankFull_;
    uint8_t autonomousPumpMask_;
    uint8_t scheduledPumpMask_;
    unsigned long lastAutonomousSwitchMs_;
    bool autonomousPumpActive_;
    uint8_t autonomousPumpIndex_;
    bool autonomousPumpEnabledForCurrentCycle_;

    static uint8_t snapTo5(uint8_t minute);
    static uint8_t minutesToSteps(int minutes);

    void saveSchedules();
    void loadSchedules();

    int findScheduleIndex(uint32_t id) const;
    void setPumpPins();
    uint8_t fillActivePumpMask() const;
    uint8_t wateringActivePumpMask() const;
    void updateOutputs();
    void activatePumpMask(uint8_t mask, uint32_t durationMs, uint32_t schedId);
    void pumpTick();
    bool scheduleMatchesToday(const Schedule &s, uint8_t weekdayIndex, uint32_t dayIndex) const;
    void checkSchedules(RtcService &rtc);
    void loadSettings();
    void saveSettings();
    void updateTankInputs();
    void updateAutonomousLogic(const EpeverTracerData &epeverData);
    void setAutonomousPumpOutput(bool on, uint8_t pumpMask);
    bool readSwitchState(int pin) const;
    String nextScheduleLabel(uint8_t pumpMask, RtcService &rtc) const;
};
}
