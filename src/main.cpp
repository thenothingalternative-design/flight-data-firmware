<<<<<<< HEAD
#include <Arduino.h>
#include <Wire.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "mbedtls/md.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_Sensor.h>
#include <WiFiManager.h>
#include "config.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_MPU6050 mpu;

// Hardware and Config
const int SWITCH_PIN = 4;
const char* PILOT_ID = "PILOT-406";
const char* LOG_FILE = "/log.txt";

enum SystemState {
  STATE_CALIBRATING,
  STATE_IDLE,
  STATE_RECORDING,
  STATE_SIGN_OFF
};

SystemState currentState = STATE_CALIBRATING;

// Debouncing Logic
int lastRawReading = HIGH;
int debouncedButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_DELAY = 50;

// Calibration & Logging Timing
const int CALIBRATION_SAMPLES = 50;
int sampleCount = 0;
float accelXSum = 0.0, accelYSum = 0.0, accelZSum = 0.0;
float gyroXSum = 0.0, gyroYSum = 0.0, gyroZSum = 0.0;
float accelXOffset = 0.0, accelYOffset = 0.0, accelZOffset = 0.0;
float gyroXOffset = 0.0, gyroYOffset = 0.0, gyroZOffset = 0.0;

unsigned long lastLogTime = 0;
const unsigned long LOG_INTERVAL = 500;
int recordedEntries = 0;
String sessionHMAC = "";

unsigned long lastRetryTime = 0;
const unsigned long RETRY_INTERVAL = 10000;

String computeFileHMAC256(const char* path) {
  File file = SPIFFS.open(path, "r");
  if (!file) return "";

  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
  mbedtls_md_init(&ctx);

  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)SECRET_KEY, strlen(SECRET_KEY));

  uint8_t buffer[256];
  while (file.available()) {
    size_t bytesRead = file.read(buffer, sizeof(buffer));
    mbedtls_md_hmac_update(&ctx, buffer, bytesRead);
  }
  file.close();

  uint8_t hmac_result[32];
  mbedtls_md_hmac_finish(&ctx, hmac_result);
  mbedtls_md_free(&ctx);

  char hmacStr[65];
  for (int i = 0; i < 32; i++) {
    sprintf(hmacStr + (i * 2), "%02x", hmac_result[i]);
  }
  hmacStr[64] = '\0';

  return String(hmacStr);
}


int highestBundleNumber() {
  int highestNumber = 0;
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String fileName = file.name();
    int namePosition = fileName.indexOf("bundle_");
    if (namePosition >= 0) {
      int bundleNumber = fileName.substring(namePosition + 7).toInt();
      if (bundleNumber > highestNumber) highestNumber = bundleNumber;
    }
    file = root.openNextFile();
  }
  return highestNumber;
}

String oldestPendingBundle() {
  String oldestPath = "";
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String filePath = file.name();
    if (filePath.indexOf("bundle_") >= 0) {
      if (!filePath.startsWith("/")) filePath = "/" + filePath;
      if (oldestPath == "" || filePath < oldestPath) oldestPath = filePath;
    }
    file = root.openNextFile();
  }
  return oldestPath;
}

bool createDataBundle(String hmacSig, const String& bundlePath) {
  File logFile = SPIFFS.open(LOG_FILE, "r");
  if (!logFile) return false;

  String rawLogs = logFile.readString();
  logFile.close();

  rawLogs.trim();
  rawLogs.replace("\n", "\",\n    \"");

  File bundleFile = SPIFFS.open(bundlePath, "w");
  if (!bundleFile) return false;

  bundleFile.println("{");
  bundleFile.printf("  \"pilot_id\": \"%s\",\n", PILOT_ID);
  bundleFile.printf("  \"upload_timestamp\": \"%lu\",\n", millis());
  bundleFile.printf("  \"sha256_signature\": \"%s\",\n", hmacSig.c_str());
  bundleFile.printf("  \"total_entries\": %d,\n", recordedEntries);
  bundleFile.println("  \"telemetry_log\": [");
  bundleFile.printf("    \"%s\"\n", rawLogs.c_str());
  bundleFile.println("  ]");
  bundleFile.println("}");

  bundleFile.close();
  return true;
}

