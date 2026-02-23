#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <AHT20.h>
#include <BH1750.h>
#include <math.h>

// WiFi credentials
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""

// MQTT broker settings
#define MQTT_BROKER    ""
#define MQTT_PORT      1883
#define MQTT_TOPIC     "esp/plant-sensor/state"
#define MQTT_CLIENT_ID "plant-sensor-1"

#define uS_TO_S_FACTOR 1000000ULL /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP  55

#define BATTERY_PIN A0
#define SOIL_PIN A1
#define SOIL_POWER_PIN 19

#define SLOW_BLINK_SPEED 500

#define BOOT_DELAY 2500
#define SOIL_MOISTURE_POWER_DELAY 500
#define LIGHT_MEASUREMENT_TIMEOUT 500
#define LIGHT_MEASUREMENT_POLL_DELAY 10

#define WIFI_CONNECT_TIMEOUT_MS 15000
#define MQTT_CONNECT_TIMEOUT_MS 10000

float soil_moisture_lower_limit = 1050;
float soil_moisture_upper_limit = 2650;

AHT20 aht20;
BH1750 light_sensor;
bool lightSensorDetected = false;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

RTC_DATA_ATTR int bootCount = 0;

void blinkAndWait(int delayMillis, long speed)
{
  int startTime = millis();
  while ((millis() - startTime) < delayMillis)
  {
    digitalWrite(LED_BUILTIN, LOW);
    delay(speed);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(speed);
  }
}

void goToSleep()
{
  // Configure the wake up source and go to sleep.
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}

int measureBatteryPercentage()
{
  // Measure 16 times and take the average.
  uint32_t batteryVoltages = 0;
  for(int i = 0; i < 16; i++) {
    batteryVoltages += analogReadMilliVolts(BATTERY_PIN);
  }
  float batteryVoltage = 2 * batteryVoltages / 16;
  float batteryPercentage = getBatteryPercentage(batteryVoltage);
  return roundPercentageDown(batteryPercentage, 1);
}

float measureSoilMoisture()
{
    // Power the soil moisture sensor.
    digitalWrite(SOIL_POWER_PIN, HIGH);
    delay(SOIL_MOISTURE_POWER_DELAY); // Allow the sensor to stabilize.

    // Measure 16 times and take the average.
    uint32_t moistureReadings = 0;
    for(int i = 0; i < 16; i++) {
      moistureReadings += analogRead(SOIL_PIN); // Read and accumulate ADC voltage
    }

    // Power off the soil moisture sensor.
    digitalWrite(SOIL_POWER_PIN, LOW);

    float moistureReading = moistureReadings / 16;
    // We assume the HW390 behaves linearly between the lower and upper limit.
    float moisturePercentage = (1 -((moistureReading - soil_moisture_lower_limit) / (soil_moisture_upper_limit - soil_moisture_lower_limit))) * 100;
    Serial.println(moisturePercentage);
    return roundPercentageDown(moisturePercentage, 1);
}



int measureIlluminance()
{
  if (!lightSensorDetected)
  {
    return -1;
  }

  int waitTime = 0;
  while (!light_sensor.measurementReady() && waitTime < LIGHT_MEASUREMENT_TIMEOUT)
  {
    delay(LIGHT_MEASUREMENT_POLL_DELAY);
    waitTime += LIGHT_MEASUREMENT_POLL_DELAY;
  }

  if (!light_sensor.measurementReady())
  {
    Serial.println("BH1750 measurement timeout.");
    return -1;
  }

  return round(light_sensor.readLightLevel());
}



void measureTemperatureAndHumidity(float* temperature, float* humidity)
{
  // Measure.
  *temperature =  aht20.getTemperature();
  *humidity = aht20.getHumidity();
}



