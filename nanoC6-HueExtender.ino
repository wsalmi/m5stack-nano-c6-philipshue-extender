/*
 * nanoC6-HueExtender.ino
 *
 * Zigbee 3.0 Range Extender (Router) disguised as a Dimmable Light Bulb for Philips Hue.
 * Target Hardware: M5Stack M5NanoC6 (ESP32-C6).
 *
 * Key Features:
 * 1. Operates as a mains-powered Zigbee 3.0 Router (repeats signals, expanding Hue mesh).
 * 2. Emulates a Philips Hue Dimmable White Bulb (LWA001) for native pairing with Hue Bridge.
 * 3. Incorporates the Philips Hue / ZLL Trust Center Key to avoid ZDO Leave disconnections.
 * 4. Turning the light ON/OFF via Hue app controls:
 *    - Built-in WS2812 RGB LED (white color with scalable brightness).
 *    - Grove Port A pins (GPIO 1 & GPIO 2 energized to HIGH 3.3V on ON, LOW 0V on OFF).
 * 5. Front Button (GPIO 9):
 *    - Short Click (< 1s): Toggles light state locally and reports to Hue Bridge.
 *    - Long Press (5s): Leaves Zigbee network, factory resets NVRAM/NVS, and reboots.
 */

#ifndef ZIGBEE_MODE_ZCZR
#error "Zigbee ZCZR (Coordinator/Router) mode must be selected in compile settings!"
#endif

#include <Arduino.h>
#include "Zigbee.h"
#include "ep/ZigbeeDimmableLight.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_secur.h"
#include "zdo/esp_zigbee_zdo_command.h"

// Hardware pin definitions for M5NanoC6
#ifndef BLUE_LED_PIN
#define BLUE_LED_PIN 7
#endif

#ifndef RGB_LED_PWR_PIN
#define RGB_LED_PWR_PIN 19
#endif

#ifndef RGB_LED_DATA_PIN
#define RGB_LED_DATA_PIN 20
#endif

#ifndef BTN_PIN
#define BTN_PIN 9
#endif

#ifndef GROVE_PIN_1
#define GROVE_PIN_1 1   // Grove Port A HY2.0 White pin (SCL)
#endif

#ifndef GROVE_PIN_2
#define GROVE_PIN_2 2   // Grove Port A HY2.0 Yellow pin (SDA)
#endif

#define ZIGBEE_LIGHT_ENDPOINT 1
#define STATUS_LED_BRIGHTNESS 120

// Philips Hue / ZLL Trust Center Link Key required for joining Philips Hue networks
static uint8_t HUE_TRUST_CENTER_KEY[16] = {
  0x81, 0x42, 0x86, 0x86, 0x5D, 0xC1, 0xC8, 0xB2,
  0xC8, 0xCB, 0xC5, 0x2E, 0x5D, 0x65, 0xD1, 0xB8
};

// State machine definitions for network connection and LED feedback
enum DeviceState {
  STATE_SEARCHING_NWK,
  STATE_CONNECTED_BLUE,
  STATE_CONNECTED_GREEN,
  STATE_OPERATIONAL
};

DeviceState currentState = STATE_SEARCHING_NWK;
unsigned long stateTimer = 0;
unsigned long blinkTimer = 0;
bool blinkState = false;

// Button state tracking
bool lastButtonState = HIGH;
unsigned long buttonPressStartTime = 0;
bool longPressTriggered = false;

// Light state variables (controlled by Hue app or local button)
bool lightState = false;
uint8_t lightLevel = 255; // Default 100% brightness

// Zigbee Dimmable Light instance
ZigbeeDimmableLight zbLight(ZIGBEE_LIGHT_ENDPOINT);

// LED helper functions for status feedback
void setStatusLeds(bool blueLedOn, uint8_t r, uint8_t g, uint8_t b) {
  digitalWrite(BLUE_LED_PIN, blueLedOn ? HIGH : LOW);
  if (r > 0 || g > 0 || b > 0) {
    digitalWrite(RGB_LED_PWR_PIN, HIGH);
  }
  rgbLedWrite(RGB_LED_DATA_PIN, r, g, b);
}

void turnStatusLedsOff() {
  digitalWrite(BLUE_LED_PIN, LOW);
  rgbLedWrite(RGB_LED_DATA_PIN, 0, 0, 0);
  digitalWrite(RGB_LED_PWR_PIN, LOW);
}

