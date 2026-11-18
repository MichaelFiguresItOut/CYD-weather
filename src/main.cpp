// ============================================================================
//  CYD Weather Station Dashboard
//  Board : ESP32-2432S032C-I (CYD) — ST7789 320x240 + GT911 capacitive touch
//  Stack : PlatformIO / Arduino / LovyanGFX / ArduinoJson
//
//  Features:
//   - Large digital clock (H:MM AM/PM, US style), full date line
//   - Current temperature, weather description, feels-like, humidity
//   - Rain status: RAIN NOW (+ forecast end time) / RAIN NEARBY / next rain period
//   - Today's high/low, last-update indicator
//   - Open-Meteo API (temps/humidity/high-low + NOAA RRFS 15-min precip via
//     minutely_15) + KJRB/KLGA/KEWR METAR live obs for current conditions;
//     refreshed every 15 minutes
//   - NTP time sync, automatic WiFi reconnection
//   - Weather-outfit dog image in the bottom-right corner (one of 17 PNGs on
//     the SD card, selected by feels-like temp + condition; see selectDogImage())
//   - Four display themes (BASIC / SKY / SPLIT / BENTO), each with
//     automatic day/night variants; selection persisted in NVS
//   - Touch targets: clock = 12h/24h toggle, date line = format cycle,
//     temperature = F/C toggle, dog corner zone = THEME picker (swipe left/right
//     to browse the 4 display themes, tap to select; choice persists in NVS),
//     anywhere else = force a weather refresh
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Arduino.h>
#include <WiFi.h>
#include "display.h"
#include "wifi_conn.h"
#include "weather.h"
#include "config.h"
#include "render.h"
#include "picker.h"
#include "settings.h"
#include "touch.h"
#include "time_util.h"

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== CYD Weather Station ===");

  // Display first so the user gets immediate feedback. Force a full panel
  // reset before init (defensive — see displayHardReset() header comment).
  displayHardReset();
  displayInit();

  // Restore the persisted user settings (NVS "cydwx") before anything that
  // reads them: the first render, NTP offset, and weather fetch all follow.
  loadSettings();
  Serial.printf("[THEME] active: %s (%d)\n", themeName(userSettings.theme), userSettings.theme);

  // SD card + dog outfit image (implementation in weather.cpp).
  weatherInitSdAndDog();
  renderScreen();

  // WiFi (today: build-time credentials from local_config.h via
  // wifiCredentials(); the future touchscreen setup saves credentials to NVS
  // and opens a setup screen when none exist — that hook goes here).
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  wifiEnsureConnected();

  // NTP
  configureTime();

  // Wait briefly for the clock: without it, fetchWeather()'s rain scan is
  // skipped and boot would show "No rain expected" even when rain is forecast.
  uint32_t t0 = millis();
  while (!ntpIsSynced() && millis() - t0 < NTP_BOOT_WAIT_MS) delay(100);

  // Initial weather fetch (best-effort)
  if (WiFi.status() == WL_CONNECTED) {
    bool got = fetchWeather();
    if (got) refreshDogImage();   // swap error.png for the real outfit
    renderScreen();
  }

  // TEMP-DEBUG: RAM probe (remove when the animation is final).
  ramProbe();
}

void loop() {
  static uint32_t lastClockDraw = 0;
  static uint32_t lastWeatherTry = 0;
  static uint32_t lastWifiCheck  = 0;
  static bool     lastWifiState  = false;
  static bool     lastNtpState   = false;

  uint32_t nowMs = millis();

  // ---- Reconnect WiFi if dropped (check every 10 s) ----
  // Not while the Settings modal is up: the join/scan pages manage
  // connectivity themselves, and the blocking ~20 s connect attempt would
  // starve handleTouch() (touches are polled only in this loop — a tap
  // made during the block is lost, and the board feels frozen).
  // wifiEnabled() keeps the loop from fighting the user's "WIFI ON/OFF: OFF".
  if (wifiEnabled() && !settingsActive() && WiFi.status() != WL_CONNECTED &&
      nowMs - lastWifiCheck >= WIFI_RETRY_MS) {
    lastWifiCheck = nowMs;
    wifiEnsureConnected();
  }

  // ---- NTP: once connected, wait for sync ----
  if (!ntpSynced && WiFi.status() == WL_CONNECTED && ntpIsSynced()) {
    ntpSynced = true;
    Serial.println("[NTP] Time synced.");
    // If weather was already fetched before the clock existed, its rain scan
    // was skipped — refetch now so the state-change redraw below shows the
    // correct rain line + outfit (no extra render: this block runs first).
    if (wx.valid && fetchWeather()) refreshDogImage();
  }

  // ---- Full redraw only when the status-bar indicators change ----
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  if (wifiOk != lastWifiState || ntpSynced != lastNtpState) {
    lastWifiState = wifiOk;
    lastNtpState  = ntpSynced;
    // Don't clobber the modal screens (picker / settings); they repaint
    // themselves when the user leaves them.
    if (!themeSelectScreen && !settingsActive()) renderScreen();
  }

  // ---- Periodic weather refresh (15 min), retry on failure ----
  bool needWeather = !wx.valid || (nowMs - wx.lastUpdateMs >= WEATHER_REFRESH_MS);
  // Skipped while Settings is open: the fetch is a multi-second network
  // block that would swallow touch events the same way the reconnect does.
  if (needWeather && wifiOk && !settingsActive() && nowMs - lastWeatherTry >= WX_MIN_RETRY_MS) {
    lastWeatherTry = nowMs;
    // Data + outfit update even while the THEME picker is up; the redraw
    // waits until the user returns to the main display.
    if (fetchWeather()) {
      refreshDogImage();
      if (!themeSelectScreen && !settingsActive()) renderScreen();
    }
  }

  // ---- Per-second tick: repaint clock/date only when their text changes ----
  bool timeChanged = ntpSynced && (nowMs / 1000 != lastClockDraw / 1000);
  if (timeChanged) {
    lastClockDraw = nowMs;
    if (!themeSelectScreen && !settingsActive())    // partial updates target the main display only
      updateDynamicTexts();
  }


  // ---- Touch ----
  handleTouch();

  delay(50);   // keep the loop light; GT911 is polled inside getTouch()
}

