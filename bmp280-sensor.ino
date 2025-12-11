#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <Adafruit_BMP280.h>
#include <time.h>

// Sketch version information
const char* SKETCH_NAME    = "bmp280-sensor";
const char* SKETCH_VERSION = "1.0.31"; // JS moved to /app.js, dual-axis fixed

// WiFi credentials
const char* ssid     = "Falconeer48_EXT";
const char* password = "0795418296";

// MQTT configuration
const char* mqtt_server = "192.168.50.232";
const char* mqtt_user   = "ian";
const char* mqtt_pass   = "Falcon1959";

// MQTT topics
const char* topic_pressure     = "home/bmp280/pressure";
const char* topic_temperature  = "home/bmp280/temperature";
const char* topic_availability = "home/bmp280/availability";

// NTP / time (South Africa = UTC+2)
const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec      = 2 * 3600;
const int   daylightOffset_sec = 0;

WiFiClient    espClient;
PubSubClient  client(espClient);
WebServer     server(80);

// SPI pins for BMP280
#define BMP_SCK  (18)
#define BMP_MISO (19)
#define BMP_MOSI (23)
#define BMP_CS   (5)

Adafruit_BMP280 bmp(BMP_CS, BMP_MOSI, BMP_MISO, BMP_SCK);

// Timers
unsigned long lastRead   = 0;
const unsigned long interval = 60000; // 1 minute

// Graph storage — 24h @ 3 min intervals
const int MAX_READINGS = 480;
const unsigned long GRAPH_INTERVAL = 180000; // 3 minutes

struct SensorReading {
  unsigned long timestamp;
  float pressure;
  float temperature;
};

SensorReading readings[MAX_READINGS];
int readingIndex   = 0;
int readingCount   = 0;
unsigned long lastGraphSave = 0;

// Live sensor values
float pressure     = 0.0;
float temperature  = 0.0;
bool  sensorOk     = false;
bool  otaInProgress = false;

// --- WiFi uptime & connection log ---
unsigned long wifiConnectTime = 0;

const int MAX_LOG_ENTRIES = 20;
struct LogEntry {
  unsigned long timestamp;
  String        message;
};
LogEntry connectionLog[MAX_LOG_ENTRIES];
int logCount = 0;
int logIndex = 0;

// Forward declarations
unsigned long currentEpoch();
String formatUptime(unsigned long millisec);

// ---------- Helpers ----------

void addLog(const String& msg) {
  unsigned long ts = currentEpoch();
  if (ts < 100000) {
    ts = millis() / 1000;
  }

  connectionLog[logIndex].timestamp = ts;
  connectionLog[logIndex].message   = msg;

  logIndex = (logIndex + 1) % MAX_LOG_ENTRIES;
  if (logCount < MAX_LOG_ENTRIES) {
    logCount++;
  }
}

String formatUptime(unsigned long millisec) {
  unsigned long seconds = millisec / 1000;
  unsigned long days    = seconds / 86400;
  seconds %= 86400;
  unsigned long hours   = seconds / 3600;
  seconds %= 3600;
  unsigned long minutes = seconds / 60;
  seconds %= 60;

  String up;
  if (days > 0)   up += String(days) + "d ";
  if (days > 0 || hours > 0)   up += String(hours) + "h ";
  if (days > 0 || hours > 0 || minutes > 0) up += String(minutes) + "m ";
  up += String(seconds) + "s";
  return up;
}

void setupTime() {
  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      Serial.println("Time synchronised via NTP");
    } else {
      Serial.println("Failed to obtain time");
    }
  }
}

unsigned long currentEpoch() {
  time_t nowEpoch = time(nullptr);
  if (nowEpoch < 100000) {
    return millis() / 1000;
  }
  return (unsigned long)nowEpoch;
}

// ---------- MQTT discovery ----------

