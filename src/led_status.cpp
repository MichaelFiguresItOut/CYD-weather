#include "led_status.h"

#include <WiFi.h>
#include "config.h"      // userSettings (NWS station id)
#include "time_util.h"   // ntpIsSynced()

// CYD RGB LED pin map (board schematic, ESP32-2432S032C-I): each channel
// is driven active-LOW (LOW = lit). See led_status.h — none of the pins is
// shared with the TFT, the GT911 touch or the SD slot.
static const uint8_t LED_PIN_R = 4;
static const uint8_t LED_PIN_G = 16;
static const uint8_t LED_PIN_B = 17;

// All three channels are driven by hardware PWM (ESP32 LEDC): 5 kHz,
// 8-bit duty (user range 0..255; 255 maps to the full-on duty 256).
// The LEDs are active-low, so duty is the fraction of the period the pin
// is HIGH — the duty cycle is inverted for the ~1.2% LED-on time:
//   lit = 3/256 LOW  = 253/256 HIGH = 253
//   off = 100% HIGH = 255 (full on in the HAL)
static const uint32_t LED_PWM_FREQ = 5000;   // Hz
static const uint8_t  LED_PWM_BITS = 8;
static const uint32_t LED_PWM_OFF  = 255;    // 100% HIGH — channel off
static const uint32_t LED_PWM_LIT  = 253;    // 98.8% HIGH — ~1.2% LED-on time (active-low: LOW 3/256)

static uint8_t  s_activity = 0;     // nested activity counter
static bool     s_advisory = false; // on-screen error warning (see header)
static LedState s_state    = LedState::OFF;

// The state decision, isolated so the priority order (BLUE > RED > GREEN)
// lives in one place.
static LedState decide(bool activity, bool red) {
  if (activity) return LedState::BLUE;
  if (red)      return LedState::RED;
  return LedState::GREEN;
}

static const char* stateName(LedState st) {
  switch (st) {
    case LedState::GREEN: return "green/ready";
    case LedState::BLUE:  return "blue/busy";
    case LedState::RED:   return "red/attention";
    default:              return "off";
  }
}

static void apply(LedState st) {
  // Drive only through the LEDC duty — the pins belong to the LEDC
  // controller after ledInit(), so no digital GPIO writes afterwards.
  // Still mutually exclusive: exactly one channel is lit, the other two
  // are off.
  ledcWrite(LED_PIN_R, (st == LedState::RED)   ? LED_PWM_LIT : LED_PWM_OFF);
  ledcWrite(LED_PIN_G, (st == LedState::GREEN) ? LED_PWM_LIT : LED_PWM_OFF);
  ledcWrite(LED_PIN_B, (st == LedState::BLUE)  ? LED_PWM_LIT : LED_PWM_OFF);
  s_state = st;
}

void ledInit() {
  // Power-up safe: on the ESP32 digitalWrite() on a pin that is still an
  // input only sets the output latch without changing the drive state, so
  // latching HIGH first and then switching to OUTPUT can never pulse a
  // channel on — no startup flash.
  digitalWrite(LED_PIN_R, HIGH); pinMode(LED_PIN_R, OUTPUT);
  digitalWrite(LED_PIN_G, HIGH); pinMode(LED_PIN_G, OUTPUT);
  digitalWrite(LED_PIN_B, HIGH); pinMode(LED_PIN_B, OUTPUT);
  // Hand the pins to the LEDC controller: 5 kHz, 8-bit, identical for all
  // three channels (they share one timer). From here on the pins are
  // driven exclusively by ledcWrite().
  const bool attR = ledcAttach(LED_PIN_R, LED_PWM_FREQ, LED_PWM_BITS);
  const bool attG = ledcAttach(LED_PIN_G, LED_PWM_FREQ, LED_PWM_BITS);
  const bool attB = ledcAttach(LED_PIN_B, LED_PWM_FREQ, LED_PWM_BITS);
  Serial.printf("[LED] attach: R=%d G=%d B=%d\n", attR, attG, attB);
  ledcWrite(LED_PIN_R, LED_PWM_OFF);
  ledcWrite(LED_PIN_G, LED_PWM_OFF);
  ledcWrite(LED_PIN_B, LED_PWM_OFF);
  s_activity = 0;
  s_advisory = false;
  s_state = LedState::OFF;
  Serial.println("[LED] ready: R=4 G=16 B=17, active low, LEDC 5 kHz 8-bit, lit = ~1.2% on-time");
}

void ledActivityBegin() {
  if (s_activity < 250) s_activity++;   // saturates; nesting is shallow
  ledRecompute();
}

void ledActivityEnd() {
  if (s_activity == 0) {
    Serial.println("[LED] activityEnd without begin (ignored)");
    return;
  }
  s_activity--;
  ledRecompute();
}

void ledAdvisoryOn() {
  if (!s_advisory) { s_advisory = true; ledRecompute(); }
}

void ledAdvisoryOff() {
  if (s_advisory) { s_advisory = false; ledRecompute(); }
}

void ledRecompute() {
  const bool activity = (s_activity > 0);
  // Persistent attention conditions — re-derived on every recompute from
  // the existing app state (no duplicate connectivity tracking):
  // "No NWS station" is an attention condition only when the last completed
  // points lookup covered the location (nwsCovered, set by the STATION SCAN
  // / location-apply discovery and persisted in NVS): outside NWS coverage
  // (non-US sites) an empty selection is the healthy state and stays green.
  const bool red = s_advisory ||
                   !WiFi.isConnected() ||            // radio off included
                   !ntpIsSynced() ||
                   (userSettings.nwsCovered &&
                    userSettings.nwsStationId[0] == '\0');
  const LedState st = decide(activity, red);
  if (st != s_state) {
    apply(st);
    Serial.printf("[LED] -> %s%s\n", stateName(st), activity ? " (activity)" : "");
  }
}

LedState ledState() { return s_state; }