bool postBundle(const String& bundlePath) {
  File bundleFile = SPIFFS.open(bundlePath, "r");
  if (!bundleFile) return false;

  String payload = bundleFile.readString();
  bundleFile.close();

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(payload);
  http.end();

  bool success = (httpCode == 200 || httpCode == 201);
  if (success) SPIFFS.remove(bundlePath);
  return success;
}

bool uploadBundleOverWiFi(const String& bundlePath) {
    if (WiFi.status() != WL_CONNECTED) {
        WiFiManager wm;
        
        wm.setConfigPortalTimeout(180); 

        if (!wm.autoConnect("ESP32-Telemetry-Setup")) {
            Serial.println("Failed to connect or hit portal timeout");
            return false;
        }
    }

    return postBundle(bundlePath);
}

void retryPendingBundles() {
  if (millis() - lastRetryTime < RETRY_INTERVAL) return;
  lastRetryTime = millis();

  String bundlePath = oldestPendingBundle();
  if (bundlePath == "") return;

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    return;
  }

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("STATUS: [ RETRY ]");
  display.println("Sending saved data...");
  display.display();

  postBundle(bundlePath);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  pinMode(SWITCH_PIN, INPUT_PULLUP);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED Failed");
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  if (!SPIFFS.begin(true)) Serial.println("SPIFFS Failed");
  if (!mpu.begin()) Serial.println("MPU Failed");

  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  WiFi.mode(WIFI_STA);
  WiFi.begin();
}

void loop() {
  int currentRawReading = digitalRead(SWITCH_PIN);
  if (currentRawReading != lastRawReading) lastDebounceTime = millis();

  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (currentRawReading != debouncedButtonState) {
      debouncedButtonState = currentRawReading;

      if (debouncedButtonState == LOW && currentState != STATE_CALIBRATING) {
        if (currentState == STATE_IDLE) {
          File logFile = SPIFFS.open(LOG_FILE, "w");
          if (logFile) logFile.close();
          recordedEntries = 0;
          lastLogTime = millis();
          currentState = STATE_RECORDING;
        } 
        else if (currentState == STATE_RECORDING) {
          display.clearDisplay();
          display.setCursor(0, 0);
          display.println("STATUS: [ SIGNING ]");
          display.println("HMAC & Uploading...");
          display.display();

          sessionHMAC = computeFileHMAC256(LOG_FILE);

          char bundlePath[32];
          snprintf(bundlePath, sizeof(bundlePath), "/bundle_%04d.json", highestBundleNumber() + 1);
          bool saved = createDataBundle(sessionHMAC, bundlePath);

          bool success = saved && uploadBundleOverWiFi(bundlePath);
          
          display.clearDisplay();
          display.setCursor(0, 0);
          if (success) {
            display.println("UPLOAD SUCCESS!");
            display.println("HMAC Verified.");
          } else if (saved) {
            display.println("UPLOAD FAILED!");
            display.println("Saved. Will retry.");
          } else {
            display.println("SAVE FAILED!");
          }
          display.display();
          delay(2000);

          currentState = STATE_SIGN_OFF;
        } 
        else if (currentState == STATE_SIGN_OFF) {
          currentState = STATE_IDLE;
        }
      }
    }
  }
  lastRawReading = currentRawReading;

  if (currentState == STATE_IDLE || currentState == STATE_SIGN_OFF) {
    retryPendingBundles();
  }

  sensors_event_t accel, gyro, temp;
  mpu.getEvent(&accel, &gyro, &temp);

  display.clearDisplay();
  display.setCursor(0, 0);

  switch (currentState) {
    case STATE_CALIBRATING: {
      accelXSum += accel.acceleration.x;
      accelYSum += accel.acceleration.y;
      accelZSum += accel.acceleration.z;
      gyroXSum += gyro.gyro.x;
      gyroYSum += gyro.gyro.y;
      gyroZSum += gyro.gyro.z;
      sampleCount++;
      if (sampleCount >= CALIBRATION_SAMPLES) {
        accelXOffset = accelXSum / CALIBRATION_SAMPLES;
        accelYOffset = accelYSum / CALIBRATION_SAMPLES;
        accelZOffset = (accelZSum / CALIBRATION_SAMPLES) - 9.81;
        gyroXOffset = gyroXSum / CALIBRATION_SAMPLES;
        gyroYOffset = gyroYSum / CALIBRATION_SAMPLES;
        gyroZOffset = gyroZSum / CALIBRATION_SAMPLES;
        currentState = STATE_IDLE;
      }
      delay(30);
      break;
    }

    case STATE_IDLE: {
      display.println("STATUS: [ IDLE ]");
      display.println("--------------------");
      display.println("Press Button to");
      display.println("START recording.");
      break;
    }

    case STATE_RECORDING: {
      float calAccelX = accel.acceleration.x - accelXOffset;
      float calAccelY = accel.acceleration.y - accelYOffset;
      float calAccelZ = accel.acceleration.z - accelZOffset;
      float calGyroX = gyro.gyro.x - gyroXOffset;
      float calGyroY = gyro.gyro.y - gyroYOffset;
      float calGyroZ = gyro.gyro.z - gyroZOffset;
      unsigned long now = millis();

      if (now - lastLogTime >= LOG_INTERVAL) {
        lastLogTime = now;
        recordedEntries++;
        File logFile = SPIFFS.open(LOG_FILE, "a");
        if (logFile) {
          logFile.printf("%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f\n",
                         now, calAccelX, calAccelY, calAccelZ,
                         calGyroX, calGyroY, calGyroZ, temp.temperature);
          logFile.close();
        }
      }

      display.println("STATUS: [ RECORDING ]");
      display.println("--------------------");
      display.print("Entries: ");
      display.println(recordedEntries);
      display.print("Accel Z: ");
      display.print(calAccelZ, 2);
      display.println(" m/s^2");
      break;
    }

    case STATE_SIGN_OFF: {
      display.println("STATUS: [ SIGN OFF ]");
      display.println("--------------------");
      display.print("Entries: ");
      display.println(recordedEntries);
      display.println("HMAC Signed!");
      display.println("Press button to Reset.");
      break;
    }
  }

  display.display();
  delay(20);
