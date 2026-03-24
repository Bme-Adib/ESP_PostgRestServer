#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <vector>

// OTA Libraries
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

// ====================================================================
// ⚙️ 1. NETWORK & API CONFIGURATION
// ====================================================================
const char* WIFI_SSID = "SSID";
const char* WIFI_PASSWORD = "PASSWORD";

const String BASE_URL = "https://esppostgrest.tbn.com";
const String BOARD_ID = "esp32_core_1";

// ====================================================================
// ⚙️ 2. HARDWARE REGISTRY
// ====================================================================

// --- Built-in NeoPixel Setup ---
#define NEOPIXEL_PIN 48
#define NUMPIXELS 1
Adafruit_NeoPixel rgbLed(NUMPIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// --- Universal Actuators (Outputs) ---
struct Actuator {
  String name;  
  String type;  
  int pin;
  String currentState; 
};

std::vector<Actuator> actuators = {
  { "builtinLED", "neopixel", NEOPIXEL_PIN, "false" },  // Index 0
  { "pump_relay", "digital", 4, "false" }               // Index 1
};

// --- Universal Sensors (Inputs) ---
struct Sensor {
  String name; 
  int pin;
  int threshold;  
  int lastReading;
};

std::vector<Sensor> sensors = {
  // {"murata_ma40s4r", 34, 50, -999}  
};

// ====================================================================
// 🌐 GLOBAL NETWORK OBJECTS & FUNCTION DECLARATIONS
// ====================================================================
WiFiClientSecure secureClient;

void connectToWiFi();
String fetchFromDatabase(String endpoint);
void postToDatabase(String endpoint, String payload);
void patchDatabase(String endpoint, String payload);
void flashRED(int index);
void flashGREEN(int index);
void flashBLUE(int index);

// ====================================================================
// 🚀 MAIN SETUP  
// ====================================================================
void setup() {
  Serial.begin(115200);

  // 1. Initialize Hardware
  rgbLed.begin();
  rgbLed.clear();
  rgbLed.show();

  for (int i = 0; i < actuators.size(); i++) {
    if (actuators[i].type == "digital") {
      pinMode(actuators[i].pin, OUTPUT);
      digitalWrite(actuators[i].pin, LOW);
    }
  }

  for (int i = 0; i < sensors.size(); i++) {
    pinMode(sensors[i].pin, INPUT);
  }

  // 2. Connect to Network
  connectToWiFi();
  secureClient.setInsecure(); 

  // 3. Initialize OTA (Over-The-Air)
  ArduinoOTA.setHostname(BOARD_ID.c_str());
  ArduinoOTA.onStart([]() { Serial.println("Start OTA"); });
  ArduinoOTA.onEnd([]() { Serial.println("\nEnd OTA"); });
  ArduinoOTA.setPassword("adibGhannam");
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.begin();
}

// ====================================================================
// 🔄 MAIN LOOP
// ====================================================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectToWiFi(); 
    return;
  }

  // Keep background OTA connection alive
  ArduinoOTA.handle(); 

  // ---------------------------------------------------------
  // PART A: PROCESS ACTUATORS (Cloud -> ESP32)
  // ---------------------------------------------------------
  String getUrl = "/actuators?board_id=eq." + BOARD_ID;
  String jsonResponse = fetchFromDatabase(getUrl);

  if (jsonResponse != "") {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonResponse);

    if (!error) {
      for (JsonObject dbItem : doc.as<JsonArray>()) {
        String dbName = dbItem["component_name"].as<String>();
        String targetState = dbItem["target_state"].as<String>();

        for (int i = 0; i < actuators.size(); i++) {
          if (actuators[i].name == dbName) {

            // STATE CHANGE DETECTED
            if (actuators[i].currentState != targetState) {
              Serial.printf("\n[ACTION] %s changing to: %s\n", dbName.c_str(), targetState.c_str());

              // --- Hardware Control Logic ---
              if (actuators[i].type == "neopixel") {
                if (targetState == "false") {
                  rgbLed.clear();
                } else if (targetState == "flash_red") {
                  flashRED(i);
                } else if (targetState == "flash_green") {
                  flashGREEN(i);
                } else if (targetState == "flash_blue") {
                  flashBLUE(i);
                } else {
                  int r = 0, g = 0, b = 0, br = 0;
                  int matchCount = sscanf(targetState.c_str(), "(%d,%d,%d,%d)", &r, &g, &b, &br);

                  if (matchCount == 4) {
                    int hardwareBrightness = map(br, 0, 100, 0, 255);
                    rgbLed.setBrightness(hardwareBrightness);
                    rgbLed.setPixelColor(0, rgbLed.Color(r, g, b));
                  } else if (matchCount == 3) {
                    rgbLed.setBrightness(255);
                    rgbLed.setPixelColor(0, rgbLed.Color(r, g, b));
                  } else {
                    Serial.println("Error: Invalid color format from database.");
                  }
                }
                rgbLed.show(); 
              }
              else if (actuators[i].type == "digital") {
                digitalWrite(actuators[i].pin, targetState == "true" ? HIGH : LOW);
              }

              // --- Memory & Audit Logging ---
              actuators[i].currentState = targetState; 

              String logPayload = "{\"board_id\": \"" + BOARD_ID + "\", \"component_name\": \"" + dbName + "\", \"state_applied\": \"" + targetState + "\"}";
              postToDatabase("/action_history", logPayload);
            }
          }
        }
      }
    }
  }

  // ---------------------------------------------------------
  // PART B: PROCESS SENSORS (ESP32 -> Cloud)
  // ---------------------------------------------------------
  for (int i = 0; i < sensors.size(); i++) {
    int currentReading = analogRead(sensors[i].pin);

    if (abs(currentReading - sensors[i].lastReading) >= sensors[i].threshold) {
      Serial.printf("\n[SENSOR] %s detected change: %d\n", sensors[i].name.c_str(), currentReading);

      sensors[i].lastReading = currentReading;

      String logPayload = "{\"board_id\": \"" + BOARD_ID + "\", \"sensor_name\": \"" + sensors[i].name + "\", \"reading\": " + String(currentReading) + "}";
      postToDatabase("/sensor_history", logPayload);
    }
  }

  // Sleep for 250ms to prevent API rate limits and watchdog crashes
  delay(250); 
}

