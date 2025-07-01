#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <AHT20.h>
#include <BH1750.h>
#include <time.h>

#define SOIL_MOISTURE_PIN A1
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""

#define MAX_ENTRIES 72

WebServer server(80);
AHT20 aht;  // AHT30 sensor
BH1750 lightMeter;

// Data structures
struct Measurement {
  time_t timestamp;
  float temperature;
  float humidity;
  float illuminance;
  int soilMoisture;
};

Measurement data[MAX_ENTRIES];
int dataIndex = 0;
int dataCount = 0;

void syncTime() {
  configTime(0, 0, "pool.ntp.org");
  while (time(nullptr) < 100000) {
    delay(500);
  }
  Serial.println("Time synced");
}

void addMeasurement(float temp, float hum, float illum, int soil) {
  Measurement m;
  m.timestamp = time(nullptr);
  m.temperature = temp;
  m.humidity = hum;
  m.illuminance = illum;
  m.soilMoisture = soil;

  data[dataIndex] = m;
  dataIndex = (dataIndex + 1) % MAX_ENTRIES;
  if (dataCount < MAX_ENTRIES) dataCount++;
}

String getChartData() {
  String json = "[";
  for (int i = 0; i < dataCount; i++) {
    // Fix: start from the oldest entry
    int idx = (dataIndex - dataCount + i + MAX_ENTRIES) % MAX_ENTRIES;
    Measurement m = data[idx];
    json += "{";
    json += "\"time\":" + String(m.timestamp) + ",";
    json += "\"temp\":" + String(m.temperature, 2) + ",";
    json += "\"hum\":" + String(m.humidity, 2) + ",";
    json += "\"illum\":" + String(m.illuminance, 1) + ",";
    json += "\"soil\":" + String(m.soilMoisture);
    json += "}";
    if (i != dataCount - 1) json += ",";
  }
  json += "]";
  return json;
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
      padding: 1.2em 2em;
      margin: 0.4em;
      border: none;
      background: transparent;
      font-size: 1.5em;
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
  </style>
</head>
<body>
  <header>🌿 Plant Sensor Dashboard</header>
  <nav>
    <button onclick="showTab('temp')" id="btn-temp">🌡 Temperature</button>
    <button onclick="showTab('hum')" id="btn-hum">💧 Humidity</button>
    <button onclick="showTab('illum')" id="btn-illum">☀️ Light</button>
    <button onclick="showTab('soil')" id="btn-soil">🌱 Soil</button>
    <button onclick="showTab('summary')" id="btn-summary">📊 Summary</button>
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
  </main>

  <footer>
    Data updates hourly – Last updated: <span id="last-updated">Loading...</span>
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
    function formatTime(time) {
      const d = new Date(time * 1000);
      const dayName = d.toLocaleDateString(undefined, { weekday: 'short' }); // e.g. "Mon"
      const timestamp = d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
      return `${dayName} ${timestamp}`;
    }

    function renderSummary(data) {
      const times = data.map(d => d.time);
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
      const maxTempTime = formatTime(data[maxTempIdx].time);
      const minTempTime = formatTime(data[minTempIdx].time);

      const maxHum = hums[maxHumIdx].toFixed(2);
      const minHum = hums[minHumIdx].toFixed(2);
      const maxHumTime = formatTime(data[maxHumIdx].time);
      const minHumTime = formatTime(data[minHumIdx].time);

      const maxIllum = illums[maxIllumIdx].toFixed(2);
      const minIllum = illums[minIllumIdx].toFixed(2);
      const maxIllumTime = formatTime(data[maxIllumIdx].time);
      const minIllumTime = formatTime(data[minIllumIdx].time);

      let totalDecline = soils[soils.length - 1] - soils[0];
      const d0 = times[0] * 1000;
      const d1 = times[times.length - 1] * 1000;
      const hours = (d1 - d0)/3600000;
      console.log(hours);
      const avgDecline = (totalDecline / hours).toFixed(3);

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

        <tr><td>Average Change in Soil Moisture</td><td>${avgDecline} % per hour</td></tr>
      `;
    }

    fetch('/data')
      .then(res => res.json())
      .then(data => {
        const labels = data.map(e => {
          const d = new Date(e.time * 1000);
          const dayName = d.toLocaleDateString(undefined, { weekday: 'short' }); // e.g. "Mon"
          const time = d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
          return `${dayName} ${time}`;
        });
        const temps = data.map(e => e.temp);
        const hums = data.map(e => e.hum);
        const illums = data.map(e => e.illum);
        const soils = data.map(e => e.soil);

        createChart(document.getElementById('canvas-temp'), 'Temperature (°C)', temps, '#e53935', labels);
        createChart(document.getElementById('canvas-hum'), 'Humidity (%)', hums, '#1e88e5', labels);
        createChart(document.getElementById('canvas-illum'), 'Illuminance (lux)', illums, '#ffb300', labels);
        createChart(document.getElementById('canvas-soil'), 'Soil Moisture (%)', soils, '#43a047', labels);

        const now = new Date();
        document.getElementById('last-updated').textContent = now.toLocaleString();
        document.getElementById('btn-temp').classList.add('active');
        renderSummary(data);
      });
  </script>
</body>
</html>
)rawliteral");
}

void handleData() {
  server.send(200, "application/json", getChartData());
}

void setup() {
  Serial.begin(115200);
  Wire.begin();

  // Static IP configuration
  IPAddress local_IP(192, 168, 0, 199);
  IPAddress gateway(192, 168, 0, 1);
  IPAddress subnet(255, 255, 255, 0);
  IPAddress dns(8, 8, 8, 8);  // Optional

  if (!WiFi.config(local_IP, gateway, subnet, dns)) {
    Serial.println("Failed to configure static IP");
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi");
  Serial.println(WiFi.localIP());

  syncTime();

  aht.begin();
  lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
}

unsigned long lastMeasureTime = 0;

void loop() {
  server.handleClient();

  if (millis() - lastMeasureTime > 3600000UL || lastMeasureTime == 0) {  // every 1 hour
    lastMeasureTime = millis();
    float temp = aht.getTemperature();
    float hum = aht.getHumidity();
    float illum = lightMeter.readLightLevel();
    int soil_raw = analogRead(SOIL_MOISTURE_PIN);
    int soil = (1 - ((soil_raw - 1580.0) / (2650.0 - 1580))) * 100;

    Serial.printf("Temp: %.2f, Hum: %.2f, Illum: %.2f, Soil: %d\n", temp, hum, illum, soil);
    addMeasurement(temp, hum, illum, soil);
  }
}
