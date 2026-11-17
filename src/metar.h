#pragma once

#include <Arduino.h>

// WMO 4677 weather-codes used by METAR reports.
const char* wmoText(int code);
bool wmoIsRain(int code);

// KJRB METAR string parsing (raw report + decoded sky cover).
bool metarHasRain(const char* txt);
String metarText(const char* txt);
int metarSkyCover(const char* txt);