=======
#include <Arduino.h>
#include <Wire.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "mbedtls/md.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_Sensor.h>
#include <WiFiManager.h>
#include "config.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_MPU6050 mpu;

// Hardware and Config
const int SWITCH_PIN = 4;
const char* PILOT_ID = "PILOT-406";
const char* LOG_FILE = "/log.txt";

enum SystemState {
  STATE_CALIBRATING,
  STATE_IDLE,
  STATE_RECORDING,
  STATE_SIGN_OFF
};

SystemState currentState = STATE_CALIBRATING;

// Debouncing Logic
int lastRawReading = HIGH;
int debouncedButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_DELAY = 50;

// Calibration & Logging Timing
const int CALIBRATION_SAMPLES = 50;
int sampleCount = 0;
float accelXSum = 0.0, accelYSum = 0.0, accelZSum = 0.0;
float gyroXSum = 0.0, gyroYSum = 0.0, gyroZSum = 0.0;
float accelXOffset = 0.0, accelYOffset = 0.0, accelZOffset = 0.0;
float gyroXOffset = 0.0, gyroYOffset = 0.0, gyroZOffset = 0.0;

unsigned long lastLogTime = 0;
const unsigned long LOG_INTERVAL = 500;
int recordedEntries = 0;
String sessionHMAC = "";

unsigned long lastRetryTime = 0;
const unsigned long RETRY_INTERVAL = 10000;

String computeFileHMAC256(const char* path) {
  File file = SPIFFS.open(path, "r");
  if (!file) return "";

  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
  mbedtls_md_init(&ctx);

  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)SECRET_KEY, strlen(SECRET_KEY));

  uint8_t buffer[256];
  while (file.available()) {
    size_t bytesRead = file.read(buffer, sizeof(buffer));
    mbedtls_md_hmac_update(&ctx, buffer, bytesRead);
  }
  file.close();

  uint8_t hmac_result[32];
  mbedtls_md_hmac_finish(&ctx, hmac_result);
  mbedtls_md_free(&ctx);

  char hmacStr[65];
  for (int i = 0; i < 32; i++) {
    sprintf(hmacStr + (i * 2), "%02x", hmac_result[i]);
  }
  hmacStr[64] = '\0';

  return String(hmacStr);
}


