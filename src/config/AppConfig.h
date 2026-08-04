#pragma once

#include <Arduino.h>

namespace gc::config
{
static constexpr const char *kApName = "tuttli-9000";
static constexpr const char *kApPassword = "";
static const IPAddress kApIp(192, 168, 4, 1);
static constexpr uint8_t kDnsPort = 53;
static constexpr uint32_t kApTimeoutMs = 15UL * 60UL * 1000UL;

static constexpr int kRestartApButtonPin = 0;
static constexpr int kLeftTankFillPumpPin = 37;
static constexpr int kRightTankFillPumpPin = 38;
static constexpr int kLeftTankWateringPumpPin = -1;
static constexpr int kRightTankWateringPumpPin = -1;
static constexpr int kTopTankFullPin = -1;
static constexpr int kLeftTankEmptyPin = -1;
static constexpr int kLeftTankFullPin = -1;
static constexpr int kRightTankEmptyPin = -1;
static constexpr int kRightTankFullPin = -1;
static constexpr float kAutonomousPumpPvVoltageThresholdV = 20.0f;
static constexpr uint32_t kAutonomousPumpCycleMs = 60000UL;

static constexpr int kI2cSdaPin = 8;
static constexpr int kI2cSclPin = 9;

static constexpr int kRs485DePin = 15;
static constexpr int kRs485TxPin = 17;
static constexpr int kRs485RxPin = 18;

static constexpr int kLcdCsPin = 10;
static constexpr int kLcdMosiPin = 11;
static constexpr int kLcdSckPin = 12;

static constexpr uint8_t kModbusSlaveAddress = 0x01;
static constexpr uint32_t kModbusBaudRate = 115200;
static constexpr uint32_t kModbusPollIntervalMs = 2000UL;
static constexpr uint32_t kModbusResponseTimeoutMs = 250UL;
static constexpr bool kModbusDebugSerial = false;

#ifndef RTC_ENABLED_DEFAULT
#define RTC_ENABLED_DEFAULT 1
#endif
#ifndef RTC_DEBUG_DEFAULT
#define RTC_DEBUG_DEFAULT 0
#endif
#ifndef USB_CDC_STARTUP_WAIT_MS
#define USB_CDC_STARTUP_WAIT_MS 0
#endif

static constexpr bool kRtcEnabledDefault = RTC_ENABLED_DEFAULT != 0;
static constexpr bool kRtcDebugDefault = RTC_DEBUG_DEFAULT != 0;
static constexpr unsigned long kUsbCdcStartupWaitMs = USB_CDC_STARTUP_WAIT_MS;

static constexpr uint16_t kRegPvVoltage = 0x3100;
static constexpr uint16_t kRegPvCurrent = 0x3101;
static constexpr uint16_t kRegBatteryVoltage = 0x3104;
static constexpr uint16_t kRegBatteryCurrent = 0x3105;
static constexpr uint16_t kRegLoadVoltage = 0x310C;
static constexpr uint16_t kRegLoadCurrent = 0x310D;
static constexpr uint16_t kRegBatteryTemperature = 0x3110;
static constexpr uint16_t kRegBatterySoc = 0x311A;
static constexpr uint16_t kRegDailyGeneratedEnergy = 0x330C;
static constexpr uint16_t kRegMonthlyGeneratedEnergy = 0x330E;
static constexpr uint16_t kRegTotalGeneratedEnergy = 0x3312;
static constexpr uint16_t kRegBatteryNetCurrent = 0x331B;

static constexpr uint16_t kEpeverRtcRegisterBase = 0x9013;

static constexpr uint8_t kMaxSchedules = 32;
static constexpr uint8_t kMaxActiveRuns = 4;
}