void publishMQTTDiscovery() {
  String pressureConfig =
    "{\"name\":\"BMP280 Pressure\",\"state_topic\":\"home/bmp280/pressure\","
    "\"unit_of_measurement\":\"hPa\",\"device_class\":\"pressure\","
    "\"state_class\":\"measurement\",\"unique_id\":\"bmp280_pressure\","
    "\"availability_topic\":\"home/bmp280/availability\","
    "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
    "\"device\":{\"identifiers\":[\"bmp280_sensor\"],\"name\":\"BMP280 Sensor\","
    "\"manufacturer\":\"Adafruit\",\"model\":\"BMP280\"}}";

  client.publish("homeassistant/sensor/bmp280_pressure/config",
                 pressureConfig.c_str(), true);

  String tempConfig =
    "{\"name\":\"BMP280 Temperature\",\"state_topic\":\"home/bmp280/temperature\","
    "\"unit_of_measurement\":\"°C\",\"device_class\":\"temperature\","
    "\"state_class\":\"measurement\",\"unique_id\":\"bmp280_temperature\","
    "\"availability_topic\":\"home/bmp280/availability\","
    "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
    "\"device\":{\"identifiers\":[\"bmp280_sensor\"],\"name\":\"BMP280 Sensor\","
    "\"manufacturer\":\"Adafruit\",\"model\":\"BMP280\"}}";

  client.publish("homeassistant/sensor/bmp280_temperature/config",
                 tempConfig.c_str(), true);

  Serial.println("MQTT discovery published");
}

void reconnect() {
  if (WiFi.status() != WL_CONNECTED) return;

  while (!client.connected()) {
    Serial.println("MQTT reconnecting…");
    if (client.connect("ESP32-BMP280", mqtt_user, mqtt_pass,
                       topic_availability, 0, true, "offline")) {
      client.publish(topic_availability, "online", true);
      publishMQTTDiscovery();
      addLog("MQTT reconnected");
      Serial.println("MQTT connected");
    } else {
      delay(5000);
    }
  }
}

// ---------- Sensor ----------

void readSensor() {
  pressure    = bmp.readPressure() / 100.0F;
  temperature = bmp.readTemperature();

  sensorOk = (pressure > 800 && pressure < 1200 &&
              temperature > -40 && temperature < 85);

  if (sensorOk) {
    Serial.printf("Pressure %.1f hPa | Temp %.1f C\n", pressure, temperature);
  } else {
    Serial.println("Invalid BMP280 reading");
  }
}

// ---------- JS served as /app.js ----------

const char APP_JS[] PROGMEM = R"rawliteral(
function updateTextWithFade(id, text) {
  const el = document.getElementById(id);
  if (!el) return;
  if (el.textContent === text) return;
  el.style.opacity = 0;
  setTimeout(() => {
    el.textContent = text;
    el.style.opacity = 1;
  }, 300);
}

function updateLastRefreshTime() {
  const now = new Date();
  const t = now.toLocaleTimeString([], {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit'
  });
  updateTextWithFade('lastRefreshTime', t);
}

let chart = null;
let currentRange = '3h';

function setRange(range) {
  currentRange = range;

  const btn3  = document.getElementById('range3h');
  const btn12 = document.getElementById('range12h');
  const btn24 = document.getElementById('range24h');

  [btn3, btn12, btn24].forEach((btn) => {
    if (!btn) return;
    btn.classList.remove('active');
    btn.classList.add('secondary');
  });

  if (range === '3h' && btn3) {
    btn3.classList.add('active');
    btn3.classList.remove('secondary');
  }
  if (range === '12h' && btn12) {
    btn12.classList.add('active');
    btn12.classList.remove('secondary');
  }
  if (range === '24h' && btn24) {
    btn24.classList.add('active');
    btn24.classList.remove('secondary');
  }

  loadData();
}

function resetZoom() {
  if (chart && chart.resetZoom) chart.resetZoom();
}

