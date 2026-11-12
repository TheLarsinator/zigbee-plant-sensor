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
#define TIME_TO_SLEEP  898

#define BATTERY_PIN A0
#define SOIL_PIN A1
#define SOIL_POWER_PIN 19
#define INTERRUPT_PULSE_PIN D6
#define INTERRUPT_PULSE_WIDTH_US 50

#define SLOW_BLINK_SPEED 50
#define FAST_BLINK_SPEED 50

#define BOOT_DELAY 250
#define PAIRING_DELAY 30000
#define TRANSMISSION_DELAY 100
#define SOIL_MOISTURE_POWER_DELAY 500
#define LIGHT_MEASUREMENT_TIMEOUT 500
#define LIGHT_MEASUREMENT_POLL_DELAY 10

#define CONNECT_TIMEOUT 10000

#define SLOW_BOOTS 2

float soil_moisture_lower_limit = 1050;
float soil_moisture_upper_limit = 2650;
float soil_moisture = 0;
unsigned long soilPowerOnTime = 0;

// Measured in setup(), before the radio is started, and reported in loop().
float temperature = 0;
float humidity = 0;
int illuminance = -1;

ZigbeeTempSensor zbTempSensor = ZigbeeTempSensor(TEMP_SENSOR_ENDPOINT_NUMBER);
AHT20 aht20;

ZigbeeIlluminanceSensor zbIlluminanceSensor = ZigbeeIlluminanceSensor(ZIGBEE_ILLUMINANCE_SENSOR_ENDPOINT);
BH1750 light_sensor;
bool lightSensorDetected = false;

// ZigbeeAnalog with access to its cluster list, so attributes can be marked reportable before the
// endpoint is registered. The stack sizes each endpoint's reporting table at registration from the
// attributes that are reportable at that point, so adding reporting info afterwards fails with
// ESP_ERR_NO_MEM.
class SoilMoistureAnalog : public ZigbeeAnalog
{
public:
  using ZigbeeAnalog::ZigbeeAnalog;

