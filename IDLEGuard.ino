#include <Wire.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>



// WI-FI AND NTFY

const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* NTFY_URL  = "https://ntfy.sh/YOUR_UNIQUE_TOPIC";   // pick a long, hard to guess topic name



// PIN DEFINITIONS
const int TEMP_PIN = 34;          

const int SDA_PIN = 21;
const int SCL_PIN = 22;

const int ENGINE_LED_PIN  = 25;
const int CIRCUIT_LED_PIN = 26;

const int BUTTON_PIN = 13;


// TEMPERATURE SETTINGS

// First warning
const float FIRST_TEMP_RISE_C = 2.0;

// Second warning
const float SECOND_TEMP_RISE_C = 4.5;



// ENGINE SHUTDOWN
const unsigned long SHUTDOWN_DELAY_MS = 20000;


// MPU6050 
uint8_t MPU_ADDRESS = 0x68;

const uint8_t MPU_ACCEL_XOUT_H = 0x3B;



// MOVEMENT DETECTION
const float MOTION_THRESHOLD = 1.00;
const int   MOTION_CONFIRM_SAMPLES = 3;
const unsigned long MOTION_HOLD_MS = 1500;
const unsigned long STATIONARY_CONFIRM_MS = 1500;


// TIMING
const unsigned long SENSOR_INTERVAL_MS = 100;
const unsigned long STATUS_INTERVAL_MS = 1000;
const unsigned long DEBOUNCE_DELAY_MS  = 50;

const unsigned long NOTIFY_COOLDOWN_MS = 10000;


 
// STATE VARIABLES

bool engineOn = false;
bool circuitActive = false;

float baseTemperatureC = 0.0;
float currentTemperatureC = 0.0;

bool firstNotificationSent = false;
bool secondNotificationSent = false;

float accelX = 0.0, accelY = 0.0, accelZ = 0.0;
float previousAccelX = 0.0, previousAccelY = 0.0, previousAccelZ = 0.0;

bool firstAccelReading = true;
bool vehicleMoving = false;
int motionCounter = 0;

unsigned long lastMotionTime = 0;
unsigned long stationarySince = 0;

bool shutdownTimerRunning = false;
unsigned long shutdownTimerStart = 0;

unsigned long engineStartMs = 0;

bool lastRawButtonState = LOW;     // released = LOW (pull-down)
bool stableButtonState = LOW;
unsigned long lastDebounceTime = 0;

unsigned long lastSensorRead = 0;
unsigned long lastStatusPrint = 0;

bool onlineNotified = false;


 
// ntfy

struct Notif
{
  char title[48];
  char body[160];
  char prio[10];
  char tags[24];
};

QueueHandle_t notifQueue;

bool sendNtfy(const Notif& n)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();   

  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(3000);

  if (!http.begin(client, NTFY_URL))
  {
    return false;
  }

  http.addHeader("Title", n.title);
  http.addHeader("Priority", n.prio);

  if (n.tags[0] != '\0')
  {
    http.addHeader("Tags", n.tags);
  }

  int code = http.POST(n.body);
  http.end();

  Serial.printf("[phone] %s -> response %d\n", n.title, code);

  return code == 200;
}

void netTask(void*)
{
  Notif n;

  for (;;)
  {
    if (xQueueReceive(notifQueue, &n, portMAX_DELAY))
    {
      for (int attempt = 0; attempt < 10; attempt++)
      {
        if (sendNtfy(n))
        {
          break;
        }

        vTaskDelay(pdMS_TO_TICKS(3000));
      }
    }
  }
}

void notify(
  const char* title,
  const char* body,
  const char* prio,
  const char* tags
)
{
  Notif n;

  strlcpy(n.title, title, sizeof(n.title));
  strlcpy(n.body,  body,  sizeof(n.body));
  strlcpy(n.prio,  prio,  sizeof(n.prio));
  strlcpy(n.tags,  tags,  sizeof(n.tags));

  xQueueSend(notifQueue, &n, 0);
}


