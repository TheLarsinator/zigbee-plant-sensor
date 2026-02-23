#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <AHT20.h>
#include <BH1750.h>
#include <Preferences.h>

// AP credentials
const char* ssid = "PlantSensor_ZPS00X";
const char* password = "ZPS00X-P";

WebServer server(80);

#define MAX_MEASUREMENTS 288
#define SOIL_MOISTURE_PIN A1

AHT20 aht;  // AHT30 sensor
BH1750 lightMeter;

Preferences preferences;
float temperatureOffset = 0.0;
int soilMoistureMin = 0.0;
int soilMoistureMax = 0.0;

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
const unsigned long measurementInterval = 900000; // 1 hour = 3600000 ms

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin();
  aht.begin();
  lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);

  // Start WiFi AP
  WiFi.softAP(ssid);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);

  // Initialize measurements with zeros
  for (int i = 0; i < MAX_MEASUREMENTS; i++) {
    measurements[i] = {0,0,0,0};
  }

  preferences.begin("calibration", false);
  temperatureOffset = preferences.getFloat("tempOffset", 0.0);
  soilMoistureMin = preferences.getInt("soilMoistureMin", 0.0);
  soilMoistureMax = preferences.getInt("soilMoistureMax", 0.0);

  // Setup web server routes
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/set_offset", HTTP_POST, handleSetOffset);
  server.on("/config", handleConfig);

  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();

  unsigned long now = millis();
  if (now - lastMeasurementMillis >= measurementInterval || lastMeasurementMillis == 0) {
    lastMeasurementMillis = now;
    takeMeasurement();
  }
}

// Simulated sensor reading function — replace with real sensors
void takeMeasurement() {
  float t = aht.getTemperature();
  float h = aht.getHumidity();
  float i = lightMeter.readLightLevel();
  float s = (1 - ((analogRead(SOIL_MOISTURE_PIN) - soilMoistureMin) / (soilMoistureMax + 1.0F - soilMoistureMin))) * 100;

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
    obj["temp"] = measurements[idx].temp + temperatureOffset;
    obj["hum"] = measurements[idx].hum;
    obj["illum"] = measurements[idx].illum;
    obj["soil"] = measurements[idx].soil;
  }

  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

void handleSetOffset() {
  if (server.hasArg("plain") == false) {
    server.send(400, "application/json", "{\"error\":\"Missing body\"}");
    return;
  }

  String body = server.arg("plain");
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, body);

  if (error) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  float offsetNew = doc["offset"] | 0.0;
  int soilMinNew = doc["soilMin"] | 0.0;
  int soilMaxNew = doc["soilMax"] | 0.0;

  if (offsetNew != 0.0 && offsetNew != temperatureOffset) {
    preferences.putFloat("tempOffset", offsetNew);
    Serial.printf("New temperature offset: %.2f\n", offsetNew);
    temperatureOffset = offsetNew;
  }
  if (soilMinNew != 0.0 && soilMinNew != soilMoistureMin) {
    preferences.putInt("soilMoistureMin", soilMinNew);
    Serial.print("New soil moisture min: ");
    Serial.println(soilMinNew);
    soilMoistureMin = soilMinNew;
  }
  if (soilMaxNew != 0.0 && soilMaxNew != soilMoistureMax) {
    preferences.putInt("soilMoistureMax", soilMaxNew);
    Serial.print("New soil moisture max: ");
    Serial.println(soilMaxNew);
    soilMoistureMax = soilMaxNew;
  }

  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleConfig() {
  StaticJsonDocument<200> json;

  json["tempOffset"] = temperatureOffset; // existing
  json["soilMin"] = soilMoistureMin;  // new
  json["soilMax"] = soilMoistureMax;  // new

  String response;
  serializeJson(json, response);
  server.send(200, "application/json", response);
}

