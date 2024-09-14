#include <WiFi.h>
#include <time.h>

const char* ssid = "Shrader House";
const char* password = "ilovesteak";

void WifiEvent(WifiEvent_t event) {
    switch (event) {
        case SYSTEM_EVENT_STA_DISCONNECTED:
            WiFi.begin(ssid, password);
            break;
    }
}

void connect_to_wifi() {
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.println("Connecting to WiFi...");
    }
    Serial.println("Connected to WiFi");

    WiFi.onEvent();
}

void sync_time() {
    configTime(-8 * 3600, 3600, "pool.ntp.org", "time.nist.gov");

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to obtain time");
        return;
    }

    setenv("TZ", "PST8PDT", 1);
    tzset();
}

time_t get_current_time() {
    time_t now;
    time(&now);
    return now;
}

void setup() {
    Serial.begin(9600);

    connect_to_wifi();
    sync_time();
}

void loop() {
    time_t now;
    char strftime_buf[64];
    struct tm timeinfo;

    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    Serial.println("The current date/time in Pacific Time is: " + String(strftime_buf));

    delay(1000);
}