void notifyThrottled(
  int slot,
  const char* title,
  const char* body,
  const char* prio,
  const char* tags
)
{
  static unsigned long lastSent[4] = {0, 0, 0, 0};

  if (
      lastSent[slot] != 0 &&
      millis() - lastSent[slot] < NOTIFY_COOLDOWN_MS
     )
  {
    return;
  }

  lastSent[slot] = millis();
  notify(title, body, prio, tags);
}


// READ TMP36

float readTemperatureC()
{
  const int NUM_SAMPLES = 20;

  uint32_t totalMillivolts = 0;

  for (int i = 0; i < NUM_SAMPLES; i++)
  {
    totalMillivolts += analogReadMilliVolts(TEMP_PIN);
    delay(2);
  }

  float averageMillivolts = totalMillivolts / (float)NUM_SAMPLES;
  float voltage = averageMillivolts / 1000.0;

  return (voltage - 0.5) * 100.0;
}


// INITIALIZE MPU6050

bool initializeMPU()
{
  Serial.println();
  Serial.println("Searching for MPU6050...");

  delay(1000);

  bool found = false;

  for (int attempt = 1; attempt <= 5; attempt++)
  {
    Serial.print("I2C scan attempt ");
    Serial.println(attempt);

    for (uint8_t addr = 1; addr < 127; addr++)
    {
      Wire.beginTransmission(addr);

      if (Wire.endTransmission() == 0)
      {
        Serial.print("Device found at 0x");
        if (addr < 16) Serial.print("0");
        Serial.println(addr, HEX);

        if (addr == 0x68 || addr == 0x69)
        {
          MPU_ADDRESS = addr;
          found = true;
          break;
        }
      }
    }

    if (found) break;

    delay(500);
  }

  if (!found)
  {
    Serial.println();
    Serial.println("ERROR: MPU6050 not found.");
    Serial.println("Check VCC, GND, SDA and SCL.");
    return false;
  }


  Wire.beginTransmission(MPU_ADDRESS);
  Wire.write(0x75);

  if (Wire.endTransmission(false) != 0)
  {
    Serial.println("ERROR reading WHO_AM_I.");
    return false;
  }

  Wire.requestFrom(MPU_ADDRESS, (uint8_t)1);

  if (Wire.available())
  {
    uint8_t whoAmI = Wire.read();
    Serial.print("WHO_AM_I = 0x");
    if (whoAmI < 16) Serial.print("0");
    Serial.println(whoAmI, HEX);
  }

  // Wake up MPU6050
  Wire.beginTransmission(MPU_ADDRESS);
  Wire.write(0x6B);
  Wire.write(0x00);

  if (Wire.endTransmission() != 0)
  {
    Serial.println("ERROR: Could not wake MPU6050.");
    return false;
  }

  delay(100);

  // Accelerometer range
  Wire.beginTransmission(MPU_ADDRESS);
  Wire.write(0x1C);
  Wire.write(0x00);

  if (Wire.endTransmission() != 0)
  {
    Serial.println("ERROR configuring accelerometer.");
    return false;
  }

  Serial.println();
  Serial.print("MPU6050 successfully initialized at 0x");
  Serial.println(MPU_ADDRESS, HEX);

  return true;
}


 
// READ ACCELERATION

bool readMPUAcceleration()
{
  Wire.beginTransmission(MPU_ADDRESS);
  Wire.write(MPU_ACCEL_XOUT_H);

  if (Wire.endTransmission(false) != 0)
  {
    return false;
  }

  Wire.requestFrom(MPU_ADDRESS, (uint8_t)6);

  if (Wire.available() < 6)
  {
    return false;
  }

  int16_t raw[3];

  for (int i = 0; i < 3; i++)
  {
    uint8_t hi = Wire.read();
    uint8_t lo = Wire.read();
    raw[i] = (int16_t)((hi << 8) | lo);
  }

  const float ACCEL_SCALE = 9.80665 / 16384.0;   // +/-2g

  accelX = raw[0] * ACCEL_SCALE;
  accelY = raw[1] * ACCEL_SCALE;
  accelZ = raw[2] * ACCEL_SCALE;

  return true;
}


 
// CHECK IF STATIONARY

bool isConfirmedStationary()
{
  if (vehicleMoving) return false;
  if (stationarySince == 0) return false;

  return (millis() - stationarySince >= STATIONARY_CONFIRM_MS);
}


 
// UPDATE MOVEMENT

