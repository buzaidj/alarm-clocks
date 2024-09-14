#include <Arduino.h>
#include <math.h>

const int pwmPin = 1;
const int pwmFrequency = 1000;
const int pwmResolution = 8;
const int sineWavePoints = 50;
const float pi = 3.14159265;

int dutyCycleArray[sineWavePoints];
const float amplitudeScale = 0.30;

const int beepDuration = 200;
const int beepInterval = 800;

const int transistorPin = 21;

void setup() {
    Serial.begin(9600);

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
}

void loop() {
    digitalWrite(transistorPin, HIGH);

    unsigned long startTime = millis();
    while (millis() - startTime < beepDuration) {
        for (int i = 0; i < sineWavePoints; i++) {
            ledcWrite(pwmPin, dutyCycleArray[i]);
            delay(10);
        }
    }

    ledcWrite(pwmPin, 0);

    digitalWrite(transistorPin, LOW);

    delay(beepInterval);
}
