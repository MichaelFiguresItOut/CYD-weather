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

// Radio on/off. Not persisted: a reboot always comes back with Wi-Fi on
// (the dashboard's default state); "WIFI ON/OFF: OFF" is a session setting.
static bool sWifiEnabled = true;

bool wifiEnabled() { return sWifiEnabled; }

void wifiSetRadio(bool on) {
  sWifiEnabled = on;
  if (!on) {
    WiFi.setAutoReconnect(false);
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    Serial.println("[WIFI] Radio turned OFF (auto-reconnect paused).");
  } else {
    // Mode change re-enables the RF; the connect itself is the caller's
    // job (a join, or the main loop's 10 s reconnect after settings closes).
    WiFi.mode(WIFI_STA);
    // RF/driver settle: a begin() issued immediately after the OFF->STA
    // switch is dropped by the driver, which made the toggle-on connect
    // fail its whole 20 s poll and show OFFLINE.
    delay(300);
    Serial.println("[WIFI] Radio turned ON (auto-reconnect resumed).");
  }
}

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
  sWifiEnabled = true;   // an explicit connect turns the radio back on
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
  // Abort the driver's attempt: the 20 s poll above only stopped WAITING -
  // the station kept searching for the network in the background (state
  // stayed "connecting" long after the timeout). Left alone, that zombie
  // made the next Settings scan return 0 results and the next begin() fail
  // with "sta is connecting, cannot set config". The main loop retries on
  // its own 10 s schedule, so nothing is lost by aborting.
  WiFi.disconnect();
  return false;
}

bool wifiEnsureConnected() {
  if (!sWifiEnabled) return false;   // user turned the radio off
  if (WiFi.status() == WL_CONNECTED) return true;
  WifiCreds c = wifiCredentials();
  return wifiConnectTo(c.ssid, c.pass);
}

// ---------------------------------------------------------------------------
//  Scan (used by the Settings > Wi-Fi page)
// ---------------------------------------------------------------------------
// wifi_auth_mode_t -> short name for the scan trace.
static const char* authName(uint8_t enc) {
  switch (enc) {
    case WIFI_AUTH_OPEN:          return "open";
    case WIFI_AUTH_WEP:           return "wep";
    case WIFI_AUTH_WPA_PSK:       return "wpa";
    case WIFI_AUTH_WPA2_PSK:      return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:  return "wpa/wpa2";
    case WIFI_AUTH_WPA3_PSK:      return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
    default:                      return "other";
  }
}

// SSID -> serial-safe copy: printable ASCII as-is, any other byte as \xNN
// (SSIDs are arbitrary bytes; control chars in the log stream would be
// indistinguishable from a UART glitch).
static void ssidToLogBuffer(const char* s, char* buf, int n) {
  int o = 0;
  for (int i = 0; s[i] && o < n - 5; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (ch >= 0x20 && ch < 0x7F) {
      buf[o++] = (char)ch;
    } else {
      o += snprintf(buf + o, (size_t)(n - o), "\\x%02X", ch);
    }
  }
  buf[o < n ? o : n - 1] = 0;
}

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
  // We do NOT reconnect here with the old credentials: the scan flow in
  // settings.cpp restores the link right after the list is shown (the
  // main loop's 10 s reconnect is paused while Settings is open).
  // Callers must also make sure the radio is on: a join re-enables it via
  // wifiConnectTo(), and the Settings page refuses to scan while it is off.
  // Any state but DISCONNECTED must be torn down first - including
  // WL_CONNECTING: a timed-out connect attempt leaves the station still
  // searching for the network, and a scan against that busy RF returns 0.
  if (WiFi.status() != WL_DISCONNECTED) {
    WiFi.setAutoReconnect(false);
    WiFi.disconnect();
  }
  for (int i = 0; i < 30 && WiFi.status() != WL_DISCONNECTED; i++) delay(50);
  delay(300);   // let the RF settle after deauth

  WiFi.setSleep(false);
  uint32_t t0 = millis();
  Serial.println("[WIFI] Scan starting (passive, all channels, 400 ms/ch)...");
  int n = WiFi.scanNetworks(false, true, true, 400, 0);
  //      (sync, show_hidden, passive, 400 ms/channel, all channels)
  if (n == WIFI_SCAN_RUNNING) {
    Serial.println("[WIFI] Scan failed: a scan was already running.");
    n = 0;
  } else if (n == WIFI_SCAN_FAILED) {
    Serial.printf("[WIFI] Scan FAILED (timeout after %u ms)\n", (unsigned)(millis() - t0));
    n = 0;
  } else {
    Serial.printf("[WIFI] Scan completed in %u ms: %d networks found\n",
                  (unsigned)(millis() - t0), n);
  }

  int c = 0, droppedHidden = 0, droppedRssi = 0, droppedDup = 0, droppedCap = 0;
  String ssid;    // ours: getNetworkInfo() fills it each iteration, so the
                 // pointer stays valid for the whole iteration. (The old
                 // WiFi.SSID(i).c_str() returned a TEMPORARY String whose
                 // buffer was freed at the end of the statement - the
                 // strcmp/strncpy below then read freed memory, corrupting
                 // or dropping valid networks.)
  uint8_t enc = 0;
  int32_t rssi = 0;
  uint8_t* bssid = nullptr;
  int32_t chan = 0;
  char logbuf[65];
  for (int i = 0; i < n; i++) {
    if (!WiFi.getNetworkInfo(i, ssid, enc, rssi, bssid, chan)) continue;
    const char* s = ssid.c_str();   // valid until end of this iteration
    ssidToLogBuffer(s, logbuf, sizeof logbuf);
    // Trace EVERY raw AP so a missing network can be attributed to the RF
    // (absent below) or to a filter (reason on the next lines). After the
    // scan, so the logging never disturbs the scan itself.
    Serial.printf("[WIFI]   raw[%02d] %-22s rssi=%ld ch=%ld auth=%s\n",
                  i, logbuf, (long)rssi, (long)chan, authName(enc));
    if (s[0] == 0) { droppedHidden++; continue; }        // hidden SSID
    if (rssi < minRssiDbm) { droppedRssi++; continue; }
    if (c >= maxN) { droppedCap++; continue; }           // display-list cap
    bool dup = false;
    for (int j = 0; j < c; j++) {
      if (strcmp(out[j].ssid, s) == 0) { dup = true; break; }
    }
    if (dup) { droppedDup++; continue; }
    strncpy(out[c].ssid, s, 32);
    out[c].ssid[32] = 0;
    out[c].rssi = (int8_t)rssi;
    out[c].secure = (enc != WIFI_AUTH_OPEN);
    c++;
  }

  // Insertion sort, strongest signal first (small n).
  for (int i = 1; i < c; i++) {
    WifiScanItem t = out[i];
    int j = i;
    while (j > 0 && out[j - 1].rssi < t.rssi) { out[j] = out[j - 1]; j--; }
    out[j] = t;
  }
  // Diagnostics: raw = driver results, then each filter's discard count,
  // then stored = copied into the caller's list. If the user's network is
  // absent from the raw trace above, the passive pass itself missed it
  // (RF/antenna/channel), not the filters.
  Serial.printf("[WIFI] Scan: %d raw, %d hidden, %d below %d dBm, %d dup, %d capped at %d, %d stored\n",
                n, droppedHidden, droppedRssi, minRssiDbm, droppedDup, droppedCap, maxN, c);
  return c;
}
