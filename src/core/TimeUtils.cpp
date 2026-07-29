#include "core/TimeUtils.h"

namespace gc::time
{
bool isLeapYear(uint16_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

uint8_t daysInMonth(uint16_t year, uint8_t month)
{
    switch (month)
    {
    case 2:
        return isLeapYear(year) ? 29 : 28;
    case 4:
    case 6:
    case 9:
    case 11:
        return 30;
    default:
        return 31;
    }
}

uint8_t dayOfWeekFromDate(uint16_t year, uint8_t month, uint8_t day)
{
    if (month < 3)
        year--;
    uint32_t y = year;
    uint32_t m = month;
    uint32_t d = day;
    uint32_t t = (y + y / 4 - y / 100 + y / 400 + (13 * m + 8) / 5 + d) % 7;
    return static_cast<uint8_t>((t + 5) % 7);
}

uint32_t daysSince2000(uint16_t year, uint8_t month, uint8_t day)
{
    uint32_t days = 0;
    for (uint16_t y = 2000; y < year; y++)
        days += isLeapYear(y) ? 366 : 365;
    for (uint8_t m = 1; m < month; m++)
        days += daysInMonth(year, m);
    days += day - 1;
    return days;
}

String formatRtcDateTime(const Ds1307Time &now)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%02u.%02u.%04u %02u:%02u",
             now.day, now.month, now.year, now.hour, now.minute);
    return String(buf);
}

String formatRtcDateInput(const Ds1307Time &now)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u", now.year, now.month, now.day);
    return String(buf);
}

String formatRtcTimeInput(const Ds1307Time &now)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%02u:%02u", now.hour, now.minute);
    return String(buf);
}

String formatRtcBottomLine(const Ds1307Time &now)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%02u.%02u.%04u %02u:%02u",
             now.day, now.month, now.year, now.hour, now.minute);
    return String(buf);
}
}