// Applies the current lamp state to the physical outputs:
// 1. WS2812 RGB LED (white light with scalable brightness)
// 2. Grove connector pins (GPIO 1 & GPIO 2 energized on ON, de-energized on OFF)
void applyLightState(bool state, uint8_t level) {
  lightState = state;
  lightLevel = level;

  // 1. Control Grove Port signal lines (GPIO 1 & GPIO 2)
  digitalWrite(GROVE_PIN_1, state ? HIGH : LOW);
  digitalWrite(GROVE_PIN_2, state ? HIGH : LOW);

  // 2. Control Onboard RGB LED (Pure White, scaled brightness)
  if (state && level > 0) {
    digitalWrite(RGB_LED_PWR_PIN, HIGH);
    rgbLedWrite(RGB_LED_DATA_PIN, level, level, level);
  } else {
    rgbLedWrite(RGB_LED_DATA_PIN, 0, 0, 0);
    digitalWrite(RGB_LED_PWR_PIN, LOW); // Turn off power gate for zero standby power
  }

  Serial.printf("[Light] State: %s | Level: %u/255 | Grove (G1/G2): %s\n",
                state ? "ON" : "OFF", level, state ? "HIGH (3.3V)" : "LOW (0V)");
}

// Callback invoked when light state or dimming level is changed via Zigbee (Hue App)
void onLightChangeCb(bool state, uint8_t level) {
  Serial.printf("[Zigbee Light] Attribute update received -> State: %d, Level: %d\n", state, level);
  if (currentState == STATE_OPERATIONAL) {
    applyLightState(state, level);
  } else {
    // Cache values to apply once the connection sequence finishes
    lightState = state;
    lightLevel = level;
  }
}