void updateMovement()
{
  if (!readMPUAcceleration())
  {
    Serial.println("WARNING: MPU read failed.");
    return;
  }

  if (firstAccelReading)
  {
    previousAccelX = accelX;
    previousAccelY = accelY;
    previousAccelZ = accelZ;

    firstAccelReading = false;
    stationarySince = millis();
    return;
  }

  float deltaX = accelX - previousAccelX;
  float deltaY = accelY - previousAccelY;
  float deltaZ = accelZ - previousAccelZ;

  float movementAmount = sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);

  previousAccelX = accelX;
  previousAccelY = accelY;
  previousAccelZ = accelZ;

  if (movementAmount > MOTION_THRESHOLD)
  {
    motionCounter++;

    if (motionCounter >= MOTION_CONFIRM_SAMPLES)
    {
      lastMotionTime = millis();
      motionCounter = MOTION_CONFIRM_SAMPLES;
    }
  }
  else
  {
    motionCounter = 0;
  }

  bool previousMovingState = vehicleMoving;

  vehicleMoving = (millis() - lastMotionTime < MOTION_HOLD_MS);

  // Started moving
  if (vehicleMoving && !previousMovingState)
  {
    digitalWrite(CIRCUIT_LED_PIN, LOW);
    Serial.println();
    Serial.println(">>> VEHICLE MOVING");

    stationarySince = 0;

    
    if (shutdownTimerRunning)
    {
      shutdownTimerRunning = false;
      
      Serial.println("Shutdown timer cancelled.");

      notifyThrottled(
        0,
        "Shutdown cancelled",
        "Movement detected, so the engine will stay on.",
        "default",
        ""
      );
    }
  }

  // Stopped moving
  if (!vehicleMoving && previousMovingState)
  {
    Serial.println();
    Serial.println(">>> VEHICLE STOPPED");

    stationarySince = millis();
  }

  if (!vehicleMoving && stationarySince == 0)
  {
    stationarySince = millis();
  }
}


 
// START ENGINE

void startEngine()
{
  engineOn = true;
  engineStartMs = millis();

  digitalWrite(ENGINE_LED_PIN, HIGH);

  circuitActive = true;
  digitalWrite(CIRCUIT_LED_PIN, HIGH);

  delay(2500);
  baseTemperatureC = readTemperatureC();
  currentTemperatureC = baseTemperatureC;

  firstNotificationSent = false;
  secondNotificationSent = false;

  shutdownTimerRunning = false;
  shutdownTimerStart = 0;

  firstAccelReading = true;
  vehicleMoving = false;
  motionCounter = 0;
  stationarySince = millis();

  Serial.println();
  Serial.println("==================================");
  Serial.println("ENGINE STARTED");
  Serial.println("MONITORING CIRCUIT ON");

  Serial.print("Base temperature: ");
  Serial.print(baseTemperatureC, 2);
  Serial.println(" C");

  Serial.print("Warning 1 at: ");
  Serial.print(baseTemperatureC + FIRST_TEMP_RISE_C, 2);
  Serial.println(" C");

  Serial.print("Warning 2 at: ");
  Serial.print(baseTemperatureC + SECOND_TEMP_RISE_C, 2);
  Serial.println(" C");

  Serial.println("==================================");

  notify(
    "Engine started",
    "Idle guardian is on. You'll get an alert as soon as the car is warmed up and ready to drive.",
    "low",
    ""
  );
}


 
// STOP ENGINE (automatic)

void stopEngine()
{
  engineOn = false;

  // Actuator first, notification second
  digitalWrite(ENGINE_LED_PIN, LOW);

  circuitActive = false;
  digitalWrite(CIRCUIT_LED_PIN, LOW);

  shutdownTimerRunning = false;

  Serial.println();
  Serial.println("##################################");
  Serial.println("ENGINE AUTOMATICALLY SHUT OFF");
  Serial.println("Monitoring circuit OFF");
  Serial.println("##################################");

  char msg[160];

  snprintf(
    msg,
    sizeof(msg),
    "The car sat idle after warming up, so the engine was turned off automatically to save fuel. It ran for %lu seconds in total.",
    (millis() - engineStartMs) / 1000
  );

  notify("Engine shut off", msg, "high", "");
}


 
// BUTTON HANDLING
 
