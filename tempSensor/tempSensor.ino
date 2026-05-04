/**************************************************************************
 Class: ECE 612
 Student Name: JoJo Thibodeaux III
 Date: 05/03/2026

 Final Project Combined Alarm System
 Description:
   Combined Nano 33 IoT sketch for PIR, LDR, DHT22, keypad, RGB LED,
   passive buzzer, fan, OLED, FreeRTOS task scheduling, WiFi, and MQTT.

   MQTT currently publishes sensor inputs/status to the Raspberry Pi.
   Receiving MQTT commands from the Raspberry Pi is included as a placeholder
   in messageReceived() and can be completed later.

 Notes:
   - RGB LED is assumed to be COMMON ANODE, so LOW turns a color ON.
   - OLED helper functions come from myiot33_library.h/.cpp.
   - Replace WIFI_SSID, WIFI_PASS, and MQTT_BROKER with your network values.
 **************************************************************************/

#include <stdio.h>
#include <Wire.h>
#include <WiFiNINA.h>
#include <MQTT.h>
#include <FreeRTOS_SAMD21.h>
#include <Keypad.h>
#include "DHT.h"
#include "myiot33_library.h"

/*************** WiFi / MQTT SETTINGS ***************/
const char gNumber[15] = "Gxxxx9820";
const char WIFI_SSID[31] = "JoJo";
const char WIFI_PASS[31] = "ymsgfd3ob6tt0";

// Use the Raspberry Pi IP address when the Pi is running the MQTT broker.
// Example: const char MQTT_BROKER[63] = "192.168.1.25";
const char MQTT_BROKER[63] = "172.20.10.7";

char topicPub[80] = "";       // Nano -> Raspberry Pi sensor/status topic
char topicSub[80] = "";       // Raspberry Pi -> Nano command topic
char mqttClientName[40] = "";
long nmrMqttMessages = 0;
String mqttStringMessage = "";
String lastPiCommand = "none";

WiFiClient wifiClient;
MQTTClient mqttClient(512);

/*************** PIN ASSIGNMENTS FROM SOURCE FILES ***************/
#define PIR_PIN 2
#define FAN_PIN 3
#define DHT_PIN 5
#define LDR_PIN A3

#define RED_PIN A1
#define GREEN_PIN A6
#define BLUE_PIN A7

// D13 was planned for the passive buzzer. It is also the built-in LED pin
// on many Arduino boards, so the built-in LED is not used as an output here.
#define BUZZER_PIN 13

// Keypad pins copied from the current combined alarm source file.
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte rowPins[ROWS] = {12, 11, 10, 9};
byte colPins[COLS] = {8, 7, 6, 4};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

/*************** THRESHOLDS / TIMING ***************/
#define LIGHT_THRES 550
const float TEMP_THRESHOLD_F = 80.0;
const float TEMP_HYSTERESIS_F = 2.0;

const unsigned long PIR_READ_INTERVAL_MS = 500;
const unsigned long LDR_READ_INTERVAL_MS = 1000;
const unsigned long DHT_READ_INTERVAL_MS = 2000;
const unsigned long OLED_UPDATE_INTERVAL_MS = 1000;
const unsigned long MQTT_PUBLISH_INTERVAL_MS = 1000;
const unsigned long TASK_C_INTERVAL_MS = 50;
const unsigned long ALARM_GRACE_PERIOD_MS = 30000;

const unsigned long PIR_BLINK_INTERVAL_MS = 250;
const unsigned long DHT_BEEP_INTERVAL_MS = 500;
const unsigned long LDR_BEEP_INTERVAL_MS = 1000;

const int BUZZER_FREQ_PIR = 2500;
const int BUZZER_FREQ_DHT = 1400;
const int BUZZER_FREQ_LDR = 650;

/*************** OLED ***************/
const int oledLib = 1;
char tmpBuffer[64];
String oledline[9];

/*************** DHT22 ***************/
DHT dht(DHT_PIN, DHT22);

