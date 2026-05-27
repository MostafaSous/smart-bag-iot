/*
 * Smart Suitcase Full System — Bag A
 *
 * Current wiring plan:
 *   LCD SDA     -> D21 / GPIO21
 *   LCD SCL     -> D22 / GPIO22
 *   ADXL345 SDA -> D21 / GPIO21
 *   ADXL345 SCL -> D22 / GPIO22
 *   HX711 DT    -> D14 / GPIO14
 *   HX711 SCK   -> D12 / GPIO12
 *   GPS TX      -> D33 / GPIO33  (ESP32 RX)
 *   GPS RX      -> D32 / GPIO32  (ESP32 TX, optional)
 *   Hall DO     -> D27 / GPIO27
 *   LED anode   -> D19 / GPIO19 through 220 ohm resistor
 *   LED cathode -> GND
 *
 * Behavior:
 *   - GPS status is reported once every minute over Serial.
 *   - If no magnet is detected, the suitcase is treated as OPEN.
 *   - HX711 weight is read only when the suitcase is unlocked and flat.
 *   - LCD line 1 shows the weight or current suitcase status.
 *   - LCD line 2 shows FLAT or NOT FLAT.
 *   - When WiFi is available: publishes sensor data to HiveMQ via MQTT.
 *   - When WiFi is unavailable: broadcasts sensor data via ESP-NOW for Bag B to relay.
 *
 * Libraries needed:
 *   - LiquidCrystal_I2C
 *   - Adafruit Unified Sensor
 *   - Adafruit ADXL345
 *   - HX711
 *   - PubSubClient
 *   - ArduinoJson
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADXL345_U.h>
#include <HX711.h>
#include <HardwareSerial.h>
#include <TinyGPS++.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ─── WiFi / MQTT credentials (fill in before flashing) ───────────────────────
#define WIFI_SSID     "Eve"
#define WIFI_PASSWORD "uob@1234"
#define MQTT_HOST     "bcb4c3772bb54988bee799265c56c625.s1.eu.hivemq.cloud"
#define MQTT_PORT     8883
#define MQTT_USER     "hivemq.webclient.1778542081950"
#define MQTT_PASS     ";0<9En8GIm1$QwOac>rT"
#define MQTT_CLIENT_ID "bagA"

// ─── ESP-NOW: MAC address of Bag B ───────────────────────────────────────────
// BAG_B_WIFI_SSID: the WiFi network Bag B connects to — used to auto-detect channel
uint8_t BAG_B_MAC[6] = {0x44, 0x1D, 0x64, 0xF4, 0x54, 0x14};
#define BAG_B_WIFI_SSID  "Events"   // ← must match WIFI_SSID in bag_b.ino
#define BAG_B_CHANNEL    1          // ← fallback only if scan fails


// ─── Publish intervals ────────────────────────────────────────────────────────
constexpr long MQTT_PUBLISH_INTERVAL_MS    = 5000;   // publish to cloud every 5 s
constexpr long ESPNOW_SEND_INTERVAL_MS     = 3000;   // send ESP-NOW every 3 s
constexpr long WIFI_RETRY_INTERVAL_MS      = 30000;  // reattempt WiFi every 30 s
constexpr int  WIFI_FAILS_BEFORE_ESPNOW    = 3;      // activate ESP-NOW after 3 failed attempts

namespace {
constexpr uint8_t LCD_COLUMNS = 16;
constexpr uint8_t LCD_ROWS = 2;
constexpr uint8_t I2C_SDA_PIN = 21;
constexpr uint8_t I2C_SCL_PIN = 22;

constexpr uint8_t HX711_DT_PIN = 14;
constexpr uint8_t HX711_SCK_PIN = 12;
constexpr uint8_t HALL_SENSOR_PIN = 27;
constexpr uint8_t HALL_LED_PIN = 19;
constexpr int HALL_MAGNET_DETECTED_STATE = LOW;

constexpr int GPS_RX_PIN = 33;
constexpr int GPS_TX_PIN = 32;
constexpr unsigned long USB_BAUD_RATE = 115200;
constexpr unsigned long GPS_BAUD_RATE = 9600;

constexpr float PITCH_ENTER_FLAT_LOWER_DEG = -8.0f;
constexpr float PITCH_ENTER_FLAT_UPPER_DEG = 8.0f;
constexpr float ROLL_ENTER_FLAT_LOWER_DEG = -8.0f;
constexpr float ROLL_ENTER_FLAT_UPPER_DEG = 8.0f;
constexpr float PITCH_EXIT_FLAT_LOWER_DEG = -10.0f;
constexpr float PITCH_EXIT_FLAT_UPPER_DEG = 10.0f;
constexpr float ROLL_EXIT_FLAT_LOWER_DEG = -10.0f;
constexpr float ROLL_EXIT_FLAT_UPPER_DEG = 10.0f;
constexpr float RAD_TO_DEGREE = 180.0f / PI;

constexpr long HX711_READ_INTERVAL_MS = 150;          // faster polling
constexpr long DISPLAY_UPDATE_INTERVAL_MS = 250;
constexpr long SENSOR_UPDATE_INTERVAL_MS = 250;
constexpr long GPS_REPORT_INTERVAL_MS = 15000;         // GPS debug every 15 s

constexpr float HX711_CALIBRATION_FACTOR = 15080.0f;
constexpr float HX711_ZERO_THRESHOLD_GRAMS = 100.0f;
constexpr uint8_t HX711_SAMPLES_PER_READ = 5;         // was 20 — much faster
constexpr float HX711_EMA_ALPHA = 0.3f;               // slightly more responsive

const uint8_t kLcdCandidateAddresses[] = {0x27, 0x3F};
const uint8_t kAdxlCandidateAddresses[] = {0x53, 0x1D};

LiquidCrystal_I2C *lcd = nullptr;
Adafruit_ADXL345_Unified adxl(12345);
HX711 scale;
HardwareSerial gpsSerial(2);
TinyGPSPlus gps;

uint8_t detectedLcdAddress = 0;
uint8_t detectedAdxlAddress = 0;
bool lcdReady = false;
bool adxlReady = false;
bool hx711Ready = false;

float currentPitchDeg = 0.0f;
float currentRollDeg = 0.0f;
float currentWeightGrams = 0.0f;
float emaWeightGrams = 0.0f;
bool emaInitialized = false;
bool currentFlatState = false;
bool currentOpenState = false;
bool currentMagnetDetected = false;
bool currentWeightValid = false;

unsigned long lastSensorUpdateMs = 0;
unsigned long lastWeightUpdateMs = 0;
unsigned long lastDisplayUpdateMs = 0;
unsigned long lastGpsReportMs = 0;
unsigned long lastMqttPublishMs = 0;
unsigned long lastEspNowSendMs = 0;
unsigned long lastWifiRetryMs = 0;
int           wifiFailCount = 0;    // increments on each failed attempt; resets on connect

bool wifiConnected = false;
bool espNowReady = false;

WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);
}  // namespace

// ─── Shared struct sent over ESP-NOW ─────────────────────────────────────────
struct __attribute__((packed)) BagAPayload {
  float lat;
  float lng;
  bool  gpsValid;
  float weightGrams;
  float pitchDeg;
  float rollDeg;
  bool  flat;
  bool  open;
  bool  magnetDetected;
};

struct SystemState {
  bool suitcaseOpen;
  bool magnetDetected;
  bool flat;
  bool weightValid;
  float pitchDeg;
  float rollDeg;
  float weightGrams;
};

// ─── I2C / LCD ────────────────────────────────────────────────────────────────
void setupI2C() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
}

uint8_t detectLcdAddress() {
  for (uint8_t address : kLcdCandidateAddresses) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      return address;
    }
  }
  return 0;
}

void initLcd(uint8_t address) {
  if (lcd != nullptr) {
    delete lcd;
    lcd = nullptr;
  }
  detectedLcdAddress = address;
  lcd = new LiquidCrystal_I2C(address, LCD_COLUMNS, LCD_ROWS);
  lcd->init();
  lcd->backlight();
  lcdReady = true;
}

void printPaddedLine(uint8_t row, const char *text) {
  if (!lcdReady || lcd == nullptr) return;
  lcd->setCursor(0, row);
  lcd->print("                ");
  lcd->setCursor(0, row);
  lcd->print(text);
}

void showScreen(const char *line1, const char *line2) {
  printPaddedLine(0, line1);
  printPaddedLine(1, line2);
}

// ─── ADXL345 ─────────────────────────────────────────────────────────────────
bool initAdxl() {
  for (uint8_t address : kAdxlCandidateAddresses) {
    if (adxl.begin(address)) {
      adxl.setRange(ADXL345_RANGE_16_G);
      detectedAdxlAddress = address;
      return true;
    }
  }
  return false;
}

// ─── HX711 ───────────────────────────────────────────────────────────────────
bool initHx711() {
  scale.begin(HX711_DT_PIN, HX711_SCK_PIN);
  Serial.println("HX711 ready. Remove all weight, taring in 2 seconds...");
  delay(2000);
  scale.set_scale(HX711_CALIBRATION_FACTOR);
  scale.tare();
  Serial.println("HX711 tare complete.");
  return true;
}

// ─── Hall sensor ─────────────────────────────────────────────────────────────
bool isMagnetDetected() {
  return digitalRead(HALL_SENSOR_PIN) == HALL_MAGNET_DETECTED_STATE;
}

void setHallLed(bool magnetDetected) {
  digitalWrite(HALL_LED_PIN, magnetDetected ? HIGH : LOW);
}

// ─── WiFi ─────────────────────────────────────────────────────────────────────
void attemptWifiConnect() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(250);
  }

  wifiConnected = (WiFi.status() == WL_CONNECTED);
  if (wifiConnected) {
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
    wifiClient.setInsecure();
    mqttClient.setServer(MQTT_HOST, MQTT_PORT);
    mqttClient.setBufferSize(512);
    wifiFailCount = 0;   // WiFi is back — reset counter
  } else {
    wifiFailCount++;
    Serial.printf("[WiFi] Failed (attempt %d/%d).\n", wifiFailCount, WIFI_FAILS_BEFORE_ESPNOW);
    WiFi.disconnect(false);  // keep radio ON — ESP-NOW needs it
  }
}

// ─── MQTT ─────────────────────────────────────────────────────────────────────
bool ensureMqttConnected() {
  if (mqttClient.connected()) return true;

  Serial.print("Connecting to MQTT broker...");
  if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
    Serial.println(" connected.");
    return true;
  }
  Serial.print(" failed, rc=");
  Serial.println(mqttClient.state());
  return false;
}

void publishJson(const char *topic, JsonDocument &doc) {
  char buf[256];
  serializeJson(doc, buf, sizeof(buf));
  mqttClient.publish(topic, buf, /*retained=*/false);
}

