#pragma once

#include <Arduino.h>

// System clock is UTC (configTzTime keeps it there); local time is computed in
// software by localOffsetSec(). Everything below is shared date/time plumbing.

extern bool ntpSynced;             // set by WiFi task after successful NTP sync

int dayOfWeek(int y, int m, int d);
long daysFromCivil(int y, int m, int d);
long makeUtcTime(int y, int mo, int d, int h, int mi, int s);
long localOffsetSec(time_t utc);

void formatUS12(char* out, size_t n, const struct tm* lt, bool withSeconds);
void formatClock(char* out, size_t n, const struct tm* lt);
const char* clockMaxString();
void formatDateLine(char* out, size_t n, const struct tm* lt);
float tempForDisplay(float f);
const char* tempUnitChar();

// NTP setup + local time query (wall-clock source for everything else).
void configureTime();
bool ntpIsSynced();
bool nowLocal(struct tm* out);
