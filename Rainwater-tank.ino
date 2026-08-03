#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <vector>

const int SENSOR_PIN = 34;
const float SENSOR_MAX_HEIGHT_CM = 146.0; 
const float LITERS_PER_CM = 20.0;          
const float MAX_VOLUME_LITERS = SENSOR_MAX_HEIGHT_CM * LITERS_PER_CM; 

const int LED_R = 26;
const int LED_G = 27;
const int LED_B = 14;

const char* password = "YOUR_WIFI_PASSWORD"; 
const char* ssid1 = "YOUR_WIFI_SSID_1";             
const char* ssid2 = "YOUR_WIFI_SSID_2";       
const char* hostName = "rainwater-tank";

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
bool wifiConnectedFlag = false;

struct LogEntry {
  String uptime;
  String type;
  String detail;
};

const int MAX_LOGS = 50; 
std::vector<LogEntry> logs;

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

void addLog(String type, String detail) {
  LogEntry entry;
  entry.uptime = getUptime();
  entry.type = type;
  entry.detail = detail;

  logs.insert(logs.begin(), entry);
  if (logs.size() > MAX_LOGS) {
    logs.pop_back();
  }
}

void setLED(bool r, bool g, bool b) {
  digitalWrite(LED_R, r ? HIGH : LOW);
  digitalWrite(LED_G, g ? HIGH : LOW);
  digitalWrite(LED_B, b ? HIGH : LOW);
}

void measureWaterLevel() {
  long sum = 0;
  for (int i = 0; i < 50; i++) {
    sum += analogRead(SENSOR_PIN);
    delay(2);
  }
  float rawADC = sum / 50.0;
  float voltage = (rawADC / 4095.0) * 3.3;
  measured_mA = (voltage / 150.0) * 1000.0;

  if (measured_mA < 3.5) {
    if (!sensorError) {
      addLog("SENSOR", "Sensor ERROR detected! Current: " + String(measured_mA, 2) + " mA");
    }
    sensorError = true;
    height_cm = 0.0;
    liters = 0.0;
  } else {
    if (sensorError) {
      addLog("SENSOR", "Sensor OK. Current: " + String(measured_mA, 2) + " mA");
    }
    sensorError = false;
    height_cm = (measured_mA - 4.0) * (SENSOR_MAX_HEIGHT_CM / 16.0);
    
    if (height_cm < 0.0) height_cm = 0.0;
    if (height_cm > SENSOR_MAX_HEIGHT_CM) height_cm = SENSOR_MAX_HEIGHT_CM;

    liters = height_cm * LITERS_PER_CM;
  }
}