void publishAllSensors() {
  if (!ensureMqttConnected()) return;

  StaticJsonDocument<128> doc;

  // GPS
  doc.clear();
  doc["lat"]   = gps.location.isValid() ? gps.location.lat() : 0.0;
  doc["lng"]   = gps.location.isValid() ? gps.location.lng() : 0.0;
  doc["valid"] = gps.location.isValid();
  publishJson("/bags/bagA/gps", doc);

  // Weight
  doc.clear();
  doc["grams"] = currentWeightValid ? currentWeightGrams : 0.0f;
  publishJson("/bags/bagA/weight", doc);

  // Tilt
  doc.clear();
  doc["pitch"] = currentPitchDeg;
  doc["roll"]  = currentRollDeg;
  doc["flat"]  = currentFlatState;
  publishJson("/bags/bagA/tilt", doc);

  // Status
  doc.clear();
  doc["open"]            = currentOpenState;
  doc["magnetDetected"]  = currentMagnetDetected;
  publishJson("/bags/bagA/status", doc);

  Serial.println("[MQTT] Published all topics.");
}

// ─── ESP-NOW ──────────────────────────────────────────────────────────────────
void onEspNowSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  (void)info;
  if (status == ESP_NOW_SEND_SUCCESS) Serial.println("[ESP-NOW] ACK received.");
  else                                 Serial.println("[ESP-NOW] No ACK — check channel/MAC.");
}