float getBatteryPercentage(int milliVolts) {
  // Model taken from: https://github.com/def1149/ESP32_Stuff/blob/main/Lipo_Battery_Pct_Capacity
  #define FULL 4200   // >= FULL 100%
  #define EMPTY 3499  // < EMPTY 0%
  float percentage;

  if (milliVolts >= FULL) {
    percentage = 100.0;
  } 
  else if (milliVolts >= 3880) {
    percentage = map(milliVolts,3880,FULL-1,600,999)/10.0;
  } 
  else if (milliVolts >= 3750) {
    percentage = map(milliVolts,3750,3879,202,599)/10.0;
  }
  else if (milliVolts >= 3700 ) {
    percentage = map(milliVolts,3700,3749,79,201)/10.0;
  }
  else if (milliVolts >= 3610 ) {
    percentage = map(milliVolts,3610,3699,20,78)/10.0;
  }
  else if (milliVolts >= EMPTY) {
    percentage = map(milliVolts,EMPTY,3609,1,19)/10.0;
  }
  else {
    percentage = 0.0;
  }

  return percentage;
}

int roundPercentageDown(int percentage, int remainder)
{
  return percentage - (percentage % remainder);
}

bool connectWiFi()
{
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECT_TIMEOUT_MS)
  {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\nWiFi connected. IP: " + WiFi.localIP().toString());
    return true;
  }

  Serial.println("\nWiFi connection timed out.");
  return false;
}

bool connectMQTT()
{
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);

  Serial.print("Connecting to MQTT broker");
  unsigned long startTime = millis();
  while (!mqttClient.connected() && (millis() - startTime) < MQTT_CONNECT_TIMEOUT_MS)
  {
    if (mqttClient.connect(MQTT_CLIENT_ID /* , MQTT_USERNAME, MQTT_PASSWORD */))
    {
      Serial.println("\nMQTT connected.");
      return true;
    }
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nMQTT connection failed. State: " + String(mqttClient.state()));
  return false;
}

void publishMeasurements(float temperature, float humidity, int illuminance, float soilMoisture, int batteryPercentage)
{
  JsonDocument doc;
  doc["temperature"]   = temperature;
  doc["humidity"]      = humidity;
  if (illuminance >= 0)
  {
    doc["illuminance"] = illuminance;
  }
  doc["soil_moisture"] = soilMoisture;
  doc["battery"]       = batteryPercentage;

  char payload[256];
  serializeJson(doc, payload);

  Serial.print("Publishing: ");
  Serial.println(payload);

  mqttClient.publish(MQTT_TOPIC, payload, true /* retain */);
}

/********************* Arduino functions **************************/
void setup() {
  Serial.begin(115200);
  Serial.print("Booting for the ");
  Serial.print(++bootCount);
  Serial.println("th time.");

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(SOIL_PIN, INPUT);
  pinMode(BATTERY_PIN, INPUT);
  pinMode(SOIL_POWER_PIN, OUTPUT);
  analogReadResolution(12);

  Wire.begin();
  lightSensorDetected = light_sensor.begin();
  if (!lightSensorDetected) {
    Serial.println("BH1750 not detected. Please check wiring.");
  }
  light_sensor.configure(BH1750::ONE_TIME_HIGH_RES_MODE_2);

  if (aht20.begin() == false)
  {
    Serial.println("AHT20 not detected. Please check wiring.");
  }
}

void loop() {
  blinkAndWait(BOOT_DELAY, SLOW_BLINK_SPEED);

  // Gather all measurements.
  int batteryPercentage = measureBatteryPercentage();
  int illuminance       = measureIlluminance();
  float soilMoisture    = measureSoilMoisture();
  float temperature, humidity;
  measureTemperatureAndHumidity(&temperature, &humidity);

  // Connect and publish all measurements in a single MQTT message.
  if (connectWiFi() && connectMQTT())
  {
    publishMeasurements(temperature, humidity, illuminance, soilMoisture, batteryPercentage);
    // Give PubSubClient a moment to complete the publish before sleeping.
    mqttClient.loop();
    delay(100);
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  goToSleep();
}