void updateButton()
{
  bool rawState = digitalRead(BUTTON_PIN);

  if (rawState != lastRawButtonState)
  {
    lastDebounceTime = millis();
    lastRawButtonState = rawState;
  }

  if (millis() - lastDebounceTime > DEBOUNCE_DELAY_MS)
  {
    if (rawState != stableButtonState)
    {
      stableButtonState = rawState;

      // Button pressed (HIGH): only starts the engine if it is off
      if (stableButtonState == HIGH && !engineOn)
      {
        startEngine();
      }
    }
  }
}


 
// MAIN CONTROL
 
void processSystem()
{
  if (!engineOn) return;

  float temperatureRise = currentTemperatureC - baseTemperatureC;

  
  if (
      circuitActive &&
      vehicleMoving &&
      temperatureRise >= FIRST_TEMP_RISE_C
     )
  {
    circuitActive = false;
    digitalWrite(CIRCUIT_LED_PIN, LOW);

    shutdownTimerRunning = false;

    Serial.println();
    Serial.println("==================================");
    Serial.println("MONITORING CIRCUIT DEACTIVATED");
    Serial.println("Vehicle is moving and temperature is high.");
    Serial.println("Circuit will remain OFF until next engine start.");
    Serial.println("==================================");

    notify(
      "Driving detected",
      "You're on the move, so idle monitoring is off for this trip. It turns back on next time you start the engine.",
      "low",
      ""
    );

    return;
  }


  if (!circuitActive) return;

 
  if (!isConfirmedStationary()) return;


  // FIRST NOTIFICATION

  if (!firstNotificationSent && temperatureRise >= FIRST_TEMP_RISE_C)
  {
    firstNotificationSent = true;

    Serial.println();
    Serial.println("**********************************");
    Serial.println("NOTIFICATION 1");
    Serial.println("Vehicle is stationary.");
    Serial.println("Temperature rise reached first threshold.");
    Serial.println("EVENT:ALERT_1");
    Serial.println("**********************************");

    notify(
      "Ready for light driving",
      "Your car is warm enough to begin city driving, ensure not to exceed 3000RPM",
      "high",
      ""
    );
  }


  //SECOND NOTIFICATION

  if (
      firstNotificationSent &&
      !secondNotificationSent &&
      temperatureRise >= SECOND_TEMP_RISE_C
     )
  {
    secondNotificationSent = true;

    Serial.println();
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    Serial.println("NOTIFICATION 2");
    Serial.println("CRITICAL TEMPERATURE WARNING");
    Serial.println("EVENT:ALERT_2");
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

    shutdownTimerRunning = true;
    shutdownTimerStart = millis();

    Serial.println("20 second shutdown countdown started.");

    char msg[160];

    snprintf(
      msg,
      sizeof(msg),
      "The car is still parked and idling, and is ready to be driven on the highway. The engine will shut off in %lu seconds unless you start driving.",
      SHUTDOWN_DELAY_MS / 1000
    );

    notify("Reached optimal driving temperature", msg, "urgent", "");
  }


  // SHUTDOWN COUNTDOWN

  if (secondNotificationSent && shutdownTimerRunning)
  {
    unsigned long elapsed = millis() - shutdownTimerStart;

    if (elapsed >= SHUTDOWN_DELAY_MS)
    {
      stopEngine();
    }
  }





}


 
// PRINT STATUS
 