function initChart() {
  const canvas = document.getElementById('sensorChart');
  if (!canvas) return;
  if (typeof Chart === 'undefined') {
    console.error('Chart.js not loaded');
    return;
  }

  const ctx = canvas.getContext('2d');

  chart = new Chart(ctx, {
    type: 'line',
    data: {
      datasets: [
        {
          label: 'Temperature',
          data: [],
          borderColor: '#f093fb',
          backgroundColor: 'rgba(240,147,251,0.1)',
          tension: 0.4,
          fill: true,
          pointRadius: 0,
          yAxisID: 'yTemp'
        },
        {
          label: 'Pressure',
          data: [],
          borderColor: '#667eea',
          backgroundColor: 'rgba(102,126,234,0.1)',
          tension: 0.4,
          fill: true,
          pointRadius: 0,
          yAxisID: 'yPressure'
        }
      ]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      spanGaps: true,
      interaction: {
        mode: 'index',
        intersect: false
      },
      plugins: {
        legend: {
          position: 'top',
          labels: {
            font: { size: 12 }
          }
        },
        zoom: {
          pan: {
            enabled: true,
            mode: 'x'
          },
          zoom: {
            wheel: { enabled: true },
            pinch: { enabled: true },
            mode: 'x'
          }
        }
      },
      scales: {
        x: {
          type: 'time',
          time: {
            unit: 'hour',
            tooltipFormat: 'MMM d HH:mm'
          },
          title: {
            display: true,
            text: 'Time'
          }
        },
        // Temperature on LEFT, master grid
        yTemp: {
          type: 'linear',
          position: 'left',
          title: {
            display: true,
            text: 'Temperature (°C)'
          },
          ticks: {
            callback: function(value) {
              return value.toFixed(1) + '°C';
            }
          },
          grid: {
            drawOnChartArea: true
          }
        },
        // Pressure on RIGHT, no grid to avoid duplication
        yPressure: {
          type: 'linear',
          position: 'right',
          title: {
            display: true,
            text: 'Pressure (hPa)'
          },
          ticks: {
            callback: function(value) {
              return value.toFixed(1) + ' hPa';
            }
          },
          grid: {
            drawOnChartArea: false
          }
        }
      }
    }
  });

  loadData();
}

function loadData() {
  if (!chart) return;

  fetch('/data')
    .then((r) => r.text())
    .then((text) => {
      if (!text) return;

      let data;
      try {
        data = JSON.parse(text);
      } catch (e) {
        console.error('JSON parse error for /data:', e, text);
        return;
      }

      if (!data || !data.data) return;

      const nowSec = data.now || Math.floor(Date.now() / 1000);
      let rangeSeconds = 3 * 3600;
      if (currentRange === '12h') rangeSeconds = 12 * 3600;
      else if (currentRange === '24h') rangeSeconds = 24 * 3600;

      let cutoff = nowSec - rangeSeconds;
      if (nowSec < 100000) cutoff = 0;

      const tempPoints     = [];
      const pressurePoints = [];
      let minTemp = Infinity;
      let maxTemp = -Infinity;

      data.data.forEach((item) => {
        if (cutoff && item.t < cutoff) return;
        const tsMs = item.t * 1000;

        const tempValid     = item.temperature > -100 && item.temperature < 100;
        const pressureValid = item.pressure > -100 && item.pressure < 2000;

        if (tempValid) {
          const tv = parseFloat(item.temperature);
          tempPoints.push({ x: tsMs, y: tv });
          if (!isNaN(tv)) {
            minTemp = Math.min(minTemp, tv);
            maxTemp = Math.max(maxTemp, tv);
          }
        } else {
          tempPoints.push({ x: tsMs, y: NaN });
        }

        if (pressureValid) {
          const pv = parseFloat(item.pressure);
          pressurePoints.push({ x: tsMs, y: pv });
        } else {
          pressurePoints.push({ x: tsMs, y: NaN });
        }
      });

      chart.data.datasets[0].data = tempPoints;     // Temperature
      chart.data.datasets[1].data = pressurePoints; // Pressure

      if (minTemp !== Infinity && maxTemp !== -Infinity) {
        const padding = 2;
        chart.options.scales.yTemp.min = minTemp - padding;
        chart.options.scales.yTemp.max = maxTemp + padding;
      } else {
        delete chart.options.scales.yTemp.min;
        delete chart.options.scales.yTemp.max;
      }

      chart.update();
    })
    .catch((err) => console.error('Error loading graph data:', err));
}

