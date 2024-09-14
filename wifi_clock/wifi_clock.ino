#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Arduino.h>
#include <math.h>
#include "driver/timer.h"
#include <vector>
#include "secrets.h"

#define FSR_PIN 0
#define BED_OCCUPIED_CUTOFF 900

struct Alarm {
    int id;
    std::vector<int> daysOfWeek0IsSunday;  // Array of days when the alarm should go off
    // TODO: add a one time functionality - maybe let api specify specific day since epoch for this one
    int hour;
    int minute;
    bool isEnabled;
};

std::vector<Alarm> alarms;
String timezone = "UTC0";

WebServer server(80);
const char* configFilePath = "/config.json";

const int pwmPin = 1;
const int sineWavePoints = 50;
const float pi = 3.14159265;

int dutyCycleArray[sineWavePoints];
float amplitudeScale = 0.3; // default to 0.3 - can be modified by route

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
    unsigned long startAttemptTime = millis();
    
    // Attempt to connect to Wi-Fi for 10 seconds
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
        delay(1000);
        Serial.println("Connecting to WiFi...");
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Failed to connect to WiFi. Resetting...");
        ESP.restart();  // Reset the ESP32
    } else {
        Serial.println("Connected to WiFi");
    }
}

unsigned long lastCheckedWifiStatus = 0;

void maybeReconnectToWifi() {
    // Check Wi-Fi status every 10 seconds
    // TODO: don't reconnect if alarm is going off
    if (millis() - lastCheckedWifiStatus >= 10000) {
        lastCheckedWifiStatus = millis();
        
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi connection lost. Attempting to reconnect...");
            WiFi.disconnect();  // Ensure previous connection is properly closed
            WiFi.begin(ssid, password);

            unsigned long startAttemptTime = millis();

            // Attempt to reconnect to Wi-Fi for 10 seconds
            while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
                delay(1000);
                Serial.println("Reconnecting to WiFi...");
            }

            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("Failed to reconnect to WiFi.");
            } else {
                Serial.println("Reconnected to WiFi successfully.");
            }
        }
    }
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
    strftime(buffer, sizeof(buffer), "%I:%M%p %b %d", &timeinfo);
    return String(buffer);
}

