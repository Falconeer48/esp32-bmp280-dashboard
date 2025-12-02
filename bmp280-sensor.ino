#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <time.h>   // For real-time clock via NTP
#include <SPI.h>
#include <Adafruit_BMP280.h>

// Sketch version information
const char* SKETCH_NAME = "bmp280-sensor";
const char* SKETCH_FOLDER = "/Users/ian/Documents/Arduino/bmp280-sensor";
const char* SKETCH_VERSION = "1.0.0";

const char* ssid = "Falconeer48_EXT";
const char* password = "0795418296";

const char* mqtt_server = "192.168.50.231";
const char* mqtt_user = "ian";
const char* mqtt_password = "Falcon1959";

// NTP / time (South Africa = UTC+2, no DST)
const char* ntpServer         = "pool.ntp.org";
const long  gmtOffset_sec     = 2 * 3600;
const int   daylightOffset_sec = 0;

// Serial output control - set to false to disable verbose Serial output (saves memory)
#define SERIAL_VERBOSE true   // Set to false to disable most Serial prints
#define SERIAL_DATA_INTERVAL 300000  // Print sensor data every 5 minutes (instead of every 60s)

WiFiClient espClient;
PubSubClient client(espClient);
WebServer server(80);

// Define the SPI pins for BMP280
#define BMP_SCK  (18)   // SCK pin (Serial Clock) - GPIO18
#define BMP_MISO (19)   // MISO pin (Serial Data Out) - GPIO19
#define BMP_MOSI (23)   // MOSI pin (Serial Data In) - GPIO23
#define BMP_CS   (5)    // CS pin (Chip Select) - GPIO5

// Initialize the BMP280 sensor for SPI communication
Adafruit_BMP280 bmp(BMP_CS, BMP_MOSI, BMP_MISO, BMP_SCK);

// Sensor status
bool sensorOk = false;

// Current readings
float temperature = 0.0;
float pressure = 0.0;
float altitude = 0.0;

// MQTT topics (matching original sketch)
const char* topic_temperature = "home/bmp280/temperature";
const char* topic_pressure = "home/bmp280/pressure";

unsigned long lastRead = 0;
const unsigned long interval = 60000;  // 60 seconds - MQTT publish interval
unsigned long lastSerialPrint = 0;  // Track last Serial print time
bool otaInProgress = false;

// Graph data storage - 3 hours at 3 minute intervals (60 readings)
const int MAX_READINGS = 60;  // 3 hours * 60 minutes / 3 = 60
const unsigned long GRAPH_INTERVAL = 180000;  // 3 minutes in milliseconds

struct SensorReading {
  unsigned long timestamp;   // epoch seconds
  float temperature;
  float pressure;
  float altitude;
};

SensorReading readings[MAX_READINGS];
int readingIndex = 0;
int readingCount = 0;
unsigned long lastGraphSave = 0;

// Function to publish MQTT discovery messages
void publish_mqtt_discovery() {
  // Publish the MQTT discovery config for temperature
  client.publish("homeassistant/sensor/bmp280_temperature/config",
                 "{\"name\": \"BMP280 Temperature\", \"state_topic\": \"home/bmp280/temperature\", \"unit_of_measurement\": \"°C\", \"device_class\": \"temperature\"}",
                 true);  // The true flag means to retain the message

  // Publish the MQTT discovery config for pressure
  client.publish("homeassistant/sensor/bmp280_pressure/config",
                 "{\"name\": \"BMP280 Pressure\", \"state_topic\": \"home/bmp280/pressure\", \"unit_of_measurement\": \"hPa\", \"device_class\": \"pressure\"}",
                 true);  // The true flag means to retain the message
}

