#include <Arduino.h>
#include <WiFi.h>
#include "wifi_conn.h"
#include "local_config.h"

// ---------------------------------------------------------------------------
//  WiFi – connect / reconnect
// ---------------------------------------------------------------------------
bool wifiEnsureConnected() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.printf("[WIFI] Connecting to %s ...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
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
