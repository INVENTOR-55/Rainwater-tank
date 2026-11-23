#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <vector>
#include <time.h>

const int SENSOR_PIN = 34;
const float SENSOR_MAX_HEIGHT_CM = 146.0;
const float LITERS_PER_CM = 20.0;
const float MAX_VOLUME_LITERS = SENSOR_MAX_HEIGHT_CM * LITERS_PER_CM;
const float ADC_CORRECTION = 1.0;

const int LED_R = 26;
const int LED_G = 27;
const int LED_B = 14;

const char* password = "YOUR_WIFI_PASSWORD";
const char* ssid1 = "YOUR_WIFI_SSID_1";
const char* ssid2 = "YOUR_WIFI_SSID_2";
const char* hostName = "rainwater-tank";

const char* ntpServer = "pool.ntp.org";
const char* timeZone = "CET-1CEST,M3.5.0,M10.5.0/3";

const char* mqtt_server = "192.168.1.100";
const int mqtt_port = 1883;
const char* mqtt_user = "YOUR_MQTT_USER";
const char* mqtt_pass = "YOUR_MQTT_PASSWORD";

WiFiMulti wifiMulti;
WebServer server(80);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

float height_cm = 0.0;
float liters = 0.0;
float measured_mA = 0.0;
bool sensorError = false;
unsigned long lastMqttRetry = 0;
bool wifiConnected = false;

struct LogEntry {
  String time;
  String type;
  String detail;
};

const int MAX_LOGS = 50;
std::vector<LogEntry> logs;

struct HistoryEntry {
  String time;
  float liters;
};

std::vector<HistoryEntry> chartHistory;
const int MAX_HISTORY = 864;
unsigned long lastHistorySave = 0;

String getUptime() {
  unsigned long sec = millis() / 1000;
  int d = sec / 86400;
  int h = (sec % 86400) / 3600;
  int m = (sec % 3600) / 60;
  int s = sec % 60;

  String res = "";
  if (d > 0) res += String(d) + "d ";
  if (h > 0 || d > 0) res += String(h) + "h ";
  res += String(m) + "m " + String(s) + "s";
  return res;
}

String getTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return getUptime();
  }
  char timeBuff[30];
  strftime(timeBuff, sizeof(timeBuff), "%d.%m. %H:%M", &timeinfo);
  return String(timeBuff);
}

void addLog(String type, String detail) {
  LogEntry entry;
  entry.time = getTimeString();
  entry.type = type;
  entry.detail = detail;

  logs.insert(logs.begin(), entry);
  if (logs.size() > MAX_LOGS) {
    logs.pop_back();
  }
}

void saveToHistory() {
  if (sensorError) return;

  HistoryEntry entry;
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char timeBuff[20];
    strftime(timeBuff, sizeof(timeBuff), "%d.%m. %H:%M", &timeinfo);
    entry.time = String(timeBuff);
  } else {
    entry.time = "Uptime";
  }
  entry.liters = liters;

  chartHistory.push_back(entry);
  if (chartHistory.size() > MAX_HISTORY) {
    chartHistory.erase(chartHistory.begin());
  }
}

void setLED(bool r, bool g, bool b) {
  digitalWrite(LED_R, r ? HIGH : LOW);
  digitalWrite(LED_G, g ? HIGH : LOW);
  digitalWrite(LED_B, b ? HIGH : LOW);
}

