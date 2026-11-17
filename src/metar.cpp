#include <Arduino.h>
#include "metar.h"

// ---------------------------------------------------------------------------
//  WMO weather-code → short human-readable text
// ---------------------------------------------------------------------------
const char* wmoText(int code) {
  switch (code) {
    case 0:  return "Clear sky";
    case 1:  return "Mainly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Moderate rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Moderate snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Rain showers";
    case 81: return "Mod. rain showers";
    case 82: return "Violent rain shw.";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunderstorm hail";
    default: return "Unknown";
  }
}

// WMO codes that mean liquid precipitation (drizzle / rain / showers).
bool wmoIsRain(int code) {
  switch (code) {
    case 51: case 53: case 55:   // drizzle
    case 56: case 57:            // freezing drizzle
    case 61: case 63: case 65:   // rain
    case 66: case 67:            // freezing rain
    case 80: case 81: case 82:   // showers
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
//  KJRB METAR parsing (live observation, aviationweather.gov)
//
//  Open-Meteo's "current" block is model-derived and can lag reality by up to
//  an hour, so current conditions + RAIN NOW come from the KJRB (Wall Street)
//  METAR instead. Temperature/humidity/high-low/forecast stay on Open-Meteo.
// ---------------------------------------------------------------------------

// True if a whitespace-separated METAR token is liquid precipitation:
// RA / -RA / +RA, SHRA, TSRA, DZ variants, FZRA/FZDZ (same semantics as wmoIsRain).
static bool metarTokenIsRain(const char* tok) {
  const char* p = tok;
  if (*p == '-' || *p == '+') p++;        // intensity prefix
  if (strncmp(p, "VC", 2) == 0) p += 2;   // vicinity prefix
  return strcmp(p, "RA") == 0    || strcmp(p, "DZ") == 0   ||
         strcmp(p, "FZRA") == 0  || strcmp(p, "FZDZ") == 0 ||
         strcmp(p, "SHRA") == 0  || strcmp(p, "TSRA") == 0;
}

bool metarHasRain(const char* raw) {
  if (!raw || !*raw) return false;
  const char* s = raw;
  while (*s) {
    while (*s == ' ') s++;
    if (!*s) break;
    const char* e = s;
    while (*e && *e != ' ') e++;
    size_t len = (size_t)(e - s);
    if (len < 12) {                       // all precip groups are short
      char tok[12];
      memcpy(tok, s, len);
      tok[len] = '\0';
      if (metarTokenIsRain(tok)) return true;
    }
    s = e;
  }
  return false;
}

// Short display text from a raw METAR string (phrasing mirrors wmoText()).
String metarText(const char* raw) {
  if (!raw || !*raw) return "Unknown";
  // Pass 1: precipitation / fog groups take priority over sky cover.
  const char* s = raw;
  while (*s) {
    while (*s == ' ') s++;
    if (!*s) break;
    const char* e = s;
    while (*e && *e != ' ') e++;
    size_t len = (size_t)(e - s);
    if (len < 12) {
      char tok[12];
      memcpy(tok, s, len);
      tok[len] = '\0';
      const char* p = tok;
      if (*p == '-' || *p == '+') p++;
      if (strncmp(p, "VC", 2) == 0) p += 2;
      if (strcmp(p, "TSRA") == 0) return "Thunderstorms";
      if (strcmp(p, "SHRA") == 0) return "Rain showers";
      if (strcmp(p, "FZRA") == 0 || strcmp(p, "FZDZ") == 0) return "Freezing rain";
      if (strcmp(p, "RA") == 0) {
        if (*tok == '-') return "Light rain";
        if (*tok == '+') return "Heavy rain";
        return "Rain";
      }
      if (strcmp(p, "DZ") == 0) return "Drizzle";
      if (strcmp(p, "FG") == 0 || strcmp(p, "BR") == 0 ||
          strcmp(p, "UP") == 0 || strcmp(p, "PU") == 0) return "Fog";
    }
    s = e;
  }
  // Pass 2: sky cover (densest group wins).
  int cover = 0;   // 0 clear .. 3 overcast
  s = raw;
  while (*s) {
    while (*s == ' ') s++;
    if (!*s) break;
    const char* e = s;
    while (*e && *e != ' ') e++;
    size_t len = (size_t)(e - s);
    if (len >= 3 && len <= 6) {
      char tok[12];
      memcpy(tok, s, len);
      tok[len] = '\0';
      int c = -1;
      if (strcmp(tok, "SKC") == 0 || strcmp(tok, "CLR") == 0) c = 0;
      else if (strncmp(tok, "FEW", 3) == 0 || strncmp(tok, "SCT", 3) == 0) c = 1;
      else if (strncmp(tok, "BKN", 3) == 0) c = 2;
      else if (strncmp(tok, "OVC", 3) == 0) c = 3;
      if (c > cover) cover = c;
    }
    s = e;
  }
  switch (cover) {
    case 1: return "Partly cloudy";
    case 2: return "Mostly cloudy";
    case 3: return "Overcast";
    default: return "Clear";
  }
}

// Sky cover from a raw METAR string for the dog's "sunny" determination:
// 0 = clear (SKC/CLR), 1 = mainly clear (FEW), >=2 = too cloudy, -1 = no sky group.
int metarSkyCover(const char* raw) {
  if (!raw || !*raw) return -1;
  int cover = -1;   // densest seen (-1 = none yet)
  const char* s = raw;
  while (*s) {
    while (*s == ' ') s++;
    if (!*s) break;
    const char* e = s;
    while (*e && *e != ' ') e++;
    size_t len = (size_t)(e - s);
    if (len >= 3 && len <= 6) {
      int c = -1;
      if (strcmp(s, "SKC") == 0 || strcmp(s, "CLR") == 0) c = 0;
      else if (strncmp(s, "FEW", 3) == 0) c = 1;   // SCT/BKN/OVC all count as cloudy
      else if (strncmp(s, "SCT", 3) == 0 || strncmp(s, "BKN", 3) == 0 ||
               strncmp(s, "OVC", 3) == 0) c = 2;
      if (c >= 0 && (cover < 0 || c > cover)) cover = c;
    }
    s = e;
  }
  return cover;
}