  bool setAnalogInputAttributeReportable(uint16_t attributeId)
  {
    esp_zb_attribute_list_t *attr =
      esp_zb_cluster_list_get_cluster(_cluster_list, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    for (; attr != nullptr; attr = attr->next)
    {
      if (attr->attribute.id == attributeId)
      {
        attr->attribute.access |= ESP_ZB_ZCL_ATTR_ACCESS_REPORTING;
        return true;
      }
    }
    Serial.printf("Analog input attribute 0x%04x not found in cluster list.\n", attributeId);
    return false;
  }
};

SoilMoistureAnalog zbSoilMoistureSensor = SoilMoistureAnalog(MOISTURE_SENSOR_ENDPOINT_NUMBER);
ZigbeeAnalog zbBatteryVoltage = ZigbeeAnalog(BATTERY_VOLTAGE_ENDPOINT_NUMBER);

RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR int lastBatteryPercentage = -10;
RTC_DATA_ATTR int lastSoilMoisturePercentage = -10;

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

void reportBatteryPercentage(float batteryPercentage)
{
  // Update.
  zbTempSensor.setBatteryPercentage(batteryPercentage);

  // Report.
  zbTempSensor.reportBatteryPercentage();
}

void powerOnSoilSensor()
{
    digitalWrite(SOIL_POWER_PIN, HIGH);
    soilPowerOnTime = millis();
}

float measureSoilMoisture()
{
    // Wait for whatever is left of the stabilization time. The other sensors are read in the meantime,
    // so this is usually shorter than SOIL_MOISTURE_POWER_DELAY.
    unsigned long poweredFor = millis() - soilPowerOnTime;
    if (poweredFor < SOIL_MOISTURE_POWER_DELAY)
    {
      delay(SOIL_MOISTURE_POWER_DELAY - poweredFor);
    }

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
    return roundPercentageDown(moisturePercentage, 1);
}

void reportSoilMoisture(float soilMoisturePercentage)
{
  // Update.
  int soilMoisturePercentageScaled = soilMoisturePercentage * 10;
  zbSoilMoistureSensor.setAnalogInput(soilMoisturePercentageScaled);

  // Report.
  zbSoilMoistureSensor.reportAnalogInput();
}

bool reportSoilMoistureAttribute(uint16_t attributeId)
{
  // Modelled on ZigbeeAnalog::reportAnalogInput(), but for an arbitrary genAnalogInput attribute.
  esp_zb_zcl_report_attr_cmd_t report_attr_cmd;
  memset(&report_attr_cmd, 0, sizeof(report_attr_cmd));
  report_attr_cmd.address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT;
  report_attr_cmd.attributeID = attributeId;
  report_attr_cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
  report_attr_cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT;
  report_attr_cmd.zcl_basic_cmd.src_endpoint = MOISTURE_SENSOR_ENDPOINT_NUMBER;
  report_attr_cmd.manuf_specific = 0x00U;
  report_attr_cmd.dis_default_resp = 0x00U;

  if (!esp_zb_lock_acquire(portMAX_DELAY))
  {
    Serial.println("Failed to acquire Zigbee lock for attribute report.");
    return false;
  }

  // The stack defines the metadata attributes without the REPORTING access flag, and
  // esp_zb_zcl_report_attr_cmd_req() rejects those with ESP_ERR_NOT_SUPPORTED. Mark the
  // attribute reportable first.
  esp_zb_zcl_attr_t *attr = esp_zb_zcl_get_attribute(MOISTURE_SENSOR_ENDPOINT_NUMBER, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
                                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attributeId);
  if (attr == nullptr)
  {
    esp_zb_lock_release();
    Serial.printf("Analog input attribute 0x%04x not found.\n", attributeId);
    return false;
  }
  attr->access |= ESP_ZB_ZCL_ATTR_ACCESS_REPORTING;

  // esp_zb_zcl_report_attr_cmd_req() asserts (esp_zigbee_zcl_command.c:263) when the attribute has
  // no reporting info entry, so create one. Intervals of 0 mean the stack never reports it on its
  // own; it is only sent by the explicit report below.
  esp_zb_zcl_attr_location_info_t location = {};
  location.endpoint_id = MOISTURE_SENSOR_ENDPOINT_NUMBER;
  location.cluster_id = ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT;
  location.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE;
  location.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
  location.attr_id = attributeId;
  if (esp_zb_zcl_find_reporting_info(location) == nullptr)
  {
    esp_zb_zcl_reporting_info_t reporting_info;
    memset(&reporting_info, 0, sizeof(reporting_info));
    reporting_info.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    reporting_info.ep = MOISTURE_SENSOR_ENDPOINT_NUMBER;
    reporting_info.cluster_id = ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT;
    reporting_info.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE;
    reporting_info.attr_id = attributeId;
    reporting_info.dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    reporting_info.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;

    esp_err_t infoRet = esp_zb_zcl_update_reporting_info(&reporting_info);
    if (infoRet != ESP_OK || esp_zb_zcl_find_reporting_info(location) == nullptr)
    {
      esp_zb_lock_release();
      Serial.printf("Failed to set up reporting for analog input attribute 0x%04x: %s\n", attributeId, esp_err_to_name(infoRet));
      return false;
    }
  }

  esp_err_t ret = esp_zb_zcl_report_attr_cmd_req(&report_attr_cmd);
  esp_zb_lock_release();

  if (ret != ESP_OK)
  {
    Serial.printf("Failed to report analog input attribute 0x%04x: %s\n", attributeId, esp_err_to_name(ret));
    return false;
  }
  return true;
}

void reportSoilMoistureMetadata()
{
  reportSoilMoistureAttribute(ESP_ZB_ZCL_ATTR_ANALOG_INPUT_APPLICATION_TYPE_ID);  // 0x0100
  reportSoilMoistureAttribute(ESP_ZB_ZCL_ATTR_ANALOG_INPUT_MIN_PRESENT_VALUE_ID); // 0x0045
  reportSoilMoistureAttribute(ESP_ZB_ZCL_ATTR_ANALOG_INPUT_MAX_PRESENT_VALUE_ID); // 0x0041
  reportSoilMoistureAttribute(ESP_ZB_ZCL_ATTR_ANALOG_INPUT_RESOLUTION_ID);        // 0x006A
  // Description is a character string; it is reported last so the numeric attributes are already
  // out if the stack turns out not to handle reporting it.
  reportSoilMoistureAttribute(ESP_ZB_ZCL_ATTR_ANALOG_INPUT_DESCRIPTION_ID);       // 0x001C

  // Small delay to allow the reports to be sent.
  delay(TRANSMISSION_DELAY);
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

void reportIlluminance(int illuminance)
{
  if (illuminance < 0)
  {
    return;
  }
  // Update.
  int zigbee_illuminance = 10000 * log10(illuminance);
  zbIlluminanceSensor.setIlluminance(zigbee_illuminance);

  // Report.
  zbIlluminanceSensor.report();
}

void measureTemperatureAndHumidity(float* temperature, float* humidity)
{
  // Measure.
  *temperature =  aht20.getTemperature();
  *humidity = aht20.getHumidity();
}

void reportTemperatureAndHumidity(float temperature, float humidity)
{
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

int roundPercentageDown(int percentage, int remainder)
{
  return percentage - (percentage % remainder);
}

void configureZigbeeStack()
{
  // Set Zigbee device name and model
  zbTempSensor.setManufacturerAndModel("LarsvdLee", "SleepyPlantSensor");

  // Set minimum and maximum temperature measurement value.
  zbTempSensor.setMinMaxValue(10, 50);

  // Set tolerance for temperature measurement in °C (lowest possible value is 0.01°C)
  zbTempSensor.setTolerance(1);

  // Set power source to battery and set battery percentage to the last measured value.
  zbTempSensor.setPowerSource(ZB_POWER_SOURCE_BATTERY, lastBatteryPercentage);

  // Add humidity cluster to the temperature sensor device with min, max and tolerance values
  zbTempSensor.addHumiditySensor(0, 100, 1);

  // Set minimum and maximum for raw illuminance value (0 min and 50000 max equals to 0 lux - 100,000 lux)
  zbIlluminanceSensor.setMinMaxValue(0, 50000);

  // Set tolerance for raw illuminance value
  zbIlluminanceSensor.setTolerance(1);

  // Setup the analog device.
  zbSoilMoistureSensor.addAnalogInput();
  zbSoilMoistureSensor.setAnalogInputDescription("Soil moisture");
  zbSoilMoistureSensor.setAnalogInputApplication(0x04 << 16); // type 4 = percentage -> Z2M unit "%", skips engineeringUnits read
  zbSoilMoistureSensor.setAnalogInputMinMax(0, 1000);         // value is reported as percent x 10
  zbSoilMoistureSensor.setAnalogInputResolution(1);
  // Must happen before Zigbee.begin() so the stack reserves reporting slots for these attributes.
  zbSoilMoistureSensor.setAnalogInputAttributeReportable(ESP_ZB_ZCL_ATTR_ANALOG_INPUT_DESCRIPTION_ID);
  zbSoilMoistureSensor.setAnalogInputAttributeReportable(ESP_ZB_ZCL_ATTR_ANALOG_INPUT_APPLICATION_TYPE_ID);
  zbSoilMoistureSensor.setAnalogInputAttributeReportable(ESP_ZB_ZCL_ATTR_ANALOG_INPUT_MIN_PRESENT_VALUE_ID);
  zbSoilMoistureSensor.setAnalogInputAttributeReportable(ESP_ZB_ZCL_ATTR_ANALOG_INPUT_MAX_PRESENT_VALUE_ID);
  zbSoilMoistureSensor.setAnalogInputAttributeReportable(ESP_ZB_ZCL_ATTR_ANALOG_INPUT_RESOLUTION_ID);
  analogReadResolution(12);

  // Add endpoints to Zigbee Core
  Zigbee.addEndpoint(&zbIlluminanceSensor);
  Zigbee.addEndpoint(&zbTempSensor);
  Zigbee.addEndpoint(&zbSoilMoistureSensor);

  // For battery powered devices, it can be better to set timeout for Zigbee Begin to lower value to save battery
  // If the timeout has been reached, the network channel mask will be reset and the device will try to connect again after reset (scanning all channels)
  Zigbee.setTimeout(10000);  // Set timeout for Zigbee Begin to 10s (default is 30s)
}

void startZigbee()
{
  // Create a custom Zigbee configuration for End Device with keep alive 10s to avoid interference with reporting data
  esp_zb_cfg_t zigbeeConfig = ZIGBEE_DEFAULT_ED_CONFIG();
  zigbeeConfig.nwk_cfg.zed_cfg.keep_alive = 20000;

  // When all EPs are registered, start Zigbee in End Device mode. If the network can't be reached
  // (coordinator down, out of range), go back to sleep and try again at the next wake-up instead of
  // staying awake with the radio on.
  if (!Zigbee.begin(&zigbeeConfig, false)) {
    Serial.println("Zigbee failed to start, sleeping until the next wake-up.");
    goToSleep();
  }
  // Pairing takes longer, so allow more time on the first boots.
  unsigned long connectTimeout = bootCount < SLOW_BOOTS ? PAIRING_DELAY : CONNECT_TIMEOUT;
  unsigned long connectStart = millis();
  Serial.println("Connecting to network");
  while (!Zigbee.connected()) {
    if (millis() - connectStart > connectTimeout) {
      Serial.println();
      Serial.println("Could not connect to the Zigbee network, sleeping until the next wake-up.");
      goToSleep();
    }
    Serial.print(".");
    delay(100);
  }
  Serial.println();
  Serial.println("Successfully connected to Zigbee network");

  zbTempSensor.setReporting(120, 120, 10);
  zbTempSensor.setHumidityReporting(120,120,10);
  zbIlluminanceSensor.setReporting(120, 120, 10);
  zbSoilMoistureSensor.setAnalogInputReporting(120, 120, 10);

  // Zigbee2MQTT reads the analog input metadata at startup, which times out for sleepy devices.
  // Reporting it lets Z2M cache the values and skip those reads. Do this on the first boot after
  // power-up and then roughly once a day (96 * 898 s).
  if (bootCount == 1 || bootCount % 96 == 0)
  {
    reportSoilMoistureMetadata();
  }
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
  pinMode(SOIL_POWER_PIN, OUTPUT);

  // Power the soil moisture sensor first and do the other measurements while it stabilizes, all
  // before the radio is started. This keeps the awake time and the radio on-time short.
  powerOnSoilSensor();

  // Connect to the AHT20 and BH1750 sensors.
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

  // The AHT20 takes ~80 ms per measurement and the BH1750 up to ~180 ms; both fit in the soil sensor's
  // stabilization time.
  measureTemperatureAndHumidity(&temperature, &humidity);
  illuminance = measureIlluminance();
  soil_moisture = measureSoilMoisture();

  configureZigbeeStack();
  startZigbee();
}

void loop() {
  // The initial pairing is slower, so we wait longer the first times we boot before sending data. We do this before the factory
  // reset check, so if the user wants to factory reset the device, they have more time to do so.
  if (bootCount < SLOW_BOOTS)
  {
    blinkAndWait(PAIRING_DELAY, FAST_BLINK_SPEED);
  }

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

  int batteryPercentage = measureBatteryPercentage();

  if (batteryPercentage != lastBatteryPercentage)
  {
    lastBatteryPercentage = batteryPercentage;
    reportBatteryPercentage(batteryPercentage);
  }

  reportIlluminance(illuminance);
  reportSoilMoisture(soil_moisture);
  reportTemperatureAndHumidity(temperature, humidity);

  // Small delay to allow all reporting to finish.
  delay(TRANSMISSION_DELAY);
  
  goToSleep();
}