// Scans WiFi to find which channel BAG_B_WIFI_SSID is on.
// Returns BAG_B_CHANNEL if the network isn't visible.
int scanForBagBChannel() {
  Serial.printf("[ESP-NOW] Scanning for '%s' to detect Bag B channel...\n", BAG_B_WIFI_SSID);
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false,
                             /*passive=*/false, /*max_ms=*/300);
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == BAG_B_WIFI_SSID) {
      int ch = WiFi.channel(i);
      WiFi.scanDelete();
      Serial.printf("[ESP-NOW] Found '%s' on channel %d.\n", BAG_B_WIFI_SSID, ch);
      return ch;
    }
  }
  WiFi.scanDelete();
  Serial.printf("[ESP-NOW] '%s' not visible; using fallback channel %d.\n",
                BAG_B_WIFI_SSID, BAG_B_CHANNEL);
  return BAG_B_CHANNEL;
}

bool initEspNow() {
  WiFi.mode(WIFI_STA);
  int channel = scanForBagBChannel();
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed.");
    return false;
  }
  esp_now_register_send_cb(onEspNowSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BAG_B_MAC, 6);
  peer.channel = channel;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("ESP-NOW add peer failed.");
    return false;
  }
  Serial.printf("[ESP-NOW] Ready on channel %d.\n", channel);
  return true;
}

