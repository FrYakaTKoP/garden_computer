#pragma once

#include <Arduino.h>
#include "core/Models.h"

namespace gc::time
{
bool isLeapYear(uint16_t year);
uint8_t daysInMonth(uint16_t year, uint8_t month);
uint8_t dayOfWeekFromDate(uint16_t year, uint8_t month, uint8_t day);
uint32_t daysSince2000(uint16_t year, uint8_t month, uint8_t day);

String formatRtcDateTime(const Ds1307Time &now);
String formatRtcDateInput(const Ds1307Time &now);
String formatRtcTimeInput(const Ds1307Time &now);
String formatRtcBottomLine(const Ds1307Time &now);
}