function loadStatus() {
  fetch('/status')
    .then((r) => r.json())
    .then((data) => {
      updateTextWithFade('pressureValue', data.pressure.toFixed(1));
      updateTextWithFade('tempValue',     data.temperature.toFixed(1));

      const ok = data.sensorOk;

      ['statusPressureIndicator', 'statusTempIndicator'].forEach((id) => {
        const el = document.getElementById(id);
        if (!el) return;
        el.classList.remove('status-ok', 'status-error');
        el.classList.add(ok ? 'status-ok' : 'status-error');
      });

      ['statusPressureText', 'statusTempText'].forEach((id) => {
        const el = document.getElementById(id);
        if (!el) return;
        el.classList.remove('status-text-ok', 'status-text-error');
        el.classList.add(ok ? 'status-text-ok' : 'status-text-error');
        el.textContent = ok ? 'Sensor Active' : 'Sensor Not Detected';
      });

      const w = document.getElementById('wifiSignalBadge');
      if (w) {
        let cls = 'badge-signal-weak';
        let txt = 'Weak (' + data.rssi + ' dBm)';
        if (data.rssi > -50) {
          cls = 'badge-signal-strong';
          txt = 'Strong (' + data.rssi + ' dBm)';
        } else if (data.rssi > -70) {
          cls = 'badge-signal-medium';
          txt = 'Medium (' + data.rssi + ' dBm)';
        }
        w.className = 'badge ' + cls;
        w.textContent = txt;
      }

      const m = document.getElementById('mqttStatusBadge');
      if (m) {
        m.className = 'badge ' + (data.mqtt ? 'badge-connected' : 'badge-disconnected');
        m.textContent = data.mqtt ? 'Connected' : 'Disconnected';
      }

      const o = document.getElementById('otaStatusBadge');
      if (o) {
        o.className = 'badge ' + (data.ota ? 'badge-updating' : 'badge-ready');
        o.textContent = data.ota ? 'Update in Progress' : 'Ready';
      }

      updateTextWithFade('wifiUptime', data.uptime || '—');

      const log = document.getElementById('logContainer');
      if (log && Array.isArray(data.log)) {
        let html = '';
        data.log.forEach((entry) => {
          html += '<div class=\"log-entry\"><span class=\"log-time\">' +
                  entry.time + '</span>' + entry.msg + '</div>';
        });
        log.innerHTML = html || '<em>No events yet</em>';
      }

      updateLastRefreshTime();
    })
    .catch((err) => console.error('Error loading status:', err));
}

window.addEventListener('load', () => {
  updateLastRefreshTime();
  initChart();
  loadStatus();
  loadData();

  const b3  = document.getElementById('range3h');
  const b12 = document.getElementById('range12h');
  const b24 = document.getElementById('range24h');
  const rz  = document.getElementById('resetZoomBtn');

  if (b3)  b3.addEventListener('click', () => setRange('3h'));
  if (b12) b12.addEventListener('click', () => setRange('12h'));
  if (b24) b24.addEventListener('click', () => setRange('24h'));
  if (rz)  rz.addEventListener('click', resetZoom);

  setInterval(loadData,   180000);
  setInterval(loadStatus, 10000);
});
)rawliteral";

// ---------- HTTP endpoints ----------

