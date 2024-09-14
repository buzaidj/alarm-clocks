#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <time.h>

const char* ssid = "Shrader House";
const char* password = "ilovesteak";
WebServer server(80);
const char* configFilePath = "/config.json";

void connect_to_wifi() {
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.println("Connecting to WiFi...");
    }
    Serial.println("Connected to WiFi");
}

void sync_time() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected. Skipping time synchronization.");
        return;
    }

    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to obtain time");
        return;
    }

    Serial.println("Time synchronized successfully to UTC.");
}

time_t get_current_time_utc() {
    time_t now;
    time(&now);
    return now;
}

String format_time(time_t rawtime, const char* timezone) {
    setenv("TZ", timezone, 1);
    tzset();
    struct tm timeinfo;
    localtime_r(&rawtime, &timeinfo);

    char buffer[64];
    strftime(buffer, sizeof(buffer), "%c", &timeinfo);
    return String(buffer);
}

void saveTimeZoneToJSON(const String& timezone) {
    // Open the file for writing
    File file = SPIFFS.open(configFilePath, FILE_WRITE);
    if (!file) {
        Serial.println("Failed to open config file for writing");
        return;
    }

    // Create a JSON object
    StaticJsonDocument<200> doc;
    doc["timezone"] = timezone;

    // Serialize the JSON object and write it to the file
    if (serializeJson(doc, file) == 0) {
        Serial.println("Failed to write to file");
    } else {
        Serial.println("Time zone saved to SPIFFS: " + timezone);
    }

    // Close the file
    file.close();
}

String loadTimeZoneFromJSON() {
    // Open the file for reading
    File file = SPIFFS.open(configFilePath, FILE_READ);
    if (!file) {
        Serial.println("Failed to open config file for reading");
        return "UTC0";  // Default to UTC if the file doesn't exist
    }

    // Create a JSON object
    StaticJsonDocument<200> doc;

    // Parse the JSON object
    DeserializationError error = deserializeJson(doc, file);
    if (error) {
        Serial.println("Failed to parse config file");
        file.close();
        return "UTC0";  // Default to UTC if parsing fails
    }

    // Get the timezone from the JSON object
    const char* timezone = doc["timezone"] | "UTC0";  // Default to UTC if not found

    // Close the file
    file.close();

    Serial.println("Time zone loaded from SPIFFS: " + String(timezone));
    return String(timezone);
}

void handleSetTimeZone() {
    if (server.hasArg("tz")) {
        String timezone = server.arg("tz");

        // Save the timezone to the JSON file
        saveTimeZoneToJSON(timezone);

        // Apply the timezone
        setenv("TZ", timezone.c_str(), 1);
        tzset();
        server.send(200, "text/plain", "Time zone updated to: " + timezone);
        Serial.println("Time zone updated to: " + timezone);
    } else {
        server.send(400, "text/plain", "Missing 'tz' parameter in query string.");
    }
}

void handleGetTimeZone() {
    const char* timezone = getenv("TZ");
    if (timezone) {
        server.send(200, "text/plain", String("Current timezone: ") + timezone);
    } else {
        server.send(500, "text/plain", "Failed to get current timezone.");
    }
}

void handleGetTimeUTC() {
    time_t now = get_current_time_utc();
    String timeStr = format_time(now, "UTC0");
    server.send(200, "text/plain", "Current UTC time: " + timeStr);
}

void handleGetTimeLocal() {
    const char* timezone = getenv("TZ");
    time_t now = get_current_time_utc();
    String timeStr = format_time(now, timezone ? timezone : "UTC0");
    server.send(200, "text/plain", "Current local time: " + timeStr);
}

void setup() {
    Serial.begin(9600);

    // Initialize SPIFFS
    if (!SPIFFS.begin(true)) {
        Serial.println("An error occurred while mounting SPIFFS");
        return;
    }

    connect_to_wifi();
    sync_time();

    // Load the timezone from the JSON file
    String timezone = loadTimeZoneFromJSON();

    // Set the timezone
    setenv("TZ", timezone.c_str(), 1);
    tzset();

    // Define routes for the server
    server.on("/", []() {
        server.send(200, "text/plain", "Use /getUTC to get UTC time, /getLocal to get local time, /setTZ?tz=<timezone> to change the timezone, and /getTZ to get the current timezone.");
    });
    server.on("/getUTC", handleGetTimeUTC);
    server.on("/getLocal", handleGetTimeLocal);
    server.on("/setTZ", handleSetTimeZone);
    server.on("/getTZ", handleGetTimeZone);

    // Start the server
    server.begin();
    Serial.println("HTTP server started");
}

void loop() {
    server.handleClient();

    time_t now = get_current_time_utc();
    const char* timezone = getenv("TZ");
    String localTimeStr = format_time(now, timezone ? timezone : "UTC0");
    Serial.println("The current local time is: " + localTimeStr);

    delay(1000);
}
