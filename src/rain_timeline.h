#pragma once

#include <ArduinoJson.h>

// One 15-minute RRFS sample; h/m/s are LOCAL wall-clock.

struct StepTime { int y, mo, d, hh, mm; };   // local wall clock from "YYYY-MM-DDTHH:MM"

struct RainTimeline {
  bool have15min = false;      // got a usable minutely_15 series
  char rainEnd[16] = "";       // cessation of the period containing now ("")
  StepTime endTime{};          // machine-readable form of rainEnd (for the NBM cross-check)
  bool hasEndTime = false;
  char nextRain[16] = "";      // onset of the next period ("")
  bool nextTomorrow = false;   // onset falls on another local day than now
  bool nextFrom15min = false;  // source: RRFS 15-min (true) vs hourly model (false)
};

// StepTime helpers (defined in rain_timeline.cpp; shared with the NBM cross-check).
bool parseStepTime(const char* ts, StepTime& out);
bool stepBefore(const StepTime& a, const StepTime& b);   // strict <
bool stepLE(const StepTime& a, const StepTime& b);
void stepMinus15min(StepTime& t);
void formatStepUS12(const StepTime& t, char* buf, size_t n);

// Fill `out` from Open-Meteo `minutely_15` data (local clock `lt`).
void analyzeRainTimeline(JsonDocument& doc, const struct tm& lt, RainTimeline& out);
