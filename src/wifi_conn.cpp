#include <Arduino.h>
#include <WiFi.h>
#include "wifi_conn.h"
#include "local_config.h"
#include "config.h"    // shared "cydwx" Preferences instance

// ---------------------------------------------------------------------------
//  Credentials: NVS ("cydwx": ssid/pass) first, local_config.h as fallback.
//  The cached static buffers keep the returned const char* pointers valid
//  until the next credentials change (matches how WifiCreds is used).
// ---------------------------------------------------------------------------
// Cached so the 10 s reconnect poll does not re-read NVS every time.
static char sSavedSsid[33];
static char sSavedPass[64];
static bool sSavedLoaded = false;

static bool loadSavedCreds() {
  // Buffer overload: writes up to bufSize-1 chars + NUL, returns whether the
  // key existed at all.
  if (!prefs.getString("ssid", sSavedSsid, sizeof sSavedSsid)) return false;
  prefs.getString("pass", sSavedPass, sizeof sSavedPass);
  return sSavedSsid[0] != 0;
}

WifiCreds wifiCredentials() {
  if (!sSavedLoaded) { loadSavedCreds(); sSavedLoaded = true; }
  if (sSavedSsid[0]) return { sSavedSsid, sSavedPass, true };
  return { WIFI_SSID, WIFI_PASS, false };
}

bool wifiHasSavedCredentials() { return prefs.getString("ssid").length() > 0; }

void wifiSaveCredentials(const char* ssid, const char* pass) {
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  strncpy(sSavedSsid, ssid, 32); sSavedSsid[32] = 0;
  strncpy(sSavedPass, pass, 63); sSavedPass[63] = 0;
  sSavedLoaded = true;
  Serial.printf("[WIFI] Credentials saved to NVS (ssid=%s)\n", ssid);
}

void wifiForgetCredentials() {
  prefs.remove("ssid");
  prefs.remove("pass");
  sSavedSsid[0] = 0;
  sSavedPass[0] = 0;
  sSavedLoaded = true;
  Serial.println("[WIFI] Saved credentials erased; build-time defaults now active.");
}

// ---------------------------------------------------------------------------
//  Connect
// ---------------------------------------------------------------------------
bool wifiConnectTo(const char* ssid, const char* pass) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect();   // the caller asked for a specific network
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  Serial.printf("[WIFI] Connecting to %s ...\n", ssid);
  WiFi.begin(ssid, pass);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] Connected. IP = %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("[WIFI] Connection failed.");
  return false;
}

bool wifiEnsureConnected() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WifiCreds c = wifiCredentials();
  return wifiConnectTo(c.ssid, c.pass);
}

// ---------------------------------------------------------------------------
//  Scan (used by the Settings > Wi-Fi page)
// ---------------------------------------------------------------------------
int wifiScanNetworks(WifiScanItem* out, int maxN, int minRssiDbm) {
  // Scanning while the station is connected — or still tearing the link
  // down — truncates the scan (the RF is busy): the classic "only a
  // couple of networks found, never the current one" symptom. So: kill
  // auto-reconnect, drop the link, wait until the station is actually
  // DISCONNECTED (not a blind delay), then let the RF settle.
  //
  // The scan itself is PASSIVE: listen for beacons, 400 ms per channel,
  // all 13 channels, hidden APs included. That is the method phones use
  // to enumerate networks; the framework default (active probe, 100-300
  // ms per channel) misses APs with weak or slow beacons — the reason a
  // phone shows 15 networks while this showed 0-3. Total ~6 s.
  //
  // Modem sleep is disabled for the scan (power-save delays scan
  // events); the CYD is a plugged-in display, the mA cost is negligible.
  //
  // We do NOT reconnect here with the old credentials: the main loop
  // re-checks connectivity every 10 s (main.cpp) and reconnects on its
  // own, and wifiConnectTo() re-enables auto-reconnect on the next
  // connect.
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setAutoReconnect(false);
    WiFi.disconnect();
  }
  for (int i = 0; i < 30 && WiFi.status() != WL_DISCONNECTED; i++) delay(50);
  delay(300);   // let the RF settle after deauth

  WiFi.setSleep(false);
  int n = WiFi.scanNetworks(false, true, true, 400, 0);
  //      (sync, show_hidden, passive, 400 ms/channel, all channels)
  if (n < 0) n = 0;

  int c = 0;
  for (int i = 0; i < n && c < maxN; i++) {
    const char* s = WiFi.SSID(i).c_str();
    if (s == nullptr || *s == 0) continue;          // hidden
    if (WiFi.RSSI(i) < minRssiDbm) continue;
    bool dup = false;
    for (int j = 0; j < c; j++) {
      if (strcmp(out[j].ssid, s) == 0) { dup = true; break; }
    }
    if (dup) continue;
    strncpy(out[c].ssid, s, 32);
    out[c].ssid[32] = 0;
    out[c].rssi = (int8_t)WiFi.RSSI(i);
    out[c].secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    c++;
  }

  // Insertion sort, strongest signal first (small n).
  for (int i = 1; i < c; i++) {
    WifiScanItem t = out[i];
    int j = i;
    while (j > 0 && out[j - 1].rssi < t.rssi) { out[j] = out[j - 1]; j--; }
    out[j] = t;
  }
  return c;
}
