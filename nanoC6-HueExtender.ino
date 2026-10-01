/*
 * nanoC6-HueExtender.ino
 *
 * Zigbee 3.0 Range Extender (Router) disguised as a Dimmable Light Bulb for Philips Hue.
 * Target Hardware: M5Stack M5NanoC6 (ESP32-C6).
 *
 * Key Features:
 * 1. Operates as a mains-powered Zigbee 3.0 Router (repeats signals, expanding Hue mesh).
 * 2. Emulates a Philips Hue Dimmable White Bulb (LWA001) on Endpoint 11 for native pairing with Hue Bridge.
 * 3. Incorporates the Philips Hue / ZLL Trust Center Key to avoid ZDO Leave disconnections.
 * 4. Includes essential Hue ZCL attributes (ON_TIME, GLOBAL_SCENE_CONTROL, SW_BUILD_ID) to pass the
 *    Hue Bridge interview and prevent 0x85 INVALID_FIELD responses during 'Off with effect'.
 * 5. Turning the light ON/OFF via Hue app controls:
 *    - Built-in WS2812 RGB LED (white color with scalable brightness).
 *    - Grove Port A pins (GPIO 1 & GPIO 2 energized to HIGH 3.3V on ON, LOW 0V on OFF).
 * 6. Front Button (GPIO 9):
 *    - Short Click (< 1s): Toggles light state locally and reports to Hue Bridge.
 *    - Long Press (5s): Leaves Zigbee network, factory resets NVRAM/NVS, and reboots.
 */

#ifndef ZIGBEE_MODE_ZCZR
#error "Zigbee ZCZR (Coordinator/Router) mode must be selected in compile settings!"
#endif

#include <Arduino.h>
#include <Preferences.h>
#include "Zigbee.h"
#include "ep/ZigbeeColorDimmableLight.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_secur.h"
#include "esp_zigbee_attribute.h"
#include "zcl/esp_zigbee_zcl_on_off.h"
#include "zcl/esp_zigbee_zcl_level.h"
#include "zcl/esp_zigbee_zcl_color_control.h"
#include "zcl/esp_zigbee_zcl_basic.h"
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

// Philips Hue compatible bulbs use Endpoint 11
#define ZIGBEE_LIGHT_ENDPOINT 11
#define STATUS_LED_BRIGHTNESS 120

// Firmware update ID used to trigger a one-time clean NVRAM wipe after updating clusters
#define CURRENT_FW_VERSION_ID 4

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
uint8_t lightLevel = 254; // Default full brightness
uint8_t lightR = 255;
uint8_t lightG = 255;
uint8_t lightB = 255;

