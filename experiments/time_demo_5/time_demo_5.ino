#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Arduino.h>
#include <math.h>
#include "driver/timer.h"
#include <vector>

struct Alarm {
    int id;
    std::vector<int> daysOfWeek0IsSunday;  // Array of days when the alarm should go off
    int hour;
    int minute;
    bool isEnabled;
};

std::vector<Alarm> alarms;
String timezone = "UTC0";

const char* ssid = "Shrader House";
const char* password = "ilovesteak";
WebServer server(80);
const char* configFilePath = "/config.json";

const int pwmPin = 1;
const int sineWavePoints = 50;
const float pi = 3.14159265;

int dutyCycleArray[sineWavePoints];
const float amplitudeScale = 0.00;

const int beepDuration = 200;
const int beepInterval = 800;

const int transistorPin = 21;

unsigned long lastTimePrint = 0;
unsigned long lastBeepTime = 0;
unsigned long beepStartTime = 0;
bool beeping = false;

volatile int sineIndex = 0;

bool IRAM_ATTR onTimer(void *arg) {
    ledcWrite(pwmPin, dutyCycleArray[sineIndex]);  // Use analogWrite for PWM output
    sineIndex = (sineIndex + 1) % sineWavePoints;
    timer_group_clr_intr_status_in_isr(TIMER_GROUP_0, TIMER_0);
    return false; // Return false as we're not yielding to higher-priority tasks
}

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

void saveSettingsToJSON(const String &timezone, const std::vector<Alarm> &alarms) {
    StaticJsonDocument<2000> doc;

    // Save timezone
    doc["timezone"] = timezone;

    // Save alarms
    JsonArray alarmsArray = doc.createNestedArray("alarms");
    for (const Alarm &alarm : alarms) {
        JsonObject alarmObj = alarmsArray.createNestedObject();
        alarmObj["id"] = alarm.id;
        alarmObj["hour"] = alarm.hour;
        alarmObj["minute"] = alarm.minute;
        alarmObj["isEnabled"] = alarm.isEnabled;

        JsonArray days = alarmObj.createNestedArray("daysOfWeek0IsSunday");
        for (int day : alarm.daysOfWeek0IsSunday) {
            days.add(day);
        }
    }

    // Write the JSON document to the file
    File file = SPIFFS.open(configFilePath, FILE_WRITE);
    if (!file) {
        Serial.println("Failed to open config file for writing");
        return;
    }

    if (serializeJson(doc, file) == 0) {
        Serial.println("Failed to write settings to file");
    } else {
        Serial.println("Settings saved to SPIFFS");
    }

    file.close();
}


void loadSettingsFromJSON(String &timezone, std::vector<Alarm> &alarms) {
    File file = SPIFFS.open(configFilePath, FILE_READ);
    if (!file) {
        Serial.println("Failed to open config file for reading");
        return;
    }

    StaticJsonDocument<2000> doc;
    DeserializationError error = deserializeJson(doc, file);
    if (error) {
        Serial.println("Failed to parse config file");
        file.close();
        return;
    }

    // Load timezone
    timezone = doc["timezone"] | "UTC0";

    // Load alarms
    alarms.clear();
    JsonArray alarmsArray = doc["alarms"];
    for (JsonObject alarmObj : alarmsArray) {
        Alarm alarm;
        alarm.id = alarmObj["id"];
        alarm.hour = alarmObj["hour"];
        alarm.minute = alarmObj["minute"];
        alarm.isEnabled = alarmObj["isEnabled"];

        JsonArray days = alarmObj["daysOfWeek0IsSunday"];
        for (int day : days) {
            alarm.daysOfWeek0IsSunday.push_back(day);
        }

        alarms.push_back(alarm);
    }

    Serial.println("Settings loaded from SPIFFS");
    file.close();
}

void handleSetTimeZone() {
    if (server.hasArg("tz")) {
        String timezone = server.arg("tz");

        // Save the timezone and alarms to the JSON file
        saveSettingsToJSON(timezone, alarms);

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
        if (millis() - beepStartTime >= beepDuration) {
            // Stop beeping after the duration is complete
            analogWrite(pwmPin, 0); // Stop the PWM output
            digitalWrite(transistorPin, LOW); // Turn off the transistor
            beeping = false;
            lastBeepTime = millis();
            timer_pause(TIMER_GROUP_0, TIMER_0); // Disable the timer to stop the sine wave generation
        }
    } else if (millis() - lastBeepTime >= beepInterval) {
        // Start a new beep
        beeping = true;
        beepStartTime = millis();
        digitalWrite(transistorPin, HIGH); // Turn on the transistor
        timer_start(TIMER_GROUP_0, TIMER_0); // Enable the timer to start sine wave generation
    }
}

