/*
 * Smart Suitcase — Bag B (Relay Node)
 *
 * Hardware: ESP32 only — no sensors.
 *
 * Role:
 *   - Listens for ESP-NOW packets broadcast by Bag A.
 *   - When a packet arrives, publishes all sensor data to HiveMQ on Bag A's
 *     behalf and marks the /bags/bagA/relay topic so the dashboard knows this
 *     data was relayed.
 *   - Sends a /bags/bagB/heartbeat every 10 s so the dashboard knows Bag B
 *     is alive.
 *
 * On first boot, print this device's MAC address to Serial so you can paste
 * it into BAG_B_MAC[] in bag_a.ino.
 *
 * Libraries needed:
 *   - PubSubClient
 *   - ArduinoJson
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_now.h>

// ─── Credentials (fill in before flashing) ────────────────────────────────────
#define WIFI_SSID     "Events"
#define WIFI_PASSWORD "uob@1234"
#define MQTT_HOST     "bcb4c3772bb54988bee799265c56c625.s1.eu.hivemq.cloud"
#define MQTT_PORT     8883
#define MQTT_USER     "hivemq.webclient.1778542081950"
#define MQTT_PASS     ";0<9En8GIm1$QwOac>rT"
#define MQTT_CLIENT_ID "bagB"
#define BAG_B_CHANNEL 1

constexpr unsigned long HEARTBEAT_INTERVAL_MS = 10000;

// ─── Shared struct — must match bag_a exactly ─────────────────────────────────
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

// ─── Globals ──────────────────────────────────────────────────────────────────
WiFiClientSecure wifiClient;
PubSubClient     mqttClient(wifiClient);

volatile bool    newPacketReady = false;
BagAPayload      pendingPayload;

unsigned long    lastHeartbeatMs = 0;

// ─── WiFi ─────────────────────────────────────────────────────────────────────
void connectWifi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.print("\nWiFi connected. IP: ");
  Serial.println(WiFi.localIP());
}

// ─── MQTT ─────────────────────────────────────────────────────────────────────
bool ensureMqttConnected() {
  if (mqttClient.connected()) return true;
  Serial.print("Connecting to MQTT...");
  if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
    Serial.println(" connected.");
    return true;
  }
  Serial.print(" failed, rc=");
  Serial.println(mqttClient.state());
  return false;
}

bool publishJson(const char *topic, JsonDocument &doc) {
  char buf[256];
  serializeJson(doc, buf, sizeof(buf));
  bool ok = mqttClient.publish(topic, buf, /*retained=*/false);
  mqttClient.loop();   // flush after every publish over TLS
  if (!ok) Serial.printf("[MQTT] Publish FAILED on %s\n", topic);
  return ok;
}

void relayPayload(const BagAPayload &p) {
  if (!ensureMqttConnected()) {
    Serial.println("[MQTT] Cannot relay — not connected.");
    return;
  }

  StaticJsonDocument<128> doc;

  // GPS
  doc.clear();
  doc["lat"]   = p.gpsValid ? p.lat : 0.0f;
  doc["lng"]   = p.gpsValid ? p.lng : 0.0f;
  doc["valid"] = p.gpsValid;
  publishJson("/bags/bagA/gps", doc);

  // Weight
  doc.clear();
  doc["grams"] = p.weightGrams;
  publishJson("/bags/bagA/weight", doc);

  // Tilt
  doc.clear();
  doc["pitch"] = p.pitchDeg;
  doc["roll"]  = p.rollDeg;
  doc["flat"]  = p.flat;
  publishJson("/bags/bagA/tilt", doc);

  // Status
  doc.clear();
  doc["open"]           = p.open;
  doc["magnetDetected"] = p.magnetDetected;
  publishJson("/bags/bagA/status", doc);

  // Relay marker
  doc.clear();
  doc["relayedBy"] = "bagB";
  publishJson("/bags/bagA/relay", doc);

  Serial.println("[MQTT] Relay complete.");
}

void sendHeartbeat() {
  if (!ensureMqttConnected()) return;
  StaticJsonDocument<32> doc;
  doc["alive"] = true;
  publishJson("/bags/bagB/heartbeat", doc);
  Serial.println("[MQTT] Heartbeat sent.");
}

// ─── ESP-NOW receive callback (runs in WiFi task context) ─────────────────────
void onDataReceived(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info;
  if (len != sizeof(BagAPayload)) {
    Serial.printf("[ESP-NOW] Unexpected packet size: %d\n", len);
    return;
  }
  memcpy((void *)&pendingPayload, data, sizeof(BagAPayload));
  newPacketReady = true;
}

// ─── setup / loop ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(250);

  connectWifi();

  // Print own MAC and channel — verify these match bag_a.ino
  Serial.println("========================================");
  Serial.print("Bag B MAC address: ");
  Serial.println(WiFi.macAddress());
  Serial.print("WiFi channel    : ");
  Serial.println(WiFi.channel());
  Serial.print("BAG_B_WIFI_SSID : ");
  Serial.println(WIFI_SSID);
  Serial.println("========================================");

  wifiClient.setInsecure();
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setBufferSize(1024);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed. Halting.");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onDataReceived);
  Serial.println("ESP-NOW listening. Bag B ready.");
}

void loop() {
  mqttClient.loop();

  // Relay any received Bag A packet
  if (newPacketReady) {
    newPacketReady = false;
    relayPayload(pendingPayload);
  }

  // Periodic heartbeat
  const unsigned long now = millis();
  if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = now;
    sendHeartbeat();
  }
}