/*************** SHARED SENSOR / SYSTEM STATE ***************/
volatile long pirTaskCounter = 0;
volatile long ldrTaskCounter = 0;
volatile long dhtTaskCounter = 0;
volatile long bTaskCounter = 0;
volatile long cTaskCounter = 0;

volatile int valPIR = LOW;
volatile int lightVal = 0;
volatile bool powerFailureDetected = false;

volatile float tempF = 0.0;
volatile float humidity = 0.0;
volatile bool dhtReadOk = false;
volatile bool highTempDetected = false;
volatile bool fanOn = false;

bool systemArmed = false;
bool alarmActive = false;
bool alarmLockout = false;
bool passwordAccepted = false;
bool passwordRejected = false;
bool greenConfirmActive = false;

String correctPassword = "12A45#7";
String adminPassword = "9876D#A";
String enteredPassword = "";
int wrongEntryCount = 0;
unsigned long alarmStartMillis = 0;
unsigned long greenConfirmStartMillis = 0;
const unsigned long GREEN_CONFIRM_TIME_MS = 2000;

bool blinkState = false;
unsigned long prevBlinkMillis = 0;
bool buzzerBeepState = false;
unsigned long prevBuzzerMillis = 0;

/*************** FREERTOS TASK HANDLES ***************/
TaskHandle_t Handle_pirTask;
TaskHandle_t Handle_ldrTask;
TaskHandle_t Handle_dhtTask;
TaskHandle_t Handle_bTask;
TaskHandle_t Handle_cTask;
TaskHandle_t Handle_oledTask;
TaskHandle_t Handle_monitorTask;

/*************** FREERTOS HELPERS FROM HWLEC10 STYLE ***************/
void myDelayUs(int us)
{
  vTaskDelay(us / portTICK_PERIOD_US);
}

void myDelayMs(int ms)
{
  vTaskDelay((ms * 1000) / portTICK_PERIOD_US);
}

void myDelayMsUntil(TickType_t *previousWakeTime, int ms)
{
  vTaskDelayUntil(previousWakeTime, (ms * 1000) / portTICK_PERIOD_US);
}

/*************** RGB LED HELPERS: COMMON ANODE ***************/
void rgbOff()
{
  digitalWrite(RED_PIN, HIGH);
  digitalWrite(GREEN_PIN, HIGH);
  digitalWrite(BLUE_PIN, HIGH);
}

void rgbRed()
{
  digitalWrite(RED_PIN, LOW);
  digitalWrite(GREEN_PIN, HIGH);
  digitalWrite(BLUE_PIN, HIGH);
}

void rgbGreen()
{
  digitalWrite(RED_PIN, HIGH);
  digitalWrite(GREEN_PIN, LOW);
  digitalWrite(BLUE_PIN, HIGH);
}

void rgbWhite()
{
  digitalWrite(RED_PIN, LOW);
  digitalWrite(GREEN_PIN, LOW);
  digitalWrite(BLUE_PIN, LOW);
}

/*************** ALARM / PASSWORD LOGIC ***************/
void startPIRAlarm()
{
  alarmActive = true;
  alarmLockout = false;
  alarmStartMillis = millis();
  wrongEntryCount = 0;
  enteredPassword = "";
  passwordAccepted = false;
  passwordRejected = false;
  greenConfirmActive = false;
  blinkState = false;
  buzzerBeepState = false;
  rgbOff();
  noTone(BUZZER_PIN);
}

void clearAlarmWithUserPassword()
{
  alarmActive = false;
  alarmLockout = false;
  systemArmed = false;
  wrongEntryCount = 0;
  greenConfirmActive = true;
  greenConfirmStartMillis = millis();
  noTone(BUZZER_PIN);
}

void clearAlarmWithAdminPassword()
{
  alarmActive = false;
  alarmLockout = false;
  systemArmed = false;
  wrongEntryCount = 0;
  greenConfirmActive = true;
  greenConfirmStartMillis = millis();
  noTone(BUZZER_PIN);
}