void handleUpdateAlarm() {
    if (server.hasArg("plain")) {
        StaticJsonDocument<300> doc;
        DeserializationError error = deserializeJson(doc, server.arg("plain"));

        if (error) {
            server.send(400, "text/plain", "Invalid JSON");
            return;
        }

        int alarmId = doc["id"];
        bool isEnabled = doc["isEnabled"];

        for (Alarm &alarm : alarms) {
            if (alarm.id == alarmId) {
                alarm.isEnabled = isEnabled;

                // Save settings to JSON file
                saveSettingsToJSON(timezone, alarms);

                server.send(200, "text/plain", "Alarm updated successfully");
                return;
            }
        }

        server.send(404, "text/plain", "Alarm not found");
    } else {
        server.send(400, "text/plain", "No JSON body provided");
    }
}


void handleSetAlarm() {
    if (server.hasArg("plain")) {
        StaticJsonDocument<300> doc;
        DeserializationError error = deserializeJson(doc, server.arg("plain"));

        if (error) {
            server.send(400, "text/plain", "Invalid JSON");
            return;
        }

        Alarm newAlarm;
        newAlarm.id = doc["id"];
        JsonArray days = doc["daysOfWeek0IsSunday"];
        newAlarm.hour = doc["hour"];
        newAlarm.minute = doc["minute"];
        newAlarm.isEnabled = true;  // Set alarm as enabled by default

        for (int day : days) {
            newAlarm.daysOfWeek0IsSunday.push_back(day);
        }

        // Add the new alarm to the vector
        alarms.push_back(newAlarm);

        // Save settings to JSON file
        saveSettingsToJSON(timezone, alarms);

        server.send(200, "text/plain", "Alarm set successfully");
    } else {
        server.send(400, "text/plain", "No JSON body provided");
    }
}

void handleGetAlarms() {
    StaticJsonDocument<1000> doc;
    JsonArray alarmsArray = doc.createNestedArray("alarms");

    for (const Alarm &alarm : alarms) {
        JsonObject alarmObj = alarmsArray.createNestedObject();
        alarmObj["id"] = alarm.id;
        alarmObj["hour"] = alarm.hour;
        alarmObj["minute"] = alarm.minute;
        alarmObj["isEnabled"] = alarm.isEnabled;

        JsonArray days = alarmObj.createNestedArray("daysOfWeek0IsSunday");
        for (int day : alarm.daysOfWeek0IsSunday) {
            days.add(day);
        }
    }

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void setup() {
    Serial.begin(9600);

    if (!SPIFFS.begin(true)) {
        Serial.println("An error occurred while mounting SPIFFS");
        return;
    }

    pinMode(transistorPin, OUTPUT);
    digitalWrite(transistorPin, LOW);

    for (int i = 0; i < sineWavePoints; i++) {
        float angle = (2.0 * pi * i) / sineWavePoints;
        float sineValue = (sin(angle) + 1.0) / 2.0;
        dutyCycleArray[i] = (int)(sineValue * 255 * amplitudeScale);
    }

    connectToWifi();
    syncTime();

    // Load settings from JSON file on startup
    loadSettingsFromJSON(timezone, alarms);
    setenv("TZ", timezone.c_str(), 1);
    tzset();

    server.on("/", []() {
        server.send(200, "text/plain", "Use /getUTC to get UTC time, /getLocal to get local time, /setTZ?tz=<timezone> to change the timezone, and /getTZ to get the current timezone.");
    });
    server.on("/getLocal", handleGetTimeLocal);
    server.on("/setTZ", handleSetTimeZone);
    server.on("/getTZ", handleGetTimeZone);

    // New routes
    server.on("/setAlarm", HTTP_POST, handleSetAlarm);
    server.on("/getAlarms", HTTP_GET, handleGetAlarms);
    server.on("/updateAlarm", HTTP_POST, handleUpdateAlarm);

    server.begin();
    Serial.println("HTTP server started");

    // Timer setup using ESP32's native API
    timer_config_t config;
    config.divider = 80;               // 1 MHz clock (80 MHz / 80)
    config.counter_dir = TIMER_COUNT_UP;  // Counting direction
    config.counter_en = TIMER_PAUSE;      // Start the counter paused
    config.alarm_en = TIMER_ALARM_EN;     // Enable alarm
    config.auto_reload = TIMER_AUTORELOAD_EN; // Auto-reload on alarm
    config.intr_type = TIMER_INTR_LEVEL;  // Level interrupt
    config.clk_src = TIMER_SRC_CLK_APB;   // Clock source (APB clock)

    timer_init(TIMER_GROUP_0, TIMER_0, &config);
    timer_set_counter_value(TIMER_GROUP_0, TIMER_0, 0x00000000ULL);
    timer_set_alarm_value(TIMER_GROUP_0, TIMER_0, 1000); // 1000 microseconds = 1 kHz
    timer_enable_intr(TIMER_GROUP_0, TIMER_0);
    timer_isr_callback_add(TIMER_GROUP_0, TIMER_0, onTimer, NULL, ESP_INTR_FLAG_IRAM);

    // Timer is paused initially
    Serial.println("Timer setup complete");
}

void loop() {
    server.handleClient();
    maybePrintTime();
    maybePlayAlarm();
}