void printStatus()
{
  if (!engineOn)
  {
    Serial.println();
    Serial.println("ENGINE: OFF");
    return;
  }

  float temperatureRise = currentTemperatureC - baseTemperatureC;

  Serial.println();
  Serial.println("----------------------------------");

  Serial.print("Engine: ");
  Serial.println(engineOn ? "ON" : "OFF");

  Serial.print("Circuit: ");
  Serial.println(circuitActive ? "ON" : "OFF");

  Serial.print("Wi-Fi: ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "OFFLINE");

  Serial.print("Temperature: ");
  Serial.print(currentTemperatureC, 2);
  Serial.println(" C");

  Serial.print("Base: ");
  Serial.print(baseTemperatureC, 2);
  Serial.println(" C");

  Serial.print("Temperature rise: ");
  if (temperatureRise >= 0) Serial.print("+");
  Serial.print(temperatureRise, 2);
  Serial.println(" C");

  Serial.print("Accel X: ");
  Serial.print(accelX, 2);
  Serial.print("   Y: ");
  Serial.print(accelY, 2);
  Serial.print("   Z: ");
  Serial.print(accelZ-10, 2);
  Serial.println(" m/s^2");

  Serial.print("Vehicle: ");

  if (vehicleMoving)
  {
    Serial.println("MOVING");
  }
  else if (isConfirmedStationary())
  {
    Serial.println("STATIONARY");
  }
  else
  {
    Serial.println("CHECKING...");
  }

  if (shutdownTimerRunning && secondNotificationSent)
  {
    unsigned long elapsed = millis() - shutdownTimerStart;
    unsigned long remaining = 0;

    if (elapsed < SHUTDOWN_DELAY_MS)
    {
      remaining = (SHUTDOWN_DELAY_MS - elapsed) / 1000;
    }

    Serial.print("Shutdown in: ");
    Serial.print(remaining);
    Serial.println(" seconds");
  }

  Serial.println("----------------------------------");
}


 
// WI-FI EVENT LOGGING
 

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info)
{
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
  {
    Serial.printf("[wifi] disconnected, reason %d\n",
                  info.wifi_sta_disconnected.reason);
  }
  else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
  {
    Serial.print("[wifi] connected, IP ");
    Serial.println(WiFi.localIP());
  }
}


 
// SETUP
 

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("==================================");
  Serial.println("E2-C2 VEHICLE SAFETY SYSTEM");
  Serial.println("==================================");

  // Notification queue and sender task
  notifQueue = xQueueCreate(8, sizeof(Notif));
  xTaskCreatePinnedToCore(netTask, "net", 8192, NULL, 1, NULL, 0);

  // Wi-Fi: non-blocking, so the shutoff works even with no network
  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // Temperature sensor
  pinMode(TEMP_PIN, INPUT);
  analogReadResolution(12);

  // LEDs (engine starts OFF)
  pinMode(ENGINE_LED_PIN, OUTPUT);
  pinMode(CIRCUIT_LED_PIN, OUTPUT);
  digitalWrite(ENGINE_LED_PIN, LOW);
  digitalWrite(CIRCUIT_LED_PIN, LOW);

  // Button
  pinMode(BUTTON_PIN, INPUT_PULLDOWN);

  // MPU6050
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);

  if (!initializeMPU())
  {
    Serial.println();
    Serial.println("MPU INITIALIZATION FAILED");

    while (true)
    {
      delay(1000);
    }
  }

  Serial.println();
  Serial.println("SYSTEM READY");
  Serial.println("Press the button (3.3V to GPIO 13) to turn the car on.");
}


 
// MAIN LOOP
 

void loop()
{

  updateButton();

  
  if (!onlineNotified && WiFi.status() == WL_CONNECTED)
  {
    onlineNotified = true;

    notify(
      "System online",
      "Idle guardian is connected and ready. Press the button to turn the car on.",
      "low",
      ""
    );
  }

  
  static unsigned long lastWifiTry = 0;

  if (
      WiFi.status() != WL_CONNECTED &&
      millis() - lastWifiTry > 15000
     )
  {
    lastWifiTry = millis();

    Serial.println("[wifi] not connected, retrying...");

    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }

  if (!engineOn)
  {
    return;
  }

  unsigned long now = millis();

  
  if (now - lastSensorRead >= SENSOR_INTERVAL_MS)
  {
    lastSensorRead = now;

    
    float rawTemperatureC = readTemperatureC();
    currentTemperatureC = 0.1 * rawTemperatureC + 0.9 * currentTemperatureC;

    updateMovement();

    processSystem();
  }

  // Status print
  if (now - lastStatusPrint >= STATUS_INTERVAL_MS)
  {
    lastStatusPrint = now;

    printStatus();
  }
}
