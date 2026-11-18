#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
//  WiFi connection + credentials
//
//  Credential source (in priority order):
//    1. User-saved credentials in NVS namespace "cydwx" (keys "ssid"/"pass"),
//       written by the Settings > Wi-Fi page (wifiSaveCredentials()).
//    2. Build-time defaults from local_config.h (WIFI_SSID / WIFI_PASS).
//  "userSaved" reports whether the returned credentials come from NVS.
//  Wi-Fi passwords are stored in plaintext (NVS is unencrypted).
// ---------------------------------------------------------------------------

struct WifiCreds {
  const char* ssid;
  const char* pass;
  bool userSaved;
};

WifiCreds wifiCredentials();

// Radio power (Settings > Wi-Fi "WIFI ON/OFF" row). Turning the radio off
// stops the main loop's auto-reconnect until it is turned back on; the
// state is not persisted — a reboot always comes back with Wi-Fi on.
bool wifiEnabled();
void wifiSetRadio(bool on);

// Connect if not already connected, using wifiCredentials(). Blocks up to
// ~20 s. Safe to call repeatedly (used by boot, the reconnect timer, the
// manual refresh touch, and the weather fetch). Returns false without
// touching the radio while it is turned off.
bool wifiEnsureConnected();

// Disconnect (if needed) and connect to the given network. Blocks up to
// ~20 s. Does NOT save the credentials — the caller decides
// (wifiSaveCredentials() on success).
bool wifiConnectTo(const char* ssid, const char* pass);

// NVS-backed user credentials.
void wifiSaveCredentials(const char* ssid, const char* pass);
void wifiForgetCredentials();
bool wifiHasSavedCredentials();

// One scanned network.
struct WifiScanItem {
  char ssid[33];
  int8_t rssi;
  bool secure;   // false for open networks (connect with empty password)
};

// Scan for networks (blocking, ~6 s — passive 400 ms/channel; see
// docs/CYD-HARDWARE.md, "WiFi radio: scanning", for the reliability
// requirements this implements). Disconnects the current link first and
// waits for full teardown. Dedupes by SSID, drops SSIDs weaker than
// minRssiDbm, sorts strongest first. Returns the count stored in out
// (at most maxN).
int wifiScanNetworks(WifiScanItem* out, int maxN, int minRssiDbm);
