# Universal ESP32 IoT Architecture

A closed-loop, hardware-agnostic telemetry system utilizing ESP32-S3, PostgREST, and Cloudflare Tunnels for real-time monitoring and actuation.

# System Overview & Architecture

This project utilizes a modular, "no-code-database" architecture. The ESP32 acts merely as a translator between physical hardware and a central Postgres database.

## The Tech Stack:

- Edge Hardware: ESP32-S3 (programmed in C++).
- Networking: Cloudflare Zero Trust Tunnels (providing secure HTTPS without opening local router ports).
- API Layer: PostgREST (auto-generates RESTful APIs directly from the Postgres schema).
- Database: PostgreSQL (managed via Adminer GUI).
- Containerization: Docker &amp; Portainer running on a local Ubuntu server.

## Data Flow:

1. **Downlink (Actuation):** The ESP32 polls the /actuators API via Cloudflare. It reads the target\_state string, translates it into a physical action (e.g., toggling a NeoPixel or Relay), and executes a POST request to /action\_history to provide a closed-loop audit trail.
2. **Uplink (Telemetry):** The ESP32 monitors analog/digital inputs. If a reading fluctuates beyond a predefined threshold, it executes a POST request to /sensor\_history.

# Server Setup (Docker & Portainer)

The backend is deployed as a single Docker Compose stack.

docker-compose.yml:

```yaml
version: '3'
services:
  db:
    image: postgres:15-alpine
    restart: always
    environment:
      POSTGRES_USER: adibGhannam
      POSTGRES_PASSWORD: supersecret123
      POSTGRES_DB: esp32_data
    ports:
      - "5432:5432"
    volumes:
      - pgdata:/var/lib/postgresql/data

  postgrest:
    image: postgrest/postgrest:latest
    restart: always
    ports:
      - "3166:3000"
    environment:
      PGRST_DB_URI: postgres://adib:supersecret123@db:5432/esp32_data
      PGRST_DB_SCHEMA: public
      PGRST_DB_ANON_ROLE: adib 
    depends_on:
      - db

  adminer:
    image: adminer:latest
    restart: always
    ports:
      - "3165:8080"
    environment:
      ADMINER_DEFAULT_SERVER: db
    depends_on:
      - db

volumes:
  pgdata:

```

### Cloudflare Tunnel Configuration:

- **Public Hostname:** `esppostgrest.tbn.com`
- **Service URL:** `http://<UBUNTU_LOCAL_IP>:3166`

> Note: If database tables are altered, the postgrest container must be restarted to clear its schema cache and prevent 404 errors.

# The Universal Database Schema

The database uses a one-to-many relationship. The `boards` table registers the microcontrollers, while the `actuators` table holds the target states as strings to accommodate any data type (boolean, integer, PWM).

**Initialization SQL:**

```sql
-- 1. Register Edge Devices
CREATE TABLE boards (
  id VARCHAR(50) PRIMARY KEY,
  location_desc VARCHAR(100)
);

-- 2. Register Actuators (Outputs)
CREATE TABLE actuators (
  id SERIAL PRIMARY KEY,
  board_id VARCHAR(50) REFERENCES boards(id),
  component_name VARCHAR(50),  
  component_type VARCHAR(20),  
  target_state VARCHAR(50)     
);

-- 3. Actuation Audit Log
CREATE TABLE action_history (
  id SERIAL PRIMARY KEY,
  board_id VARCHAR(50),
  component_name VARCHAR(50),
  state_applied VARCHAR(50),
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- 4. Sensor Audit Log
CREATE TABLE sensor_history (
  id SERIAL PRIMARY KEY,
  board_id VARCHAR(50),
  sensor_name VARCHAR(50),
  reading NUMERIC,             
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

```

# The ESP32-S3 Master Firmware

