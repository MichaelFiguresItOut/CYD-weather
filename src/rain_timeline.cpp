#include <Arduino.h>
#include "rain_timeline.h"

// Rain thresholds, inches. Open-Meteo quantizes RRFS output to ~0.004 in steps
// (~0.1 mm); the 15-min threshold sits above that floor so trace amounts are
// skipped, and a period needs >= RAIN_MIN_WET_STEPS wet steps before it is
// announced (an isolated single-step spike is model noise). The hourly
// threshold likewise skips trace amounts (drizzle / model noise):
static const float RAIN_15MIN_THRESH_IN  = 0.01f;   // per 15-min step (~0.25 mm, ~1 mm/hr)
static const float RAIN_HOURLY_THRESH_IN = 0.02f;   // per hour    (~0.5 mm/hr)
static const int   RAIN_MAX_DRY_STEPS    = 2;       // bridge <=30 min lulls inside one rain event
static const int   RAIN_MIN_WET_STEPS    = 2;       // min wet steps for a period to be announced

// ---------------------------------------------------------------------------
//  Rain timeline: NOAA RRFS 15-min precip (Open-Meteo "minutely_15") for the
//  near term, extended by the hourly model rain beyond that window.
//
//  A step stamped T covers the interval (T - 15 min, T] ("preceding 15 minutes
//  sum"). Wet = >= RAIN_15MIN_THRESH_IN. Consecutive wet steps form a period;
//  runs separated by <= RAIN_MAX_DRY_STEPS dry steps are bridged into one
//  period (a short lull does not end the event). Periods with fewer than
//  RAIN_MIN_WET_STEPS wet steps are ignored (isolated spikes = model noise).
//  Period onset = first wet
//  stamp - 15 min, cessation = last wet stamp. An end time is only reported
//  when the data actually shows a subsequent dry stretch -- if rain runs to
//  the edge of the forecast window the end stays unknown (""). Hourly values are
//  trailing sums over (T - 60 min, T]; their onset is reported at the start of
//  that interval (T - 60 min).
// ---------------------------------------------------------------------------


bool parseStepTime(const char* ts, StepTime& out) {
  if (!ts || strlen(ts) < 16) return false;
  out.y = atoi(ts); out.mo = atoi(ts + 5); out.d = atoi(ts + 8);
  out.hh = atoi(ts + 11); out.mm = atoi(ts + 14);
  return (out.y >= 2020 && out.mo >= 1 && out.mo <= 12 && out.d >= 1 && out.d <= 31
          && out.hh <= 23 && out.mm <= 59);
}

bool stepBefore(const StepTime& a, const StepTime& b) {   // strict <
  if (a.y != b.y) return a.y < b.y;
  if (a.mo != b.mo) return a.mo < b.mo;
  if (a.d != b.d) return a.d < b.d;
  if (a.hh != b.hh) return a.hh < b.hh;
  return a.mm < b.mm;
}

bool stepLE(const StepTime& a, const StepTime& b) { return !stepBefore(b, a); }

// t -= 15 minutes, with day/month/year borrow for the rare midnight crossing.
void stepMinus15min(StepTime& t) {
  if (t.mm >= 15) { t.mm -= 15; return; }
  t.mm += 45;
  if (t.hh > 0) { t.hh--; return; }
  t.hh = 23;
  if (t.d > 1) { t.d--; return; }
  static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (--t.mo < 1) { t.mo = 12; t.y--; }
  t.d = mdays[t.mo - 1];
  if (t.mo == 2 && ((t.y % 4 == 0 && t.y % 100 != 0) || t.y % 400 == 0)) t.d = 29;
}

void formatStepUS12(const StepTime& t, char* buf, size_t n) {   // "H:MM AM/PM"
  int h = t.hh % 12; if (h == 0) h = 12;
  snprintf(buf, n, "%d:%02d %s", h, t.mm, (t.hh < 12) ? "AM" : "PM");
}