void saveSettingsToJSON(const String &timezone, const std::vector<Alarm> &alarms, const float &amplitude) {
    StaticJsonDocument<2000> doc;

    // Save timezone
    doc["timezone"] = timezone;
    doc["amplitude"] = amplitude;

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


void loadSettingsFromJSON(String &timezone, std::vector<Alarm> &alarms, float &amplitude) {
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
    amplitude = doc["amplitude"] | 0.3;

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
    timer_pause(TIMER_GROUP_0, TIMER_0);

    if (server.hasArg("tz")) {
        String timezone = server.arg("tz");

        // Save the timezone and alarms to the JSON file
        saveSettingsToJSON(timezone, alarms, amplitudeScale);

        setenv("TZ", timezone.c_str(), 1);
        tzset();
        server.send(200, "text/plain", "Time zone updated to: " + timezone);
        Serial.println("Time zone updated to: " + timezone);
    } else {
        server.send(400, "text/plain", "Missing 'tz' parameter in query string.");
    }

    timer_start(TIMER_GROUP_0, TIMER_0);
}

void handleGetTimeZone() {
    timer_pause(TIMER_GROUP_0, TIMER_0);

    const char* timezone = getenv("TZ");
    if (timezone) {
        server.send(200, "text/plain", timezone);
    } else {
        server.send(500, "text/plain", "Failed to get current timezone.");
    }

    timer_start(TIMER_GROUP_0, TIMER_0);
}

void handleGetTimeLocal() {
    timer_pause(TIMER_GROUP_0, TIMER_0);

    const char* timezone = getenv("TZ");
    time_t now = getCurrentTimeUtc();
    String timeStr = formatTime(now, timezone ? timezone : "UTC0");
    server.send(200, "text/plain", timeStr);

    timer_start(TIMER_GROUP_0, TIMER_0);
}

bool isTimeInitialized(time_t time) {
    return time > 315532800; // 315532800 is the Unix timestamp for 1980-01-01 00:00:00 UTC
}

void maybePrintTime() {
    if (millis() - lastTimePrint >= 60000) {
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

void playAlarm() {
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

void handleUpsertAlarm() {
    timer_pause(TIMER_GROUP_0, TIMER_0);

    if (server.hasArg("plain")) {
        StaticJsonDocument<300> doc;
        DeserializationError error = deserializeJson(doc, server.arg("plain"));

        if (error) {
            server.send(400, "text/plain", "Invalid JSON");
            timer_start(TIMER_GROUP_0, TIMER_0);
            return;
        }

        int alarmId = doc["id"];
        bool isExisting = false;

        for (Alarm &alarm : alarms) {
            if (alarm.id == alarmId) {
                // Update existing alarm
                alarm.daysOfWeek0IsSunday.clear();
                JsonArray days = doc["daysOfWeek0IsSunday"];
                for (int day : days) {
                    alarm.daysOfWeek0IsSunday.push_back(day);
                }
                alarm.hour = doc["hour"];
                alarm.minute = doc["minute"];
                alarm.isEnabled = doc["isEnabled"];
                isExisting = true;
                break;
            }
        }

        if (!isExisting) {
            // Create a new alarm
            Alarm newAlarm;
            newAlarm.id = alarmId;
            JsonArray days = doc["daysOfWeek0IsSunday"];
            for (int day : days) {
                newAlarm.daysOfWeek0IsSunday.push_back(day);
            }
            newAlarm.hour = doc["hour"];
            newAlarm.minute = doc["minute"];
            newAlarm.isEnabled = doc["isEnabled"];
            alarms.push_back(newAlarm);
        }

        // Save settings to JSON file
        saveSettingsToJSON(timezone, alarms, amplitudeScale);

        server.send(200, "text/plain", "Alarm upserted successfully");
    } else {
        server.send(400, "text/plain", "No JSON body provided");
    }

    timer_start(TIMER_GROUP_0, TIMER_0);
}

void handleGetAlarms() {
    timer_pause(TIMER_GROUP_0, TIMER_0);

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

    timer_start(TIMER_GROUP_0, TIMER_0);
}

void handleDeleteAlarm() {
    timer_pause(TIMER_GROUP_0, TIMER_0);

    if (server.hasArg("id")) {
        int alarmId = server.arg("id").toInt();
        auto it = std::remove_if(alarms.begin(), alarms.end(), [alarmId](Alarm &alarm) {
            return alarm.id == alarmId;
        });

        if (it != alarms.end()) {
            alarms.erase(it, alarms.end());
            saveSettingsToJSON(timezone, alarms, amplitudeScale);
            server.send(200, "text/plain", "Alarm deleted successfully");
        } else {
            server.send(404, "text/plain", "Alarm not found");
        }
    } else {
        server.send(400, "text/plain", "Missing 'id' parameter");
    }

    timer_start(TIMER_GROUP_0, TIMER_0);
}

std::vector<int> alarmIdsToPlay;
unsigned long lastUpdateAlarmsTime = 0;

void maybeUpdateShouldPlayAlarm() {
    if (millis() - lastUpdateAlarmsTime >= 1000) {
        lastUpdateAlarmsTime = millis();

        std::vector<int> alarmIdsToPlayInner;
        time_t now = getCurrentTimeUtc();

        if (!isTimeInitialized(now)) {
            return;
        }

        struct tm timeinfo;
        localtime_r(&now, &timeinfo);

        // Define the time one hour ago
        time_t oneHourAgo = now - 3600;

        for (const Alarm &alarm : alarms) {
            if (!alarm.isEnabled) continue;

            // Check if today is a day the alarm is set for
            bool isTodayAlarmDay = std::find(alarm.daysOfWeek0IsSunday.begin(), alarm.daysOfWeek0IsSunday.end(), timeinfo.tm_wday) != alarm.daysOfWeek0IsSunday.end();
            bool isYesterdayAlarmDay = std::find(alarm.daysOfWeek0IsSunday.begin(), alarm.daysOfWeek0IsSunday.end(), (timeinfo.tm_wday + 6) % 7) != alarm.daysOfWeek0IsSunday.end();

            if (isTodayAlarmDay) {
                // Create a struct tm representing the alarm time today
                struct tm alarmTimeInfo = timeinfo;  // Start with the current time
                alarmTimeInfo.tm_hour = alarm.hour;
                alarmTimeInfo.tm_min = alarm.minute;
                alarmTimeInfo.tm_sec = 0;

                // Convert this struct tm to a time_t
                time_t alarmTimeToday = mktime(&alarmTimeInfo);

                // Check if the alarm time is within the last hour
                if (alarmTimeToday >= oneHourAgo && alarmTimeToday <= now) {
                    alarmIdsToPlayInner.push_back(alarm.id);
                    Serial.println("Alarm triggered: ID " + String(alarm.id));
                }
            }

            if (isYesterdayAlarmDay) {
                // Handle the case where the current time is just after midnight
                if (alarm.hour == 23 && timeinfo.tm_hour == 0) {
                    // Check if the alarm time was set for the last hour of the previous day
                    if (alarm.minute >= timeinfo.tm_min) {
                        alarmIdsToPlayInner.push_back(alarm.id);
                        Serial.println("Alarm triggered: ID " + String(alarm.id));
                    }
                }
            } 
        }

        alarmIdsToPlay = alarmIdsToPlayInner;  // Assign the inner vector to the global vector
    }
}

const int numReadings = 25;
int readings[numReadings];      // the readings from the FSR
int readIndex = 0;              // the index of the current reading
int total = 0;                  // the running total
int average = 0;                // the average

unsigned long lastReadFSRTime = 0;

void initFSRArray() {
    for (int i = 0; i < numReadings; i++) {
        readings[i] = 0;
    }
}

void maybeReadFSR() {
    if (millis() - lastReadFSRTime >= 200) {
        lastReadFSRTime = millis();

        // subtract the last reading from the total:
        total = total - readings[readIndex];
        // read from the sensor:
        readings[readIndex] = analogRead(FSR_PIN);
        // add the reading to the total:
        total = total + readings[readIndex];
        // advance to the next position in the array:
        readIndex = (readIndex + 1) % numReadings;

        // calculate the average:
        average = total / numReadings;

        if (readIndex == 0) {
            Serial.println("Average FSR Value: " + String(average));
        }
    }
}

void handleRoot() {
    timer_pause(TIMER_GROUP_0, TIMER_0);

    if (SPIFFS.exists("/index.html")) {
        File file = SPIFFS.open("/index.html", "r");
        server.streamFile(file, "text/html");
        file.close();
    } else {
        server.send(404, "text/plain", "File not found");
    }

    timer_start(TIMER_GROUP_0, TIMER_0);
}

void handleFileUpload() {
    timer_pause(TIMER_GROUP_0, TIMER_0);

    HTTPUpload& upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("Upload Start: %s\n", upload.filename.c_str());
        if (!upload.filename.endsWith(".html")) {
            Serial.println("Invalid file type. Only .html files are allowed.");
            server.send(400, "text/plain", "Invalid file type. Only .html files are allowed.");
            return;
        }
        File file = SPIFFS.open("/index.html", FILE_WRITE);
        if (!file) {
            Serial.println("Failed to open file for writing");
            server.send(500, "text/plain", "Failed to open file for writing");
            return;
        }
        file.close();
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        File file = SPIFFS.open("/index.html", FILE_APPEND);
        if (file) {
            file.write(upload.buf, upload.currentSize);
            file.close();
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        Serial.printf("Upload End: %s, Size: %u\n", upload.filename.c_str(), upload.totalSize);
        server.send(200, "text/plain", "File uploaded successfully");
    } else {
        server.send(500, "text/plain", "File upload failed");
    }

    timer_start(TIMER_GROUP_0, TIMER_0);
}

void handleSetLoudness() {
    timer_pause(TIMER_GROUP_0, TIMER_0);

    if (server.hasArg("plain")) {
        StaticJsonDocument<200> doc;
        DeserializationError error = deserializeJson(doc, server.arg("plain"));

        if (error) {
            server.send(400, "text/plain", "Invalid JSON");
            timer_start(TIMER_GROUP_0, TIMER_0);
            return;
        }

        float newAmplitude = doc["amplitude"];
        if (newAmplitude < 0 || newAmplitude > 2) {
            server.send(400, "text/plain", "Amplitude must be between 0 and 2");
        } else {
            amplitudeScale = newAmplitude;

            // Recalculate dutyCycleArray based on new amplitude
            for (int i = 0; i < sineWavePoints; i++) {
                float angle = (2.0 * pi * i) / sineWavePoints;
                float sineValue = (sin(angle) + 1.0) / 2.0;
                dutyCycleArray[i] = (int)(sineValue * 255 * amplitudeScale);
            }

            // Save settings to JSON file
            saveSettingsToJSON(timezone, alarms, amplitudeScale);

            server.send(200, "text/plain", "Loudness set successfully");
        }
    } else {
        server.send(400, "text/plain", "No JSON body provided");
    }

    timer_start(TIMER_GROUP_0, TIMER_0);
}

void handleTestAlarm() {
    timer_pause(TIMER_GROUP_0, TIMER_0);

    // If an alarm is currently playing, stop it
    if (beeping) {
        analogWrite(pwmPin, 0);  // Stop the PWM output
        digitalWrite(transistorPin, LOW);  // Turn off the transistor
        beeping = false;
        lastBeepTime = millis();
        timer_pause(TIMER_GROUP_0, TIMER_0);  // Disable the timer to stop the sine wave generation
    }

    delay(100);

    // Play the alarm immediately
    beeping = true;
    beepStartTime = millis();
    digitalWrite(transistorPin, HIGH);  // Turn on the transistor
    timer_start(TIMER_GROUP_0, TIMER_0);  // Enable the timer to start sine wave generation

    server.send(200, "text/plain", "Alarm test triggered");

    // Ensure to stop the alarm after beep duration
    delay(beepDuration);
    analogWrite(pwmPin, 0);  // Stop the PWM output
    digitalWrite(transistorPin, LOW);  // Turn off the transistor
    beeping = false;
    lastBeepTime = millis();
    timer_pause(TIMER_GROUP_0, TIMER_0);  // Disable the timer to stop the sine wave generation

    timer_start(TIMER_GROUP_0, TIMER_0);  // Resume other timers after testing
}

void handleIsBedOccupied() {
    timer_pause(TIMER_GROUP_0, TIMER_0);

    // Check if the global average exceeds the bed occupied cutoff
    if (average > BED_OCCUPIED_CUTOFF) {
        server.send(200, "text/plain", "true");
    } else {
        server.send(200, "text/plain", "false");
    }

    timer_start(TIMER_GROUP_0, TIMER_0);
}

void setup() {
    Serial.begin(9600);

    if (!SPIFFS.begin(true)) {
        Serial.println("An error occurred while mounting SPIFFS");
        return;
    }

    pinMode(transistorPin, OUTPUT);
    digitalWrite(transistorPin, LOW);

    initFSRArray();

    for (int i = 0; i < sineWavePoints; i++) {
        float angle = (2.0 * pi * i) / sineWavePoints;
        float sineValue = (sin(angle) + 1.0) / 2.0;
        dutyCycleArray[i] = (int)(sineValue * 255 * amplitudeScale);
    }

    connectToWifi();
    syncTime();

    // Load settings from JSON file on startup
    loadSettingsFromJSON(timezone, alarms, amplitudeScale);
    setenv("TZ", timezone.c_str(), 1);
    tzset();

    server.on("/", HTTP_GET, handleRoot);

    server.on("/getLocal", handleGetTimeLocal);
    server.on("/setTZ", handleSetTimeZone);
    server.on("/getTZ", handleGetTimeZone);

    // New routes
    server.on("/upsertAlarm", HTTP_POST, handleUpsertAlarm);
    server.on("/getAlarms", HTTP_GET, handleGetAlarms);
    server.on("/deleteAlarm", HTTP_POST, handleDeleteAlarm);

    server.on("/setLoudness", HTTP_POST, handleSetLoudness);
    server.on("/testAlarm", HTTP_GET, handleTestAlarm);

    server.on("/isBedOccupied", HTTP_GET, handleIsBedOccupied);

    // Upload for index.html file
    server.on("/upload", HTTP_POST, []() {
        server.send(200);  // Send OK response when starting upload
    }, handleFileUpload);  // Handle the file upload

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

void checkAndResetIfNeeded() {
    // Check if millis() has exceeded 10 days
    if (millis() > 864000000UL) {  // 864000000 milliseconds = 10 days
        Serial.println("Millis exceeded 10 days. Resetting device...");
        delay(1000);  // Optional delay to allow the message to be sent
        ESP.restart();  // Reset the device
    }
}

bool shouldPlayAlarm() {
    return alarmIdsToPlay.size() > 0 && average > BED_OCCUPIED_CUTOFF;
}

void loop() {
    checkAndResetIfNeeded();
    server.handleClient();
    maybePrintTime();
    if (!shouldPlayAlarm()) {
        maybeReconnectToWifi();
    }
    maybeReadFSR();

    maybeUpdateShouldPlayAlarm();
    if (shouldPlayAlarm()) {
        playAlarm();
    }

}
