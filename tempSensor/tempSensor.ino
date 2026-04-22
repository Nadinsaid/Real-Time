#include <Wire.h>
#include "myiot33_library.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "DHT.h"

int pinDHT22 = 5;
DHT dht(pinDHT22, DHT22);

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

unsigned long currMillis, prevOLEDMillis, prevDHTMillis;
char tmpBuffer[64];
String oledline[9];

const float tempThreshold = 80.0;
const float hysteresis = 2.0;

float temp = 0;
float humidity = 0;

bool highTempDetected = false;
const int oledLib = 1;

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);

  dht.begin();
  iot33StartOLED(oledLib);

  oledline[1] = "ECE 612 Project";
}

void loop() {
  currMillis = millis();
  if (currMillis - prevDHTMillis >= 2000) {
    prevDHTMillis = currMillis;

    temp = dht.readTemperature(true);
    humidity = dht.readHumidity();

    if (isnan(temp) || isnan(humidity)) {
      Serial.println("Failed to read from DHT22!");

      oledline[4] = "Sensor Read Error";
      oledline[5] = "Check DHT22";
      oledline[6] = "";
      oledline[7] = "";

      convDDHHMMSS(millis() / 1000, tmpBuffer);
      oledline[8] = "Up: " + String(tmpBuffer);

      displayTextOLED(oledline, oledLib);
      return;
    }
  }
  
  if (currMillis - prevOLEDMillis > 1000) {
    prevOLEDMillis = currMillis;

    if (!highTempDetected && temp >= tempThreshold) {
      highTempDetected = true;
      digitalWrite(LED_BUILTIN, HIGH);
    }
    else if (highTempDetected && temp <= (tempThreshold - hysteresis)) {
      highTempDetected = false;
      digitalWrite(LED_BUILTIN, LOW);
    }

    Serial.print("Temperature: ");
    Serial.print(temp);
    Serial.print(" °F | Humidity: ");
    Serial.print(humidity);
    Serial.print("% | Status: ");

    if (temp >= 68.0 && temp <= 72.0) Serial.println("IDEAL DATA CENTER RANGE");

    else if (highTempDetected) Serial.println("HIGH");

    else Serial.println("NORMAL");

    oledline[4] = "Humidity: " + String(humidity, 1) + "%";
    oledline[5] = "Temp: " + String(temp, 1) + " F";

    if (temp >= 68.0 && temp <= 72.0) oledline[6] = "Status: IDEAL";

    else if (highTempDetected) oledline[6] = "Status: HIGH TEMP";
    
    else oledline[6] = "Status: NORMAL";
    
    if (highTempDetected) {
      oledline[7] = "Fan: ON";
    } else {
      oledline[7] = "Fan: OFF";
    }

    convDDHHMMSS(millis() / 1000, tmpBuffer);
    oledline[8] = "Up: " + String(tmpBuffer);
    displayTextOLED(oledline, oledLib);
  }
}

