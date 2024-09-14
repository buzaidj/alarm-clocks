#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Arduino.h>
#include <math.h>

const char* ssid = "Shrader House";
const char* password = "ilovesteak";
WebServer server(80);
const char* configFilePath = "/config.json";

const int pwmPin = 1;
const int pwmFrequency = 5000;
const int pwmResolution = 8;
const int sineWavePoints = 50;
const float pi = 3.14159265;

int dutyCycleArray[sineWavePoints];
const float amplitudeScale = 0.30;

const int beepDuration = 200;
const int beepInterval = 800;

const int transistorPin = 21;

unsigned long lastTimePrint = 0;
unsigned long lastBeepTime = 0;
unsigned long beepStartTime = 0;
bool beeping = false;

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
    time(&now);
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

void saveTimeZoneToJSON(const String& timezone) {
    File file = SPIFFS.open(configFilePath, FILE_WRITE);
    if (!file) {
        Serial.println("Failed to open config file for writing");
        return;
    }

    StaticJsonDocument<200> doc;
    doc["timezone"] = timezone;

    if (serializeJson(doc, file) == 0) {
        Serial.println("Failed to write to file");
    } else {
        Serial.println("Time zone saved to SPIFFS: " + timezone);
    }

    file.close();
}

String loadTimeZoneFromJSON() {
    File file = SPIFFS.open(configFilePath, FILE_READ);
    if (!file) {
        Serial.println("Failed to open config file for reading");
        return "UTC0";
    }

    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, file);
    if (error) {
        Serial.println("Failed to parse config file");
        file.close();
        return "UTC0";
    }

    const char* timezone = doc["timezone"] | "UTC0";
    file.close();

    Serial.println("Time zone loaded from SPIFFS: " + String(timezone));
    return String(timezone);
}

void handleSetTimeZone() {
    if (server.hasArg("tz")) {
        String timezone = server.arg("tz");

        saveTimeZoneToJSON(timezone);

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

bool isTimeInitialized(time_t time) {
    return time > 315532800; // 315532800 is the Unix timestamp for 1980-01-01 00:00:00 UTC
}

void maybePrintTime() {
    if (millis() - lastTimePrint >= 1000) {
        lastTimePrint = millis();
        time_t now = getCurrentTimeUtc();
        
        // Check if the time is initialized using the new function
        if (isTimeInitialized(now)) {
            const char* timezone = getenv("TZ");
            String localTimeStr = formatTime(now, timezone ? timezone : "UTC0");
            Serial.println("The current local time is: " + localTimeStr);
        } else {
            Serial.println("Time not initialized yet.");
        }
    }
}

void maybePlayAlarm() {
    if (beeping) {
        if (millis() - beepStartTime < beepDuration) {
            static int sineIndex = 0;
            ledcWrite(pwmPin, dutyCycleArray[sineIndex]);
            sineIndex = (sineIndex + 1) % sineWavePoints;
        } else {
            ledcWrite(pwmPin, 0);
            digitalWrite(transistorPin, LOW);
            beeping = false;
            lastBeepTime = millis();
        }
    } else if (millis() - lastBeepTime >= beepInterval) {
        beeping = true;
        beepStartTime = millis();
        digitalWrite(transistorPin, HIGH);
    }
}

void setup() {
    Serial.begin(9600);

    if (!SPIFFS.begin(true)) {
        Serial.println("An error occurred while mounting SPIFFS");
        return;
    }

    pinMode(transistorPin, OUTPUT);
    digitalWrite(transistorPin, LOW);

    bool success = ledcAttach(pwmPin, pwmFrequency, pwmResolution);
    if (!success) {
        Serial.println("Failed to configure PWM!");
        while (true);
    }

    for (int i = 0; i < sineWavePoints; i++) {
        float angle = (2.0 * pi * i) / sineWavePoints;
        float sineValue = (sin(angle) + 1.0) / 2.0;
        dutyCycleArray[i] = (int)(sineValue * 255 * amplitudeScale);
    }

    connectToWifi();
    syncTime();

    String timezone = loadTimeZoneFromJSON();
    setenv("TZ", timezone.c_str(), 1);
    tzset();

    server.on("/", []() {
        server.send(200, "text/plain", "Use /getUTC to get UTC time, /getLocal to get local time, /setTZ?tz=<timezone> to change the timezone, and /getTZ to get the current timezone.");
    });
    server.on("/getUTC", handleGetTimeUTC);
    server.on("/getLocal", handleGetTimeLocal);
    server.on("/setTZ", handleSetTimeZone);
    server.on("/getTZ", handleGetTimeZone);

    server.begin();
    Serial.println("HTTP server started");
}

void loop() {
    server.handleClient();
    maybePrintTime();
    maybePlayAlarm();
}