void sendEspNow() {
  BagAPayload payload;
  payload.lat           = gps.location.isValid() ? (float)gps.location.lat() : 0.0f;
  payload.lng           = gps.location.isValid() ? (float)gps.location.lng() : 0.0f;
  payload.gpsValid      = gps.location.isValid();
  payload.weightGrams   = currentWeightValid ? currentWeightGrams : 0.0f;
  payload.pitchDeg      = currentPitchDeg;
  payload.rollDeg       = currentRollDeg;
  payload.flat          = currentFlatState;
  payload.open          = currentOpenState;
  payload.magnetDetected = currentMagnetDetected;

  esp_now_send(BAG_B_MAC, (uint8_t *)&payload, sizeof(payload));
  Serial.println("[ESP-NOW] Packet sent to Bag B.");
}

// ─── Sensor helpers ───────────────────────────────────────────────────────────
bool isWithinRange(float value, float lower, float upper) {
  return value >= lower && value <= upper;
}

bool isFlatEnough(float pitchDeg, float rollDeg) {
  const bool insideEnterWindow =
      isWithinRange(pitchDeg, PITCH_ENTER_FLAT_LOWER_DEG, PITCH_ENTER_FLAT_UPPER_DEG) &&
      isWithinRange(rollDeg, ROLL_ENTER_FLAT_LOWER_DEG, ROLL_ENTER_FLAT_UPPER_DEG);
  const bool insideExitWindow =
      isWithinRange(pitchDeg, PITCH_EXIT_FLAT_LOWER_DEG, PITCH_EXIT_FLAT_UPPER_DEG) &&
      isWithinRange(rollDeg, ROLL_EXIT_FLAT_LOWER_DEG, ROLL_EXIT_FLAT_UPPER_DEG);

  if (currentFlatState) return insideExitWindow;
  return insideEnterWindow;
}

void updateTiltState() {
  sensors_event_t event;
  adxl.getEvent(&event);
  const float x = event.acceleration.x;
  const float y = event.acceleration.y;
  const float z = event.acceleration.z;
  currentRollDeg  = atan2f(y, z) * RAD_TO_DEGREE;
  currentPitchDeg = atan2f(-x, sqrtf((y * y) + (z * z))) * RAD_TO_DEGREE;
  currentFlatState = isFlatEnough(currentPitchDeg, currentRollDeg);
}

void updateWeightState() {
  currentWeightValid = false;
  if (!hx711Ready || !scale.is_ready()) return;

  float rawGrams = scale.get_units(HX711_SAMPLES_PER_READ) * 1000.0f;
  if (rawGrams < HX711_ZERO_THRESHOLD_GRAMS) rawGrams = 0.0f;

  if (!emaInitialized) {
    emaWeightGrams = rawGrams;
    emaInitialized = true;
  } else {
    emaWeightGrams = HX711_EMA_ALPHA * rawGrams + (1.0f - HX711_EMA_ALPHA) * emaWeightGrams;
  }
  currentWeightGrams = emaWeightGrams;
  currentWeightValid = true;
}

