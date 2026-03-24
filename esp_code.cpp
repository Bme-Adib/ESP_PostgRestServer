#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <vector>

// ====================================================================
// ⚙️ 1. NETWORK & API CONFIGURATION
// ====================================================================
const char* WIFI_SSID = "Herculife5G";
const char* WIFI_PASSWORD = "zaq1ZAQ!";

const String BASE_URL = "https://esppostgrest.thebiomednest.com";
const String BOARD_ID = "esp32_core_1";

// ====================================================================
// ⚙️ 2. HARDWARE REGISTRY (Define your pins here)
// ====================================================================

// --- Built-in NeoPixel Setup ---
#define NEOPIXEL_PIN 48
#define NUMPIXELS 1
Adafruit_NeoPixel rgbLed(NUMPIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// --- Universal Actuators (Outputs) ---
struct Actuator {
  String name;  // Must match 'component_name' in DB
  String type;  // "digital", "pwm", or "neopixel"
  int pin;
  String currentState;  // We store state as a string to handle anything ("true", "255", "90")
};

std::vector<Actuator> actuators = {
  { "builtinLED", "neopixel", NEOPIXEL_PIN, "false" },  // The built-in S3 RGB LED
  { "pump_relay", "digital", 4, "false" }               // A standard relay on Pin 4
};

// --- Universal Sensors (Inputs) ---
struct Sensor {
  String name;  // Must match 'sensor_name' in DB
  int pin;
  int threshold;  // Minimum change required before saving to DB
  int lastReading;
};

// Template for setting up a sensor device
std::vector<Sensor> sensors = {
  // {"murata_ma40s4r", 34, 50, -999}  // Ultrasonic sensor on Pin 34
};

// ====================================================================
// 🌐 GLOBAL NETWORK OBJECTS
// ====================================================================
WiFiClientSecure secureClient;

// Function declarations (Logic is at the bottom of the file to keep things clean)
void connectToWiFi();
String fetchFromDatabase(String endpoint);
void postToDatabase(String endpoint, String payload);

// ====================================================================
// 🚀 MAIN SETUP
// ====================================================================
void setup() {
  Serial.begin(115200);

  // 1. Initialize NeoPixel
  rgbLed.begin();
  rgbLed.clear();
  rgbLed.show();

  // 2. Initialize Standard Output Pins
  for (int i = 0; i < actuators.size(); i++) {
    if (actuators[i].type == "digital") {
      pinMode(actuators[i].pin, OUTPUT);
      digitalWrite(actuators[i].pin, LOW);
    }
  }

  // 3. Initialize Sensor Pins
  for (int i = 0; i < sensors.size(); i++) {
    pinMode(sensors[i].pin, INPUT);
  }

  // 4. Connect to Network
  connectToWiFi();
  secureClient.setInsecure();  // Required for Cloudflare Tunnels
}

// ====================================================================
// 🔄 MAIN LOOP
// ====================================================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectToWiFi();  // Reconnect if connection drops
    return;
  }

  // ---------------------------------------------------------
  // PART A: PROCESS ACTUATORS (Cloud -> ESP32)
  // ---------------------------------------------------------
  // Fetch the target states for this specific board
  String getUrl = "/actuators?board_id=eq." + BOARD_ID;
  String jsonResponse = fetchFromDatabase(getUrl);

  if (jsonResponse != "") {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonResponse);

    if (!error) {
      // Loop through the JSON array returned by the database
      for (JsonObject dbItem : doc.as<JsonArray>()) {
        String dbName = dbItem["component_name"].as<String>();
        String targetState = dbItem["target_state"].as<String>();

        // Find the matching hardware in our ESP32 registry
        for (int i = 0; i < actuators.size(); i++) {
          if (actuators[i].name == dbName) {

            // STATE CHANGE DETECTED!
            if (actuators[i].currentState != targetState) {
              Serial.printf("\n[ACTION] %s changing to: %s\n", dbName.c_str(), targetState.c_str());

              // --- Hardware Control Logic ---

              if (actuators[i].type == "neopixel") {
                if (targetState == "false") {
                  // Turn off
                  rgbLed.clear();
                } else if (targetState == "flash_red") {
                  flashRED(i);
                } else if (targetState == "flash_green") {
                  flashGREEN(i);
                }else if (targetState == "flash_blue") {
                  flashBLUE(i);
                } else {
                  // Extract RGB and Brightness from the "(R,G,B,Brightness)" string
                  int r = 0, g = 0, b = 0, br = 0;

                  // sscanf now expects 4 items. It returns the number of successful matches.
                  int matchCount = sscanf(targetState.c_str(), "(%d,%d,%d,%d)", &r, &g, &b, &br);

                  if (matchCount == 4) {
                    // Convert your 0-100% database value into the 0-255 hardware value
                    int hardwareBrightness = map(br, 0, 100, 0, 255);

                    rgbLed.setBrightness(hardwareBrightness);
                    rgbLed.setPixelColor(0, rgbLed.Color(r, g, b));

                  } else if (matchCount == 3) {
                    // Fallback: If you accidentally only send (R,G,B), default to 100% brightness
                    rgbLed.setBrightness(255);
                    rgbLed.setPixelColor(0, rgbLed.Color(r, g, b));

                  } else {
                    Serial.println("Error: Invalid color format from database.");
                  }
                }
                rgbLed.show();  // Push the changes to the LED
              }



              else if (actuators[i].type == "digital") {
                digitalWrite(actuators[i].pin, targetState == "true" ? HIGH : LOW);
              }

              // --- Memory & Audit Logging ---
              actuators[i].currentState = targetState;  // Update short-term memory

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

    // STATE CHANGE DETECTED! (Reading fluctuated beyond threshold)
    if (abs(currentReading - sensors[i].lastReading) >= sensors[i].threshold) {
      Serial.printf("\n[SENSOR] %s detected change: %d\n", sensors[i].name.c_str(), currentReading);

      // Update short-term memory
      sensors[i].lastReading = currentReading;

      // Log to Sensor History
      String logPayload = "{\"board_id\": \"" + BOARD_ID + "\", \"sensor_name\": \"" + sensors[i].name + "\", \"reading\": " + String(currentReading) + "}";
      postToDatabase("/sensor_history", logPayload);
    }
  }

  delay(1000);  // Wait 1 second before polling the database again
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

  // Unlike GET and POST, PATCH requires the sendRequest() method
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
    rgbLed.setBrightness(255);  // Max brightness for the alert
    rgbLed.setPixelColor(0, rgbLed.Color(255, 0, 0));
    rgbLed.show();
    delay(200);
    rgbLed.clear();
    rgbLed.show();
    delay(200);
  }
  
  // Update Database using the specific actuator's name
  String patchEndpoint = "/actuators?board_id=eq." + String(BOARD_ID) + "&component_name=eq." + actuators[index].name;
  String patchPayload = "{\"target_state\": \"false\"}";
  
  // Send the PATCH request
  patchDatabase(patchEndpoint, patchPayload);
  
  // Update the ESP32's short-term memory so it knows it is now OFF
  actuators[index].currentState = "false";
}

void flashGREEN(int index) {
  for (int f = 0; f < 3; f++) {
    rgbLed.setBrightness(255);  // Max brightness for the alert
    rgbLed.setPixelColor(0, rgbLed.Color(0, 255, 0));
    rgbLed.show();
    delay(200);
    rgbLed.clear();
    rgbLed.show();
    delay(200);
  }
  
  // Update Database using the specific actuator's name
  String patchEndpoint = "/actuators?board_id=eq." + String(BOARD_ID) + "&component_name=eq." + actuators[index].name;
  String patchPayload = "{\"target_state\": \"false\"}";
  
  // Send the PATCH request
  patchDatabase(patchEndpoint, patchPayload);
  
  // Update the ESP32's short-term memory so it knows it is now OFF
  actuators[index].currentState = "false";
}

void flashBLUE(int index) {
  for (int f = 0; f < 3; f++) {
    rgbLed.setBrightness(255);  // Max brightness for the alert
    rgbLed.setPixelColor(0, rgbLed.Color(0, 0, 255));
    rgbLed.show();
    delay(200);
    rgbLed.clear();
    rgbLed.show();
    delay(200);
  }
  
  // Update Database using the specific actuator's name
  String patchEndpoint = "/actuators?board_id=eq." + String(BOARD_ID) + "&component_name=eq." + actuators[index].name;
  String patchPayload = "{\"target_state\": \"false\"}";
  
  // Send the PATCH request
  patchDatabase(patchEndpoint, patchPayload);
  
  // Update the ESP32's short-term memory so it knows it is now OFF
  actuators[index].currentState = "false";
}