void handleRoot() {
  server.send_P(200, "text/html", R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>Plant Sensor Dashboard</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    html, body {
      height: 100%;
      margin: 0;
      font-family: sans-serif;
      display: flex;
      flex-direction: column;
      box-shadow: 0 2px 4px rgba(0,0,0,0.2);
    }

    header {
      background: #2e7d32;
      color: white;
      padding: 1em;
      text-align: center;
      font-size: 1.5em;
    }

    nav {
      display: flex;
      background: #c8e6c9;
      justify-content: center;
      box-shadow: 0 1px 2px rgba(0,0,0,0.1);
    }

    nav button {
      padding: 0.3em 0.3em;
      margin: 0.4em;
      border: none;
      background: transparent;
      font-size: 4em;
      font-weight: 600;
      border-radius: 10px;
      cursor: pointer;
      transition: background 0.2s, transform 0.1s;
    }

    nav button:hover {
      background: #a5d6a7;
      transform: scale(1.05);
    }

    nav button.active {
      background: #66bb6a;
      color: white;
      font-weight: 700;
    }

    /* Allow main content to grow and fill all remaining space */
    main {
      flex: 1;
      display: flex;
      flex-direction: column;
      padding: 0.5em;
      overflow: hidden;
    }

    /* Make each tab fill all space inside <main> */
    .tab-content {
      flex: 1;
      display: none;
      padding: 0;
      margin: 0;
    }

    .tab-content.active {
      display: flex;
      flex-direction: column;
    }

    canvas {
      flex: 1;
      width: 100% !important;
      height: 100% !important;
    }

    footer {
      text-align: center;
      padding: 0.8em;
      font-size: 0.9em;
      background: #eee;
      color: #555;
    }

    #summary-table {
      width: 100%;
      border-collapse: separate;
      border-spacing: 0 12px;
      font-size: 1.25em;
      font-weight: 500;
      color: #333;
    }

    #summary-table thead tr th {
      text-align: left;
      border-bottom: 2px solid #2196f3;
      padding-bottom: 8px;
      font-weight: 700;
      color: #2196f3;
    }

    #summary-table tbody tr {
      background: #f9fafb;
      box-shadow: 0 3px 6px rgba(33, 150, 243, 0.1);
      border-radius: 12px;
      transition: background 0.3s ease;
    }

    #summary-table tbody tr:hover {
      background: #e3f2fd;
    }

    #summary-table tbody tr td {
      padding: 12px 16px;
      border: none;
    }

    #summary-table tbody tr td:first-child {
      font-weight: 600;
      color: #1976d2;
    }
    
    #calibration-section h2 {
      color: #2e7d32;
      font-weight: 700;
      margin-bottom: 0.5em;
      font-size: 1.5em;
      text-align: center;
    }

    #calibration-section label {
      font-weight: 600;
      color: #2e7d32;
      display: block;
      margin-bottom: 0.3em;
      font-size: 1.2em;
    }

    #calibration-section input {
      width: 100%;
      padding: 0.6em 1em;
      font-size: 1.1em;
      border: 2px solid #66bb6a;
      border-radius: 10px;
      margin-bottom: 1em;
      box-sizing: border-box;
      transition: border-color 0.2s;
    }

    #calibration-section input:focus {
      border-color: #2e7d32;
      outline: none;
    }

    #calibration-section button {
      width: 100%;
      padding: 12px 0;
      font-size: 1.3em;
      background-color: #2e7d32;
      color: white;
      border-radius: 10px;
      border: none;
      font-weight: 700;
      cursor: pointer;
      transition: background-color 0.2s;
    }

    #calibration-section button:hover {
      background-color: #27632a;
    }
  </style>