```esp32
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <vector>

// ====================================================================
// ⚙️ 1. NETWORK & API CONFIGURATION
// ====================================================================
const char* WIFI_SSID     = "SSID";
const char* WIFI_PASSWORD = "PASSWORD";

const String BASE_URL     = "https://esppostgrest.tbn.com";
const String BOARD_ID     = "esp32_core_1";

// ====================================================================
// ⚙️ 2. HARDWARE REGISTRY (Define your pins here)
// ====================================================================

// --- Built-in NeoPixel Setup ---
#define NEOPIXEL_PIN 48
#define NUMPIXELS 1
Adafruit_NeoPixel rgbLed(NUMPIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// --- Universal Actuators (Outputs) ---
struct Actuator {
  String name;           // Must match 'component_name' in DB
  String type;           // "digital", "pwm", or "neopixel"
  int pin;
  String currentState;   // We store state as a string to handle anything ("true", "255", "90")
};

std::vector<Actuator> actuators = {
  {"builtinLED", "neopixel", NEOPIXEL_PIN, "false"}, // The built-in S3 RGB LED
  {"pump_relay", "digital", 4, "false"}            // A standard relay on Pin 4
};

// --- Universal Sensors (Inputs) ---
struct Sensor {
  String name;           // Must match 'sensor_name' in DB
  int pin;
  int threshold;         // Minimum change required before saving to DB
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
  secureClient.setInsecure(); // Required for Cloudflare Tunnels
}

// ====================================================================
// 🔄 MAIN LOOP
// ====================================================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectToWiFi(); // Reconnect if connection drops
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
                if (targetState == "true") rgbLed.setPixelColor(0, rgbLed.Color(0, 255, 0)); // Green
                else rgbLed.clear(); // Off
                rgbLed.show();
              } 
              else if (actuators[i].type == "digital") {
                digitalWrite(actuators[i].pin, targetState == "true" ? HIGH : LOW);
              }

              // --- Memory & Audit Logging ---
              actuators[i].currentState = targetState; // Update short-term memory
              
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

  delay(1000); // Wait 1 second before polling the database again
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

```

### Key Notes to include below the code in BookStack:

- **Dependencies:** Requires `ArduinoJson` and `Adafruit_NeoPixel`.
- **ESP32-S3 Quirk:** The built-in LED on most S3 boards is a WS2812 RGB NeoPixel (usually GPIO 48). Standard `digitalWrite` will not work on this specific pin; it must be handled via the Adafruit library.
- **HTTPS Handling:** Cloudflare enforces strict SSL. The `WiFiClientSecure` library is used with `.setInsecure()` to bypass strict root certificate validation, preventing the device from breaking when Cloudflare auto-rotates its certs.

# SOP - Adding New Hardware

Because the architecture is universal, adding a new hardware component (like a water pump or a temperature sensor) requires zero structural code changes.

### To add a new Output (Actuator):

1. Open Adminer (http://&lt;SERVER\_IP&gt;:3165).
2. Insert a new row into the actuators table:

- `board_id`: esp32\_core\_1
- `component_name`: water\_pump
- `component_type`: digital
- `target_state`: false

3. In the ESP32 code Configuration Zone, add the hardware definition to the actuators vector:
    
    `{"water_pump", "digital", 4, "false"}`
4. Flash the board.

### To add a new Input (Sensor):

1. In the ESP32 code Configuration Zone, add the hardware definition to the sensors vector:
    
    `{"temp_probe", 35, 5, -999}` (Where 5 is the threshold for logging a change).
2. Flash the board. The ESP32 will automatically begin logging data to the sensor\_history table whenever the value changes by 5 points.

# Frontend Integration (Flutter/Web)

To control the hardware from a user interface (like a Flutter app), perform HTTP PATCH requests directly to the PostgREST API.

**Example: Turning on the NeoPixel via API**

- Endpoint: `PATCH https://esppostgrest.tbn.com/actuators?board_id=eq.esp32_core_1&component_name=eq.main_led`
- Headers: `Content-Type: application/json`
- Body:

```json
{
  "target_state": "true"
}

```

Once this PATCH request hits the database, the ESP32 will pick up the change on its next polling cycle **(within 1 second)** and actuate the physical hardware.
