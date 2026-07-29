#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace gc
{
class ModbusRtuClient
{
public:
    ModbusRtuClient(HardwareSerial &serial, int dePin, int txPin, int rxPin, uint8_t slaveAddress, uint32_t baudRate, uint32_t timeoutMs, bool debug);

    void begin();
    bool readInputRegisters(uint16_t startRegister, uint16_t registerCount, uint16_t *registers);
    bool writeMultipleRegisters(uint16_t startRegister, uint16_t registerCount, const uint16_t *registers);

private:
    HardwareSerial &serial_;
    int dePin_;
    int txPin_;
    int rxPin_;
    uint8_t slaveAddress_;
    uint32_t baudRate_;
    uint32_t timeoutMs_;
    bool debug_;
    SemaphoreHandle_t mutex_;

    void ensureMutex();
    uint16_t crc16(const uint8_t *buf, uint16_t len) const;
    void serialPrintHexBuffer(const uint8_t *buffer, size_t len) const;
};
}
