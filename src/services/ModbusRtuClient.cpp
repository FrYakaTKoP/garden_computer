#include "services/ModbusRtuClient.h"

namespace gc
{
ModbusRtuClient::ModbusRtuClient(HardwareSerial &serial, int dePin, int txPin, int rxPin, uint8_t slaveAddress, uint32_t baudRate, uint32_t timeoutMs, bool debug)
    : serial_(serial),
      dePin_(dePin),
      txPin_(txPin),
      rxPin_(rxPin),
      slaveAddress_(slaveAddress),
      baudRate_(baudRate),
      timeoutMs_(timeoutMs),
      debug_(debug),
      mutex_(nullptr)
{
}

void ModbusRtuClient::begin()
{
    pinMode(dePin_, OUTPUT);
    digitalWrite(dePin_, LOW);
    serial_.begin(baudRate_, SERIAL_8N1, rxPin_, txPin_);
    serial_.setTimeout(timeoutMs_);
}

void ModbusRtuClient::ensureMutex()
{
    if (mutex_ == nullptr)
        mutex_ = xSemaphoreCreateMutex();
}

uint16_t ModbusRtuClient::crc16(const uint8_t *buf, uint16_t len) const
{
    uint16_t crc = 0xFFFF;
    for (uint16_t pos = 0; pos < len; pos++)
    {
        crc ^= buf[pos];
        for (uint8_t i = 0; i < 8; i++)
        {
            if (crc & 1)
            {
                crc >>= 1;
                crc ^= 0xA001;
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}

void ModbusRtuClient::serialPrintHexBuffer(const uint8_t *buffer, size_t len) const
{
    for (size_t i = 0; i < len; i++)
    {
        Serial.printf("%02X", buffer[i]);
        if (i + 1 < len)
            Serial.print(' ');
    }
}

bool ModbusRtuClient::readInputRegisters(uint16_t startRegister, uint16_t registerCount, uint16_t *registers)
{
    ensureMutex();
    if (mutex_ == nullptr || xSemaphoreTake(mutex_, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        if (debug_)
            Serial.println("Modbus mutex timeout (read)");
        return false;
    }

    uint8_t request[8] = {
        slaveAddress_,
        0x04,
        static_cast<uint8_t>(startRegister >> 8),
        static_cast<uint8_t>(startRegister & 0xFF),
        static_cast<uint8_t>(registerCount >> 8),
        static_cast<uint8_t>(registerCount & 0xFF),
        0,
        0};

    uint16_t requestCrc = crc16(request, 6);
    request[6] = static_cast<uint8_t>(requestCrc & 0xFF);
    request[7] = static_cast<uint8_t>(requestCrc >> 8);

    if (debug_)
    {
        Serial.printf("Modbus TX addr=0x%02X func=0x04 reg=0x%04X count=%u frame=", slaveAddress_, startRegister, registerCount);
        serialPrintHexBuffer(request, sizeof(request));
        Serial.println();
    }

    while (serial_.available())
        serial_.read();

    digitalWrite(dePin_, HIGH);
    delayMicroseconds(200);
    serial_.write(request, sizeof(request));
    serial_.flush();
    delayMicroseconds(200);
    digitalWrite(dePin_, LOW);

    size_t expectedBytes = 5 + static_cast<size_t>(registerCount) * 2;
    bool gotExceptionFrame = false;
    uint8_t response[64] = {0};
    size_t received = 0;
    unsigned long startMs = millis();

    while (received < expectedBytes && millis() - startMs < timeoutMs_)
    {
        while (serial_.available() && received < expectedBytes)
            response[received++] = static_cast<uint8_t>(serial_.read());

        if (!gotExceptionFrame && received >= 2 && response[1] == static_cast<uint8_t>(0x80 | 0x04))
        {
            gotExceptionFrame = true;
            expectedBytes = 5;
        }
    }

    if (received != expectedBytes)
    {
        if (debug_)
        {
            Serial.printf("Modbus timeout reg=0x%04X expected=%u received=%u timeoutMs=%lu",
                          startRegister,
                          static_cast<unsigned>(expectedBytes),
                          static_cast<unsigned>(received),
                          static_cast<unsigned long>(timeoutMs_));
            if (received > 0)
            {
                Serial.print(" rx=");
                serialPrintHexBuffer(response, received);
            }
            Serial.println();
        }
        xSemaphoreGive(mutex_);
        return false;
    }

    if (gotExceptionFrame)
    {
        if (received != 5)
        {
            xSemaphoreGive(mutex_);
            return false;
        }

        uint16_t responseCrc = (static_cast<uint16_t>(response[4]) << 8) | response[3];
        uint16_t calcCrc = crc16(response, 3);
        if (responseCrc != calcCrc)
        {
            xSemaphoreGive(mutex_);
            return false;
        }

        if (debug_)
        {
            const uint8_t exceptionCode = response[2];
            const char *reason = "unknown";
            if (exceptionCode == 0x01)
                reason = "illegal function";
            else if (exceptionCode == 0x02)
                reason = "illegal data address";
            else if (exceptionCode == 0x03)
                reason = "illegal data value";
            else if (exceptionCode == 0x04)
                reason = "slave device failure";

            Serial.printf("Modbus exception reg=0x%04X code=0x%02X (%s)\n",
                          startRegister,
                          exceptionCode,
                          reason);
        }
        xSemaphoreGive(mutex_);
        return false;
    }

    if (response[0] != slaveAddress_ || response[1] != 0x04 || response[2] != registerCount * 2)
    {
        if (debug_)
        {
            Serial.printf("Modbus header mismatch reg=0x%04X got=[%02X %02X %02X] expected=[%02X 04 %02X] rx=",
                          startRegister,
                          response[0], response[1], response[2],
                          slaveAddress_,
                          static_cast<uint8_t>(registerCount * 2));
            serialPrintHexBuffer(response, expectedBytes);
            Serial.println();
        }
        xSemaphoreGive(mutex_);
        return false;
    }

    uint16_t responseCrc = (static_cast<uint16_t>(response[expectedBytes - 1]) << 8) | response[expectedBytes - 2];
    uint16_t calcCrc = crc16(response, static_cast<uint16_t>(expectedBytes - 2));
    if (responseCrc != calcCrc)
    {
        if (debug_)
            Serial.printf("Modbus CRC mismatch reg=0x%04X got=0x%04X calc=0x%04X\n", startRegister, responseCrc, calcCrc);
        xSemaphoreGive(mutex_);
        return false;
    }

    for (uint16_t i = 0; i < registerCount; i++)
        registers[i] = (static_cast<uint16_t>(response[3 + i * 2]) << 8) | response[4 + i * 2];

    if (debug_)
    {
        Serial.printf("Modbus RX ok reg=0x%04X count=%u firstReg=0x%04X\n",
                      startRegister,
                      registerCount,
                      registerCount > 0 ? registers[0] : 0);
    }

    xSemaphoreGive(mutex_);
    return true;
}

bool ModbusRtuClient::writeMultipleRegisters(uint16_t startRegister, uint16_t registerCount, const uint16_t *registers)
{
    ensureMutex();
    if (mutex_ == nullptr || xSemaphoreTake(mutex_, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        if (debug_)
            Serial.println("Modbus mutex timeout (write)");
        return false;
    }

    if (registerCount == 0 || registerCount > 16)
    {
        xSemaphoreGive(mutex_);
        return false;
    }

    uint8_t request[64] = {0};
    request[0] = slaveAddress_;
    request[1] = 0x10;
    request[2] = static_cast<uint8_t>(startRegister >> 8);
    request[3] = static_cast<uint8_t>(startRegister & 0xFF);
    request[4] = static_cast<uint8_t>(registerCount >> 8);
    request[5] = static_cast<uint8_t>(registerCount & 0xFF);
    request[6] = static_cast<uint8_t>(registerCount * 2);

    for (uint16_t i = 0; i < registerCount; i++)
    {
        request[7 + i * 2] = static_cast<uint8_t>(registers[i] >> 8);
        request[8 + i * 2] = static_cast<uint8_t>(registers[i] & 0xFF);
    }

    uint16_t requestCrc = crc16(request, static_cast<uint16_t>(7 + registerCount * 2));
    request[7 + registerCount * 2] = static_cast<uint8_t>(requestCrc & 0xFF);
    request[8 + registerCount * 2] = static_cast<uint8_t>(requestCrc >> 8);

    if (debug_)
    {
        Serial.printf("Modbus TX addr=0x%02X func=0x10 reg=0x%04X count=%u frame=", slaveAddress_, startRegister, registerCount);
        serialPrintHexBuffer(request, static_cast<size_t>(9 + registerCount * 2));
        Serial.println();
    }

    while (serial_.available())
        serial_.read();

    digitalWrite(dePin_, HIGH);
    delayMicroseconds(200);
    serial_.write(request, static_cast<size_t>(9 + registerCount * 2));
    serial_.flush();
    delayMicroseconds(200);
    digitalWrite(dePin_, LOW);

    uint8_t response[8] = {0};
    size_t received = 0;
    unsigned long startMs = millis();
    while (received < sizeof(response) && millis() - startMs < timeoutMs_)
    {
        while (serial_.available() && received < sizeof(response))
            response[received++] = static_cast<uint8_t>(serial_.read());
    }

    if (received != sizeof(response))
    {
        xSemaphoreGive(mutex_);
        return false;
    }

    uint16_t responseCrc = (static_cast<uint16_t>(response[7]) << 8) | response[6];
    uint16_t calcCrc = crc16(response, 6);
    if (response[0] != slaveAddress_ || response[1] != 0x10 || response[2] != request[2] || response[3] != request[3] || response[4] != request[4] || response[5] != request[5] || responseCrc != calcCrc)
    {
        xSemaphoreGive(mutex_);
        return false;
    }

    if (debug_)
        Serial.printf("Modbus write ok reg=0x%04X count=%u\n", startRegister, registerCount);

    xSemaphoreGive(mutex_);
    return true;
}
}