void checkPassword()
{
  if (alarmLockout) {
    if (enteredPassword == adminPassword) {
      passwordAccepted = true;
      passwordRejected = false;
      clearAlarmWithAdminPassword();
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
      clearAlarmWithUserPassword();
    }
    else {
      systemArmed = !systemArmed;
      greenConfirmActive = true;
      greenConfirmStartMillis = millis();
    }
  }
  else {
    passwordAccepted = false;
    passwordRejected = true;

    if (alarmActive) {
      wrongEntryCount++;
      if (wrongEntryCount >= 3) {
        alarmLockout = true;
      }
    }
  }

  enteredPassword = "";
}

void readKeypad()
{
  char key = keypad.getKey();

  if (key) {
    int neededLength = alarmLockout ? adminPassword.length() : correctPassword.length();

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

void checkAlarmTimeout()
{
  if (alarmActive && !alarmLockout) {
    if (millis() - alarmStartMillis >= ALARM_GRACE_PERIOD_MS) {
      alarmLockout = true;
      enteredPassword = "";
      passwordAccepted = false;
      passwordRejected = false;
    }
  }
}

/*************** OUTPUT PRIORITY LOGIC ***************/
void updateFanOutput()
{
  digitalWrite(FAN_PIN, fanOn ? HIGH : LOW);
}

void updateRGBOutput()
{
  unsigned long nowMillis = millis();

  // Green confirmation briefly shows a correct password only when no sensor
  // output has priority.
  if (greenConfirmActive && !alarmActive && !highTempDetected && !powerFailureDetected) {
    rgbGreen();
    if (nowMillis - greenConfirmStartMillis >= GREEN_CONFIRM_TIME_MS) {
      greenConfirmActive = false;
      rgbOff();
    }
    return;
  }

  // Highest priority: PIR alarm. Blink red while in grace period;
  // hold red during lockout.
  if (alarmActive) {
    if (alarmLockout) {
      rgbRed();
    }
    else {
      if (nowMillis - prevBlinkMillis >= PIR_BLINK_INTERVAL_MS) {
        prevBlinkMillis = nowMillis;
        blinkState = !blinkState;
      }
      if (blinkState) rgbRed();
      else rgbOff();
    }
    return;
  }

  // Next priority: high temperature. Solid red until temp drops below
  // threshold - hysteresis.
  if (highTempDetected) {
    rgbRed();
    return;
  }

  // Last sensor priority: LDR darkness/power failure. White until light returns.
  if (powerFailureDetected) {
    rgbWhite();
    return;
  }

  rgbOff();
}

void updateBuzzerOutput()
{
  unsigned long nowMillis = millis();

  // Highest priority: PIR.
  if (alarmActive) {
    if (alarmLockout) {
      tone(BUZZER_PIN, BUZZER_FREQ_PIR);
      return;
    }

    if (nowMillis - prevBuzzerMillis >= PIR_BLINK_INTERVAL_MS) {
      prevBuzzerMillis = nowMillis;
      buzzerBeepState = !buzzerBeepState;
    }

    if (buzzerBeepState) tone(BUZZER_PIN, BUZZER_FREQ_PIR);
    else noTone(BUZZER_PIN);
    return;
  }

  // Next priority: DHT22 high temperature.
  if (highTempDetected) {
    if (nowMillis - prevBuzzerMillis >= DHT_BEEP_INTERVAL_MS) {
      prevBuzzerMillis = nowMillis;
      buzzerBeepState = !buzzerBeepState;
    }

    if (buzzerBeepState) tone(BUZZER_PIN, BUZZER_FREQ_DHT);
    else noTone(BUZZER_PIN);
    return;
  }

  // Last sensor priority: LDR power failure.
  if (powerFailureDetected) {
    if (nowMillis - prevBuzzerMillis >= LDR_BEEP_INTERVAL_MS) {
      prevBuzzerMillis = nowMillis;
      buzzerBeepState = !buzzerBeepState;
    }

    if (buzzerBeepState) tone(BUZZER_PIN, BUZZER_FREQ_LDR);
    else noTone(BUZZER_PIN);
    return;
  }

  noTone(BUZZER_PIN);
}

/*************** MQTT HELPERS ***************/
void connectWiFi()
{
  Serial.println("connectWiFi: connecting...");
  int status = WL_IDLE_STATUS;

  while (status != WL_CONNECTED) {
    status = WiFi.begin(WIFI_SSID, WIFI_PASS);
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("connectWiFi: connected, IP = ");
  Serial.println(WiFi.localIP());
}

void connectMqtt(char *clientName)
{
  Serial.println();
  Serial.println("connectMqtt: checking WiFi...");

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print("WiFi failed. Trying SSID: ");
    Serial.println(WIFI_SSID);

    WiFi.begin(WIFI_SSID, WIFI_PASS);

    for (int i = 0; i < 5; i++) {
      Serial.print("W");
      delay(1000);
    }

    Serial.println();
  }

  Serial.println("connectMqtt: WiFi OK");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  Serial.println("connectMqtt: checking MQTT...");
  Serial.print("Broker IP: ");
  Serial.println(MQTT_BROKER);
  Serial.print("Client name: ");
  Serial.println(clientName);

  while (!mqttClient.connect(clientName)) {
    Serial.println("MQTT failed. Retrying...");
    Serial.print("Broker IP is currently set to: ");
    Serial.println(MQTT_BROKER);

    for (int i = 0; i < 5; i++) {
      Serial.print("M");
      delay(1000);
    }

    Serial.println();
  }

  Serial.println("connectMqtt: MQTT OK");

  mqttClient.subscribe(topicSub);
  Serial.print("Subscribed to: ");
  Serial.println(topicSub);
}

String boolText(bool value)
{
  return value ? "true" : "false";
}

void publishSensorSnapshot()
{
  nmrMqttMessages++;

  mqttStringMessage = "{";
  mqttStringMessage += "\"device\":\"nano33iot\",";
  mqttStringMessage += "\"gnumber\":\"" + String(gNumber) + "\",";
  mqttStringMessage += "\"messageNumber\":" + String(nmrMqttMessages) + ",";
  mqttStringMessage += "\"uptimeSeconds\":" + String(millis() / 1000) + ",";
  mqttStringMessage += "\"pir\":" + String(valPIR == HIGH ? 1 : 0) + ",";
  mqttStringMessage += "\"motionDetected\":" + boolText(valPIR == HIGH) + ",";
  mqttStringMessage += "\"ldrValue\":" + String(lightVal) + ",";
  mqttStringMessage += "\"powerFailure\":" + boolText(powerFailureDetected) + ",";
  mqttStringMessage += "\"temperatureF\":" + String(tempF, 1) + ",";
  mqttStringMessage += "\"humidity\":" + String(humidity, 1) + ",";
  mqttStringMessage += "\"dhtReadOk\":" + boolText(dhtReadOk) + ",";
  mqttStringMessage += "\"highTemp\":" + boolText(highTempDetected) + ",";
  mqttStringMessage += "\"fanOn\":" + boolText(fanOn) + ",";
  mqttStringMessage += "\"systemArmed\":" + boolText(systemArmed) + ",";
  mqttStringMessage += "\"alarmActive\":" + boolText(alarmActive) + ",";
  mqttStringMessage += "\"alarmLockout\":" + boolText(alarmLockout) + ",";
  mqttStringMessage += "\"wrongEntryCount\":" + String(wrongEntryCount) + ",";
  mqttStringMessage += "\"wifiRssi\":" + String(WiFi.RSSI());
  mqttStringMessage += "}";

  mqttClient.publish(topicPub, mqttStringMessage);
  Serial.println("Topic: " + String(topicPub) + " Message: " + mqttStringMessage);
}

void messageReceived(String &topic, String &payload)
{
  Serial.println("incoming: " + topic + " - " + payload);
  lastPiCommand = payload;

  // Raspberry Pi command handling can be added later here.
  // Example future commands:
  //   fan:on, fan:off, led:red, led:white, buzzer:off, alarm:clear
  // For now, sensor-priority local logic still controls the outputs.
}

/*************** THREAD 1: PIR EVERY 0.5 SECOND ***************/
static void threadPIR(void *pvParameters)
{
  Serial.println("Thread PIR: Started");
  pinMode(PIR_PIN, INPUT);

  TickType_t previousWakeTime = xTaskGetTickCount();

  while (1) {
    pirTaskCounter++;
    valPIR = digitalRead(PIR_PIN);
    myDelayMsUntil(&previousWakeTime, PIR_READ_INTERVAL_MS);
  }
}

/*************** THREAD 2: LDR EVERY 1 SECOND ***************/
static void threadLDR(void *pvParameters)
{
  Serial.println("Thread LDR: Started");

  TickType_t previousWakeTime = xTaskGetTickCount();

  while (1) {
    ldrTaskCounter++;
    lightVal = analogRead(LDR_PIN);
    powerFailureDetected = (lightVal < LIGHT_THRES);
    myDelayMsUntil(&previousWakeTime, LDR_READ_INTERVAL_MS);
  }
}

/*************** THREAD 3: DHT22 EVERY 2 SECONDS ***************/
static void threadDHT22(void *pvParameters)
{
  Serial.println("Thread DHT22: Started");

  TickType_t previousWakeTime = xTaskGetTickCount();

  while (1) {
    dhtTaskCounter++;

    float readTempF = dht.readTemperature(true);
    float readHumidity = dht.readHumidity();

    if (!isnan(readTempF) && !isnan(readHumidity)) {
      tempF = readTempF;
      humidity = readHumidity;
      dhtReadOk = true;

      if (!highTempDetected && tempF >= TEMP_THRESHOLD_F) {
        highTempDetected = true;
        fanOn = true;
      }
      else if (highTempDetected && tempF <= (TEMP_THRESHOLD_F - TEMP_HYSTERESIS_F)) {
        highTempDetected = false;
        fanOn = false;
      }
    }
    else {
      dhtReadOk = false;
    }

    myDelayMsUntil(&previousWakeTime, DHT_READ_INTERVAL_MS);
  }
}

/*************** THREAD 4: TASK B / MQTT SERVICE ***************/
static void threadB(void *pvParameters)
{
  Serial.println("Thread B MQTT: Started");
  unsigned long prevMqttPublishMillis = 0;

  while (1) {
    bTaskCounter++;

    mqttClient.loop();

    if (!mqttClient.connected()) {
      connectMqtt(mqttClientName);
    }

    unsigned long currMillis = millis();
    if (currMillis - prevMqttPublishMillis >= MQTT_PUBLISH_INTERVAL_MS) {
      prevMqttPublishMillis = currMillis;
      publishSensorSnapshot();
    }

    myDelayMs(20);
  }
}

/*************** THREAD 5: TASK C / KEYPAD + OUTPUT CONTROL ***************/
static void threadC(void *pvParameters)
{
  Serial.println("Thread C Outputs: Started");
  TickType_t previousWakeTime = xTaskGetTickCount();

  while (1) {
    cTaskCounter++;

    if (systemArmed && !alarmActive && valPIR == HIGH) {
      startPIRAlarm();
    }

    readKeypad();
    checkAlarmTimeout();
    updateFanOutput();
    updateRGBOutput();
    updateBuzzerOutput();

    myDelayMsUntil(&previousWakeTime, TASK_C_INTERVAL_MS);
  }
}

/*************** THREAD 6: OLED EVERY 1 SECOND ***************/
static void threadOled(void *pvParameters)
{
  Serial.println("Thread OLED: Started");

  iot33StartOLED(oledLib);
  for (int jj = 1; jj <= 8; jj++) oledline[jj] = "";
  oledline[1] = "Data Center Mon";
  displayTextOLED(oledline, oledLib);

  TickType_t previousWakeTime = xTaskGetTickCount();

  while (1) {
    oledline[1] = "Data Center Mon";

    if (alarmLockout) oledline[2] = "Alarm: LOCKOUT";
    else if (alarmActive) oledline[2] = "Alarm: ACTIVE";
    else oledline[2] = String("Alarm: ") + (systemArmed ? "ARMED" : "DISARMED");

    oledline[3] = String("PIR: ") + (valPIR == HIGH ? "MOTION" : "NONE");

    oledline[4] = "PW: ";
    for (int ii = 0; ii < enteredPassword.length(); ii++) oledline[4] += "*";

    if (alarmLockout) {
      oledline[5] = "ADMIN PW NEEDED";
    }
    else if (alarmActive) {
      unsigned long elapsed = millis() - alarmStartMillis;
      int secondsLeft = 0;
      if (elapsed < ALARM_GRACE_PERIOD_MS) {
        secondsLeft = (ALARM_GRACE_PERIOD_MS - elapsed) / 1000;
      }
      oledline[5] = "Left:" + String(secondsLeft) + "s Try:" + String(wrongEntryCount) + "/3";
    }
    else if (passwordAccepted) {
      oledline[5] = "Password: CORRECT";
    }
    else if (passwordRejected) {
      oledline[5] = "Password: WRONG";
    }
    else {
      oledline[5] = "Password: READY";
    }

    if (dhtReadOk) {
      oledline[6] = String("T:") + String(tempF, 1) + "F H:" + String(humidity, 1) + "%";
    }
    else {
      oledline[6] = "DHT22: READ ERR";
    }

    if (highTempDetected) {
      oledline[7] = String("Temp: HIGH Fan:") + (fanOn ? "ON" : "OFF");
    }
    else if (tempF >= 68.0 && tempF <= 72.0) {
      oledline[7] = String("Temp: IDEAL Fan:") + (fanOn ? "ON" : "OFF");
    }
    else {
      oledline[7] = String("Temp: NORM Fan:") + (fanOn ? "ON" : "OFF");
    }

    convDDHHMMSS(millis() / 1000, tmpBuffer);
    oledline[8] = String("Pwr:") + (powerFailureDetected ? "FAIL " : "OK ") + String(tmpBuffer);

    displayTextOLED(oledline, oledLib);
    myDelayMsUntil(&previousWakeTime, OLED_UPDATE_INTERVAL_MS);
  }
}

/*************** THREAD 7: TASK MONITOR EVERY 10 SECONDS ***************/
static char ptrTaskList[700];

void taskMonitor(void *pvParameters)
{
  int measurement;

  Serial.println("Task Monitor: Started");

  while (1) {
    myDelayMs(10000);

    Serial.flush();
    Serial.println("");
    Serial.println("****************************************************");
    Serial.print("Free Heap: ");
    Serial.print(xPortGetFreeHeapSize());
    Serial.println(" bytes");

    Serial.print("Min Heap: ");
    Serial.print(xPortGetMinimumEverFreeHeapSize());
    Serial.println(" bytes");

    Serial.print("PIR/LDR/DHT/B/C Counters: ");
    Serial.print(pirTaskCounter); Serial.print("/");
    Serial.print(ldrTaskCounter); Serial.print("/");
    Serial.print(dhtTaskCounter); Serial.print("/");
    Serial.print(bTaskCounter); Serial.print("/");
    Serial.println(cTaskCounter);
    Serial.flush();

    pirTaskCounter = 0;
    ldrTaskCounter = 0;
    dhtTaskCounter = 0;
    bTaskCounter = 0;
    cTaskCounter = 0;

    Serial.println("****************************************************");
    Serial.println("Task            ABS             %Util");
    Serial.println("****************************************************");
    vTaskGetRunTimeStats(ptrTaskList);
    Serial.println(ptrTaskList);
    Serial.flush();

    Serial.println("****************************************************");
    Serial.println("Task            State   Prio    Stack   Num     Core");
    Serial.println("****************************************************");
    vTaskList(ptrTaskList);
    Serial.println(ptrTaskList);
    Serial.flush();

    Serial.println("****************************************************");
    Serial.println("[Stacks Free Bytes Remaining]");

    measurement = uxTaskGetStackHighWaterMark(Handle_pirTask);
    Serial.print("Thread PIR: "); Serial.println(measurement);

    measurement = uxTaskGetStackHighWaterMark(Handle_ldrTask);
    Serial.print("Thread LDR: "); Serial.println(measurement);

    measurement = uxTaskGetStackHighWaterMark(Handle_dhtTask);
    Serial.print("Thread DHT22: "); Serial.println(measurement);

    measurement = uxTaskGetStackHighWaterMark(Handle_bTask);
    Serial.print("Thread B MQTT: "); Serial.println(measurement);

    measurement = uxTaskGetStackHighWaterMark(Handle_cTask);
    Serial.print("Thread C Outputs: "); Serial.println(measurement);

    measurement = uxTaskGetStackHighWaterMark(Handle_oledTask);
    Serial.print("Thread OLED: "); Serial.println(measurement);

    measurement = uxTaskGetStackHighWaterMark(Handle_monitorTask);
    Serial.print("Monitor Stack: "); Serial.println(measurement);

    Serial.println("****************************************************");
    Serial.flush();
  }

  Serial.println("Task Monitor: Deleting");
  vTaskDelete(NULL);
}

/*************** SETUP / LOOP ***************/
void setup()
{
  Serial.begin(115200);

  while (!Serial && millis() < 5000) {
    ; // wait up to 5 seconds for Serial Monitor
  }

  delay(500);

  Serial.println("");
  Serial.println("******************************");
  Serial.println("  ECE 612 Combined Start");
  Serial.println("******************************");

  pinMode(PIR_PIN, INPUT);
  pinMode(LDR_PIN, INPUT);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BLUE_PIN, OUTPUT);

  digitalWrite(FAN_PIN, LOW);
  noTone(BUZZER_PIN);
  rgbOff();

  dht.begin();

  sprintf(topicPub, "ece612/%s/nano33iot/sensors", gNumber);
  sprintf(topicSub, "ece612/%s/pi/commands", gNumber);
  getMqttClientName(A0, mqttClientName, gNumber);

  // MQTT is serviced from Task B so sensor/output tasks still run even
  // if WiFi or the Raspberry Pi broker is temporarily unavailable.
  mqttClient.begin(MQTT_BROKER, wifiClient);
  mqttClient.onMessage(messageReceived);

  xTaskCreate(threadPIR,     "Task PIR",     256, NULL, tskIDLE_PRIORITY + 6, &Handle_pirTask);
  xTaskCreate(threadLDR,     "Task LDR",     256, NULL, tskIDLE_PRIORITY + 5, &Handle_ldrTask);
  xTaskCreate(threadDHT22,   "Task DHT22",   384, NULL, tskIDLE_PRIORITY + 4, &Handle_dhtTask);
  xTaskCreate(threadB,       "Task B MQTT",  512, NULL, tskIDLE_PRIORITY + 3, &Handle_bTask);
  xTaskCreate(threadC,       "Task C OUT",   384, NULL, tskIDLE_PRIORITY + 2, &Handle_cTask);
  xTaskCreate(threadOled,    "Task OLED",    384, NULL, tskIDLE_PRIORITY + 1, &Handle_oledTask);
  xTaskCreate(taskMonitor,   "Task Monitor", 512, NULL, tskIDLE_PRIORITY + 1, &Handle_monitorTask);

  vTaskStartScheduler();

  while (1) {
    Serial.println("Scheduler Failed!");
    delay(1000);
  }
}

void loop()
{
  // FreeRTOS scheduler runs the application tasks.
  delay(1000);
}