void measureLevel() {
  long sum = 0;
  for (int i = 0; i < 100; i++) {
    sum += analogRead(SENSOR_PIN);
    delay(2);
  }
  float rawADC = sum / 100.0;

  float voltage = ((rawADC / 4095.0) * 3.3) * ADC_CORRECTION;
  measured_mA = (voltage / 150.0) * 1000.0;

  if (measured_mA < 3.0) {
    if (!sensorError) {
      addLog("SENSOR", "Sensor disconnected / error! Current: " + String(measured_mA, 2) + " mA");
    }
    sensorError = true;
    height_cm = 0.0;
    liters = 0.0;
  } else {
    if (sensorError) {
      addLog("SENSOR", "Sensor OK. Current: " + String(measured_mA, 2) + " mA");
    }
    sensorError = false;

    float delta_mA = measured_mA - 3.5;
    if (delta_mA < 0) delta_mA = 0;

    height_cm = delta_mA * 39.74;
    if (height_cm > SENSOR_MAX_HEIGHT_CM) height_cm = SENSOR_MAX_HEIGHT_CM;

    liters = height_cm * LITERS_PER_CM;
    if (liters > MAX_VOLUME_LITERS) liters = MAX_VOLUME_LITERS;
  }
}

void updateLEDStatus() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (sensorError) {
    setLED(true, false, true);
  } else if (liters < 500.0) {
    setLED(true, false, false);
  } else if ((liters / MAX_VOLUME_LITERS) >= 0.95) {
    setLED(false, true, false);
  } else {
    setLED(false, false, true);
  }
}

void sendHADiscovery() {
  String device = ",\"dev\":{\"ids\":[\"rainwater_esp32\"],\"name\":\"Rainwater Tank\",\"mf\":\"ESP32\",\"mdl\":\"TL136 Sensor Controller\"}";

  String discoveryHl = "{\"name\":\"Rainwater - Volume hL\",\"stat_t\":\"rainwater/hl\",\"unit_of_meas\":\"hL\",\"stat_cla\":\"measurement\",\"ic\":\"mdi:water\",\"uniq_id\":\"rainwater_hl\"" + device + "}";
  mqttClient.publish("homeassistant/sensor/rainwater_hl/config", discoveryHl.c_str(), true);

  String discoveryError = "{\"name\":\"Rainwater - Sensor Error\",\"stat_t\":\"rainwater/error\",\"dev_cla\":\"problem\",\"uniq_id\":\"rainwater_error\"" + device + "}";
  mqttClient.publish("homeassistant/binary_sensor/rainwater_error/config", discoveryError.c_str(), true);
}

void connectMQTT() {
  if (!mqttClient.connected()) {
    if (millis() - lastMqttRetry > 5000) {
      lastMqttRetry = millis();
      Serial.print("Connecting to MQTT...");
      if (mqttClient.connect(hostName, mqtt_user, mqtt_pass)) {
        Serial.println(" Connected!");
        sendHADiscovery();
      } else {
        Serial.print(" Failed, rc=");
        Serial.println(mqttClient.state());
      }
    }
  }
}

void publishToMQTT() {
  if (!mqttClient.connected()) return;

  float hl = liters / 100.0;
  mqttClient.publish("rainwater/hl", String(hl, 1).c_str());
  mqttClient.publish("rainwater/error", sensorError ? "ON" : "OFF");
}

int getWifiStrength() {
  int rssi = WiFi.RSSI();
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}

void handleApiData() {
  measureLevel();

  int pct = round((liters / MAX_VOLUME_LITERS) * 100.0);
  if (pct > 100) pct = 100;
  if (pct < 0) pct = 0;

  float hl = liters / 100.0;
  float m3 = liters / 1000.0;

  String json = "{";
  json += "\"pct\":" + String(pct) + ",";
  json += "\"liters\":" + String((int)round(liters)) + ",";
  json += "\"hl\":\"" + String(hl, 1) + "\",";
  json += "\"m3\":\"" + String(m3, 2) + "\",";
  json += "\"height\":" + String((int)round(height_cm)) + ",";
  json += "\"ma\":\"" + String(measured_mA, 2) + "\",";
  json += "\"error\":" + String(sensorError ? "true" : "false") + ",";
  json += "\"wifi_ssid\":\"" + WiFi.SSID() + "\",";
  json += "\"wifi_strength\":" + String(getWifiStrength()) + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"uptime\":\"" + getUptime() + "\",";

  json += "\"history\":[";
  for (size_t i = 0; i < chartHistory.size(); i++) {
    json += "{\"t\":\"" + chartHistory[i].time + "\",\"l\":" + String(chartHistory[i].liters, 1) + "}";
    if (i < chartHistory.size() - 1) json += ",";
  }
  json += "]";

  json += "}";

  server.send(200, "application/json", json);
}

