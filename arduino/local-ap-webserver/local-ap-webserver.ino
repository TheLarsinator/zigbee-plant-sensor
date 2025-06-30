#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <AHT20.h>
#include <BH1750.h>

// AP credentials
const char* ssid = "PlantSensor_ZPS00X";
const char* password = "ZPS00X-P";

WebServer server(80);

#define MAX_MEASUREMENTS 48
#define SOIL_MOISTURE_PIN A1

AHT20 aht;  // AHT30 sensor
BH1750 lightMeter;

struct Measurement {
  float temp;
  float hum;
  float illum;
  float soil;
};

Measurement measurements[MAX_MEASUREMENTS];
int writeIndex = 0;
int storedCount = 0;

unsigned long lastMeasurementMillis = 0;
const unsigned long measurementInterval = 3600000; // 1 hour = 3600000 ms

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin();
  aht.begin();
  lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);

  // Start WiFi AP
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);

  // Initialize measurements with zeros
  for (int i = 0; i < MAX_MEASUREMENTS; i++) {
    measurements[i] = {0,0,0,0};
  }

  // Setup web server routes
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();

  unsigned long now = millis();
  if (now - lastMeasurementMillis >= measurementInterval) {
    lastMeasurementMillis = now;
    takeMeasurement();
  }
}

// Simulated sensor reading function — replace with real sensors
void takeMeasurement() {
  float t = aht.getTemperature();
  float h = aht.getHumidity();
  float i = lightMeter.readLightLevel();
  float s = (1 - ((analogRead(SOIL_MOISTURE_PIN) - 1580.0) / (2650.0 - 1580))) * 100;

  addMeasurement(t,h,i,s);

  Serial.printf("Measured: T=%.2f, H=%.2f, Illum=%.1f, Soil=%.3f\n", t, h, i, s);
}

void addMeasurement(float t, float h, float i, float s) {
  measurements[writeIndex] = {t,h,i,s};
  writeIndex = (writeIndex + 1) % MAX_MEASUREMENTS;
  if (storedCount < MAX_MEASUREMENTS) storedCount++;
}

void handleData() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();

  int oldestIndex = (writeIndex + MAX_MEASUREMENTS - storedCount) % MAX_MEASUREMENTS;
  
  for (int i = 0; i < storedCount; i++) {
    int idx = (oldestIndex + i) % MAX_MEASUREMENTS;
    JsonObject obj = arr.createNestedObject();
    obj["hoursAgo"] = storedCount - 1 - i;  // 0 newest, increasing backwards
    obj["temp"] = measurements[idx].temp;
    obj["hum"] = measurements[idx].hum;
    obj["illum"] = measurements[idx].illum;
    obj["soil"] = measurements[idx].soil;
  }

  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

void handleRoot() {
  server.send_P(200, "text/html", R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>Sensor Charts</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body, html { height: 100%; font-family: sans-serif; display: flex; flex-direction: column; }
    header { background: #2196f3; color: white; padding: 1em; text-align: center; font-size: 1.5em; }
    nav { display: flex; background: #e0e0e0; }
    nav button {
      flex: 1; padding: 10px; font-size: 1em; border: none;
      background: #f5f5f5; cursor: pointer;
    }
    nav button.active {
      background: #cfd8dc; font-weight: bold;
    }
    .tab-content {
      flex: 1; display: none; padding: 8px;
    }
    .tab-content.active {
      display: block;
    }
    canvas {
      width: 100% !important;
      height: 100% !important;
    }
  </style>
</head>
<body>
  <header>Sensor Charts</header>
  <nav>
    <button onclick="showTab('temp')" id="btn-temp">Temperature</button>
    <button onclick="showTab('hum')" id="btn-hum">Humidity</button>
    <button onclick="showTab('illum')" id="btn-illum">Illuminance</button>
    <button onclick="showTab('soil')" id="btn-soil">Soil Moisture</button>
  </nav>

  <div id="tab-temp" class="tab-content active"><canvas id="canvas-temp"></canvas></div>
  <div id="tab-hum" class="tab-content"><canvas id="canvas-hum"></canvas></div>
  <div id="tab-illum" class="tab-content"><canvas id="canvas-illum"></canvas></div>
  <div id="tab-soil" class="tab-content"><canvas id="canvas-soil"></canvas></div>

  <script>
    function showTab(id) {
      document.querySelectorAll('.tab-content').forEach(div => div.classList.remove('active'));
      document.querySelectorAll('nav button').forEach(btn => btn.classList.remove('active'));
      document.getElementById('tab-' + id).classList.add('active');
      document.getElementById('btn-' + id).classList.add('active');
    }

    function createChart(ctx, label, values, color, labels) {
      return new Chart(ctx, {
        type: 'line',
        data: {
          labels: labels,
          datasets: [{
            label,
            data: values,
            borderColor: color,
            backgroundColor: 'transparent',
            pointRadius: 2,
            pointHoverRadius: 4,
            tension: 0.3
          }]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          scales: {
            x: {
              ticks: {
                maxRotation: 60,
                minRotation: 45
              }
            }
          },
          plugins: {
            legend: {
              display: true
            }
          }
        }
      });
    }

    function formatLabel(hoursAgo) {
      const date = new Date(Date.now() - hoursAgo * 3600000);
      const options = { weekday: 'short', hour: '2-digit', hour12: false };
      return date.toLocaleString('en-US', options);
    }

    fetch('/data')
      .then(res => res.json())
      .then(raw => {
        const labels = raw.map(d => formatLabel(d.hoursAgo));
        const temps = raw.map(d => d.temp);
        const hums = raw.map(d => d.hum);
        const illums = raw.map(d => d.illum);
        const soils = raw.map(d => d.soil);

        createChart(document.getElementById('canvas-temp'), 'Temperature (°C)', temps, 'red', labels);
        createChart(document.getElementById('canvas-hum'), 'Humidity (%)', hums, 'blue', labels);
        createChart(document.getElementById('canvas-illum'), 'Illuminance (lux)', illums, 'orange', labels);
        createChart(document.getElementById('canvas-soil'), 'Soil Moisture', soils, 'green', labels);

        document.getElementById('btn-temp').classList.add('active');
      });
  </script>
</body>
</html>
)rawliteral");
}

