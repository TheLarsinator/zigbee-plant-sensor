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
  String html = R"rawliteral(
  <!DOCTYPE html><html lang="en"><head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Sensor Charts</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
      * { box-sizing: border-box; }
      body {
        font-family: sans-serif;
        margin: 0;
        padding: 20px;
        background: #f9f9f9;
      }
      h2 { text-align: center; margin-bottom: 20px; }

      .tabs {
        display: flex;
        flex-wrap: wrap;
        justify-content: center;
        gap: 10px;
        margin-bottom: 10px;
      }

      .tab {
        padding: 10px 15px;
        background: #e0e0e0;
        border-radius: 5px;
        cursor: pointer;
        transition: background 0.2s;
      }

      .tab.active {
        background: #007bff;
        color: white;
      }

      .chart-container {
        display: none;
        width: 100%;
        max-width: 800px;
        margin: auto;
        background: white;
        padding: 10px;
        border-radius: 10px;
        box-shadow: 0 0 10px rgba(0,0,0,0.05);
      }

      .chart-container.active {
        display: block;
      }

      canvas {
        width: 100% !important;
        height: auto !important;
        max-height: 350px;
      }
    </style>
  </head><body>
    <h2>Sensor Data (Last 72 Hours)</h2>
    <div class="tabs">
      <div class="tab active" data-chart="tempChart">Temperature</div>
      <div class="tab" data-chart="humChart">Humidity</div>
      <div class="tab" data-chart="illumChart">Illuminance</div>
      <div class="tab" data-chart="soilChart">Soil Moisture</div>
    </div>

    <div id="tempChart" class="chart-container active"><canvas></canvas></div>
    <div id="humChart" class="chart-container"><canvas></canvas></div>
    <div id="illumChart" class="chart-container"><canvas></canvas></div>
    <div id="soilChart" class="chart-container"><canvas></canvas></div>

    <script>
      document.querySelectorAll('.tab').forEach(tab => {
        tab.addEventListener('click', () => {
          document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
          document.querySelectorAll('.chart-container').forEach(c => c.classList.remove('active'));
          tab.classList.add('active');
          document.getElementById(tab.dataset.chart).classList.add('active');
        });
      });

      fetch("/data").then(r => r.json()).then(data => {
        const labels = data.map(e => {
          const d = new Date(e.time * 1000);
          const dayName = d.toLocaleDateString(undefined, { weekday: 'short' }); // e.g. "Mon"
          const time = d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
          return `${dayName} ${time}`;
        });
        const temp = data.map(e => e.temp);
        const hum = data.map(e => e.hum);
        const illum = data.map(e => e.illum);
        const soil = data.map(e => e.soil);

        const config = (label, data, color) => ({
          type: 'line',
          data: {
            labels,
            datasets: [{ label, data, borderColor: color, backgroundColor: color + '22', fill: false }]
          },
          options: {
            responsive: true,
            plugins: {
              legend: { display: true },
              tooltip: { mode: 'index', intersect: false }
            },
            scales: {
              x: { ticks: { autoSkip: true, maxTicksLimit: 10 } },
              y: { beginAtZero: true }
            }
          }
        });

        new Chart(document.querySelector("#tempChart canvas"), config("Temperature (°C)", temp, "red"));
        new Chart(document.querySelector("#humChart canvas"), config("Humidity (%)", hum, "blue"));
        new Chart(document.querySelector("#illumChart canvas"), config("Illuminance (lx)", illum, "orange"));
        new Chart(document.querySelector("#soilChart canvas"), config("Soil Moisture (%)", soil, "green"));
      });
    </script>
  </body></html>
  )rawliteral";

  server.send(200, "text/html", html);
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

  if (millis() - lastMeasureTime > 10000UL || lastMeasureTime == 0) {  // every 1 hour
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