void analyzeRainTimeline(JsonDocument& doc, const struct tm& lt, RainTimeline& out) {
  StepTime now{ lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min };

  JsonVariant vT = doc["minutely_15"]["time"];
  JsonVariant vP = doc["minutely_15"]["precipitation"];
  size_t n15 = 0;
  if (vT.is<JsonArray>() && vP.is<JsonArray>()) {
    size_t a = vT.size(), b = vP.size();
    n15 = (a < b) ? a : b;
  }
  out.have15min = (n15 > 4);   // need a few steps for this to mean anything

  StepTime after = now;        // hourly scan starts strictly after this time
  if (out.have15min) {
    const JsonArray tA = vT.as<JsonArray>();
    const JsonArray pA = vP.as<JsonArray>();

    int startIdx = -1, lastWetIdx = -1;   // open period
    int wetCount = 0;                     // wet steps in the open period
    int dryRun = 0;                       // consecutive sub-threshold steps inside it

    auto closePeriod = [&](int s, int e, bool closedByDry) {
      if (wetCount < RAIN_MIN_WET_STEPS) return;   // isolated spike: model noise
      StepTime ts, te;
      if (!parseStepTime(tA[s].as<const char*>(), ts) ||
          !parseStepTime(tA[e].as<const char*>(), te)) return;
      StepTime onset = ts;
      stepMinus15min(onset);
      // "Contains now" only when the data shows the rain actually stopping (te).
      if (closedByDry && stepLE(onset, now) && stepBefore(now, te)) {
        formatStepUS12(te, out.rainEnd, sizeof(out.rainEnd));
        out.endTime = te;          // machine-readable end time for the NBM cross-check
        out.hasEndTime = true;
      } else if (stepBefore(now, onset) && !out.nextRain[0]) {   // first future period wins
        formatStepUS12(onset, out.nextRain, sizeof(out.nextRain));
        out.nextTomorrow  = !(onset.y == now.y && onset.mo == now.mo && onset.d == now.d);
        out.nextFrom15min = true;
      }
    };

    for (size_t i = 0; i < n15; i++) {
      float p = (float)pA[i];
      bool wet = !pA[i].isNull() && !isnan(p) && p >= RAIN_15MIN_THRESH_IN;
      if (wet) {
        if (startIdx < 0) startIdx = (int)i;
        lastWetIdx = (int)i;
        wetCount++;
        dryRun = 0;
      } else if (startIdx >= 0 && ++dryRun > RAIN_MAX_DRY_STEPS) {
        closePeriod(startIdx, lastWetIdx, true);
        startIdx = -1;
        wetCount = 0;
        dryRun = 0;
      }
    }
    if (startIdx >= 0) closePeriod(startIdx, lastWetIdx, false);   // runs to end of data: no known end

    parseStepTime(tA[n15 - 1].as<const char*>(), after);   // hourly picks up beyond this
  }

  // Hourly extension: first hour with >= threshold strictly after `after`.
  JsonVariant hT = doc["hourly"]["time"];
  JsonVariant hR = doc["hourly"]["rain"];
  if (!out.nextRain[0] && hT.is<JsonArray>() && hR.is<JsonArray>()) {
    const JsonArray tH = hT.as<JsonArray>();
    const JsonArray rH = hR.as<JsonArray>();
    size_t nh = (tH.size() < rH.size()) ? tH.size() : rH.size();
    for (size_t i = 0; i < nh && !out.nextRain[0]; i++) {
      StepTime th;
      if (!parseStepTime(tH[i].as<const char*>(), th)) continue;
      if (!stepBefore(after, th)) continue;   // past, or already covered by the 15-min window
      float r = (float)rH[i];
      if (!rH[i].isNull() && !isnan(r) && r >= RAIN_HOURLY_THRESH_IN) {
        // Hourly value stamped T accumulates over (T - 60 min, T] (trailing sum);
        // report onset at the start of that interval.
        StepTime onset = th;
        if (onset.hh > 0) {
          onset.hh--;
        } else {
          onset.hh = 23;
          if (onset.d > 1) { onset.d--; }
          else {
            static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
            if (--onset.mo < 1) { onset.mo = 12; onset.y--; }
            onset.d = mdays[onset.mo - 1];
            if (onset.mo == 2 && ((onset.y % 4 == 0 && onset.y % 100 != 0) || onset.y % 400 == 0)) onset.d = 29;
          }
        }
        if (!stepBefore(now, onset)) continue;   // interval already started: METAR covers "now"
        formatStepUS12(onset, out.nextRain, sizeof(out.nextRain));
        out.nextTomorrow  = !(onset.y == now.y && onset.mo == now.mo && onset.d == now.d);
        out.nextFrom15min = false;
      }
    }
  }
}
