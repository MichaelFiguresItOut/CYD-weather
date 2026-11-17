#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
//  Weather data model
// ---------------------------------------------------------------------------
struct WeatherData {
  bool     valid        = false;
  float    temp         = NAN;   // current °F (API returns fahrenheit)
  float    feelsLike    = NAN;   // apparent temperature °F
  int      humidity     = -1;    // %
  float    tMax         = NAN;   // today high °F
  float    tMin         = NAN;   // today low °F
  String   description  = "";
  bool     isDay        = true;  // Open-Meteo current.is_day (1 = daytime)
  bool     isDayKnown   = false; // true once a usable is_day value has been received
  int      wmoCode      = -1;    // Open-Meteo current.weather_code (-1 if missing)
  bool     sunny        = false; // known daytime + clear/mainly-clear (drives "_sun" outfits)
  bool     rainingNow   = false; // currently raining at KJRB (METAR precip codes, Open-Meteo fallback)
  float    rainNowIn    = NAN;   // current-hour rain amount, inches (model)
  char     rainEnd[16] = "";     // "H:MM AM/PM" end of the current wet period per RRFS ("" = unknown)
  bool     rainMayContinue = false; // NBM cross-check: precip may continue past the RRFS end time
  bool     rainNearby   = false; // fresh precip at KLGA/KEWR while KJRB is not reporting rain now
  char     nearbyStns[8] = "";   // which stations report: "LGA", "EWR" or "LGA/EWR"
  char     nextRain[16] = "";    // "H:MM AM/PM" local time of the next rain period ("" = none in window)
  bool     nextRainTomorrow = false;
  bool     nextRainFrom15min = false;   // nextRain came from RRFS 15-min data (vs hourly model)
  uint32_t lastUpdateMs = 0;     // millis() of last successful fetch (refresh cadence)
  time_t   lastUpdateEpoch = 0;  // UTC epoch of last successful fetch (displayed)
};

// Shared timing constants (defined in weather.cpp).
extern const uint32_t WEATHER_REFRESH_MS;
extern const uint32_t WIFI_RETRY_MS;
extern const uint32_t WX_MIN_RETRY_MS;
extern const uint32_t NTP_BOOT_WAIT_MS;

extern WeatherData wx;          // current weather state (fetched over HTTPS)
extern bool sdCardReady;        // SD card present and initialized

// SD card bring-up + initial dog image (moved out of setup()).
void weatherInitSdAndDog();
void refreshDogImage();
bool fetchWeather();
