#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode is not selected in Tools->Zigbee mode"
#endif

#include "Zigbee.h"
#include <AHT20.h>
#include <BH1750.h>
#include <math.h>

#define ZIGBEE_ILLUMINANCE_SENSOR_ENDPOINT 9
#define TEMP_SENSOR_ENDPOINT_NUMBER 10
#define MOISTURE_SENSOR_ENDPOINT_NUMBER 11
#define BATTERY_VOLTAGE_ENDPOINT_NUMBER 12

#define uS_TO_S_FACTOR 1000000ULL /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP  897

#define BATTERY_PIN A0
#define SOIL_PIN A1

#define SLOW_BLINK_SPEED 500
#define FAST_BLINK_SPEED 250

#define BOOT_DELAY 2800
#define PAIRING_DELAY 30000
#define TRANSMISSION_DELAY 100

#define SLOW_BOOTS 2

float soil_moisture_lower_limit = 1050;
float soil_moisture_upper_limit = 2700;

ZigbeeTempSensor zbTempSensor = ZigbeeTempSensor(TEMP_SENSOR_ENDPOINT_NUMBER);
AHT20 aht20;

ZigbeeIlluminanceSensor zbIlluminanceSensor = ZigbeeIlluminanceSensor(ZIGBEE_ILLUMINANCE_SENSOR_ENDPOINT);
BH1750 light_sensor;

ZigbeeAnalog zbSoilMoistureSensor = ZigbeeAnalog(MOISTURE_SENSOR_ENDPOINT_NUMBER);
ZigbeeAnalog zbBatteryVoltage = ZigbeeAnalog(BATTERY_VOLTAGE_ENDPOINT_NUMBER);

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

void measureAndReportBatteryPercentage()
{
  // Measure.
  uint32_t batteryVoltages = 0;
  for(int i = 0; i < 16; i++) {
    batteryVoltages += analogReadMilliVolts(BATTERY_PIN);
  }
  float batteryVoltage = 2 * batteryVoltages / 16;
  float batteryPercentage = getBatteryPercentage(batteryVoltage);

  // Update.
  zbTempSensor.setBatteryPercentage(batteryPercentage);

  // Report.
  zbTempSensor.reportBatteryPercentage();
}

void measureAndReportSoilMoisture()
{
  // Measure.
  uint32_t moistureReadings = 0;
  for(int i = 0; i < 16; i++) {
    moistureReadings += analogRead(SOIL_PIN); // Read and accumulate ADC voltage
  }
  float moistureReading = moistureReadings / 16;
  float soil_moisture_percentage = (1 -((moistureReading - soil_moisture_lower_limit) / (soil_moisture_upper_limit - soil_moisture_lower_limit))) * 100;
  int soil_moisture_percentage_scaled = soil_moisture_percentage * 10;
  // Update.
  zbSoilMoistureSensor.setAnalogInput(soil_moisture_percentage_scaled);

  // Report.
  zbSoilMoistureSensor.reportAnalogInput();
}

void measureAndReportIlluminance()
{
  // Measure.
  int illuminance = light_sensor.readLightLevel();
  int zigbee_illuminance = 10000 * log10(illuminance);

  // Update.
  zbIlluminanceSensor.setIlluminance(zigbee_illuminance);

  // Report.
  zbIlluminanceSensor.report();
}

