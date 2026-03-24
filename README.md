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


### Cloudflare Tunnel Configuration:

- **Public Hostname:** `esppostgrest.tbn.com`
- **Service URL:** `http://<UBUNTU_LOCAL_IP>:3166`

> Note: If database tables are altered, the postgrest container must be restarted to clear its schema cache and prevent 404 errors.

# The Universal Database Schema

The database uses a one-to-many relationship. The `boards` table registers the microcontrollers, while the `actuators` table holds the target states as strings to accommodate any data type (boolean, integer, PWM).

# The ESP32-S3 Master Firmware
Found in the repo

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