// Callback invoked when Hue App triggers the "Identify" function
void onIdentifyCb(uint16_t time) {
  static bool idBlink = false;
  if (time == 0) {
    if (currentState == STATE_OPERATIONAL) {
      applyLightState(lightState, lightLevel);
    } else {
      turnStatusLedsOff();
    }
    return;
  }
  idBlink = !idBlink;
  if (idBlink) {
    digitalWrite(BLUE_LED_PIN, HIGH);
    digitalWrite(RGB_LED_PWR_PIN, HIGH);
    rgbLedWrite(RGB_LED_DATA_PIN, 255, 255, 255);
  } else {
    turnStatusLedsOff();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n================================================");
  Serial.println("  M5NanoC6 - Philips Hue Zigbee Extender Router ");
  Serial.println("  (Fake Lamp + Grove GPIO 1 & 2 Actuation)      ");
  Serial.println("================================================");

  // Pin initialization
  pinMode(BLUE_LED_PIN, OUTPUT);
  digitalWrite(BLUE_LED_PIN, LOW);

  pinMode(RGB_LED_PWR_PIN, OUTPUT);
  digitalWrite(RGB_LED_PWR_PIN, HIGH);
  delay(10);
  rgbLedWrite(RGB_LED_DATA_PIN, 0, 0, 0);
  digitalWrite(RGB_LED_PWR_PIN, LOW);

  pinMode(GROVE_PIN_1, OUTPUT);
  pinMode(GROVE_PIN_2, OUTPUT);
  digitalWrite(GROVE_PIN_1, LOW);
  digitalWrite(GROVE_PIN_2, LOW);

  pinMode(BTN_PIN, INPUT_PULLUP);

  // Configure Zigbee Dimmable Light endpoint
  zbLight.onLightChange(onLightChangeCb);
  zbLight.onIdentify(onIdentifyCb);
  // Setting Manufacturer and Model recognized natively by Philips Hue Bridge
  zbLight.setManufacturerAndModel("Philips", "LWA001");
  zbLight.setVersion(1); // Critical: app_device_version = 1 for Hue Bridge discovery
  zbLight.setPowerSource(ZB_POWER_SOURCE_MAINS);

  Serial.println("[Zigbee] Registering Dimmable Light endpoint (EP 1)...");
  Zigbee.addEndpoint(&zbLight);

  // Configure Router mode with up to 20 child end devices (expands mesh coverage)
  esp_zb_cfg_t zigbeeConfig = ZIGBEE_DEFAULT_ROUTER_CONFIG();
  zigbeeConfig.nwk_cfg.zczr_cfg.max_children = 20;

  Serial.println("[Zigbee] Starting Zigbee stack as Router...");
  if (!Zigbee.begin(&zigbeeConfig)) {
    Serial.println("[Zigbee] Initialization failed! Rebooting in 2 seconds...");
    delay(2000);
    ESP.restart();
  }

  // Configure Hue Bridge Trust Center Key & Distributed Joining
  esp_zb_enable_joining_to_distributed(true);
  esp_zb_secur_TC_standard_distributed_key_set(HUE_TRUST_CENTER_KEY);
  esp_zb_secur_multi_standard_distributed_key_add(HUE_TRUST_CENTER_KEY);

  Serial.println("[Zigbee] Stack started. Searching for Philips Hue network...");
  currentState = STATE_SEARCHING_NWK;
  blinkTimer = millis();
}

void updateLedStateMachine() {
  unsigned long now = millis();

  switch (currentState) {
    case STATE_SEARCHING_NWK:
      // Flashes blue every 1s (500ms ON / 500ms OFF) while pairing
      if (now - blinkTimer >= 500) {
        blinkTimer = now;
        blinkState = !blinkState;
        if (blinkState) {
          setStatusLeds(true, 0, 0, STATUS_LED_BRIGHTNESS);
        } else {
          turnStatusLedsOff();
        }
      }

      // Check if network connection has been established
      if (Zigbee.connected()) {
        Serial.println("[Zigbee] Connected to Philips Hue network!");
        currentState = STATE_CONNECTED_BLUE;
        stateTimer = now;
        setStatusLeds(true, 0, 0, STATUS_LED_BRIGHTNESS); // Solid blue
      }
      break;

    case STATE_CONNECTED_BLUE:
      // Hold solid blue for 2 seconds (2000ms)
      if (now - stateTimer >= 2000) {
        Serial.println("[Status] Network joined. Transitioning to Solid Green for 3s...");
        currentState = STATE_CONNECTED_GREEN;
        stateTimer = now;
        setStatusLeds(false, 0, STATUS_LED_BRIGHTNESS, 0); // Solid green
      }
      break;

    case STATE_CONNECTED_GREEN:
      // Hold solid green for 3 seconds (3000ms)
      if (now - stateTimer >= 3000) {
        Serial.println("[Status] Connection sequence completed. Entering operational mode.");
        currentState = STATE_OPERATIONAL;
        digitalWrite(BLUE_LED_PIN, LOW); // Turn off blue status LED
        // Restore/apply the light state
        applyLightState(lightState, lightLevel);
      }
      break;

    case STATE_OPERATIONAL:
      // If connection is dropped unexpectedly, return to searching state
      if (!Zigbee.connected()) {
        Serial.println("[Zigbee] Connection lost! Returning to search mode...");
        currentState = STATE_SEARCHING_NWK;
        blinkTimer = now;
      }
      break;
  }
}

void handleButton() {
  int reading = digitalRead(BTN_PIN);
  unsigned long now = millis();

  // Button pressed (Active-Low: HIGH -> LOW)
  if (lastButtonState == HIGH && reading == LOW) {
    buttonPressStartTime = now;
    longPressTriggered = false;
  }

  // Button held down
  if (reading == LOW) {
    unsigned long heldDuration = now - buttonPressStartTime;

    // Held for 5 seconds or more: Trigger factory reset
    if (heldDuration >= 5000 && !longPressTriggered) {
      longPressTriggered = true;
      Serial.println("\n[Button] Button held for 5s! Resetting Zigbee stack...");

      // Visual feedback: Solid Red for 1s
      setStatusLeds(false, STATUS_LED_BRIGHTNESS, 0, 0);
      delay(1000);
      turnStatusLedsOff();

      // Clear NVS and reset Zigbee stack
      Zigbee.factoryReset(true);
    }
  }

  // Button released (LOW -> HIGH)
  if (lastButtonState == LOW && reading == HIGH) {
    unsigned long pressedDuration = now - buttonPressStartTime;

    // Short click: Between 50ms (debounce) and 4999ms
    if (!longPressTriggered && pressedDuration >= 50) {
      Serial.printf("[Button] Quick click detected (%lu ms). Toggling light state...\n", pressedDuration);

      // Invert current light state
      bool nextState = !lightState;
      zbLight.setLightState(nextState);
      applyLightState(nextState, lightLevel);
    }
  }

  lastButtonState = reading;
}

void loop() {
  updateLedStateMachine();
  handleButton();
  delay(10);
}