void sendGraphData() {
  unsigned long nowEpoch = currentEpoch();
  unsigned long cutoff   = (nowEpoch > 100000UL) ? nowEpoch - 24UL * 3600UL : 0;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  String header = "{\"now\":" + String(nowEpoch) + ",\"data\":[";
  server.sendContent(header);

  int start = (readingCount < MAX_READINGS ? 0 : readingIndex);
  int count = (readingCount < MAX_READINGS ? readingCount : MAX_READINGS);
  bool first = true;

  for (int i = 0; i < count; i++) {
    int idx = (start + i) % MAX_READINGS;
    if (readings[idx].timestamp == 0) continue;
    if (cutoff > 0 && readings[idx].timestamp < cutoff) continue;

    if (!first) server.sendContent(",");
    first = false;

    String point = "{\"t\":" + String(readings[idx].timestamp) +
                   ",\"pressure\":" + String(readings[idx].pressure, 1) +
                   ",\"temperature\":" + String(readings[idx].temperature, 1) +
                   "}";
    server.sendContent(point);
  }

  if (!first) server.sendContent(",");
  String live = "{\"t\":" + String(nowEpoch) +
                ",\"pressure\":" + String(sensorOk ? pressure : -127, 1) +
                ",\"temperature\":" + String(sensorOk ? temperature : -127, 1) +
                "}";
  server.sendContent(live);
  server.sendContent("]}");
}

