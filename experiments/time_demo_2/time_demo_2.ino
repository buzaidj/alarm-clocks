#include <WiFi.h>
#include <WebServer.h>
#include <time.h>

const char* ssid = "Shrader House";
const char* password = "ilovesteak";
WebServer server(80);  // Create a web server object that listens on port 80

void connectToWifi() {
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.println("Connecting to WiFi...");
    }
    Serial.println("Connected to WiFi");
}

void syncTime() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected. Skipping time synchronization.");
        return;
    }

    // Configure time to be stored in UTC
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to obtain time");
        return;
    }

    Serial.println("Time synchronized successfully to UTC.");
}

time_t getCurrentTimeUtc() {
    time_t now;
    time(&now);  // Get the current time in UTC
    return now;
}

String formatTime(time_t rawtime, const char* timezone) {
    setenv("TZ", timezone, 1);
    tzset();
    struct tm timeinfo;
    localtime_r(&rawtime, &timeinfo);

    char buffer[64];
    strftime(buffer, sizeof(buffer), "%c", &timeinfo);
    return String(buffer);
}

void handleSetTimeZone() {
    if (server.hasArg("tz")) {
        String timezone = server.arg("tz");

        // Store the timezone in NVS
        preferences.begin("config", false);  // Open the "config" namespace in NVS
        preferences.putString("timezone", timezone);
        preferences.end();  // Close NVS

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
    time_t now = getCurrentTimeUtc();
    String timeStr = formatTime(now, "UTC0");
    server.send(200, "text/plain", "Current UTC time: " + timeStr);
}

void handleGetTimeLocal() {
    const char* timezone = getenv("TZ");
    time_t now = getCurrentTimeUtc();
    String timeStr = formatTime(now, timezone ? timezone : "UTC0");
    server.send(200, "text/plain", "Current local time: " + timeStr);
}

void setup() {
    Serial.begin(9600);
    delay(1000);

    connectToWifi();
    syncTime();

    Serial.println("ESP32 IP address: ");
    Serial.println(WiFi.localIP());

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
    server.handleClient();  // Handle incoming client requests

    // Example of logging the current time
    time_t now = getCurrentTimeUtc();
    const char* timezone = getenv("TZ");
    String localTimeStr = formatTime(now, timezone ? timezone : "UTC0");
    Serial.println("The current local time is: " + localTimeStr);

    delay(1000);  // Delay for demonstration purposes
}