void measureAndReportTemperatureAndHumidity()
{
  // Measure.
  float temperature =  aht20.getTemperature();
  float humidity = aht20.getHumidity();

  // Update.
  zbTempSensor.setTemperature(temperature);
  zbTempSensor.setHumidity(humidity);

  // Report.
  zbTempSensor.report();
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

/********************* Arduino functions **************************/
void setup() {
  Serial.begin(115200);
  Serial.print("Booting for the ");
  Serial.print(++bootCount);
  Serial.println("th time.");

  // Init BOOT_PIN switch
  pinMode(BOOT_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(SOIL_PIN, INPUT);
  pinMode(BATTERY_PIN, INPUT);

  // Connect to AHT30 sensor.
  Wire.begin();
  if (!light_sensor.begin()) {
    Serial.println("BH1750 not detected. Please check wiring.");
  }

  if (aht20.begin() == false)
  {
    Serial.println("AHT20 not detected. Please check wiring.");
  }

  // Optional: set Zigbee device name and model
  zbTempSensor.setManufacturerAndModel("LarsvdLee", "SleepyPlantSensor");

  // Set minimum and maximum temperature measurement value (10-50°C is default range for chip temperature measurement)
  zbTempSensor.setMinMaxValue(10, 50);

  // Set tolerance for temperature measurement in °C (lowest possible value is 0.01°C)
  zbTempSensor.setTolerance(1);

  // Set power source to battery and set battery percentage to measured value (now 100% for demonstration)
  // The value can be also updated by calling zbTempSensor.setBatteryPercentage(percentage) anytime
  zbTempSensor.setPowerSource(ZB_POWER_SOURCE_BATTERY, 100);

  // Add humidity cluster to the temperature sensor device with min, max and tolerance values
  zbTempSensor.addHumiditySensor(0, 100, 1);

  // Set minimum and maximum for raw illuminance value (0 min and 50000 max equals to 0 lux - 100,000 lux)
  zbIlluminanceSensor.setMinMaxValue(0, 50000);

  // Optional: Set tolerance for raw illuminance value
  zbIlluminanceSensor.setTolerance(1);

  // Setup the analog device.
  zbSoilMoistureSensor.addAnalogInput();
  analogReadResolution(12);

  // Add endpoint to Zigbee Core
  Zigbee.addEndpoint(&zbIlluminanceSensor);
  Zigbee.addEndpoint(&zbTempSensor);
  Zigbee.addEndpoint(&zbSoilMoistureSensor);

  // Create a custom Zigbee configuration for End Device with keep alive 10s to avoid interference with reporting data
  esp_zb_cfg_t zigbeeConfig = ZIGBEE_DEFAULT_ED_CONFIG();
  zigbeeConfig.nwk_cfg.zed_cfg.keep_alive = 20000;

  // For battery powered devices, it can be better to set timeout for Zigbee Begin to lower value to save battery
  // If the timeout has been reached, the network channel mask will be reset and the device will try to connect again after reset (scanning all channels)
  Zigbee.setTimeout(10000);  // Set timeout for Zigbee Begin to 10s (default is 30s)

  // When all EPs are registered, start Zigbee in End Device mode
  if (!Zigbee.begin(&zigbeeConfig, false)) {
    Serial.println("Zigbee failed to start!");
    Serial.println("Rebooting...");
    ESP.restart();  // If Zigbee failed to start, reboot the device and try again
  }
  Serial.println("Connecting to network");
  while (!Zigbee.connected()) {
    Serial.print(".");
    delay(100);
  }
  Serial.println();
  Serial.println("Successfully connected to Zigbee network");

  zbTempSensor.setReporting(120, 120, 10);
  zbTempSensor.setHumidityReporting(120,120,10);
  zbIlluminanceSensor.setReporting(120, 120, 10);
  zbSoilMoistureSensor.setAnalogInputReporting(120, 120, 10);
}

void loop() {
  // Smal delay to allow establishing a proper connection with the coordinator. Done before the loop to allow for pressing the boot button after starting.
  blinkAndWait(BOOT_DELAY, SLOW_BLINK_SPEED);

  // Checking BOOT_PIN for factory reset
  if (digitalRead(BOOT_PIN) == LOW) {  // Push BOOT_PIN pressed
    // Key debounce handling
    delay(100);
    int startTime = millis();
    while (digitalRead(BOOT_PIN) == LOW) {
      delay(SLOW_BLINK_SPEED);
      if ((millis() - startTime) > 10000) {
        // If key pressed for more than 10secs, factory reset Zigbee and reboot
        Serial.println("Resetting Zigbee to factory and rebooting in 1s.");
        delay(1000);
        // Optional set reset in factoryReset to false, to not restart device after erasing nvram, but set it to endless sleep manually instead
        Zigbee.factoryReset(false);
        Serial.println("Going to endless sleep, press RESET BOOT_PIN or power off/on the device to wake up");
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
        esp_deep_sleep_start();
      }
      digitalWrite(LED_BUILTIN, LOW);
      delay(SLOW_BLINK_SPEED);
      digitalWrite(LED_BUILTIN, HIGH);
    }
  }

  // The initial pairing is slower, so we wait longer the first 2 times we boot before sending data.
  if (bootCount < SLOW_BOOTS)
  {
    blinkAndWait(PAIRING_DELAY, FAST_BLINK_SPEED);
  }

  measureAndReportTemperatureAndHumidity();
  measureAndReportIlluminance();
  measureAndReportSoilMoisture();
  measureAndReportBatteryPercentage();

  // Small delay to allow all reporting to finish.
  delay(TRANSMISSION_DELAY);

  goToSleep();
}