</head>
<body>
  <header>🌿 Plant Sensor Dashboard</header>
  <nav>
    <button onclick="showTab('temp')" id="btn-temp">🌡️</button>
    <button onclick="showTab('hum')" id="btn-hum">☁️</button>
    <button onclick="showTab('illum')" id="btn-illum">☀️</button>
    <button onclick="showTab('soil')" id="btn-soil">🌱</button>
    <button onclick="showTab('summary')" id="btn-summary">📊</button>
    <button onclick="showTab('calibration')" id="btn-calibartion">⚙️</button>
  </nav>

  <main>
    <div id="tab-temp" class="tab-content active"><canvas id="canvas-temp"></canvas></div>
    <div id="tab-hum" class="tab-content"><canvas id="canvas-hum"></canvas></div>
    <div id="tab-illum" class="tab-content"><canvas id="canvas-illum"></canvas></div>
    <div id="tab-soil" class="tab-content"><canvas id="canvas-soil"></canvas></div>
    <div id="tab-summary" class="tab-content">
      <table id="summary-table">
        <thead>
          <tr>
            <th>Metric</th>
            <th>Value</th>
          </tr>
        </thead>
        <tbody>
          <!-- Will be populated via JS -->
        </tbody>
      </table>
    </div>
    <div id="tab-calibration" class="tab-content">
      <div style="font-size: 1.2em; max-width: 400px; margin: auto;" id="calibration-section">
        <label for="tempOffset">🌡️ Temperature Offset (°C):</label>
        <input type="number" step="0.1" id="tempOffset">

        <label for="soilMin">💧 Soil Moisture Wet:</label>
        <input type="number" step="1" id="soilMin">

        <label for="soilMax">🌵 Soil Moisture Dry:</label>
        <input type="number" step="1" id="soilMax">

        <button onclick="saveCalibration()">💾 Save</button>
      </div>
    </div>
  </main>

  <footer>
    Data updates every 15 minutes – Last updated: <span id="last-updated">Loading...</span>
  </footer>

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
            backgroundColor: 'rgba(0,0,0,0.03)',
            pointRadius: 2,
            pointHoverRadius: 4,
            fill: true,
            tension: 0.25
          }]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          scales: {
            x: {
              ticks: {
                maxRotation: 60,
                minRotation: 45,
                font: {
                  size: 30  // ← Bigger x-axis labels
                }
              }
            },
            y: {
              ticks: {
                font: {
                  size: 30  // ← Bigger y-axis labels
                }
              }
            }
          },
          plugins: {
            legend: {
              display: true,
              labels: {
                font: {
                  size: 30  // ← Bigger legend text
                }
              }
            }
          }
        }
      });
    }

    function formatLabel(hoursAgo) {
      const now = new Date();
      now.setMinutes(0, 0, 0); // truncate to full hour
      const date = new Date(now.getTime() - hoursAgo * 3600000);
      const day = date.toLocaleDateString('en-US', { weekday: 'short' });
      const hour = date.getHours().toString().padStart(2, '0');
      return `${day} ${hour}:00`;
    }

    // --- Stats Calculation ---
    function formatTime(hoursAgo) {
      const now = new Date();
      now.setMinutes(0, 0, 0); // truncate to full hour
      const date = new Date(now.getTime() - hoursAgo * 3600000);
      return date.toLocaleString('en-US', { weekday: 'short', hour: '2-digit', minute: '2-digit', hour12: false });
    }

    function saveCalibration() {
      console.log("Save calibration");
      const offset = parseFloat(document.getElementById("tempOffset").value);
      const soilMin = parseFloat(document.getElementById("soilMin").value);
      const soilMax = parseFloat(document.getElementById("soilMax").value);

      fetch("/set_offset", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ offset, soilMin, soilMax })
      }).then(res => {
        if (res.ok) {
          location.reload();
        } else {
          alert("Failed to save calibration.");
        }
      });
    }

    function renderSummary(data) {
      const temps = data.map(d => d.temp);
      const hums = data.map(d => d.hum);
      const illums = data.map(d => d.illum);
      const soils = data.map(d => d.soil);

      const avg = arr => (arr.reduce((a,b) => a + b, 0) / arr.length).toFixed(2);
      const maxIndex = arr => arr.indexOf(Math.max(...arr));
      const minIndex = arr => arr.indexOf(Math.min(...arr));

      const avgTemp = avg(temps);
      const maxTempIdx = maxIndex(temps);
      const minTempIdx = minIndex(temps);

      const avgHum = avg(hums);
      const maxHumIdx = maxIndex(hums);
      const minHumIdx = minIndex(hums);

      const avgIllum = avg(illums);
      const maxIllumIdx = maxIndex(illums);
      const minIllumIdx = minIndex(illums);

      const maxTemp = temps[maxTempIdx].toFixed(2);
      const minTemp = temps[minTempIdx].toFixed(2);
      const maxTempTime = formatTime(data[maxTempIdx].hoursAgo);
      const minTempTime = formatTime(data[minTempIdx].hoursAgo);

      const maxHum = hums[maxHumIdx].toFixed(2);
      const minHum = hums[minHumIdx].toFixed(2);
      const maxHumTime = formatTime(data[maxHumIdx].hoursAgo);
      const minHumTime = formatTime(data[minHumIdx].hoursAgo);

      const maxIllum = illums[maxIllumIdx].toFixed(2);
      const minIllum = illums[minIllumIdx].toFixed(2);
      const maxIllumTime = formatTime(data[maxIllumIdx].hoursAgo);
      const minIllumTime = formatTime(data[minIllumIdx].hoursAgo);

      let totalDecline = soils[0] - soils[soils.length - 1];
      const avgDecline = (totalDecline / (soils.length - 1)).toFixed(3);

      const table = document.querySelector("#summary-table tbody");
      table.innerHTML = `
        <tr><td>Average Temperature</td><td>${avgTemp} °C</td></tr>
        <tr><td>Highest Temperature</td><td>${maxTemp} °C at ${maxTempTime}</td></tr>
        <tr><td>Lowest Temperature</td><td>${minTemp} °C at ${minTempTime}</td></tr>

        <tr><td>Average Humidity</td><td>${avgHum} %</td></tr>
        <tr><td>Highest Humidity</td><td>${maxHum} % at ${maxHumTime}</td></tr>
        <tr><td>Lowest Humidity</td><td>${minHum} % at ${minHumTime}</td></tr>

        <tr><td>Average Illuminance</td><td>${avgIllum} lux</td></tr>
        <tr><td>Highest Illuminance</td><td>${maxIllum} lux at ${maxIllumTime}</td></tr>
        <tr><td>Lowest Illuminance</td><td>${minIllum} lux at ${minIllumTime}</td></tr>

        <tr><td>Average Decline in Soil Moisture</td><td>${avgDecline} % per hour</td></tr>
      `;
    }

    fetch('/data')
      .then(res => res.json())
      .then(data => {
        const labels = data.map(d => formatLabel(d.hoursAgo));
        const temps = data.map(d => d.temp);
        const hums = data.map(d => d.hum);
        const illums = data.map(d => d.illum);
        const soils = data.map(d => d.soil);

        createChart(document.getElementById('canvas-temp'), 'Temperature (°C)', temps, '#e53935', labels);
        createChart(document.getElementById('canvas-hum'), 'Humidity (%)', hums, '#1e88e5', labels);
        createChart(document.getElementById('canvas-illum'), 'Illuminance (lux)', illums, '#ffb300', labels);
        createChart(document.getElementById('canvas-soil'), 'Soil Moisture (%)', soils, '#43a047', labels);

        const now = new Date();
        document.getElementById('last-updated').textContent = now.toLocaleString();
        document.getElementById('btn-temp').classList.add('active');
        renderSummary(data);
      });

      window.addEventListener('DOMContentLoaded', () => {
        // Load current offset from ESP
        fetch("/config")
          .then(response => response.json())
          .then(data => {
            document.getElementById("tempOffset").value = data.tempOffset.toFixed(1);
            document.getElementById("soilMin").value = data.soilMin.toFixed(1);
            document.getElementById("soilMax").value = data.soilMax.toFixed(1);
          });
      });
  </script>
</body>
</html>
)rawliteral");
}