int highestBundleNumber() {
  int highestNumber = 0;
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String fileName = file.name();
    int namePosition = fileName.indexOf("bundle_");
    if (namePosition >= 0) {
      int bundleNumber = fileName.substring(namePosition + 7).toInt();
      if (bundleNumber > highestNumber) highestNumber = bundleNumber;
    }
    file = root.openNextFile();
  }
  return highestNumber;
}

String oldestPendingBundle() {
  String oldestPath = "";
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String filePath = file.name();
    if (filePath.indexOf("bundle_") >= 0) {
      if (!filePath.startsWith("/")) filePath = "/" + filePath;
      if (oldestPath == "" || filePath < oldestPath) oldestPath = filePath;
    }
    file = root.openNextFile();
  }
  return oldestPath;
}

bool createDataBundle(String hmacSig, const String& bundlePath) {
  File logFile = SPIFFS.open(LOG_FILE, "r");
  if (!logFile) return false;

  String rawLogs = logFile.readString();
  logFile.close();

  rawLogs.trim();
  rawLogs.replace("\n", "\",\n    \"");

  File bundleFile = SPIFFS.open(bundlePath, "w");
  if (!bundleFile) return false;

  bundleFile.println("{");
  bundleFile.printf("  \"pilot_id\": \"%s\",\n", PILOT_ID);
  bundleFile.printf("  \"upload_timestamp\": \"%lu\",\n", millis());
  bundleFile.printf("  \"sha256_signature\": \"%s\",\n", hmacSig.c_str());
  bundleFile.printf("  \"total_entries\": %d,\n", recordedEntries);
  bundleFile.println("  \"telemetry_log\": [");
  bundleFile.printf("    \"%s\"\n", rawLogs.c_str());
  bundleFile.println("  ]");
  bundleFile.println("}");

  bundleFile.close();
  return true;
}

bool postBundle(const String& bundlePath) {
  File bundleFile = SPIFFS.open(bundlePath, "r");
  if (!bundleFile) return false;

  String payload = bundleFile.readString();
  bundleFile.close();

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(payload);
  http.end();

  bool success = (httpCode == 200 || httpCode == 201);
  if (success) SPIFFS.remove(bundlePath);
  return success;
}

bool uploadBundleOverWiFi(const String& bundlePath) {
    if (WiFi.status() != WL_CONNECTED) {
        WiFiManager wm;
        
        wm.setConfigPortalTimeout(180); 

        if (!wm.autoConnect("ESP32-Telemetry-Setup")) {
            Serial.println("Failed to connect or hit portal timeout");
            return false;
        }
    }

    return postBundle(bundlePath);
}

void retryPendingBundles() {
  if (millis() - lastRetryTime < RETRY_INTERVAL) return;
  lastRetryTime = millis();

  String bundlePath = oldestPendingBundle();
  if (bundlePath == "") return;

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    return;
  }

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("STATUS: [ RETRY ]");
  display.println("Sending saved data...");
  display.display();

  postBundle(bundlePath);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  pinMode(SWITCH_PIN, INPUT_PULLUP);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED Failed");
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  if (!SPIFFS.begin(true)) Serial.println("SPIFFS Failed");
  if (!mpu.begin()) Serial.println("MPU Failed");

  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  WiFi.mode(WIFI_STA);
  WiFi.begin();
}

