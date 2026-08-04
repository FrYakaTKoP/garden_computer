#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>

#include "config/AppConfig.h"
#include "services/DisplayManager.h"
#include "services/EpeverService.h"
#include "services/ModbusRtuClient.h"
#include "services/PumpScheduler.h"
#include "services/RtcService.h"
#include "services/WebPortal.h"

using namespace gc;

static Preferences prefs;

static RtcService rtc(
    gc::config::kI2cSdaPin,
    gc::config::kI2cSclPin,
    gc::config::kRtcEnabledDefault,
    gc::config::kRtcDebugDefault);

static ModbusRtuClient modbus(
    Serial1,
    gc::config::kRs485DePin,
    gc::config::kRs485TxPin,
    gc::config::kRs485RxPin,
    gc::config::kModbusSlaveAddress,
    gc::config::kModbusBaudRate,
    gc::config::kModbusResponseTimeoutMs,
    gc::config::kModbusDebugSerial);

static EpeverService epever(modbus, rtc, gc::config::kModbusPollIntervalMs);

static PumpScheduler scheduler(
    prefs,
    gc::config::kLeftTankFillPumpPin,
    gc::config::kRightTankFillPumpPin,
    gc::config::kLeftTankWateringPumpPin,
    gc::config::kRightTankWateringPumpPin,
    gc::config::kTopTankFullPin,
    gc::config::kLeftTankEmptyPin,
    gc::config::kLeftTankFullPin,
    gc::config::kRightTankEmptyPin,
    gc::config::kRightTankFullPin,
    gc::config::kMaxSchedules,
    gc::config::kMaxActiveRuns);

static DisplayManager display(
    gc::config::kLcdSckPin,
    gc::config::kLcdMosiPin,
    gc::config::kLcdCsPin);

static PumpRuntimeStatus cachedPumpRuntime;
static unsigned long lastPumpRuntimeRefreshMs = 0;

static WebPortal portal(
    gc::config::kApName,
    gc::config::kApPassword,
    gc::config::kApIp,
    gc::config::kDnsPort,
    gc::config::kApTimeoutMs,
    gc::config::kRestartApButtonPin,
    rtc,
    epever,
    scheduler);

static void waitForUsbSerial(unsigned long timeoutMs = gc::config::kUsbCdcStartupWaitMs)
{
    if (timeoutMs == 0)
        return;

    unsigned long startMs = millis();
    while (!Serial && (millis() - startMs) < timeoutMs)
        delay(10);
}

static void logStartupBanner()
{
    Serial.println();
    Serial.println("=== Garden Computer Boot ===");
    Serial.printf("Build: %s %s\n", __DATE__, __TIME__);
    Serial.printf("CPU: ESP32-S3 @ %lu MHz\n", static_cast<unsigned long>(getCpuFrequencyMhz()));
    Serial.printf("Flash config: board=%s fs=littlefs\n", "esp32-s3-devkitc-1");
    Serial.printf("Pins: I2C SDA=%d SCL=%d | LCD CS=%d MOSI=%d SCK=%d | RS485 TX=%d RX=%d DE=%d\n",
                  gc::config::kI2cSdaPin,
                  gc::config::kI2cSclPin,
                  gc::config::kLcdCsPin,
                  gc::config::kLcdMosiPin,
                  gc::config::kLcdSckPin,
                  gc::config::kRs485TxPin,
                  gc::config::kRs485RxPin,
                  gc::config::kRs485DePin);
}

void setup()
{
    Serial.begin(115200);
    waitForUsbSerial();
    delay(50);

    logStartupBanner();

    auto pinConflictsWithRs485 = [](int pin) -> bool
    {
        return pin >= 0 && (pin == gc::config::kRs485TxPin || pin == gc::config::kRs485RxPin || pin == gc::config::kRs485DePin);
    };

    if (pinConflictsWithRs485(gc::config::kLeftTankFillPumpPin) ||
        pinConflictsWithRs485(gc::config::kRightTankFillPumpPin) ||
        pinConflictsWithRs485(gc::config::kLeftTankWateringPumpPin) ||
        pinConflictsWithRs485(gc::config::kRightTankWateringPumpPin))
    {
        Serial.println("Pin conflict detected between pump outputs and RS485 pins");
    }

    if (!LittleFS.begin(true))
        Serial.println("LittleFS mount failed");
    else
        Serial.println("LittleFS mounted");

    prefs.begin("gc", false);

    rtc.begin();
    rtc.init();
    Serial.printf("RTC config: enabled=%s present=%s debug=%s\n",
                  rtc.enabled() ? "yes" : "no",
                  rtc.present() ? "yes" : "no",
                  rtc.debug() ? "yes" : "no");

    modbus.begin();
    Serial.printf("RS485 UART1 ready on pins TX=%d RX=%d DE=%d (active Epever Modbus polling)\n",
                  gc::config::kRs485TxPin,
                  gc::config::kRs485RxPin,
                  gc::config::kRs485DePin);
    Serial.printf("Modbus config: slave=0x%02X baud=%lu poll=%lums timeout=%lums debug=%s\n",
                  gc::config::kModbusSlaveAddress,
                  static_cast<unsigned long>(gc::config::kModbusBaudRate),
                  static_cast<unsigned long>(gc::config::kModbusPollIntervalMs),
                  static_cast<unsigned long>(gc::config::kModbusResponseTimeoutMs),
                  gc::config::kModbusDebugSerial ? "on" : "off");

    scheduler.begin();
    Serial.printf("Schedules loaded: %u\n", scheduler.scheduleCount());
    cachedPumpRuntime = scheduler.runtimeStatus(rtc);
    lastPumpRuntimeRefreshMs = millis();

    display.begin();
    display.update(
        portal.isApActive(),
        epever.data(),
        cachedPumpRuntime,
        epever.pvDailyWh(),
        epever.pvMonthlyWh(),
        epever.pvTotalWh(),
        rtc);
    portal.begin();
    epever.refreshIfNeeded();

    Serial.println("Async server started");
}

void loop()
{
    portal.loop();

    unsigned long nowMs = millis();
    if (nowMs - lastPumpRuntimeRefreshMs >= 1000UL)
    {
        cachedPumpRuntime = scheduler.runtimeStatus(rtc);
        lastPumpRuntimeRefreshMs = nowMs;
    }

    display.update(
        portal.isApActive(),
        epever.data(),
        cachedPumpRuntime,
        epever.pvDailyWh(),
        epever.pvMonthlyWh(),
        epever.pvTotalWh(),
        rtc);

    scheduler.loop(rtc, epever.data());

    // Keep the UI loop responsive even when Modbus polling blocks on timeouts.
    epever.refreshIfNeeded();
    epever.updateEnergyCounters();
    delay(10);
}