void sendStatus() {
  int    rssi   = WiFi.RSSI();
  String uptime = "—";
  if (wifiConnectTime != 0) {
    uptime = formatUptime(millis() - wifiConnectTime);
  }

  String logJson = "[";
  if (logCount > 0) {
    int entries = min(logCount, MAX_LOG_ENTRIES);
    for (int i = entries - 1; i >= 0; i--) {
      int idx = (logIndex - 1 - i + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;
      if (logJson.length() > 1) {
        logJson += ",";
      }

      time_t ts = (time_t)connectionLog[idx].timestamp;
      struct tm timeinfo;
      bool timeValid = localtime_r(&ts, &timeinfo);
      char timeStr[20];

      if (timeValid) {
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
      } else {
        snprintf(timeStr, sizeof(timeStr), "Epoch %lu", connectionLog[idx].timestamp);
      }

      logJson += "{\"time\":\"" + String(timeStr) +
                 "\",\"msg\":\"" + connectionLog[idx].message + "\"}";
    }
  }
  logJson += "]";

  String json = "{";
  json += "\"pressure\":"   + String(pressure, 2) + ",";
  json += "\"temperature\":" + String(temperature, 2) + ",";
  json += "\"sensorOk\":"   + String(sensorOk ? "true" : "false") + ",";
  json += "\"rssi\":"       + String(rssi) + ",";
  json += "\"mqtt\":"       + String(client.connected() ? "true" : "false") + ",";
  json += "\"ota\":"        + String(otaInProgress ? "true" : "false") + ",";
  json += "\"uptime\":\""   + uptime + "\",";
  json += "\"log\":"        + logJson;
  json += "}";

  server.send(200, "application/json", json);
}

// ---------- Main page HTML ----------

void buildPage() {
  int rssi = WiFi.RSSI();
  String signalClass = (rssi > -50) ? "badge-signal-strong"
                      : (rssi > -70) ? "badge-signal-medium"
                                     : "badge-signal-weak";
  String signalText = String(rssi) + " dBm";

  String page =
    "<!DOCTYPE html><html lang='en'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
    "<title>BMP280 Sensor Dashboard</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box;}"
    "body{font-family:'Segoe UI',Tahoma,Geneva,Verdana,sans-serif;"
      "background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);"
      "min-height:100vh;padding:20px;}"
    ".container{max-width:900px;margin:0 auto;background:rgba(255,255,255,0.95);"
      "border-radius:20px;box-shadow:0 10px 40px rgba(0,0,0,0.2);padding:40px;}"
    "h1{color:#333;text-align:center;margin-bottom:10px;font-size:2.5em;font-weight:600;}"
    ".subtitle{text-align:center;color:#666;margin-bottom:30px;font-size:0.9em;}"
    ".sensor-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));"
      "gap:20px;margin-bottom:30px;}"
    ".sensor-card{background:#f8f9fa;border-radius:15px;padding:25px;"
      "box-shadow:0 4px 6px rgba(0,0,0,0.1);transition:transform 0.3s ease;}"
    ".sensor-card:hover{transform:translateY(-5px);"
      "box-shadow:0 6px 12px rgba(0,0,0,0.15);}"
    ".sensor-title{font-size:1.2em;font-weight:600;color:#555;margin-bottom:15px;"
      "display:flex;align-items:center;gap:10px;}"
    ".status-indicator{width:12px;height:12px;border-radius:50%;display:inline-block;}"
    ".status-ok{background:#28a745;box-shadow:0 0 10px rgba(40,167,69,0.5);}"
    ".status-error{background:#dc3545;box-shadow:0 0 10px rgba(220,53,69,0.5);}"
    ".value{font-size:2.5em;font-weight:700;color:#667eea;margin:10px 0;}"
    ".unit{font-size:0.6em;color:#999;}"
    ".status-text{font-size:0.9em;}"
    ".status-text-ok{color:#28a745;}"
    ".status-text-error{color:#dc3545;}"
    ".info-section{background:#f8f9fa;border-radius:15px;padding:25px;margin-top:20px;}"
    ".info-title{font-size:1.3em;font-weight:600;color:#333;margin-bottom:15px;"
      "border-bottom:2px solid #667eea;padding-bottom:10px;}"
    ".info-row{display:flex;justify-content:space-between;align-items:center;"
      "padding:12px 0;border-bottom:1px solid #e0e0e0;}"
    ".info-row:last-child{border-bottom:none;}"
    ".info-label{font-weight:600;color:#555;}"
    ".info-value{color:#333;font-family:'Courier New',monospace;}"
    ".badge{display:inline-block;padding:5px 15px;border-radius:20px;font-size:0.85em;font-weight:600;}"
    ".badge-connected{background:#d4edda;color:#155724;}"
    ".badge-disconnected{background:#f8d7da;color:#721c24;}"
    ".badge-ready{background:#d1ecf1;color:#0c5460;}"
    ".badge-updating{background:#fff3cd;color:#856404;}"
    ".badge-signal-strong{background:#d4edda;color:#155724;}"
    ".badge-signal-medium{background:#fff3cd;color:#856404;}"
    ".badge-signal-weak{background:#f8d7da;color:#721c24;}"
    ".chart-section{background:#f8f9fa;border-radius:15px;padding:25px;margin-top:20px;}"
    ".chart-title{font-size:1.3em;font-weight:600;color:#333;margin-bottom:15px;"
      "border-bottom:2px solid #667eea;padding-bottom:10px;}"
    ".chart-container{position:relative;height:300px;margin-top:20px;}"
    ".chart-controls{display:flex;flex-wrap:wrap;gap:10px;margin-top:15px;}"
    ".chart-controls button{border:none;border-radius:999px;padding:6px 14px;font-size:0.85em;"
      "cursor:pointer;background:#667eea;color:#fff;box-shadow:0 2px 6px rgba(0,0,0,0.15);}"
    ".chart-controls button.secondary{background:#e0e3ff;color:#333;}"
    ".chart-controls button.active{background:#4338ca;color:#fff;}"
    ".log-section{background:#f8f9fa;border-radius:15px;padding:25px;margin-top:20px;}"
    ".log-entry{padding:8px 0;border-bottom:1px solid #eee;font-family:monospace;font-size:0.9em;}"
    ".log-entry:last-child{border-bottom:none;}"
    ".log-time{color:#666;margin-right:10px;}"
    "</style>"
    "<script src='https://cdn.jsdelivr.net/npm/chart.js@4.4.4/dist/chart.umd.min.js'></script>"
    "<script src='https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns@3.0.0/dist/chartjs-adapter-date-fns.bundle.min.js'></script>"
    "<script src='https://cdn.jsdelivr.net/npm/chartjs-plugin-zoom@2.2.0'></script>"
    "</head><body>"
    "<div class='container'>"
    "<h1>BMP280 Sensor Dashboard</h1>"
    "<p class='subtitle'>Real-time Pressure & Temperature Monitoring</p>"
    "<div class='sensor-grid'>"
      "<div class='sensor-card'>"
        "<div class='sensor-title'>"
          "<span id='statusPressureIndicator' class='status-indicator " +
          String(sensorOk ? "status-ok" : "status-error") +
          "'></span>Pressure"
        "</div>"
        "<div class='value'><span id='pressureValue' class='fade-value'>" +
          String(sensorOk ? String(pressure, 1) : "—") +
          "</span><span class='unit'> hPa</span></div>"
        "<div id='statusPressureText' class='status-text " +
          String(sensorOk ? "status-text-ok'>Sensor Active"
                          : "status-text-error'>Sensor Not Detected") +
        "</div>"
      "</div>"
      "<div class='sensor-card'>"
        "<div class='sensor-title'>"
          "<span id='statusTempIndicator' class='status-indicator " +
          String(sensorOk ? "status-ok" : "status-error") +
          "'></span>Temperature"
        "</div>"
        "<div class='value'><span id='tempValue' class='fade-value'>" +
          String(sensorOk ? String(temperature, 1) : "—") +
          "</span><span class='unit'>°C</span></div>"
        "<div id='statusTempText' class='status-text " +
          String(sensorOk ? "status-text-ok'>Sensor Active"
                          : "status-text-error'>Sensor Not Detected") +
        "</div>"
      "</div>"
    "</div>" // sensor-grid
    "<div class='info-section'>"
      "<div class='info-title'>System Information</div>"
      "<div class='info-row'><span class='info-label'>Wi-Fi IP Address</span>"
        "<span class='info-value'>" + WiFi.localIP().toString() + "</span></div>"
      "<div class='info-row'><span class='info-label'>Wi-Fi Signal Strength</span>"
        "<span class='info-value'><span id='wifiSignalBadge' class='badge " +
          signalClass + "'>" + signalText + "</span></span></div>"
      "<div class='info-row'><span class='info-label'>WiFi Uptime</span>"
        "<span class='info-value' id='wifiUptime'>—</span></div>"
      "<div class='info-row'><span class='info-label'>MQTT Status</span>"
        "<span class='info-value'><span id='mqttStatusBadge' class='badge " +
          String(client.connected() ? "badge-connected'>Connected"
                                    : "badge-disconnected'>Disconnected") +
        "</span></span></div>"
      "<div class='info-row'><span class='info-label'>OTA Status</span>"
        "<span class='info-value'><span id='otaStatusBadge' class='badge " +
          String(otaInProgress ? "badge-updating'>Update in Progress"
                               : "badge-ready'>Ready") +
        "</span></span></div>"
      "<div class='info-row'><span class='info-label'>Last Refresh</span>"
        "<span class='info-value' id='lastRefreshTime'>—</span></div>"
      "<div class='info-row'><span class='info-label'>Sketch Name</span>"
        "<span class='info-value'>" + String(SKETCH_NAME) + "</span></div>"
      "<div class='info-row'><span class='info-label'>Version</span>"
        "<span class='info-value'>" + String(SKETCH_VERSION) + "</span></div>"
    "</div>"
    "<div class='chart-section'>"
      "<div class='chart-title'>Sensor History (Last 3 Hours)</div>"
      "<div class='chart-container'><canvas id='sensorChart'></canvas></div>"
      "<div class='chart-controls'>"
        "<button id='range3h' class='active'>Last 3h</button>"
        "<button id='range12h' class='secondary'>Last 12h</button>"
        "<button id='range24h' class='secondary'>Last 24h</button>"
        "<button id='resetZoomBtn' class='secondary'>Reset zoom</button>"
      "</div>"
    "</div>"
    "<div class='log-section'>"
      "<div class='info-title'>Connection Log (latest first)</div>"
      "<div id='logContainer'>Loading...</div>"
    "</div>"
    "</div>" // container
    "<script src='/app.js'></script>"
    "</body></html>";

  server.send(200, "text/html", page);
}

// ---------- Web server setup ----------

void setupWebServer() {
  server.on("/", []() { buildPage(); });
  server.on("/data", []() { sendGraphData(); });
  server.on("/status", []() { sendStatus(); });
  server.on("/app.js", HTTP_GET, []() {
    server.send_P(200, "application/javascript", APP_JS);
  });
  server.on("/favicon.ico", HTTP_GET, []() {
    server.send(204);
  });

  server.begin();
  Serial.println("Web server started");
}

// ---------- Setup & loop ----------

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n\nStarting BMP280 (%s v%s)\n", SKETCH_NAME, SKETCH_VERSION);

  if (!bmp.begin()) {
    Serial.println("BMP280 not found! Check wiring.");
    while (1) delay(10);
  }

  bmp.setSampling(
      Adafruit_BMP280::MODE_NORMAL,
      Adafruit_BMP280::SAMPLING_X2,
      Adafruit_BMP280::SAMPLING_X16,
      Adafruit_BMP280::FILTER_X16,
      Adafruit_BMP280::STANDBY_MS_500
  );

  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    Serial.print('.');
    delay(250);
    tries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi OK, IP: " + WiFi.localIP().toString());
    wifiConnectTime = millis();
    addLog("WiFi connected - " + WiFi.localIP().toString());
  } else {
    Serial.println("WiFi failed");
    addLog("WiFi initial connection failed");
  }

  setupTime();
  client.setServer(mqtt_server, 1883);

  ArduinoOTA.setHostname("bmp280-sensor");
  ArduinoOTA.onStart([]() { otaInProgress = true; });
  ArduinoOTA.onEnd([]() { otaInProgress = false; });
  ArduinoOTA.begin();

  if (WiFi.status() == WL_CONNECTED) {
    MDNS.begin("bmp280-sensor");
  }

  delay(1500);
  readSensor();

  for (int i = 0; i < MAX_READINGS; i++) {
    readings[i].timestamp   = 0;
    readings[i].pressure    = -127;
    readings[i].temperature = -127;
  }

  unsigned long now = currentEpoch();
  readings[0].timestamp   = now;
  readings[0].pressure    = pressure;
  readings[0].temperature = temperature;
  readingIndex   = 1;
  readingCount   = 1;
  lastGraphSave  = millis();

  setupWebServer();
  Serial.println("Setup complete");
}

