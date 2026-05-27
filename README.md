# Smart Bag IoT System

Full-stack IoT project for a smart suitcase with real-time tracking and tamper detection. Two ESP32s (Bag A and Bag B) communicate sensor data over MQTT/ESP-NOW to a Node.js backend with a live web dashboard.

## Architecture
```
Bag A (ESP32)  ──MQTT──►  HiveMQ Cloud  ──►  Node.js Backend  ──►  Dashboard
      │                                              │
      └──ESP-NOW──►  Bag B (ESP32)  ──relay──►  MQTT
```

## Bag A — Wiring
| Component | ESP32 Pin |
|-----------|-----------|
| LCD SDA | GPIO 21 |
| LCD SCL | GPIO 22 |
| ADXL345 SDA | GPIO 21 |
| ADXL345 SCL | GPIO 22 |
| HX711 DT | GPIO 14 |
| HX711 SCK | GPIO 12 |
| GPS TX → ESP32 RX | GPIO 33 |
| GPS RX → ESP32 TX | GPIO 32 |
| Hall sensor (magnet/open detection) | GPIO 27 |
| Status LED | GPIO 19 (+220Ω) |

## Bag B
Acts as a WiFi relay for Bag A when Bag A is out of range. Receives data over ESP-NOW and forwards to MQTT.

## Sensors (Bag A)
- **GPS** — real-time location
- **ADXL345** — tilt/orientation (detects if suitcase is flat or tilted)
- **HX711 + load cell** — weight measurement
- **Hall sensor** — detects magnet → closed/open state

## Backend
Node.js + Express + PostgreSQL + MQTT. JWT-authenticated REST API with Server-Sent Events (SSE) for the dashboard.

```bash
cd backend
npm install
docker-compose up   # starts PostgreSQL + backend
```

Set environment variables: `MQTT_URL`, `MQTT_USER`, `MQTT_PASS`, `JWT_SECRET`.

## Dashboard
Static HTML/JS in `dashboard/`. Open `index.html` after logging in via `login.html`. Connects to the backend via SSE for live updates.

## Firmware Setup
1. Fill in WiFi credentials and HiveMQ details in `firmware/bag_a/bag_a.ino`
2. Flash Bag A and Bag B using Arduino IDE with ESP32 board support
3. Required libraries: `LiquidCrystal_I2C`, `Adafruit_ADXL345`, `HX711`, `TinyGPS++`, `PubSubClient`, `ArduinoJson`
