# Security Fixes for Smart Bag IoT

## 🚨 Critical Issues Fixed

### 1. **Hardcoded Credentials** ✅
**Problem**: WiFi SSID/password and MQTT credentials were hardcoded in source code.

**Solution**: 
- Created `credentials.h.example` template
- Updated `bag_a.ino` to `#include "credentials.h"`
- Instructions:
  1. Copy `credentials.h.example` to `credentials.h`
  2. Fill in your actual credentials
  3. Add `credentials.h` to `.gitignore` (already done)
  4. NEVER commit `credentials.h` to git

**Status**: 🟢 FIXED

---

### 2. **Insecure TLS Configuration** ✅
**Problem**: `wifiClient.setInsecure()` disabled SSL/TLS verification, allowing MITM attacks.

**Solution**:
- Added CA certificate support in `credentials.h`
- Modified `attemptWifiConnect()` to use `wifiClient.setCACert(ca_cert)`
- Now properly verifies the MQTT broker's certificate

**How to get your CA certificate**:
```bash
# For HiveMQ:
# 1. Go to your HiveMQ console
# 2. Download the CA certificate (usually DigiCert or Let's Encrypt)
# 3. Paste it into credentials.h in the ca_cert variable
```

**Status**: 🟢 FIXED

---

### 3. **Unencrypted ESP-NOW** ✅
**Problem**: ESP-NOW packets were sent in plaintext (`encrypt = false`).

**Solution**:
- Added encryption key array: `uint8_t esp_now_key[16]`
- Modified `initEspNow()` to:
  - Enable encryption: `peer.encrypt = true`
  - Set the key: `memcpy(peer.key, esp_now_key, 16)`
  - Register PMK: `esp_now_set_pmk(esp_now_key)`

**⚠️ IMPORTANT**: Update **both Bag A and Bag B** with the same encryption key!

**Status**: 🟢 FIXED

---

### 4. **Credentials in Git History** ⚠️
**Problem**: Old commits contain your MQTT credentials and WiFi password.

**Solution** (one-time cleanup):
```bash
# Remove credentials from git history using BFG Repo-Cleaner:
git clone --mirror https://github.com/MostafaSous/smart-bag-iot.git
cd smart-bag-iot.git
bfg --replace-text passwords.txt
git reflog expire --expire=now --all
git gc --prune=now --aggressive
git push --force
```

Or use this interactive approach:
```bash
# Install git-filter-repo (recommended by GitHub)
pip install git-filter-repo
git filter-repo --invert-paths --path firmware/bag_a/bag_a.ino  # if needed
```

**Status**: 🟡 MANUAL - Please follow instructions above

---

## 📋 Setup Instructions

### Step 1: Create `credentials.h`
```bash
cp firmware/bag_a/credentials.h.example firmware/bag_a/credentials.h
```

### Step 2: Fill in your credentials
Edit `firmware/bag_a/credentials.h`:
```cpp
#define WIFI_SSID     "Your_WiFi_Network"
#define WIFI_PASSWORD "Your_WiFi_Password"
#define MQTT_HOST     "your-cluster.hivemq.cloud"
#define MQTT_USER     "your-username"
#define MQTT_PASS     "your-password"

const char* ca_cert = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDjTCCAnWgAwIBAgIQAzrx5qcRqaC7KGSxsTQKYDANBgkqhkiG9w0BAQsFADBh
... (paste full certificate)
-----END CERTIFICATE-----
)EOF";
```

### Step 3: Update Bag B (if applicable)
Make sure `firmware/bag_b/bag_b.ino` also uses:
```cpp
#include "credentials.h"
// And same encryption key for ESP-NOW
uint8_t esp_now_key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                            0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
```

### Step 4: Verify .gitignore
Check that `.gitignore` contains:
```
credentials.h
*_credentials.h
secrets/
config.h
```

---

## 🔐 Additional Security Recommendations

### 1. **Change Your Credentials Immediately**
Your exposed credentials should be rotated:
- Change your WiFi password
- Regenerate MQTT user/password in HiveMQ console
- Revoke old API keys

### 2. **Use Strong Passwords**
- Minimum 16 characters for MQTT password
- Use special characters, numbers, and mixed case
- Avoid dictionary words

### 3. **Consider Hardware Security**
- Use ESP32's encrypted partition support for storing keys
- Implement secure boot if available
- Use OTA (Over-The-Air) firmware updates over HTTPS only

### 4. **Network Security**
- Use WPA3 encryption for WiFi if available (WPA2 minimum)
- Disable WPS on your router
- Use MAC filtering if available

### 5. **MQTT Broker Security**
- Enable client authentication (username/password) ✅
- Use TLS/SSL (port 8883) ✅
- Consider IP whitelisting if your ISP supports it
- Use MQTT ACL rules to restrict what each client can access

### 6. **Regular Updates**
- Keep the PubSubClient library updated
- Monitor Arduino-ESP32 core for security updates
- Subscribe to security advisories for your dependencies

---

## 🧪 Testing

### Test WiFi + MQTT Connection
1. Upload updated `bag_a.ino` with your `credentials.h`
2. Check Serial Monitor for:
   ```
   Connecting to WiFi: Your_WiFi_Network
   WiFi connected. IP: 192.168.x.x
   [TLS] Using CA certificate for secure connection.
   Connecting to MQTT broker... connected.
   [MQTT] Published all topics.
   ```

### Test ESP-NOW Encryption
1. Upload to both Bag A and Bag B
2. Check for:
   ```
   [ESP-NOW] Ready on channel X (encrypted).
   [ESP-NOW] Packet sent to Bag B.
   [ESP-NOW] ACK received.
   ```

---

## 📚 References

- [MQTT Security Best Practices](https://mqtt.org/general/security-best-practices)
- [ESP-NOW Security](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_now.html)
- [HiveMQ TLS Setup](https://docs.hivemq.com/hivemq/latest/user-guide/security.html)
- [Git Credential Removal](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/removing-sensitive-data-from-a-repository)

---

## ❓ Questions?

If you encounter issues:
1. Check the Serial Monitor output
2. Verify all credentials are correct in `credentials.h`
3. Ensure `.gitignore` prevents accidental commits
4. Test WiFi/MQTT connection with a simple Arduino sketch first