// ====================================================================
// 🛠️ HELPER FUNCTIONS (Keeps the main loop clean)
// ====================================================================

void connectToWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nSuccessfully connected to WiFi!");
}

String fetchFromDatabase(String endpoint) {
  HTTPClient http;
  http.begin(secureClient, BASE_URL + endpoint);
  http.addHeader("Cache-Control", "no-cache");

  int responseCode = http.GET();
  String payload = "";

  if (responseCode == 200) {
    payload = http.getString();
  } else if (responseCode > 0) {
    Serial.printf("[HTTP GET] Error: %d\n", responseCode);
  }

  http.end();
  return payload;
}

void postToDatabase(String endpoint, String payload) {
  HTTPClient http;
  http.begin(secureClient, BASE_URL + endpoint);
  http.addHeader("Content-Type", "application/json");

  int responseCode = http.POST(payload);

  if (responseCode == 201) {
    Serial.println("[HTTP POST] Success - Logged to Database.");
  } else {
    Serial.printf("[HTTP POST] Error: %d\n", responseCode);
  }

  http.end();
}

void patchDatabase(String endpoint, String payload) {
  HTTPClient http;
  http.begin(secureClient, BASE_URL + endpoint);
  http.addHeader("Content-Type", "application/json");

  int responseCode = http.sendRequest("PATCH", payload);

  if (responseCode >= 200 && responseCode < 300) {
    Serial.println("[HTTP PATCH] Success - Target state auto-reset.");
  } else {
    Serial.printf("[HTTP PATCH] Error: %d\n", responseCode);
  }

  http.end();
}

void flashRED(int index) {
  for (int f = 0; f < 3; f++) {
    rgbLed.setBrightness(255); 
    rgbLed.setPixelColor(0, rgbLed.Color(255, 0, 0));
    rgbLed.show();
    delay(200);
    rgbLed.clear();
    rgbLed.show();
    delay(200);
  }
  
  String patchEndpoint = "/actuators?board_id=eq." + String(BOARD_ID) + "&component_name=eq." + actuators[index].name;
  String patchPayload = "{\"target_state\": \"false\"}";
  patchDatabase(patchEndpoint, patchPayload);
  
  actuators[index].currentState = "false";
}

void flashGREEN(int index) {
  for (int f = 0; f < 3; f++) {
    rgbLed.setBrightness(255); 
    rgbLed.setPixelColor(0, rgbLed.Color(0, 255, 0));
    rgbLed.show();
    delay(200);
    rgbLed.clear();
    rgbLed.show();
    delay(200);
  }
  
  String patchEndpoint = "/actuators?board_id=eq." + String(BOARD_ID) + "&component_name=eq." + actuators[index].name;
  String patchPayload = "{\"target_state\": \"false\"}";
  patchDatabase(patchEndpoint, patchPayload);
  
  actuators[index].currentState = "false";
}

void flashBLUE(int index) {
  for (int f = 0; f < 3; f++) {
    rgbLed.setBrightness(255); 
    rgbLed.setPixelColor(0, rgbLed.Color(0, 0, 255));
    rgbLed.show();
    delay(200);
    rgbLed.clear();
    rgbLed.show();
    delay(200);
  }
  
  String patchEndpoint = "/actuators?board_id=eq." + String(BOARD_ID) + "&component_name=eq." + actuators[index].name;
  String patchPayload = "{\"target_state\": \"false\"}";
  patchDatabase(patchEndpoint, patchPayload);
  
  actuators[index].currentState = "false";
}