void updateLEDStatus() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (sensorError) {
    setLED(true, false, true);
  } else if (liters < 300.0) {
    setLED(true, false, false);
  } else if ((liters / MAX_VOLUME_LITERS) >= 0.90) {
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

void sendToMQTT() {
  if (!mqttClient.connected()) return;

  float hl = liters / 100.0;
  mqttClient.publish("rainwater/hl", String(hl, 1).c_str());
  mqttClient.publish("rainwater/error", sensorError ? "ON" : "OFF");
}

int getWifiSignalStrength() {
  int rssi = WiFi.RSSI();
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}

void handleApiData() {
  measureWaterLevel();
  
  int percentage = round((liters / MAX_VOLUME_LITERS) * 100.0);
  if (percentage > 100) percentage = 100;
  if (percentage < 0) percentage = 0;

  float hl = liters / 100.0;
  float m3 = liters / 1000.0;

  String json = "{";
  json += "\"percentage\":" + String(percentage) + ",";
  json += "\"liters\":" + String((int)round(liters)) + ",";
  json += "\"hl\":\"" + String(hl, 1) + "\",";
  json += "\"m3\":\"" + String(m3, 2) + "\",";
  json += "\"height\":" + String((int)height_cm) + ",";
  json += "\"ma\":\"" + String(measured_mA, 2) + "\",";
  json += "\"error\":" + String(sensorError ? "true" : "false") + ",";
  json += "\"wifi_ssid\":\"" + WiFi.SSID() + "\",";
  json += "\"wifi_signal\":" + String(getWifiSignalStrength()) + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"uptime\":\"" + getUptime() + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleApiLeave() {
  if (server.hasArg("duration")) {
    String duration = server.arg("duration");
    String ip = server.client().remoteIP().toString();
    addLog("VISIT", "Leave IP " + ip + " (session duration: " + duration + " s)");
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
  html += "<h1>📋 System Logs <span style=\"font-size:12px; font-weight:normal; color:#666;\">Max 50 entries</span></h1>";
  
  html += "<table><thead><tr><th>Uptime</th><th>Type</th><th>Detail</th></tr></thead><tbody>";

  if (logs.empty()) {
    html += "<tr><td colspan=\"3\" style=\"text-align:center; color:#999;\">No logs available</td></tr>";
  } else {
    for (const auto& log : logs) {
      html += "<tr>";
      html += "<td style=\"white-space:nowrap; color:#666;\">" + log.uptime + "</td>";
      html += "<td><span class=\"badge badge-" + log.type + "\">" + log.type + "</span></td>";
      html += "<td>" + log.detail + "</td>";
      html += "</tr>";
    }
  }

  html += "</tbody></table>";

  html += "<div class=\"btn-group\">";
  html += "<a href=\"/\" class=\"btn-back\">⬅️ Back to Overview</a>";
  html += "<a href=\"/log/clear\" class=\"btn-clear\" onclick=\"return confirm('Clear all logs?')\">🗑️ Clear Logs</a>";
  html += "</div>";

  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

void handleRoot() {
  measureWaterLevel();
  updateLEDStatus();

  String ip = server.client().remoteIP().toString();
  String ua = server.header("User-Agent");
  String deviceType = "Unknown Device";
  
  if (ua.indexOf("Mobile") != -1 || ua.indexOf("Android") != -1 || ua.indexOf("iPhone") != -1) {
    deviceType = "Mobile Device";
  } else if (ua.indexOf("Windows") != -1 || ua.indexOf("Macintosh") != -1 || ua.indexOf("Linux") != -1) {
    deviceType = "Computer";
  }
  
  addLog("VISIT", "Access from IP " + ip + " (" + deviceType + ")");

  String html = "<!DOCTYPE html><html lang=\"en\"><head>";
  html += "<meta charset=\"utf-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no\">";
  html += "<title>RAINWATER STATUS</title>";
  html += "<link rel=\"icon\" href=\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'%3E%3Ctext y='.9em' font-size='90'%3E%F0%9F%92%A7%3C/text%3E%3C/svg%3E\">";
  html += "<style>";
  html += "* { box-sizing: border-box; }";
  html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif, 'Apple Color Emoji', 'Segoe UI Emoji'; background-color: #f6f8fa; margin: 0; padding: 20px 10px; text-align: center; color: #333; }";
  html += ".card { background: #ffffff; border-radius: 28px; padding: 25px 20px; box-shadow: 0 10px 30px rgba(0,0,0,0.05); max-width: 360px; margin: 10px auto; }";
  html += "h1 { font-size: 20px; color: #0288d1; margin: 0 0 15px 0; font-weight: 700; display: flex; align-items: center; justify-content: center; gap: 8px; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI Emoji', 'Apple Color Emoji', sans-serif; }";
  
  html += ".tank-container { width: 110px; height: 160px; border: 3px solid #0288d1; border-radius: 20px; margin: 0 auto 15px auto; position: relative; overflow: hidden; background: #e1f5fe; }";
  html += ".tank-fill { position: absolute; bottom: 0; width: 100%; height: 0%; background: linear-gradient(180deg, #29b6f6 0%, #0288d1 100%); transition: height 0.6s ease; }";
  html += ".tank-text { position: absolute; width: 100%; top: 50%; transform: translateY(-50%); font-size: 20px; font-weight: 800; color: #000000; z-index: 2; text-shadow: 0px 0px 4px rgba(255,255,255,0.8); }";
  
  html += ".liters-box { font-size: 42px; font-weight: 900; color: #1565c0; margin: 10px 0 2px 0; line-height: 1; }";
  html += ".liters-unit { font-size: 24px; font-weight: 700; color: #666; margin-left: 5px; }";
  html += ".sub-units { font-size: 17px; font-weight: 700; color: #0288d1; margin-bottom: 15px; }";
  
  html += ".status-pill { background-color: #e1f5fe; color: #555; font-size: 16px; font-weight: 500; padding: 10px 15px; border-radius: 16px; margin: 0 auto 15px auto; display: block; width: 100%; }";
  html += ".status-pill strong { color: #000; font-weight: 800; }";
  
  html += ".btn { border: none; border-radius: 14px; padding: 12px 20px; font-size: 15px; font-weight: 800; cursor: pointer; width: 100%; display: flex; align-items: center; justify-content: center; gap: 8px; text-decoration: none; margin-bottom: 10px; }";
  html += ".btn-refresh { background-color: #0288d1; color: white; box-shadow: 0 4px 12px rgba(2, 136, 209, 0.25); }";
  html += ".btn:active { transform: scale(0.98); }";

  html += ".footer-info { font-size: 11px; color: #888; margin-top: 15px; line-height: 1.5; }";
  html += ".wifi-icon { width: 14px; height: 14px; fill: currentColor; vertical-align: -2px; }";
  html += ".log-link { color: #0288d1; font-weight: bold; text-decoration: none; margin-top: 8px; display: inline-block; }";
  html += ".diag-text { font-size: 10px; color: #aaa; margin-top: 4px; }";
  html += "</style></head><body>";

  html += "<div class=\"card\">";
  html += "<h1><span style=\"font-size: 22px;\">&#128167;</span> RAINWATER STATUS</h1>";
  
  html += "<div class=\"tank-container\">";
  html += "<div id=\"tank-fill\" class=\"tank-fill\"></div>";
  html += "<div id=\"tank-text\" class=\"tank-text\">-- %</div>";
  html += "</div>";

  html += "<div id=\"liters-container\">";
  html += "<div id=\"liters-box\" class=\"liters-box\">-- <span class=\"liters-unit\">liters</span></div>";
  html += "<div id=\"sub-units\" class=\"sub-units\">-- hl &nbsp;|&nbsp; -- m³</div>";
  html += "</div>";
  
  html += "<div class=\"status-pill\">Water Height: <strong id=\"height-val\">-- cm</strong></div>";

  html += "<button class=\"btn btn-refresh\" onclick=\"loadData()\">🔄 REFRESH DATA</button>";
  
  html += "<div class=\"footer-info\">";
  html += "<div><svg class=\"wifi-icon\" viewBox=\"0 0 24 24\"><path d=\"M12 3C7.95 3 4.21 4.34 1.2 6.6L3 9C5.5 7 8.6 5.8 12 5.8C15.4 5.8 18.5 7 21 9L22.8 6.6C19.79 4.34 16.05 3 12 3ZM12 9C9.3 9 6.8 9.9 4.8 11.4L6.6 13.8C8.1 12.7 9.9 12 12 12C14.1 12 15.9 12.7 17.4 13.8L19.2 11.4C17.2 9.9 14.7 9 12 9ZM12 15C10.6 15 9.3 15.5 8.3 16.3L12 21L15.7 16.3C14.7 15.5 13.4 15 12 15Z\"/></svg> ";
  html += "<span id=\"wifi-ssid\">--</span> (<span id=\"wifi-signal\">--</span> %) &nbsp;|&nbsp; IP: <span id=\"ip-addr\">--</span></div>";
  html += "<div>Uptime: <span id=\"uptime\">--</span></div>";
  html += "<div class=\"diag-text\">Sensor GPIO34: <span id=\"diag-ma\">--</span> mA</div>";
  html += "<div><a href=\"/log\" class=\"log-link\">📋 View System Log</a></div>";
  html += "</div>";
  
  html += "</div>";

  html += "<script>";
  html += "let startTime = Date.now();";
  
  html += "function loadData() {";
  html += "  fetch('/api/data').then(r => r.json()).then(d => {";
  html += "    if(d.error) {";
  html += "      document.getElementById('tank-fill').style.height = '0%';";
  html += "      document.getElementById('tank-text').innerText = 'ERR';";
  html += "      document.getElementById('liters-container').innerHTML = '<div class=\"liters-box\" style=\"font-size:28px; color:#d32f2f;\">SENSOR ERROR</div><div class=\"sub-units\">&nbsp;</div>';";
  html += "      document.getElementById('height-val').innerText = '-';";
  html += "    } else {";
  html += "      document.getElementById('tank-fill').style.height = d.percentage + '%';";
  html += "      document.getElementById('tank-text').innerText = d.percentage + ' %';";
  html += "      document.getElementById('liters-container').innerHTML = '<div class=\"liters-box\">' + d.liters + '<span class=\"liters-unit\">liters</span></div><div class=\"sub-units\">' + d.hl + ' hl &nbsp;|&nbsp; ' + d.m3 + ' m³</div>';";
  html += "      document.getElementById('height-val').innerText = d.height + ' cm';";
  html += "    }";

  html += "    document.getElementById('wifi-ssid').innerText = d.wifi_ssid;";
  html += "    document.getElementById('wifi-signal').innerText = d.wifi_signal;";
  html += "    document.getElementById('ip-addr').innerText = d.ip;";
  html += "    document.getElementById('uptime').innerText = d.uptime;";
  html += "    document.getElementById('diag-ma').innerText = d.ma;";
  html += "  }).catch(e => console.error(e));";
  html += "}";
  
  html += "window.addEventListener('beforeunload', function() {";
  html += "  let duration = Math.round((Date.now() - startTime) / 1000);";
  html += "  navigator.sendBeacon('/api/leave?duration=' + duration);";
  html += "});";

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

  addLog("SYSTEM", "ESP32 booted / restarted");

  Serial.print("Connecting to Wi-Fi");
  while (wifiMulti.run() != WL_CONNECTED) {
    setLED(false, false, true);
    delay(250);
    setLED(false, false, false);
    delay(250);
    Serial.print(".");
  }
  
  wifiConnectedFlag = true;
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

  measureWaterLevel();
  updateLEDStatus();
}

void loop() {
  if (wifiMulti.run() != WL_CONNECTED) {
    if (wifiConnectedFlag) {
      wifiConnectedFlag = false;
      addLog("WIFI", "Disconnected from Wi-Fi!");
    }
    setLED(false, false, (millis() / 300) % 2);
  } else {
    if (!wifiConnectedFlag) {
      wifiConnectedFlag = true;
      addLog("WIFI", "Reconnected to " + WiFi.SSID() + " (IP: " + WiFi.localIP().toString() + ")");
    }

    connectMQTT();

    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 5000) {
      lastCheck = millis();
      measureWaterLevel();
      updateLEDStatus();
      sendToMQTT();
    }
  }

  mqttClient.loop();
  ArduinoOTA.handle();
  server.handleClient();
}