void reconnect() {
  while (!client.connected()) {
    if (SERIAL_VERBOSE) Serial.print("Attempting MQTT connection...");
    if (client.connect("ESP32Client", mqtt_user, mqtt_password)) {
      Serial.println("MQTT connected");

      // Call the publish_mqtt_discovery function to publish discovery messages
      publish_mqtt_discovery();
      
    } else {
      Serial.print("MQTT failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void setupTime() {
  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
      Serial.println("WARNING: Failed to obtain time from NTP");
    } else {
      if (SERIAL_VERBOSE) Serial.println("Time synchronised via NTP");
    }
  }
}

unsigned long currentEpoch() {
  time_t nowEpoch = time(nullptr);
  if (nowEpoch < 100000) {
    // Time not set yet, fallback to millis-based relative seconds
    return millis() / 1000;
  }
  return (unsigned long)nowEpoch;
}

String getFormattedTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "Time not set";
  }
  
  char timeString[64];
  strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeString);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  if (SERIAL_VERBOSE) {
    Serial.println("\n\nESP32 BMP280 Sensor System Starting...");
    Serial.print("Free heap at startup: ");
    Serial.print(ESP.getFreeHeap());
    Serial.println(" bytes");
  }
  
  // Initialize BMP280 sensor
  if (SERIAL_VERBOSE) Serial.println("Initializing BMP280 sensor...");
  if (!bmp.begin()) {
    Serial.println("ERROR: Could not find a valid BMP280 sensor!");
    Serial.println("Check wiring and SPI connections.");
    sensorOk = false;
  } else {
    if (SERIAL_VERBOSE) Serial.println("BMP280 found and initialized");
    sensorOk = true;
    
    /* Default settings from datasheet */
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,     // Operating Mode
                    Adafruit_BMP280::SAMPLING_X2,      // Temp oversampling
                    Adafruit_BMP280::SAMPLING_X16,     // Pressure oversampling
                    Adafruit_BMP280::FILTER_X16,       // Filtering
                    Adafruit_BMP280::STANDBY_MS_500); // Standby time
    if (SERIAL_VERBOSE) Serial.println("BMP280 configured successfully");
  }
  
  // Set up Wi-Fi
  if (SERIAL_VERBOSE) {
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(ssid);
  }
  
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    if (SERIAL_VERBOSE) Serial.print(".");
    attempts++;
  }
  if (SERIAL_VERBOSE) Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR: WiFi failed to connect.");
  } else {
    Serial.println("WiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  }

  // Set up time via NTP
  setupTime();

  // Set up MQTT
  client.setServer(mqtt_server, 1883);
  
  // Start OTA service
  ArduinoOTA.setHostname("bmp280-sensor");
  ArduinoOTA.onStart([]() { otaInProgress = true; });
  ArduinoOTA.onEnd([]()   { otaInProgress = false; });
  ArduinoOTA.begin();
  if (SERIAL_VERBOSE) Serial.println("OTA Ready");
  
  // Connect to MQTT
  reconnect();
  
  // Read initial sensor values
  if (sensorOk) {
    temperature = bmp.readTemperature();
    pressure = bmp.readPressure() / 100.0F;  // Convert Pa to hPa
    altitude = bmp.readAltitude(1013.25);     // Sea level pressure in hPa
  }
  
  // Initialize readings array
  for (int i = 0; i < MAX_READINGS; i++) {
    readings[i].timestamp = 0;
    readings[i].temperature = -127;
    readings[i].pressure = -1;
    readings[i].altitude = -127;
  }
  
  unsigned long nowEpoch = currentEpoch();
  if (sensorOk) {
    readings[readingIndex].timestamp = nowEpoch;
    readings[readingIndex].temperature = temperature;
    readings[readingIndex].pressure = pressure;
    readings[readingIndex].altitude = altitude;
  }
  
  readingIndex = (readingIndex + 1) % MAX_READINGS;
  readingCount = 1;
  lastGraphSave = millis();
  
  // /data endpoint: graph history JSON (last 3 hours)
  server.on("/data", []() {
    unsigned long nowEpoch = currentEpoch();
    // Use a more lenient cutoff - if time isn't set properly, show all data
    unsigned long cutoffTime = 0;
    if (nowEpoch > 100000) {  // Time is set properly
      cutoffTime = nowEpoch - 3UL * 3600UL;  // last 3 hours
    }
    
    // Send response in chunks to avoid truncation
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    
    // Send header
    String header = "{\"now\":";
    header += String(nowEpoch);
    header += ",\"data\":[";
    server.sendContent(header);
    
    int startIdx = (readingCount < MAX_READINGS) ? 0 : readingIndex;
    int count = (readingCount < MAX_READINGS) ? readingCount : MAX_READINGS;
    bool first = true;
    int dataPoints = 0;
    
    for (int i = 0; i < count; i++) {
      int idx = (startIdx + i) % MAX_READINGS;
      // Include data if timestamp is valid and within range (or if time not set, include all)
      if (readings[idx].timestamp != 0 && (cutoffTime == 0 || readings[idx].timestamp >= cutoffTime)) {
        if (!first) {
          server.sendContent(",");
        }
        first = false;
        dataPoints++;
        
        // Build each data point as a separate chunk
        String point = "{\"t\":";
        point += String(readings[idx].timestamp);
        point += ",\"temperature\":";
        point += String(readings[idx].temperature, 2);
        point += ",\"pressure\":";
        point += String(readings[idx].pressure, 2);
        point += ",\"altitude\":";
        point += String(readings[idx].altitude, 2);
        point += "}";
        server.sendContent(point);
      }
    }
    
    // Close JSON
    server.sendContent("]}");
    server.sendContent("");  // Finalize
    
    if (SERIAL_VERBOSE) {
      Serial.print("Sent ");
      Serial.print(dataPoints);
      Serial.println(" data points to graph");
    }
  });

  // /status endpoint: live values + status for AJAX
  server.on("/status", []() {
    int rssi = WiFi.RSSI();
    String json = "{";
    json += "\"temperature\":" + String(temperature, 2) + ",";
    json += "\"pressure\":" + String(pressure, 2) + ",";
    json += "\"altitude\":" + String(altitude, 2) + ",";
    json += "\"sensorOk\":" + String(sensorOk ? "true" : "false") + ",";
    json += "\"rssi\":" + String(rssi) + ",";
    json += "\"mqtt\":" + String(client.connected() ? "true" : "false") + ",";
    json += "\"ota\":" + String(otaInProgress ? "true" : "false") + ",";
    json += "\"timestamp\":\"" + getFormattedTime() + "\"";
    json += "}";
    server.send(200, "application/json", json);
  });
  
  server.on("/", []() {
    String page = "<!DOCTYPE html><html lang='en'><head>";
    page += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    page += "<title>BMP280 Sensor Dashboard</title>";
    page += "<style>";
    page += "* { margin: 0; padding: 0; box-sizing: border-box; }";
    page += "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; padding: 20px; }";
    page += ".container { max-width: 1000px; margin: 0 auto; background: rgba(255, 255, 255, 0.95); border-radius: 20px; box-shadow: 0 10px 40px rgba(0, 0, 0, 0.2); padding: 40px; }";
    page += "h1 { color: #333; text-align: center; margin-bottom: 10px; font-size: 2.5em; font-weight: 600; }";
    page += ".subtitle { text-align: center; color: #666; margin-bottom: 30px; font-size: 0.9em; }";
    page += ".sensor-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 20px; margin-bottom: 30px; }";
    page += ".sensor-card { background: #f8f9fa; border-radius: 15px; padding: 25px; box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1); transition: transform 0.3s ease; }";
    page += ".sensor-card:hover { transform: translateY(-5px); box-shadow: 0 6px 12px rgba(0, 0, 0, 0.15); }";
    page += ".sensor-title { font-size: 1.2em; font-weight: 600; color: #555; margin-bottom: 15px; display: flex; align-items: center; gap: 10px; }";
    page += ".status-indicator { width: 12px; height: 12px; border-radius: 50%; display: inline-block; }";
    page += ".status-ok { background: #28a745; box-shadow: 0 0 10px rgba(40, 167, 69, 0.5); }";
    page += ".status-error { background: #dc3545; box-shadow: 0 0 10px rgba(220, 53, 69, 0.5); }";
    page += ".value { font-size: 2.5em; font-weight: 700; color: #667eea; margin: 10px 0; }";
    page += ".unit { font-size: 0.6em; color: #999; }";
    page += ".status-text { font-size: 0.9em; }";
    page += ".status-text-ok { color: #28a745; }";
    page += ".status-text-error { color: #dc3545; }";
    page += ".info-section { background: #f8f9fa; border-radius: 15px; padding: 25px; margin-top: 20px; }";
    page += ".fade-value { transition: opacity 0.3s ease; }";
    page += ".info-title { font-size: 1.3em; font-weight: 600; color: #333; margin-bottom: 15px; border-bottom: 2px solid #667eea; padding-bottom: 10px; }";
    page += ".info-row { display: flex; justify-content: space-between; align-items: center; padding: 12px 0; border-bottom: 1px solid #e0e0e0; }";
    page += ".info-row:last-child { border-bottom: none; }";
    page += ".info-label { font-weight: 600; color: #555; }";
    page += ".info-value { color: #333; font-family: 'Courier New', monospace; }";
    page += ".badge { display: inline-block; padding: 5px 15px; border-radius: 20px; font-size: 0.85em; font-weight: 600; }";
    page += ".badge-connected { background: #d4edda; color: #155724; }";
    page += ".badge-disconnected { background: #f8d7da; color: #721c24; }";
    page += ".badge-ready { background: #d1ecf1; color: #0c5460; }";
    page += ".badge-updating { background: #fff3cd; color: #856404; }";
    page += ".badge-signal-strong { background: #d4edda; color: #155724; }";
    page += ".badge-signal-medium { background: #fff3cd; color: #856404; }";
    page += ".badge-signal-weak { background: #f8d7da; color: #721c24; }";
    page += ".chart-section { background: #f8f9fa; border-radius: 15px; padding: 25px; margin-top: 20px; }";
    page += ".chart-title { font-size: 1.3em; font-weight: 600; color: #333; margin-bottom: 15px; border-bottom: 2px solid #667eea; padding-bottom: 10px; }";
    page += ".chart-container { position: relative; height: 300px; margin-top: 20px; }";
    page += ".timestamp { text-align: center; color: #666; font-size: 0.9em; margin-top: 15px; padding: 10px; background: rgba(102, 126, 234, 0.1); border-radius: 10px; }";
    page += "@media (max-width: 600px) { .container { padding: 20px; } h1 { font-size: 1.8em; } .value { font-size: 2em; } .chart-container { height: 250px; } }";
    page += "</style>";
    page += "<script src='https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js'></script>";
    page += "</head><body>";
    page += "<div class='container'>";
    page += "<h1>BMP280 Sensor Dashboard</h1>";
    page += "<p class='subtitle'>Real-time Pressure & Temperature Monitoring</p>";
    page += "<div class='sensor-grid'>";
    
    // Temperature Card
    page += "<div class='sensor-card'>";
    page += "<div class='sensor-title'>";
    page += "<span id='statusIndicator' class='status-indicator " + String(sensorOk ? "status-ok" : "status-error") + "'></span>";
    page += "Temperature";
    page += "</div>";
    if (sensorOk) {
      page += "<div class='value'><span id='tempValue' class='fade-value'>" + String(temperature, 2) + "</span><span class='unit'>°C</span></div>";
      page += "<div id='statusText' class='status-text status-text-ok'>Sensor Active</div>";
    } else {
      page += "<div class='value'><span id='tempValue' class='fade-value'>—</span><span class='unit'>°C</span></div>";
      page += "<div id='statusText' class='status-text status-text-error'>Sensor Not Detected</div>";
    }
    page += "</div>";
    
    // Pressure Card
    page += "<div class='sensor-card'>";
    page += "<div class='sensor-title'>Pressure</div>";
    if (sensorOk) {
      page += "<div class='value'><span id='pressureValue' class='fade-value'>" + String(pressure, 2) + "</span><span class='unit'>hPa</span></div>";
    } else {
      page += "<div class='value'><span id='pressureValue' class='fade-value'>—</span><span class='unit'>hPa</span></div>";
    }
    page += "</div>";
    
    // Altitude Card (optional, calculated from pressure)
    page += "<div class='sensor-card'>";
    page += "<div class='sensor-title'>Altitude</div>";
    if (sensorOk) {
      page += "<div class='value'><span id='altitudeValue' class='fade-value'>" + String(altitude, 2) + "</span><span class='unit'>m</span></div>";
    } else {
      page += "<div class='value'><span id='altitudeValue' class='fade-value'>—</span><span class='unit'>m</span></div>";
    }
    page += "</div>";
    
    page += "</div>"; // Close sensor-grid
    
    // Timestamp
    page += "<div class='timestamp'>";
    page += "<strong>Last Update:</strong> <span id='timestampValue'>" + getFormattedTime() + "</span>";
    page += "</div>";
    
    // System Information Section
    page += "<div class='info-section'>";
    page += "<div class='info-title'>System Information</div>";
    page += "<div class='info-row'><span class='info-label'>Wi-Fi IP Address</span><span class='info-value'>" + WiFi.localIP().toString() + "</span></div>";
    
    // WiFi Signal Strength with color coding
    int rssi = WiFi.RSSI();
    String signalStrength;
    String signalClass;
    if (rssi > -50) {
      signalStrength = "Strong (" + String(rssi) + " dBm)";
      signalClass = "badge-signal-strong";
    } else if (rssi > -70) {
      signalStrength = "Medium (" + String(rssi) + " dBm)";
      signalClass = "badge-signal-medium";
    } else {
      signalStrength = "Weak (" + String(rssi) + " dBm)";
      signalClass = "badge-signal-weak";
    }
    page += "<div class='info-row'><span class='info-label'>Wi-Fi Signal Strength</span><span class='info-value'><span id='wifiSignalBadge' class='badge " + signalClass + "'>" + signalStrength + "</span></span></div>";
    
    page += "<div class='info-row'><span class='info-label'>MQTT Status</span><span class='info-value'><span id='mqttStatusBadge' class='badge " + String(client.connected() ? "badge-connected" : "badge-disconnected") + "'>" + String(client.connected() ? "Connected" : "Disconnected") + "</span></span></div>";
    page += "<div class='info-row'><span class='info-label'>OTA Status</span><span class='info-value'><span id='otaStatusBadge' class='badge " + String(otaInProgress ? "badge-updating" : "badge-ready") + "'>" + String(otaInProgress ? "Update in Progress" : "Ready") + "</span></span></div>";
    page += "<div class='info-row'><span class='info-label'>Sketch Name</span><span class='info-value'>" + String(SKETCH_NAME) + "</span></div>";
    page += "<div class='info-row'><span class='info-label'>Version</span><span class='info-value'>" + String(SKETCH_VERSION) + "</span></div>";
    page += "</div>"; // info-section
    
    // Chart Section
    page += "<div class='chart-section'>";
    page += "<div class='chart-title'>Sensor History (Last 3 Hours)</div>";
    page += "<div class='chart-container'><canvas id='sensorChart'></canvas></div>";
    page += "</div>";
    
    page += "</div>"; // container
    
    // JavaScript
    page += "<script>";
    page += "let chart = null;";
    
    page += "function updateTextWithFade(id, text) {";
    page += "  const el = document.getElementById(id);";
    page += "  if (!el) return;";
    page += "  if (el.textContent === text) return;";
    page += "  el.style.opacity = 0;";
    page += "  setTimeout(() => { el.textContent = text; el.style.opacity = 1; }, 300);";
    page += "}";
    
    page += "function initChart() {";
    page += "  const canvas = document.getElementById('sensorChart');";
    page += "  if (!canvas || typeof Chart === 'undefined') return;";
    page += "  const ctx = canvas.getContext('2d');";
    page += "  chart = new Chart(ctx, {";
    page += "    type: 'line',";
    page += "    data: { labels: [], datasets: [";
    page += "      { label: 'Temperature (°C)', data: [], borderColor: '#667eea', backgroundColor: 'rgba(102, 126, 234, 0.1)', tension: 0.4, fill: true, pointRadius: 0, pointHoverRadius: 0, pointStyle: false, showLine: true, yAxisID: 'y' },";
    page += "      { label: 'Pressure (hPa)', data: [], borderColor: '#f093fb', backgroundColor: 'rgba(240, 147, 251, 0.1)', tension: 0.4, fill: true, pointRadius: 0, pointHoverRadius: 0, pointStyle: false, showLine: true, yAxisID: 'y1' },";
    page += "      { label: 'Altitude (m)', data: [], borderColor: '#4facfe', backgroundColor: 'rgba(79, 172, 254, 0.1)', tension: 0.4, fill: true, pointRadius: 0, pointHoverRadius: 0, pointStyle: false, showLine: true, yAxisID: 'y2' }";
    page += "    ] },";
    page += "    options: {";
    page += "      responsive: true,";
    page += "      maintainAspectRatio: false,";
    page += "      spanGaps: true,";
    page += "      elements: { point: { radius: 0, hoverRadius: 0, display: false }, line: { borderWidth: 2 } },";
    page += "      plugins: { legend: { position: 'top', labels: { font: { size: 12 } } } },";
    page += "      scales: {";
    page += "        x: {";
    page += "          display: true,";
    page += "          title: { display: true, text: 'Time' },";
    page += "          min: 0,";
    page += "          max: 180";  // 3 hours in minutes
    page += "        },";
    page += "        y: {";
    page += "          type: 'linear',";
    page += "          display: true,";
    page += "          position: 'left',";
    page += "          title: { display: true, text: 'Temperature (°C)' },";
    page += "          beginAtZero: false,";
    page += "          suggestedMin: -10,";
    page += "          suggestedMax: 50";
    page += "        },";
    page += "        y1: {";
    page += "          type: 'linear',";
    page += "          display: true,";
    page += "          position: 'right',";
    page += "          title: { display: true, text: 'Pressure (hPa)' },";
    page += "          grid: { drawOnChartArea: false },";
    page += "          beginAtZero: false,";
    page += "          suggestedMin: 800,";
    page += "          suggestedMax: 1100";
    page += "        },";
    page += "        y2: {";
    page += "          type: 'linear',";
    page += "          display: false";
    page += "        }";
    page += "      }";
    page += "    }";
    page += "  });";
    page += "  loadData();";
    page += "}";
    
    page += "function loadData() {";
    page += "  if (!chart) return;";
    page += "  fetch('/data').then(r => r.json()).then(result => {";
    page += "    const labels = []; const tempData = []; const pressureData = []; const altitudeData = [];";
    page += "    if (result.data && result.data.length > 0) {";
    page += "      result.data.forEach(item => {";
    page += "        const date = new Date(item.t * 1000);";  // epoch seconds → ms
    page += "        const label = date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });";
    page += "        const tempValid = item.temperature > -100 && item.temperature < 100;";
    page += "        const pressureValid = item.pressure > 0 && item.pressure < 2000;";
    page += "        const altitudeValid = item.altitude > -1000 && item.altitude < 10000;";
    page += "        if (tempValid || pressureValid || altitudeValid) {";
    page += "          labels.push(label);";
    page += "          tempData.push(tempValid ? parseFloat(item.temperature) : NaN);";
    page += "          pressureData.push(pressureValid ? parseFloat(item.pressure) : NaN);";
    page += "          altitudeData.push(altitudeValid ? parseFloat(item.altitude) : NaN);";
    page += "        }";
    page += "      });";
    page += "    }";
    page += "    chart.data.labels = labels;";
    page += "    chart.data.datasets[0].data = tempData;";
    page += "    chart.data.datasets[1].data = pressureData;";
    page += "    chart.data.datasets[2].data = altitudeData;";
    page += "    chart.update();";
    page += "  }).catch(err => console.error('Error loading data:', err));";
    page += "}";
    
    page += "function loadStatus() {";
    page += "  fetch('/status').then(r => r.json()).then(data => {";
    page += "    updateTextWithFade('tempValue', data.temperature.toFixed(2));";
    page += "    updateTextWithFade('pressureValue', data.pressure.toFixed(2));";
    page += "    updateTextWithFade('altitudeValue', data.altitude.toFixed(2));";
    page += "    document.getElementById('timestampValue').textContent = data.timestamp;";
    
    page += "    const sensorOk = data.sensorOk;";
    
    page += "    const ind = document.getElementById('statusIndicator');";
    page += "    const txt = document.getElementById('statusText');";
    page += "    if (ind && txt) {";
    page += "      ind.classList.remove('status-ok','status-error');";
    page += "      ind.classList.add(sensorOk ? 'status-ok' : 'status-error');";
    page += "      txt.classList.remove('status-text-ok','status-text-error');";
    page += "      txt.classList.add(sensorOk ? 'status-text-ok' : 'status-text-error');";
    page += "      txt.textContent = sensorOk ? 'Sensor Active' : 'Sensor Not Detected';";
    page += "    }";
    
    page += "    const wifiBadge = document.getElementById('wifiSignalBadge');";
    page += "    if (wifiBadge) {";
    page += "      let signalClass = 'badge-signal-weak';";
    page += "      let signalText = 'Weak (' + data.rssi + ' dBm)';";
    page += "      if (data.rssi > -50) { signalClass = 'badge-signal-strong'; signalText = 'Strong (' + data.rssi + ' dBm)'; }";
    page += "      else if (data.rssi > -70) { signalClass = 'badge-signal-medium'; signalText = 'Medium (' + data.rssi + ' dBm)'; }";
    page += "      wifiBadge.classList.remove('badge-signal-strong','badge-signal-medium','badge-signal-weak');";
    page += "      wifiBadge.classList.add(signalClass);";
    page += "      wifiBadge.textContent = signalText;";
    page += "    }";
    
    page += "    const mqttBadge = document.getElementById('mqttStatusBadge');";
    page += "    if (mqttBadge) {";
    page += "      mqttBadge.classList.remove('badge-connected','badge-disconnected');";
    page += "      mqttBadge.classList.add(data.mqtt ? 'badge-connected' : 'badge-disconnected');";
    page += "      mqttBadge.textContent = data.mqtt ? 'Connected' : 'Disconnected';";
    page += "    }";
    
    page += "    const otaBadge = document.getElementById('otaStatusBadge');";
    page += "    if (otaBadge) {";
    page += "      otaBadge.classList.remove('badge-ready','badge-updating');";
    page += "      otaBadge.classList.add(data.ota ? 'badge-updating' : 'badge-ready');";
    page += "      otaBadge.textContent = data.ota ? 'Update in Progress' : 'Ready';";
    page += "    }";
    page += "  }).catch(err => console.error('Error loading status:', err));";
    page += "}";
    
    page += "window.addEventListener('load', function() {";
    page += "  initChart();";
    page += "  loadStatus();";
    page += "  setInterval(loadData, 180000);";  // graph every 3 minutes (matches data collection interval)
    page += "  setInterval(loadStatus, 10000);"; // status every 10 seconds
    page += "});";
    
    page += "</script>";
    
    page += "</body></html>";
    server.send(200, "text/html", page);
  });
  
  // Only start mDNS and web server if WiFi is connected
  if (WiFi.status() == WL_CONNECTED) {
    if (!MDNS.begin("bmp280-sensor")) {
      // mDNS failed to start - could add error handling
    }
    server.begin();
    if (SERIAL_VERBOSE) Serial.println("Web server started");
  }
}

void loop() {
  // Handle OTA updates
  ArduinoOTA.handle();
  
  // Only handle web server if WiFi is connected
  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();
  }
  
  // Handle MQTT
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
  
  // Non-blocking delay using millis()
  unsigned long now = millis();
  if (now - lastRead >= interval) {
    lastRead = now;

    // Read sensor values
    if (sensorOk) {
      temperature = bmp.readTemperature();
      pressure = bmp.readPressure() / 100.0F; // Convert to hPa
      altitude = bmp.readAltitude(1013.25);   // Sea level pressure in hPa
      
      // Print to Serial Monitor (only if verbose enabled and interval elapsed)
      if (SERIAL_VERBOSE && (now - lastSerialPrint >= SERIAL_DATA_INTERVAL)) {
        lastSerialPrint = now;
        Serial.print("Temperature: ");
        Serial.print(temperature);
        Serial.println(" *C");

        Serial.print("Pressure: ");
        Serial.print(pressure);
        Serial.println(" hPa");
        
        // Print memory info occasionally
        Serial.print("Free heap: ");
        Serial.print(ESP.getFreeHeap());
        Serial.println(" bytes");
      }
      
      // Always log warnings/errors (important for debugging)
      if (temperature < -40 || temperature > 85 || isnan(temperature)) {
        Serial.println("WARNING: Temperature reading out of range!");
      }
      if (pressure < 300 || pressure > 1100 || isnan(pressure)) {
        Serial.println("WARNING: Pressure reading out of range!");
      }
    }

    // Publish sensor data to MQTT topics
    if (sensorOk && client.connected()) {
      char tempString[8];
      dtostrf(temperature, 1, 2, tempString);
      client.publish(topic_temperature, tempString);

      char pressureString[8];
      dtostrf(pressure, 1, 2, pressureString);
      client.publish(topic_pressure, pressureString);
    }
    
    // Save to graph data
    if (now - lastGraphSave >= GRAPH_INTERVAL) {
      lastGraphSave = now;
      unsigned long nowEpoch = currentEpoch();
      readings[readingIndex].timestamp = nowEpoch;
      if (sensorOk) {
        readings[readingIndex].temperature = temperature;
        readings[readingIndex].pressure = pressure;
        readings[readingIndex].altitude = altitude;
      } else {
        readings[readingIndex].temperature = -127;
        readings[readingIndex].pressure = -1;
        readings[readingIndex].altitude = -127;
      }
      
      readingIndex = (readingIndex + 1) % MAX_READINGS;
      if (readingCount < MAX_READINGS) readingCount++;
    }
  }
}