// Subclass to configure Philips Hue specific clusters and attributes
class HueColorDimmableLight : public ZigbeeColorDimmableLight {
public:
  HueColorDimmableLight(uint8_t endpoint) : ZigbeeColorDimmableLight(endpoint) {
    setupHueClusters();
  }

private:
  void setupHueClusters() {
    // 1. On/Off Cluster (0x0006)
    // Philips Hue sends "Off With Effect" during interview and operation.
    // Spec mandates ON_TIME and GLOBAL_SCENE_CONTROL. Without them, ZBOSS returns 0x85 (INVALID_FIELD).
    esp_zb_attribute_list_t *onoff_attr = esp_zb_cluster_list_get_cluster(
      _cluster_list, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE
    );
    if (onoff_attr) {
      static uint16_t on_off_on_time = 0;
      static uint16_t on_off_off_wait_time = 0;
      static bool on_off_global_scene_control = true;
      static uint8_t on_off_startup = 1; // 1 = On

      esp_zb_on_off_cluster_add_attr(onoff_attr, ESP_ZB_ZCL_ATTR_ON_OFF_ON_TIME, &on_off_on_time);
      esp_zb_on_off_cluster_add_attr(onoff_attr, ESP_ZB_ZCL_ATTR_ON_OFF_OFF_WAIT_TIME, &on_off_off_wait_time);
      esp_zb_on_off_cluster_add_attr(onoff_attr, ESP_ZB_ZCL_ATTR_ON_OFF_GLOBAL_SCENE_CONTROL, &on_off_global_scene_control);
      esp_zb_on_off_cluster_add_attr(onoff_attr, ESP_ZB_ZCL_ATTR_ON_OFF_START_UP_ON_OFF, &on_off_startup);
    }

    // 2. Level Control Cluster (0x0008)
    esp_zb_attribute_list_t *level_attr = esp_zb_cluster_list_get_cluster(
      _cluster_list, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE
    );
    if (level_attr) {
      static uint8_t startup_level = 254;
      static uint8_t level_options = 0;
      esp_zb_level_cluster_add_attr(level_attr, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_START_UP_CURRENT_LEVEL_ID, &startup_level);
      esp_zb_level_cluster_add_attr(level_attr, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_OPTIONS_ID, &level_options);
    }

    // 3. Basic Cluster (0x0000)
    esp_zb_attribute_list_t *basic_attr = esp_zb_cluster_list_get_cluster(
      _cluster_list, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE
    );
    if (basic_attr) {
      static char sw_build_id[] = "\x07""1.108.7";
      static char date_code[] = "\x08""20240115";
      esp_zb_basic_cluster_add_attr(basic_attr, ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID, sw_build_id);
      esp_zb_basic_cluster_add_attr(basic_attr, ESP_ZB_ZCL_ATTR_BASIC_DATE_CODE_ID, date_code);
    }
  }
};

// Zigbee Color Dimmable Light instance on Endpoint 11
HueColorDimmableLight zbLight(ZIGBEE_LIGHT_ENDPOINT);

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
// 1. WS2812 RGB LED (scalable brightness and color)
// 2. Grove connector pins (GPIO 1 & GPIO 2 energized on ON, de-energized on OFF)
void applyLightState(bool state, uint8_t level, uint8_t r = 255, uint8_t g = 255, uint8_t b = 255) {
  lightState = state;
  lightLevel = level;
  lightR = r;
  lightG = g;
  lightB = b;

  // 1. Control Grove Port signal lines (GPIO 1 & GPIO 2)
  digitalWrite(GROVE_PIN_1, state ? HIGH : LOW);
  digitalWrite(GROVE_PIN_2, state ? HIGH : LOW);

  // 2. Control Onboard RGB LED (scaled by brightness level)
  if (state && level > 0) {
    digitalWrite(RGB_LED_PWR_PIN, HIGH);
    uint32_t scaledR = ((uint32_t)r * level) / 255;
    uint32_t scaledG = ((uint32_t)g * level) / 255;
    uint32_t scaledB = ((uint32_t)b * level) / 255;
    rgbLedWrite(RGB_LED_DATA_PIN, (uint8_t)scaledR, (uint8_t)scaledG, (uint8_t)scaledB);
  } else {
    rgbLedWrite(RGB_LED_DATA_PIN, 0, 0, 0);
    digitalWrite(RGB_LED_PWR_PIN, LOW); // Turn off power gate for zero standby power
  }

  Serial.printf("[Light] State: %s | Level: %u/254 | RGB: (%u,%u,%u) | Grove (G1/G2): %s\n",
                state ? "ON" : "OFF", level, r, g, b, state ? "HIGH (3.3V)" : "LOW (0V)");
}

// Callback invoked when light RGB state changes via Zigbee (Hue App)
void onLightChangeRgbCb(bool state, uint8_t r, uint8_t g, uint8_t b, uint8_t level) {
  Serial.printf("[Zigbee Light] RGB update received -> State: %d, RGB: (%u,%u,%u), Level: %u\n", state, r, g, b, level);
  if (currentState == STATE_OPERATIONAL) {
    applyLightState(state, level, r, g, b);
  } else {
    lightState = state;
    lightLevel = level;
    lightR = r;
    lightG = g;
    lightB = b;
  }
}