void loop() {
  ArduinoOTA.handle();

  static bool lastWifiState = false;
  bool currentWifiState = (WiFi.status() == WL_CONNECTED);

  if (currentWifiState != lastWifiState) {
    if (currentWifiState) {
      wifiConnectTime = millis();
      addLog("WiFi reconnected - " + WiFi.localIP().toString());
    } else {
      addLog("WiFi disconnected");
    }
    lastWifiState = currentWifiState;
  }

  if (currentWifiState) {
    server.handleClient();
  } else {
    Serial.println("WiFi lost, reconnecting…");
    WiFi.begin(ssid, password);
    delay(5000);
  }

  if (currentWifiState && !client.connected()) {
    reconnect();
  }
  if (currentWifiState) {
    client.loop();
  }

  unsigned long now = millis();

  // Sensor read & MQTT publish
  if (now - lastRead >= interval) {
    lastRead = now;
    readSensor();
    if (sensorOk && client.connected()) {
      client.publish(topic_pressure,    String(pressure, 2).c_str(), true);
      client.publish(topic_temperature, String(temperature, 2).c_str(), true);
    }
  }

  // Graph data update
  if (now - lastGraphSave >= GRAPH_INTERVAL) {
    lastGraphSave = now;
    unsigned long t = currentEpoch();

    readings[readingIndex].timestamp   = t;
    readings[readingIndex].pressure    = sensorOk ? pressure    : -127;
    readings[readingIndex].temperature = sensorOk ? temperature : -127;

    readingIndex = (readingIndex + 1) % MAX_READINGS;
    if (readingCount < MAX_READINGS) {
      readingCount++;
    }
  }

  delay(50);
}