void loop() {
  int currentRawReading = digitalRead(SWITCH_PIN);
  if (currentRawReading != lastRawReading) lastDebounceTime = millis();

  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (currentRawReading != debouncedButtonState) {
      debouncedButtonState = currentRawReading;

      if (debouncedButtonState == LOW && currentState != STATE_CALIBRATING) {
        if (currentState == STATE_IDLE) {
          File logFile = SPIFFS.open(LOG_FILE, "w");
          if (logFile) logFile.close();
          recordedEntries = 0;
          lastLogTime = millis();
          currentState = STATE_RECORDING;
        } 
        else if (currentState == STATE_RECORDING) {
          display.clearDisplay();
          display.setCursor(0, 0);
          display.println("STATUS: [ SIGNING ]");
          display.println("HMAC & Uploading...");
          display.display();

          sessionHMAC = computeFileHMAC256(LOG_FILE);

          char bundlePath[32];
          snprintf(bundlePath, sizeof(bundlePath), "/bundle_%04d.json", highestBundleNumber() + 1);
          bool saved = createDataBundle(sessionHMAC, bundlePath);

          bool success = saved && uploadBundleOverWiFi(bundlePath);
          
          display.clearDisplay();
          display.setCursor(0, 0);
          if (success) {
            display.println("UPLOAD SUCCESS!");
            display.println("HMAC Verified.");
          } else if (saved) {
            display.println("UPLOAD FAILED!");
            display.println("Saved. Will retry.");
          } else {
            display.println("SAVE FAILED!");
          }
          display.display();
          delay(2000);

          currentState = STATE_SIGN_OFF;
        } 
        else if (currentState == STATE_SIGN_OFF) {
          currentState = STATE_IDLE;
        }
      }
    }
  }
  lastRawReading = currentRawReading;

  if (currentState == STATE_IDLE || currentState == STATE_SIGN_OFF) {
    retryPendingBundles();
  }

  sensors_event_t accel, gyro, temp;
  mpu.getEvent(&accel, &gyro, &temp);

  display.clearDisplay();
  display.setCursor(0, 0);

  switch (currentState) {
    case STATE_CALIBRATING: {
      accelXSum += accel.acceleration.x;
      accelYSum += accel.acceleration.y;
      accelZSum += accel.acceleration.z;
      gyroXSum += gyro.gyro.x;
      gyroYSum += gyro.gyro.y;
      gyroZSum += gyro.gyro.z;
      sampleCount++;
      if (sampleCount >= CALIBRATION_SAMPLES) {
        accelXOffset = accelXSum / CALIBRATION_SAMPLES;
        accelYOffset = accelYSum / CALIBRATION_SAMPLES;
        accelZOffset = (accelZSum / CALIBRATION_SAMPLES) - 9.81;
        gyroXOffset = gyroXSum / CALIBRATION_SAMPLES;
        gyroYOffset = gyroYSum / CALIBRATION_SAMPLES;
        gyroZOffset = gyroZSum / CALIBRATION_SAMPLES;
        currentState = STATE_IDLE;
      }
      delay(30);
      break;
    }

    case STATE_IDLE: {
      display.println("STATUS: [ IDLE ]");
      display.println("--------------------");
      display.println("Press Button to");
      display.println("START recording.");
      break;
    }

    case STATE_RECORDING: {
      float calAccelX = accel.acceleration.x - accelXOffset;
      float calAccelY = accel.acceleration.y - accelYOffset;
      float calAccelZ = accel.acceleration.z - accelZOffset;
      float calGyroX = gyro.gyro.x - gyroXOffset;
      float calGyroY = gyro.gyro.y - gyroYOffset;
      float calGyroZ = gyro.gyro.z - gyroZOffset;
      unsigned long now = millis();

      if (now - lastLogTime >= LOG_INTERVAL) {
        lastLogTime = now;
        recordedEntries++;
        File logFile = SPIFFS.open(LOG_FILE, "a");
        if (logFile) {
          logFile.printf("%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f\n",
                         now, calAccelX, calAccelY, calAccelZ,
                         calGyroX, calGyroY, calGyroZ, temp.temperature);
          logFile.close();
        }
      }

      display.println("STATUS: [ RECORDING ]");
      display.println("--------------------");
      display.print("Entries: ");
      display.println(recordedEntries);
      display.print("Accel Z: ");
      display.print(calAccelZ, 2);
      display.println(" m/s^2");
      break;
    }

    case STATE_SIGN_OFF: {
      display.println("STATUS: [ SIGN OFF ]");
      display.println("--------------------");
      display.print("Entries: ");
      display.println(recordedEntries);
      display.println("HMAC Signed!");
      display.println("Press button to Reset.");
      break;
    }
  }

  display.display();
  delay(20);
>>>>>>> 908c8667afd8189dfca830f1594c16474dba4785
}