void captureGpsData() {
  while (gpsSerial.available() > 0) {
    if (gps.encode(gpsSerial.read())) {
      // A complete NMEA sentence was just parsed — location is fresh
      if (gps.location.isValid()) {
        Serial.print("[GPS] Fix: ");
        Serial.print(gps.location.lat(), 6);
        Serial.print(", ");
        Serial.println(gps.location.lng(), 6);
      }
    }
  }
}

void reportGpsStatusIfDue() {
  const unsigned long now = millis();
  if (now - lastGpsReportMs < GPS_REPORT_INTERVAL_MS) return;
  lastGpsReportMs = now;

  Serial.println();
  Serial.println("[GPS report]");
  Serial.print("  Chars processed : "); Serial.println(gps.charsProcessed());
  Serial.print("  Sentences good  : "); Serial.println(gps.sentencesWithFix());
  Serial.print("  Checksum fail   : "); Serial.println(gps.failedChecksum());
  Serial.print("  Satellites      : ");
  Serial.println(gps.satellites.isValid() ? gps.satellites.value() : 0);

  if (!gps.location.isValid()) {
    Serial.println("  Fix: NONE — move near a window or outdoors.");
    return;
  }
  Serial.print("  Latitude  : "); Serial.println(gps.location.lat(), 6);
  Serial.print("  Longitude : "); Serial.println(gps.location.lng(), 6);
  Serial.print("  Age (ms)  : "); Serial.println(gps.location.age());
}

SystemState readSystemState() {
  SystemState state;
  state.magnetDetected = currentMagnetDetected;
  state.suitcaseOpen   = currentOpenState;
  state.pitchDeg       = currentPitchDeg;
  state.rollDeg        = currentRollDeg;
  state.flat           = adxlReady ? currentFlatState : false;
  state.weightGrams    = currentWeightGrams;
  state.weightValid    = currentWeightValid;
  return state;
}

void printStateToSerial(const SystemState &state) {
  Serial.print("Suitcase: ");
  Serial.print(state.suitcaseOpen ? "OPEN" : "CLOSED");
  Serial.print(" | Magnet: ");
  Serial.print(state.magnetDetected ? "DETECTED" : "NOT DETECTED");
  Serial.print(" | Pitch: ");   Serial.print(state.pitchDeg, 1);
  Serial.print(" | Roll: ");    Serial.print(state.rollDeg, 1);
  Serial.print(" | Flat: ");    Serial.print(state.flat ? "YES" : "NO");
  Serial.print(" | Weight: ");  Serial.print(state.weightGrams, 0); Serial.print(" g");
  Serial.print(" | Lat: ");
  if (gps.location.isValid()) {
    Serial.print(gps.location.lat(), 6);
    Serial.print(" | Lng: ");
    Serial.println(gps.location.lng(), 6);
  } else {
    Serial.println("No Signal");
  }
  Serial.print(" | WiFi: ");
  Serial.print(wifiConnected ? "YES" : "NO");
  Serial.print(" | MQTT: ");
  Serial.println(mqttClient.connected() ? "CONNECTED" : "OFFLINE");
}

void updateLcd(const SystemState &state) {
  if (!lcdReady) return;

  char line1[LCD_COLUMNS + 1];
  char line2[LCD_COLUMNS + 1];

  // Always show weight — live reading or last known value
  if (emaInitialized) {
    if (state.weightValid) {
      snprintf(line1, sizeof(line1), "Wt:%6.0fg LIVE", state.weightGrams);
    } else {
      snprintf(line1, sizeof(line1), "Wt:%6.0fg last", emaWeightGrams);
    }
  } else {
    snprintf(line1, sizeof(line1), state.suitcaseOpen ? "OPEN" : "CLOSED");
  }
  printPaddedLine(0, line1);

  // Line 2: tilt + connectivity status
  if (!wifiConnected && !espNowReady && wifiFailCount > 0) {
    snprintf(line2, sizeof(line2), "%s WiFi %d/%d",
             state.flat ? "FLAT" : "TILT", wifiFailCount, WIFI_FAILS_BEFORE_ESPNOW);
  } else if (!wifiConnected && espNowReady) {
    snprintf(line2, sizeof(line2), "%s ESP-NOW",
             state.flat ? "FLAT" : "TILT");
  } else {
    snprintf(line2, sizeof(line2), "%s | %s",
             state.flat ? "FLAT" : "TILT",
             wifiConnected ? "WiFi" : "...");
  }
  printPaddedLine(1, line2);
}