void handleApiLeave() {
  if (server.hasArg("duration")) {
    String duration = server.arg("duration");
    String ip = server.client().remoteIP().toString();
    addLog("VISIT", "Left IP " + ip + " (duration: " + duration + " s)");
  }
  server.send(200, "text/plain", "OK");
}

void handleClearLogs() {
  logs.clear();
  addLog("SYSTEM", "Log history cleared");
  server.sendHeader("Location", "/log");
  server.send(303);
}

void handleLogPage() {
  String html = "<!DOCTYPE html><html lang=\"en\"><head>";
  html += "<meta charset=\"utf-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">";
  html += "<title>SYSTEM LOGS</title>";
  html += "<style>";
  html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background-color: #f6f8fa; margin: 0; padding: 20px 10px; color: #333; }";
  html += ".card { background: #ffffff; border-radius: 20px; padding: 20px; box-shadow: 0 10px 30px rgba(0,0,0,0.05); max-width: 700px; margin: 0 auto; }";
  html += "h1 { font-size: 20px; color: #0288d1; margin-top: 0; display: flex; justify-content: space-between; align-items: center; }";
  html += "table { width: 100%; border-collapse: collapse; margin-top: 15px; font-size: 13px; }";
  html += "th, td { text-align: left; padding: 10px 8px; border-bottom: 1px solid #eee; }";
  html += "th { background-color: #f1f5f9; color: #475569; font-weight: 700; }";
  html += ".badge { display: inline-block; padding: 3px 8px; border-radius: 6px; font-size: 11px; font-weight: 700; }";
  html += ".badge-WIFI { background-color: #e0f2fe; color: #0369a1; }";
  html += ".badge-VISIT { background-color: #f0fdf4; color: #15803d; }";
  html += ".badge-SYSTEM { background-color: #fef3c7; color: #b45309; }";
  html += ".badge-SENSOR { background-color: #fee2e2; color: #b91c1c; }";
  html += ".btn-group { display: flex; gap: 10px; margin-top: 20px; }";
  html += "a, button { text-decoration: none; padding: 10px 15px; border-radius: 10px; font-size: 13px; font-weight: 700; cursor: pointer; border: none; text-align: center; }";
  html += ".btn-back { background: #0288d1; color: white; }";
  html += ".btn-clear { background: #ef4444; color: white; }";
  html += "</style></head><body>";

  html += "<div class=\"card\">";
  html += "<h1>System Logs <span style=\"font-size:12px; font-weight:normal; color:#666;\">Max 50 entries</span></h1>";

  html += "<table><thead><tr><th>Time</th><th>Type</th><th>Detail</th></tr></thead><tbody>";

  if (logs.empty()) {
    html += "<tr><td colspan=\"3\" style=\"text-align:center; color:#999;\">No records available</td></tr>";
  } else {
    for (const auto& log : logs) {
      html += "<tr>";
      html += "<td style=\"white-space:nowrap; color:#666;\">" + log.time + "</td>";
      html += "<td><span class=\"badge badge-" + log.type + "\">" + log.type + "</span></td>";
      html += "<td>" + log.detail + "</td>";
      html += "</tr>";
    }
  }

  html += "</tbody></table>";

  html += "<div class=\"btn-group\">";
  html += "<a href=\"/\" class=\"btn-back\">Back to Overview</a>";
  html += "<a href=\"/log/clear\" class=\"btn-clear\" onclick=\"return confirm('Clear logs?')\">Clear Logs</a>";
  html += "</div>";

  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

void handleRoot() {
  measureLevel();
  updateLEDStatus();

  String ip = server.client().remoteIP().toString();
  String ua = server.header("User-Agent");
  String device = "Unknown device";

  if (ua.indexOf("Mobile") != -1 || ua.indexOf("Android") != -1 || ua.indexOf("iPhone") != -1) {
    device = "Mobile device";
  } else if (ua.indexOf("Windows") != -1 || ua.indexOf("Macintosh") != -1 || ua.indexOf("Linux") != -1) {
    device = "Computer";
  }

  addLog("VISIT", "Access from IP " + ip + " (" + device + ")");

  String html = "<!DOCTYPE html><html lang=\"en\"><head>";
  html += "<meta charset=\"utf-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no\">";
  html += "<title>RAINWATER STATUS</title>";
  html += "<link rel=\"icon\" href=\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'%3E%3Ctext y='.9em' font-size='90'%3E%F0%9F%92%A7%3C/text%3E%3C/svg%3E\">";
  html += "<script src=\"https://cdn.jsdelivr.net/npm/chart.js\"></script>";
  html += "<style>";
  html += "* { box-sizing: border-box; }";
  html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; background-color: #f6f8fa; margin: 0; padding: 20px 10px; text-align: center; color: #333; }";
  html += ".card { background: #ffffff; border-radius: 28px; padding: 25px 20px; box-shadow: 0 10px 30px rgba(0,0,0,0.05); max-width: 380px; margin: 10px auto; }";
  html += "h1 { font-size: 20px; color: #0288d1; margin: 0 0 15px 0; font-weight: 700; display: flex; align-items: center; justify-content: center; gap: 8px; transition: color 0.3s ease; }";

  html += ".tank-container { width: 110px; height: 160px; border: 3px solid #0288d1; border-radius: 20px; margin: 0 auto 15px auto; position: relative; overflow: hidden; background: #e1f5fe; transition: border-color 0.3s ease, background-color 0.3s ease; }";
  html += ".tank-fill { position: absolute; bottom: 0; width: 100%; height: 0%; background: linear-gradient(180deg, #29b6f6 0%, #0288d1 100%); transition: height 0.6s ease, background 0.3s ease; }";
  html += ".tank-text { position: absolute; width: 100%; top: 50%; transform: translateY(-50%); font-size: 20px; font-weight: 800; color: #000000; z-index: 2; text-shadow: 0px 0px 4px rgba(255,255,255,0.8); }";

  html += ".liters-box { font-size: 42px; font-weight: 900; color: #1565c0; margin: 10px 0 2px 0; line-height: 1; transition: color 0.3s ease; }";
  html += ".liters-unit { font-size: 24px; font-weight: 700; color: #666; margin-left: 5px; }";
  html += ".sub-units { font-size: 17px; font-weight: 700; color: #0288d1; margin-bottom: 15px; transition: color 0.3s ease; }";

  html += ".status-pill { background-color: #e1f5fe; color: #555; font-size: 16px; font-weight: 500; padding: 10px 15px; border-radius: 16px; margin: 0 auto 15px auto; display: block; width: 100%; transition: background-color 0.3s ease, color 0.3s ease; }";
  html += ".status-pill strong { color: #000; font-weight: 800; }";

  html += ".chart-wrapper { background: #f8fafc; border: 1px solid #e2e8f0; border-radius: 16px; padding: 10px; margin-bottom: 15px; cursor: pointer; position: relative; transition: transform 0.1s ease, box-shadow 0.2s ease; }";
  html += ".chart-wrapper:hover { box-shadow: 0 4px 15px rgba(2, 136, 209, 0.15); border-color: #0288d1; }";
  html += ".chart-hint { font-size: 10px; color: #0288d1; margin-top: 5px; font-weight: 600; }";

  html += ".modal { display: none; position: fixed; z-index: 1000; left: 0; top: 0; width: 100%; height: 100%; background-color: rgba(0,0,0,0.6); backdrop-filter: blur(4px); align-items: center; justify-content: center; padding: 15px; box-sizing: border-box; }";
  html += ".modal-content { background: #ffffff; border-radius: 28px; padding: 25px 20px; width: 100%; max-width: 650px; box-shadow: 0 20px 40px rgba(0,0,0,0.25); position: relative; text-align: center; }";
  html += ".modal-content h2 { font-size: 18px; color: #0288d1; margin-top: 0; margin-bottom: 15px; }";
  html += ".close-btn { position: absolute; top: 15px; right: 20px; font-size: 26px; font-weight: bold; color: #888; cursor: pointer; background: none; border: none; }";
  html += ".close-btn:hover { color: #333; }";
  html += ".modal-chart-container { position: relative; width: 100%; height: 350px; }";

  html += ".btn { border: none; border-radius: 14px; padding: 12px 20px; font-size: 15px; font-weight: 800; cursor: pointer; width: 100%; display: flex; align-items: center; justify-content: center; gap: 8px; text-decoration: none; margin-bottom: 10px; transition: background-color 0.3s ease, box-shadow 0.3s ease; }";
  html += ".btn-refresh { background-color: #0288d1; color: white; box-shadow: 0 4px 12px rgba(2, 136, 209, 0.25); }";
  html += ".btn:active { transform: scale(0.98); }";

  html += ".footer-info { font-size: 11px; color: #888; margin-top: 15px; line-height: 1.5; }";
  html += ".wifi-icon { width: 14px; height: 14px; fill: currentColor; vertical-align: -2px; }";
  html += ".log-link { color: #0288d1; font-weight: bold; text-decoration: none; margin-top: 8px; display: inline-block; transition: color 0.3s ease; }";
  html += ".diag-text { font-size: 10px; color: #aaa; margin-top: 4px; }";
  html += "</style></head><body>";

  html += "<div class=\"card\">";
  html += "<h1 id=\"main-title\"><span style=\"font-size: 22px;\">&#128167;</span> RAINWATER STATUS</h1>";

  html += "<div id=\"tank-container\" class=\"tank-container\">";
  html += "<div id=\"tank-fill\" class=\"tank-fill\"></div>";
  html += "<div id=\"tank-text\" class=\"tank-text\">-- %</div>";
  html += "</div>";

  html += "<div id=\"liters-container\">";
  html += "<div id=\"liters-box\" class=\"liters-box\">-- <span class=\"liters-unit\">liters</span></div>";
  html += "<div id=\"sub-units\" class=\"sub-units\">-- hl &nbsp;|&nbsp; -- m³</div>";
  html += "</div>";

  html += "<div id=\"status-pill\" class=\"status-pill\">Water level: <strong id=\"height-val\">-- cm</strong></div>";

  html += "<div class=\"chart-wrapper\" onclick=\"openModal()\">";
  html += "<canvas id=\"waterChart\"></canvas>";
  html += "<div class=\"chart-hint\"></div>";
  html += "</div>";

  html += "<button id=\"btn-refresh\" class=\"btn btn-refresh\" onclick=\"loadData()\">REFRESH DATA</button>";

  html += "<div class=\"footer-info\">";
  html += "<div><svg class=\"wifi-icon\" viewBox=\"0 0 24 24\"><path d=\"M12 3C7.95 3 4.21 4.34 1.2 6.6L3 9C5.5 7 8.6 5.8 12 5.8C15.4 5.8 18.5 7 21 9L22.8 6.6C19.79 4.34 16.05 3 12 3ZM12 9C9.3 9 6.8 9.9 4.8 11.4L6.6 13.8C8.1 12.7 9.9 12 12 12C14.1 12 15.9 12.7 17.4 13.8L19.2 11.4C17.2 9.9 14.7 9 12 9ZM12 15C10.6 15 9.3 15.5 8.3 16.3L12 21L15.7 16.3C14.7 15.5 13.4 15 12 15Z\"/></svg> ";
  html += "<span id=\"wifi-ssid\">--</span> (<span id=\"wifi-strength\">--</span> %) &nbsp;|&nbsp; IP: <span id=\"ip-addr\">--</span></div>";
  html += "<div>Uptime: <span id=\"uptime\">--</span></div>";
  html += "<div class=\"diag-text\">Sensor GPIO34: <span id=\"diag-ma\">--</span> mA</div>";
  html += "<div><a id=\"log-link\" href=\"/log\" class=\"log-link\">View system log</a></div>";
  html += "</div>";

  html += "</div>";

  html += "<div id=\"chartModal\" class=\"modal\" onclick=\"closeModal()\">";
  html += "<div class=\"modal-content\" onclick=\"event.stopPropagation()\">";
  html += "<button class=\"close-btn\" onclick=\"closeModal()\">&times;</button>";
  html += "<h2>Rainwater Level Chart</h2>";
  html += "<div class=\"modal-chart-container\">";
  html += "<canvas id=\"modalWaterChart\"></canvas>";
  html += "</div>";
  html += "</div>";
  html += "</div>";

  html += "<script>";
  html += "let startTime = Date.now();";
  html += "let chart, modalChart;";

  html += "const chartSegments = {";
  html += "  borderColor: ctx => {";
  html += "    let val = ctx.p0.parsed.y;";
  html += "    if (val < 500) return '#ef4444';";
  html += "    if (val >= 3439) return '#22c55e';";
  html += "    return '#0288d1';";
  html += "  },";
  html += "  backgroundColor: ctx => {";
  html += "    let val = ctx.p0.parsed.y;";
  html += "    if (val < 500) return 'rgba(239, 68, 68, 0.1)';";
  html += "    if (val >= 3439) return 'rgba(34, 197, 94, 0.1)';";
  html += "    return 'rgba(2, 136, 209, 0.1)';";
  html += "  }";
  html += "};";

  html += "function initCharts() {";
  html += "  const ctx1 = document.getElementById('waterChart').getContext('2d');";
  html += "  chart = new Chart(ctx1, {";
  html += "    type: 'line',";
  html += "    data: { labels: [], datasets: [{ label: 'Liters', data: [], borderWidth: 2, fill: true, tension: 0.2, pointRadius: 0, segment: chartSegments }] },";
  html += "    options: { responsive: true, maintainAspectRatio: true, plugins: { legend: { display: false } }, scales: { x: { display: true, ticks: { font: { size: 9 }, maxTicksLimit: 5 } }, y: { beginAtZero: true, max: " + String((int)MAX_VOLUME_LITERS) + ", ticks: { font: { size: 9 } } } } }";
  html += "  });";

  html += "  const ctx2 = document.getElementById('modalWaterChart').getContext('2d');";
  html += "  modalChart = new Chart(ctx2, {";
  html += "    type: 'line',";
  html += "    data: { labels: [], datasets: [{ label: 'Liters', data: [], borderWidth: 3, fill: true, tension: 0.2, pointRadius: 2, segment: chartSegments }] },";
  html += "    options: { responsive: true, maintainAspectRatio: false, plugins: { legend: { display: false } }, scales: { x: { display: true, ticks: { font: { size: 11 } } }, y: { beginAtZero: true, max: " + String((int)MAX_VOLUME_LITERS) + ", ticks: { font: { size: 11 } } } } }";
  html += "  });";
  html += "}";

  html += "function openModal() {";
  html += "  document.getElementById('chartModal').style.display = 'flex';";
  html += "  modalChart.update();";
  html += "}";

  html += "function closeModal() {";
  html += "  document.getElementById('chartModal').style.display = 'none';";
  html += "}";

  html += "function loadData() {";
  html += "  fetch('/api/data').then(r => r.json()).then(d => {";
  html += "    const tankFill = document.getElementById('tank-fill');";
  html += "    const tankContainer = document.getElementById('tank-container');";
  html += "    const statusPill = document.getElementById('status-pill');";
  html += "    const btnRefresh = document.getElementById('btn-refresh');";
  html += "    const mainTitle = document.getElementById('main-title');";
  html += "    const logLink = document.getElementById('log-link');";

  html += "    if (d.history && d.history.length > 0) {";
  html += "      let labels = [];";
  html += "      let values = [];";
  html += "      d.history.forEach(item => {";
  html += "        labels.push(item.t);";
  html += "        values.push(item.l);";
  html += "      });";
  html += "      chart.data.labels = labels;";
  html += "      chart.data.datasets[0].data = values;";
  html += "      chart.update();";

  html += "      modalChart.data.labels = labels;";
  html += "      modalChart.data.datasets[0].data = values;";
  html += "      modalChart.update();";
  html += "    }";

  html += "    if(d.error) {";
  html += "      tankFill.style.height = '0%';";
  html += "      tankFill.style.background = 'linear-gradient(180deg, #29b6f6 0%, #0288d1 100%)';";
  html += "      document.getElementById('tank-text').innerText = 'ERR';";
  html += "      document.getElementById('liters-container').innerHTML = '<div class=\"liters-box\" style=\"font-size:28px; color:#ef4444;\">SENSOR ERROR</div><div id=\"sub-units\" class=\"sub-units\">&nbsp;</div>';";
  html += "      document.getElementById('height-val').innerText = '-';";
  html += "    } else {";
  html += "      tankFill.style.height = d.pct + '%';";
  html += "      document.getElementById('tank-text').innerText = d.pct + ' %';";
  html += "      document.getElementById('liters-container').innerHTML = '<div id=\"liters-box\" class=\"liters-box\">' + d.liters + '<span class=\"liters-unit\">liters</span></div><div id=\"sub-units\" class=\"sub-units\">' + d.hl.replace('.',',') + ' hl &nbsp;|&nbsp; ' + d.m3.replace('.',',') + ' m³</div>';";
  html += "      document.getElementById('height-val').innerText = d.height + ' cm';";

  html += "      const litersBox = document.getElementById('liters-box');";
  html += "      const updatedSubUnits = document.getElementById('sub-units');";

  html += "      if (d.liters < 500) {";
  html += "        tankFill.style.background = 'linear-gradient(180deg, #f87171 0%, #ef4444 100%)';";
  html += "        tankContainer.style.borderColor = '#ef4444';";
  html += "        tankContainer.style.backgroundColor = '#fef2f2';";
  html += "        statusPill.style.backgroundColor = '#fef2f2';";
  html += "        btnRefresh.style.backgroundColor = '#ef4444';";
  html += "        btnRefresh.style.boxShadow = '0 4px 12px rgba(239, 68, 68, 0.25)';";
  html += "        if (litersBox) litersBox.style.color = '#ef4444';";
  html += "        if (updatedSubUnits) updatedSubUnits.style.color = '#ef4444';";
  html += "        mainTitle.style.color = '#ef4444';";
  html += "        logLink.style.color = '#ef4444';";
  html += "      } else if (d.pct >= 95) {";
  html += "        tankFill.style.background = 'linear-gradient(180deg, #4ade80 0%, #22c55e 100%)';";
  html += "        tankContainer.style.borderColor = '#22c55e';";
  html += "        tankContainer.style.backgroundColor = '#f0fdf4';";
  html += "        statusPill.style.backgroundColor = '#f0fdf4';";
  html += "        btnRefresh.style.backgroundColor = '#22c55e';";
  html += "        btnRefresh.style.boxShadow = '0 4px 12px rgba(34, 197, 94, 0.25)';";
  html += "        if (litersBox) litersBox.style.color = '#22c55e';";
  html += "        if (updatedSubUnits) updatedSubUnits.style.color = '#22c55e';";
  html += "        mainTitle.style.color = '#22c55e';";
  html += "        logLink.style.color = '#22c55e';";
  html += "      } else {";
  html += "        tankFill.style.background = 'linear-gradient(180deg, #29b6f6 0%, #0288d1 100%)';";
  html += "        tankContainer.style.borderColor = '#0288d1';";
  html += "        tankContainer.style.backgroundColor = '#e1f5fe';";
  html += "        statusPill.style.backgroundColor = '#e1f5fe';";
  html += "        btnRefresh.style.backgroundColor = '#0288d1';";
  html += "        btnRefresh.style.boxShadow = '0 4px 12px rgba(2, 136, 209, 0.25)';";
  html += "        if (litersBox) litersBox.style.color = '#1565c0';";
  html += "        if (updatedSubUnits) updatedSubUnits.style.color = '#0288d1';";
  html += "        mainTitle.style.color = '#0288d1';";
  html += "        logLink.style.color = '#0288d1';";
  html += "      }";
  html += "    }";

  html += "    document.getElementById('wifi-ssid').innerText = d.wifi_ssid;";
  html += "    document.getElementById('wifi-strength').innerText = d.wifi_strength;";
  html += "    document.getElementById('ip-addr').innerText = d.ip;";
  html += "    document.getElementById('uptime').innerText = d.uptime;";
  html += "    document.getElementById('diag-ma').innerText = d.ma;";
  html += "  }).catch(e => console.error(e));";
  html += "}";

  html += "window.addEventListener('beforeunload', function() {";
  html += "  let duration = Math.round((Date.now() - startTime) / 1000);";
  html += "  navigator.sendBeacon('/api/leave?duration=' + duration);";
  html += "});";

  html += "initCharts();";
  html += "loadData();";
  html += "setInterval(loadData, 3000);";
  html += "</script>";

  html += "</body></html>";

  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);

  pinMode(SENSOR_PIN, INPUT);
  analogSetAttenuation(ADC_11db);

  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);

  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(ssid1, password);
  wifiMulti.addAP(ssid2, password);

  Serial.print("Connecting to Wi-Fi");
  while (wifiMulti.run() != WL_CONNECTED) {
    setLED(false, false, true);
    delay(250);
    setLED(false, false, false);
    delay(250);
    Serial.print(".");
  }

  wifiConnected = true;

  configTzTime(timeZone, ntpServer);

  addLog("SYSTEM", "ESP32 started / restarted");
  addLog("WIFI", "Connected to " + WiFi.SSID() + " (IP: " + WiFi.localIP().toString() + ")");

  Serial.println("\nConnected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  mqttClient.setBufferSize(512);
  mqttClient.setServer(mqtt_server, mqtt_port);

  ArduinoOTA.setHostname(hostName);
  ArduinoOTA.begin();

  const char* headerKeys[] = {"User-Agent"};
  server.collectHeaders(headerKeys, 1);

  server.on("/", handleRoot);
  server.on("/log", handleLogPage);
  server.on("/log/clear", handleClearLogs);
  server.on("/api/data", handleApiData);
  server.on("/api/leave", handleApiLeave);
  server.begin();

  measureLevel();
  updateLEDStatus();

  saveToHistory();
}

void loop() {
  if (wifiMulti.run() != WL_CONNECTED) {
    if (wifiConnected) {
      wifiConnected = false;
      addLog("WIFI", "Disconnected from Wi-Fi network!");
    }
    setLED(false, false, (millis() / 300) % 2);
  } else {
    if (!wifiConnected) {
      wifiConnected = true;
      addLog("WIFI", "Reconnected to " + WiFi.SSID() + " (IP: " + WiFi.localIP().toString() + ")");
    }

    connectMQTT();

    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 5000) {
      lastCheck = millis();
      measureLevel();
      updateLEDStatus();
      publishToMQTT();
    }

    if (millis() - lastHistorySave > 300000 || lastHistorySave == 0) {
      lastHistorySave = millis();
      saveToHistory();
    }
  }

  mqttClient.loop();
  ArduinoOTA.handle();
  server.handleClient();
}