// Callback invoked when Hue App triggers the "Identify" function
void onIdentifyCb(uint16_t time) {
  static bool idBlink = false;
  if (time == 0) {
    if (currentState == STATE_OPERATIONAL) {
      applyLightState(lightState, lightLevel, lightR, lightG, lightB);
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

void printNetworkDetails() {
  esp_zb_ieee_addr_t ext_pan;
  esp_zb_get_extended_pan_id(ext_pan);
  esp_zb_ieee_addr_t mac;
  esp_zb_get_long_address(mac);

  Serial.println("\n---------------- ZIGBEE STATUS & HUE INFO ----------------");
  Serial.printf("Device IEEE / MAC : %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[7], mac[6], mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]);
  Serial.printf(">>> HUE APP SERIAL NUMBER : %02X%02X%02X <<<\n",
                mac[2], mac[1], mac[0]);
  Serial.printf("Current Zigbee Channel : %d\n", esp_zb_get_current_channel());
  Serial.printf("Network PAN ID         : 0x%04X\n", esp_zb_get_pan_id());
  Serial.printf("Device Short Address   : 0x%04X\n", esp_zb_get_short_address());
  Serial.printf("Extended PAN ID        : %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\n",
                ext_pan[7], ext_pan[6], ext_pan[5], ext_pan[4], ext_pan[3], ext_pan[2], ext_pan[1], ext_pan[0]);
  Serial.println("----------------------------------------------------------\n");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n================================================");
  Serial.println("  M5NanoC6 - Philips Hue Zigbee Extender Router ");
  Serial.println("  (GLEDOPTO GL-C-008P on EP 11 + Grove Actuator) ");
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

  // Check if a one-time NVRAM wipe is needed after updating endpoint & clusters
  Preferences prefs;
  prefs.begin("hue_ext", false);
  uint32_t savedFwId = prefs.getUInt("fw_id", 0);
  bool cleanWipe = false;
  if (savedFwId != CURRENT_FW_VERSION_ID) {
    Serial.println("[NVS] New firmware version detected! Performing one-time NVRAM clean wipe...");
    prefs.putUInt("fw_id", CURRENT_FW_VERSION_ID);
    cleanWipe = true;
  }
  prefs.end();

  // Configure Zigbee Color Dimmable Light endpoint
  zbLight.onLightChangeRgb(onLightChangeRgbCb);
  zbLight.onIdentify(onIdentifyCb);
  // Setting GLEDOPTO manufacturer & model, universally recognized by Philips Hue Bridge
  zbLight.setManufacturerAndModel("GLEDOPTO", "GL-C-008P");
  zbLight.setVersion(1); // Critical: app_device_version = 1 for Hue Bridge discovery
  zbLight.setPowerSource(ZB_POWER_SOURCE_MAINS);

  Serial.println("[Zigbee] Registering Color Dimmable Light endpoint (EP 11)...");
  Zigbee.addEndpoint(&zbLight);

  // Configure Router mode with up to 20 child end devices (expands mesh coverage)
  esp_zb_cfg_t zigbeeConfig = ZIGBEE_DEFAULT_ROUTER_CONFIG();
  zigbeeConfig.nwk_cfg.zczr_cfg.max_children = 20;

  Serial.printf("[Zigbee] Starting Zigbee stack as Router (clean wipe: %s)...\n", cleanWipe ? "YES" : "NO");
  if (!Zigbee.begin(&zigbeeConfig, cleanWipe)) {
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
        printNetworkDetails();
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
        applyLightState(lightState, lightLevel, lightR, lightG, lightB);
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
      applyLightState(nextState, lightLevel, lightR, lightG, lightB);
    }
  }

  lastButtonState = reading;
}

void loop() {
  updateLedStateMachine();
  handleButton();
  delay(10);
}