// ─── setup / loop ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(USB_BAUD_RATE);
  delay(250);

  pinMode(HALL_SENSOR_PIN, INPUT);
  pinMode(HALL_LED_PIN, OUTPUT);
  digitalWrite(HALL_LED_PIN, LOW);

  setupI2C();

  const uint8_t lcdAddress = detectLcdAddress();
  if (lcdAddress != 0) {
    initLcd(lcdAddress);
    showScreen("Smart Suitcase", "Booting...");
  } else {
    Serial.println("LCD not found.");
  }

  adxlReady  = initAdxl();
  if (!adxlReady) Serial.println("ADXL345 not found.");

  hx711Ready = initHx711();
  if (!hx711Ready) Serial.println("HX711 not ready.");

  gpsSerial.begin(GPS_BAUD_RATE, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  // Try WiFi — ESP-NOW only activates after 60 s without WiFi (handled in loop)
  attemptWifiConnect();

  Serial.println("Smart suitcase full system started.");
  Serial.println("Open + flat => live weight on LCD.");
  Serial.println("Line 2 shows FLAT / NOT FLAT.");

  if (lcdReady) delay(1000);
}

void loop() {
  captureGpsData();
  reportGpsStatusIfDue();

  const unsigned long now = millis();

  // ── Sensor updates ──────────────────────────────────────────────────────
  if (now - lastSensorUpdateMs >= SENSOR_UPDATE_INTERVAL_MS) {
    lastSensorUpdateMs = now;
    currentMagnetDetected = isMagnetDetected();
    currentOpenState      = !currentMagnetDetected;
    setHallLed(currentMagnetDetected);
    if (adxlReady) updateTiltState();
  }

  if (currentOpenState && currentFlatState &&
      now - lastWeightUpdateMs >= HX711_READ_INTERVAL_MS) {
    lastWeightUpdateMs = now;
    updateWeightState();
  } else if (!currentOpenState || !currentFlatState) {
    currentWeightValid = false;
  }

  // ── MQTT publish (WiFi is up) ────────────────────────────────────────────
  if (wifiConnected && now - lastMqttPublishMs >= MQTT_PUBLISH_INTERVAL_MS) {
    lastMqttPublishMs = now;
    publishAllSensors();
    mqttClient.loop();
  }

  // ── WiFi retry every 30 s while disconnected ─────────────────────────────
  if (!wifiConnected && now - lastWifiRetryMs >= WIFI_RETRY_INTERVAL_MS) {
    lastWifiRetryMs = now;
    Serial.println("[WiFi] Retrying connection...");
    if (espNowReady) {
      esp_now_deinit();
      espNowReady = false;
    }
    // Full radio reset — clears the channel lock set by esp_wifi_set_channel()
    WiFi.mode(WIFI_OFF);
    delay(100);
    WiFi.mode(WIFI_STA);
    attemptWifiConnect();
  }

  // ── Activate ESP-NOW after 3 failed WiFi attempts ────────────────────────
  if (!wifiConnected && !espNowReady && wifiFailCount >= WIFI_FAILS_BEFORE_ESPNOW) {
    Serial.println("[ESP-NOW] 3 WiFi failures — activating relay mode via Bag B.");
    showScreen("No WiFi x3", "ESP-NOW ON");
    espNowReady = initEspNow();
  }

  // ── ESP-NOW send to Bag B (relay mode active) ─────────────────────────────
  if (!wifiConnected && espNowReady &&
      now - lastEspNowSendMs >= ESPNOW_SEND_INTERVAL_MS) {
    lastEspNowSendMs = now;
    sendEspNow();
  }

  // ── Display + Serial ────────────────────────────────────────────────────
  if (now - lastDisplayUpdateMs >= DISPLAY_UPDATE_INTERVAL_MS) {
    lastDisplayUpdateMs = now;
    const SystemState state = readSystemState();
    updateLcd(state);
    printStateToSerial(state);
  }
}
