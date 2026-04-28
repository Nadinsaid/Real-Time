#include <Wire.h>
#include "myiot33_library.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Keypad.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

#define PIR_PIN 2

const int oledLib = 1;

char tmpBuffer[64];
String oledline[9];

unsigned long currMillis = 0;
unsigned long prevPIRMillis = 0;
unsigned long prevOLEDMillis = 0;
unsigned long prevAlarmMillis = 0;

const unsigned long PIR_READ_INTERVAL = 100;
const unsigned long OLED_UPDATE_INTERVAL = 250;
const unsigned long ALARM_BLINK_INTERVAL = 250;
const unsigned long ALARM_GRACE_PERIOD = 30000;

int valPIR = LOW;

const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

byte rowPins[ROWS] = {12, 11, 9, 8};
byte colPins[COLS] = {7, 6, 4, 3};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

String correctPassword = "12A45#7";
String adminPassword = "9876D#A";
String enteredPassword = "";

bool passwordAccepted = false;
bool passwordRejected = false;

bool systemArmed = false;
bool alarmActive = false;
bool alarmLockout = false;
bool alarmLedState = false;

unsigned long alarmStartMillis = 0;
int wrongEntryCount = 0;

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIR_PIN, INPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  iot33StartOLED(oledLib);

  oledline[1] = "ECE 612 Project";
  oledline[2] = "Alarm System";
  oledline[3] = "Status: DISARMED";
  oledline[4] = "Enter code to ARM";
  oledline[5] = "Input:";
  oledline[6] = "Entry auto-clears";
  oledline[7] = "";
  oledline[8] = "";
  displayTextOLED(oledline, oledLib);
}

void loop() {
  currMillis = millis();

  readPIRSensor();
  checkAlarmTimeout();
  readKeypad();
  updateAlarmPlaceholder();
  updateOLED();
}

void readPIRSensor() {
  if (currMillis - prevPIRMillis >= PIR_READ_INTERVAL) {
    prevPIRMillis = currMillis;
    valPIR = digitalRead(PIR_PIN);

    if (systemArmed && !alarmActive && valPIR == HIGH) {
      alarmActive = true;
      alarmLockout = false;
      alarmStartMillis = currMillis;
      wrongEntryCount = 0;
      enteredPassword = "";
      passwordAccepted = false;
      passwordRejected = false;
    }
  }
}

void readKeypad() {
  char key = keypad.getKey();

  if (key) {
    int neededLength;

    if (alarmLockout) {
      neededLength = adminPassword.length();
    }
    else {
      neededLength = correctPassword.length();
    }

    if (enteredPassword.length() < neededLength) {
      enteredPassword += key;
      passwordAccepted = false;
      passwordRejected = false;
    }

    if (enteredPassword.length() == neededLength) {
      checkPassword();
    }
  }
}

void checkPassword() {
  if (alarmLockout) {
    if (enteredPassword == adminPassword) {
      passwordAccepted = true;
      passwordRejected = false;

      alarmActive = false;
      alarmLockout = false;
      systemArmed = false;
      wrongEntryCount = 0;
    }
    else {
      passwordAccepted = false;
      passwordRejected = true;
    }
  }
  else if (enteredPassword == correctPassword) {
    passwordAccepted = true;
    passwordRejected = false;

    if (alarmActive) {
      alarmActive = false;
      systemArmed = false;
      wrongEntryCount = 0;
    }
    else if (systemArmed) {
      systemArmed = false;
    }
    else {
      systemArmed = true;
    }

    digitalWrite(LED_BUILTIN, systemArmed ? HIGH : LOW);
  }
  else {
    passwordAccepted = false;
    passwordRejected = true;

    if (alarmActive) {
      wrongEntryCount++;

      if (wrongEntryCount >= 3) {
        alarmLockout = true;
        enteredPassword = "";
      }
    }
  }

  enteredPassword = "";
}

void checkAlarmTimeout() {
  if (alarmActive && !alarmLockout) {
    if (currMillis - alarmStartMillis >= ALARM_GRACE_PERIOD) {
      alarmLockout = true;
      enteredPassword = "";
      passwordAccepted = false;
      passwordRejected = false;
    }
  }
}

void updateAlarmPlaceholder() {
  if (alarmActive) {
    if (alarmLockout) {
      digitalWrite(LED_BUILTIN, HIGH);
    }
    else {
      if (currMillis - prevAlarmMillis >= ALARM_BLINK_INTERVAL) {
        prevAlarmMillis = currMillis;
        alarmLedState = !alarmLedState;
        digitalWrite(LED_BUILTIN, alarmLedState);
      }
    }
  }
  else {
    alarmLedState = false;
    digitalWrite(LED_BUILTIN, systemArmed ? HIGH : LOW);
  }
}

void updateOLED() {
  if (currMillis - prevOLEDMillis >= OLED_UPDATE_INTERVAL) {
    prevOLEDMillis = currMillis;

    oledline[1] = "ECE 612 Project";
    oledline[2] = "Alarm System";

    if (alarmLockout) {
      oledline[3] = "Status: LOCKOUT";
      oledline[4] = "Admin code needed";
    }
    else if (alarmActive) {
      oledline[3] = "Status: ALARM";
      unsigned long elapsed = currMillis - alarmStartMillis;
      int secondsLeft = (ALARM_GRACE_PERIOD - elapsed) / 1000;
      oledline[4] = "Time left: " + String(secondsLeft) + "s";
    }
    else if (systemArmed) {
      oledline[3] = "Status: ARMED";
      oledline[4] = "Enter code to DISARM";
    }
    else {
      oledline[3] = "Status: DISARMED";
      oledline[4] = "Enter code to ARM";
    }

    oledline[5] = "Input: ";
    for (int i = 0; i < enteredPassword.length(); i++) {
      oledline[5] += "*";
    }

    if (alarmActive) {
      oledline[6] = "Wrong tries: " + String(wrongEntryCount) + "/3";
    }
    else {
      oledline[6] = "Motion: ";
      oledline[6] += (valPIR == HIGH ? "DETECTED" : "NONE");
    }

    if (passwordAccepted) {
      oledline[7] = "Code: CORRECT";
    }
    else if (passwordRejected) {
      oledline[7] = "Code: WRONG";
    }
    else {
      oledline[7] = "Entry auto-clears";
    }

    convDDHHMMSS(millis() / 1000, tmpBuffer);
    oledline[8] = "Up: " + String(tmpBuffer);

    displayTextOLED(oledline, oledLib);
  }
}
