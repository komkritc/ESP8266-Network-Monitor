/*
 * ESP8266 Ultimate Network Monitor v9.0
 * 
 * Features:
 * - WiFi connection monitoring with automatic reconnection
 * - Internet connectivity checking (ping test)
 * - API endpoint monitoring
 * - Web configuration interface (192.168.4.1 when in AP mode)
 * - Over-the-Air (OTA) updates support
 * - mDNS support (espmonitor.local)
 * - Hardware status LEDs
 * - Reset to default via physical button (hold D6/GPIO12 >5s)
 * - Web interface with:
 *   - Network status dashboard
 *   - Reboot functionality
 *   - Config mode switching
 * 
 * Pin Definitions:
 * - D4/GPIO2  → Status LED (blinks during operation)
 * - D6/GPIO12 → Reset button (pull to ground to reset settings)
 * - D7/GPIO13 → No internet indicator (active high)
 * - D8/GPIO15 → Internet OK indicator (active high)
 * 
 * Default Credentials:
 * - AP SSID: ESP8266_Config
 * - AP Password: 12345678
 * - Web Interface: http://espmonitor.local or device IP
 * 
 * Configuration:
 * 1. Connect to ESP8266_Config access point
 * 2. Visit http://192.168.4.1
 * 3. Enter your WiFi credentials and API endpoint
 * 4. Device will restart and connect to your network
 * 
 * Normal Operation:
 * - Checks internet connectivity every 60 seconds
 * - Blinks status LED during checks
 * - Solid D8 LED when internet is available
 * - Solid D7 LED when internet is down
 * 
 * Web Interface Features:
 * - Real-time network status
 * - Signal strength indicator
 * - Reboot button
 * - Config mode switch button
 * - API endpoint display
 * 
 * Libraries Required:
 * - ESP8266WiFi
 * - ESP8266HTTPClient
 * - WiFiClient
 * - ESP8266WebServer
 * - ESP8266mDNS
 * - ArduinoOTA
 * - ESP (built-in)
 * - EEPROM (built-in)
 * - DNSServer
 * 
 * Version History:
 * v9.0 - Added Config Mode web button, improved UI, stability fixes
 * v8.0 - Added OTA support, mDNS, improved web interface
 * v7.0 - Initial release with basic monitoring
 * 
 * Author: Your Name
 * Date: 2023-11-15
 * License: MIT
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <ESP.h>
#include <EEPROM.h>
#include <DNSServer.h>

// Pin Definitions
const int pinD4 = 2;   // GPIO2  → Blink/status
const int pinD6 = 12;  // GPIO12 → Reset to default (hold >5s)
const int pinD7 = 13;  // GPIO13 → No internet
const int pinD8 = 15;  // GPIO15 → Internet OK


// EEPROM Setup
#define EEPROM_SSID 0
#define EEPROM_PASSWORD 32
#define EEPROM_API_URL 96
#define EEPROM_SIZE 256

String ssid, password, apiURL;
bool internetOK = false;

// Hostname & AP
const char* hostName = "espmonitor";
const char* apSSID = "Internet_Monitor";
const char* apPassword = "12345678";  // Min 8 chars for WPA2

// Timing
const unsigned long checkInterval = 30000;
unsigned long lastCheckTime = 0;
const int maxRetries = 3;
const int retryDelay = 5000;

// Uptime in RTC Memory
struct UptimeData {
  uint32_t bootCount;
  uint32_t internetUp;
  uint32_t apiSuccess;
} rtcData;

// Servers & Captive Portal
ESP8266WebServer server(80);
DNSServer dnsServer;
const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);


String loadString(int addr, int len) {
  String result;
  for (int i = 0; i < len; i++) {
    char c = EEPROM.read(addr + i);
    if (c == '\0' || c == 255) break;
    result += c;
  }
  return result;
}

void saveString(const String& str, int addr, int len) {
  for (int i = 0; i < len; i++) {
    char c = (i < str.length()) ? str[i] : '\0';
    EEPROM.write(addr + i, c);
  }
  EEPROM.commit();
}

void blinkD4() {
  digitalWrite(pinD4, HIGH);
  delay(100);
  digitalWrite(pinD4, LOW);
}

void blinkPattern(int count, int on, int off) {
  for (int i = 0; i < count; i++) {
    digitalWrite(pinD4, HIGH);
    delay(on);
    digitalWrite(pinD4, LOW);
    if (i < count - 1) delay(off);
  }
}


void handleResetSettings() {
  saveString("myssid", EEPROM_SSID, 32);
  saveString("mypassword", EEPROM_PASSWORD, 64);
  saveString("http://login.npu.ac.th/login?123456", EEPROM_API_URL, 128);
  Serial.println("🗑️ Settings reset to default");
}

bool connectToSavedWiFi(unsigned long timeout) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeout) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Connected: " + ssid);
    return true;
  }
  Serial.println("\n❌ Failed");
  return false;
}

void checkWiFiHealth() {
  static int rssiFailCount = 0;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("🔴 Wi-Fi lost!");
    ESP.restart();
  }
  int rssi = WiFi.RSSI();
  if (rssi < -90) rssiFailCount++;
  else rssiFailCount = 0;
  if (rssiFailCount >= 6) {
    Serial.println("🔁 Restarting Wi-Fi...");
    WiFi.disconnect();
    delay(500);
    WiFi.begin(ssid.c_str(), password.c_str());
    rssiFailCount = 0;
  }
}

void performInternetCheck() {
  blinkD4();  // Visual indicator that check is starting

  //bool internetOK = false;
  bool apiOK = false;
  int pingAttempts = 0;
  int apiAttempts = 0;

  // Check internet connectivity with retries
  while (pingAttempts < maxRetries && !internetOK) {
    pingAttempts++;
    Serial.printf("Internet check attempt %d/%d\n", pingAttempts, maxRetries);

    if (pingHost("www.google.com") || pingHost("www.yahoo.com")) {
      internetOK = true;
      Serial.println("Internet connection OK");

      // Check API only if internet is available
      while (apiAttempts < maxRetries && !apiOK) {
        apiAttempts++;
        Serial.printf("API check attempt %d/%d\n", apiAttempts, maxRetries);

        if (httpGETRequest(apiURL.c_str())) {
          apiOK = true;
          Serial.println("API endpoint reachable");
        } else {
          Serial.println("API endpoint unreachable");
          if (apiAttempts < maxRetries) delay(retryDelay);
        }
      }
    } else {
      Serial.println("No internet connection");
      if (pingAttempts < maxRetries) delay(retryDelay);
    }
  }

  // Update status LEDs
  digitalWrite(pinD8, internetOK ? HIGH : LOW);  // Green LED for internet
  digitalWrite(pinD7, internetOK ? LOW : HIGH);  // Red LED for no internet

  // Visual feedback
  if (internetOK && apiOK) {
    blinkPattern(2, 100, 100);  // Double blink for success
    rtcData.apiSuccess++;
  } else if (!internetOK) {
    blinkPattern(3, 100, 100);  // Triple blink for internet failure
  } else {
    blinkPattern(1, 500, 0);  // Single long blink for API failure
  }

  // Update statistics
  if (internetOK) rtcData.internetUp++;
  ESP.rtcUserMemoryWrite(0, (uint32_t*)&rtcData, sizeof(rtcData));

  // Log final status
  Serial.printf("Final Status - Internet: %s, API: %s\n",
                internetOK ? "OK" : "FAIL",
                apiOK ? "OK" : "FAIL");
}

bool pingHost(const char* host) {
  IPAddress dummy;
  if (!WiFi.hostByName(host, dummy)) return false;
  WiFiClient client;
  if (!client.connect(dummy, 80)) return false;
  client.println("HEAD / HTTP/1.1");
  client.print("Host: ");
  client.println(host);
  client.println("Connection: close");
  client.println();
  unsigned long start = millis();
  while (millis() - start < 3000) {
    if (client.available()) {
      client.stop();
      return true;
    }
    delay(10);
  }
  client.stop();
  return false;
}

bool httpGETRequest(const char* url) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  bool success = (code > 0);
  http.end();
  return success;
}

void handleRoot_configmode() {
  String page = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Device Configuration | ESP8266</title>
  <style>
    :root {
      --primary: #4361ee;
      --primary-hover: #3a56d4;
      --danger: #ef476f;
      --text: #2b2d42;
      --text-light: #8d99ae;
      --background: #f8f9fa;
      --card: #ffffff;
      --border: #e9ecef;
    }
    
    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
    }
    
    body {
      font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
      line-height: 1.6;
      color: var(--text);
      background-color: var(--background);
      padding: 1rem;
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
    }
    
    .card {
      width: 100%;
      max-width: 480px;
      background: var(--card);
      border-radius: 12px;
      box-shadow: 0 4px 16px rgba(0, 0, 0, 0.08);
      overflow: hidden;
      margin: 1rem;
    }
    
    .card-header {
      padding: 1.5rem;
      border-bottom: 1px solid var(--border);
      position: relative;
    }
    
    .card-title {
      font-size: 1.5rem;
      font-weight: 600;
      text-align: center;
      color: var(--primary);
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 0.5rem;
    }
    
    .card-body {
      padding: 1.5rem;
    }
    
    .form-group {
      margin-bottom: 1.25rem;
    }
    
    label {
      display: block;
      margin-bottom: 0.5rem;
      font-size: 0.875rem;
      font-weight: 500;
    }
    
    input, textarea, select {
      width: 100%;
      padding: 0.75rem;
      border: 1px solid var(--border);
      border-radius: 8px;
      font-family: inherit;
      font-size: 0.9375rem;
      transition: border-color 0.2s;
    }
    
    input:focus, textarea:focus {
      outline: none;
      border-color: var(--primary);
      box-shadow: 0 0 0 3px rgba(67, 97, 238, 0.1);
    }
    
    textarea {
      min-height: 100px;
      resize: vertical;
    }
    
    .btn {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      padding: 0.75rem 1.25rem;
      border-radius: 8px;
      font-weight: 500;
      font-size: 0.9375rem;
      cursor: pointer;
      border: none;
      transition: all 0.2s;
      text-decoration: none;
      width: 100%;
    }
    
    .btn-primary {
      background-color: var(--primary);
      color: white;
    }
    
    .btn-primary:hover {
      background-color: var(--primary-hover);
    }
    
    .btn-danger {
      background-color: var(--danger);
      color: white;
    }
    
    .btn-danger:hover {
      opacity: 0.9;
    }
    
    .helper-text {
      font-size: 0.8125rem;
      color: var(--text-light);
      margin-top: 0.5rem;
    }
    
    .debug-link {
      position: absolute;
      top: 1.5rem;
      right: 1.5rem;
      font-size: 0.8125rem;
      color: var(--text-light);
      text-decoration: none;
    }
    
    .debug-link:hover {
      text-decoration: underline;
    }
    
    .divider {
      height: 1px;
      background-color: var(--border);
      margin: 1.5rem 0;
    }
    
    .text-center {
      text-align: center;
    }
  </style>
</head>
<body>
  <div class="card">
    <div class="card-header">
      <a href="/debug" class="debug-link">Debug Info</a>
      <h1 class="card-title">
        <svg width="24" height="24" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg">
          <path d="M12 15C13.6569 15 15 13.6569 15 12C15 10.3431 13.6569 9 12 9C10.3431 9 9 10.3431 9 12C9 13.6569 10.3431 15 12 15Z" fill="currentColor"/>
          <path fill-rule="evenodd" clip-rule="evenodd" d="M22 12C22 17.5228 17.5228 22 12 22C6.47715 22 2 17.5228 2 12C2 6.47715 6.47715 2 12 2C17.5228 2 22 6.47715 22 12ZM20 12C20 16.4183 16.4183 20 12 20C7.58172 20 4 16.4183 4 12C4 7.58172 7.58172 4 12 4C16.4183 4 20 7.58172 20 12Z" fill="currentColor"/>
        </svg>
        Device Configuration
      </h1>
    </div>
    
    <div class="card-body">
      <form action="/save" method="post">
        <div class="form-group">
          <label for="ssid">Wi-Fi SSID</label>
          <input type="text" id="ssid" name="ssid" placeholder="Your network name" value=")html"
                + ssid + R"html(" required>
        </div>
        
        <div class="form-group">
          <label for="password">Wi-Fi Password</label>
          <input type="password" id="password" name="password" placeholder="Your network password" value=")html"
                + password + R"html(">
        </div>
        
        <div class="form-group">
          <label for="api_url">API Endpoint URL</label>
          <textarea id="api_url" name="api_url" placeholder="https://api.example.com/endpoint">)html"
                + apiURL + R"html(</textarea>
        </div>
        
        <button type="submit" class="btn btn-primary">Save & Reconnect</button>
      </form>
      
      <div class="divider"></div>
      
      <div class="text-center">
        <p class="helper-text">Currently connected to: <strong>)html"
                + String(apSSID) + R"html(</strong></p>
        <button onclick="window.location='/reset'" class="btn btn-danger" style="margin-top: 1rem;">Reset to Factory Defaults</button>
      </div>
    </div>
  </div>
</body>
</html>
)html";
  server.send(200, "text/html", page);
}

void handleRoot_main() {
  bool internetStatus = digitalRead(pinD8) == HIGH;

  // Pre-calculate dynamic values
  String wifiStatusClass = (WiFi.status() == WL_CONNECTED) ? "online" : "offline";
  String wifiStatusText = (WiFi.status() == WL_CONNECTED) ? "Connected" : "Disconnected";

  String internetStatusClass = internetOK ? "online" : "offline";
  String internetStatusText = internetOK ? "Online" : "Offline";

  // Determine RSSI color class
  String rssiColorClass;
  int rssi = WiFi.RSSI();
  if (rssi > -70) {
    rssiColorClass = "rssi-strong";
  } else if (rssi > -80) {
    rssiColorClass = "rssi-medium";
  } else {
    rssiColorClass = "rssi-weak";
  }

  String html = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP8266 Network Monitor</title>
  <style>
    :root {
      --primary: #4361ee;
      --success: #2ecc71;
      --danger: #e74c3c;
      --warning: #f39c12;
      --light: #f8f9fa;
      --dark: #212529;
      --gray: #6c757d;
      --border-radius: 8px;
      --shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
      --transition: all 0.3s ease;
    }
    
    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
    }
    
    body {
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      line-height: 1.6;
      color: var(--dark);
      background-color: #f5f7fa;
      padding: 20px;
    }
    
    .container {
      max-width: 600px;
      margin: 0 auto;
    }
    
    header {
      text-align: center;
      margin-bottom: 30px;
    }
    
    h1 {
      color: var(--primary);
      margin-bottom: 10px;
      font-size: 28px;
    }
    
    .status-bar {
      display: flex;
      justify-content: center;
      gap: 20px;
      margin-bottom: 20px;
    }
    
    .status-item {
      display: flex;
      align-items: center;
      background: white;
      padding: 10px 15px;
      border-radius: var(--border-radius);
      box-shadow: var(--shadow);
    }
    
    .status-indicator {
      width: 12px;
      height: 12px;
      border-radius: 50%;
      margin-right: 8px;
    }
    
    .online {
      background-color: var(--success);
    }
    
    .offline {
      background-color: var(--danger);
    }
    
    .card {
      background: white;
      border-radius: var(--border-radius);
      box-shadow: var(--shadow);
      padding: 20px;
      margin-bottom: 20px;
    }
    
    h2 {
      color: var(--primary);
      margin-bottom: 15px;
      font-size: 20px;
      display: flex;
      align-items: center;
    }
    
    h2 i {
      margin-right: 10px;
    }
    
    .info-row {
      display: flex;
      justify-content: space-between;
      margin-bottom: 10px;
      padding-bottom: 10px;
      border-bottom: 1px solid #eee;
    }
    
    .info-label {
      font-weight: 500;
      color: var(--gray);
    }
    
    .info-value {
      font-weight: 600;
    }
    
    .rssi-strong {
      color: var(--success);
    }
    
    .rssi-medium {
      color: var(--warning);
    }
    
    .rssi-weak {
      color: var(--danger);
    }
    
    .btn-group {
      display: flex;
      gap: 10px;
      margin-top: 20px;
    }
    
    button {
      flex: 1;
      padding: 12px;
      border: none;
      border-radius: var(--border-radius);
      background-color: var(--primary);
      color: white;
      font-weight: 600;
      cursor: pointer;
      transition: var(--transition);
    }
    
    button:hover {
      background-color: #3a56d4;
      transform: translateY(-2px);
    }
    
    .btn-secondary {
      background-color: var(--gray);
    }
    
    .btn-secondary:hover {
      background-color: #5a6268;
    }
    
    .btn-warning {
      background-color: var(--warning);
    }
    
    .btn-warning:hover {
      background-color: #e67e22;
    }
    
    .api-container {
      margin-top: 15px;
    }
    
    .api-form {
      display: flex;
      gap: 10px;
      margin-top: 10px;
      align-items: center;
    }
    
    .api-input {
      flex: 1;
      padding: 10px;
      border: 1px solid #ddd;
      border-radius: var(--border-radius);
      font-family: monospace;
      font-size: 14px;
    }
    
    .btn-send {
      background-color: var(--success);
      width: 30%;
      min-width: 40px;
      padding: 10px 5px;
      white-space: nowrap;
    }

    .btn-send:hover {
      background-color: #27ae60;
    }
    .response-container {
      margin-top: 15px;
      padding: 10px;
      background-color: #f8f9fa;
      border-radius: var(--border-radius);
      font-family: monospace;
      font-size: 14px;
      max-height: 200px;
      overflow-y: auto;
      display: none;
    }
    
    .response-title {
      font-weight: 600;
      margin-bottom: 5px;
    }
  </style>
</head>
<body>
  <div class="container">
    <header>
      <h1>📶 Network Monitor Dashboard</h1>
      <div class="status-bar">
        <div class="status-item">
          <div class="status-indicator )html"
                + wifiStatusClass + R"html("></div>
          <span>Wi-Fi: )html"
                + wifiStatusText + R"html(</span>
        </div>
        <div class="status-item">
          <div class="status-indicator )html"
                + internetStatusClass + R"html("></div>
          <span>Internet: )html"
                + internetStatusText + R"html(</span>
        </div>
      </div>
    </header>
    
    <div class="card">
      <h2><i>📶</i> Network Information</h2>
      <div class="info-row">
        <span class="info-label">SSID:</span>
        <span class="info-value">)html"
                + ssid + R"html(</span>
      </div>
      <div class="info-row">
        <span class="info-label">IP Address:</span>
        <span class="info-value">)html"
                + WiFi.localIP().toString() + R"html(</span>
      </div>
      <div class="info-row">
        <span class="info-label">Signal Strength:</span>
        <span class="info-value )html"
                + rssiColorClass + R"html(">)html" + String(rssi) + R"html( dBm</span>
      </div>
    </div>
    
    <div class="card">
      <h2><i>🔗</i> API Endpoint</h2>
      <div class="api-container">
        <form id="apiForm" class="api-form">
          <input type="text" id="apiUrl" class="api-input" value=")html"
                + apiURL + R"html(" placeholder="Enter API URL">
          <button type="button" class="btn-send" onclick="sendApiRequest()">Send</button>
        </form>
        <div class="response-container" id="responseContainer">
          <div class="response-title">Response:</div>
          <div id="responseContent"></div>
        </div>
      </div>
    </div>
    
    <div class="card">
      <h2><i>⚙️</i> Device Control</h2>
      <div class="btn-group">
        <button onclick="location.reload()">Refresh Status</button>
        <button class="btn-secondary" onclick="if(confirm('Are you sure you want to reboot the device?')) window.location='/reboot'">Reboot Device</button>
        <button class="btn-warning" onclick="if(confirm('Switch to config mode? ')) window.location='/configmode'">Config Mode</button>
      </div>
    </div>
  </div>

  <script>
    function sendApiRequest() {
      const apiUrl = document.getElementById('apiUrl').value;
      const responseContainer = document.getElementById('responseContainer');
      const responseContent = document.getElementById('responseContent');
      
      responseContent.textContent = 'Sending request...';
      responseContainer.style.display = 'block';
      
      fetch('/sendApiRequest?url=' + encodeURIComponent(apiUrl))
        .then(response => response.text())
        .then(data => {
          responseContent.textContent = data;
        })
        .catch(error => {
          responseContent.textContent = 'Error: ' + error;
        });
    }
  </script>
</body>
</html>
  )html";
  server.send(200, "text/html", html);
}


void configMode() {
  Serial.println("\n🔧 Starting Config Mode (AP)");

  // 1. Clean up previous WiFi connections
  WiFi.disconnect(true);  // Disconnect and disable STA
  delay(1000);
  WiFi.mode(WIFI_AP);  // Set only AP mode

  // 2. Configure AP with static IP
  if (!WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0))) {
    Serial.println("❌ Failed to configure AP!");
    blinkPattern(5, 200, 200);  // Visual error indication
    delay(1000);
    ESP.restart();
  }

  // 3. Start Access Point with verification
  Serial.println("Starting AP...");
  if (!WiFi.softAP(apSSID, apPassword, 1, 0, 4)) {  // Channel 1, hidden=0, max_conn=4
    Serial.println("❌ AP failed to start!");
    blinkPattern(3, 500, 500);
    delay(1000);
    ESP.restart();
  }

  // 4. Print AP details for debugging
  Serial.println("\n🔥 AP Details:");
  Serial.printf("SSID: %s\n", apSSID);
  Serial.printf("Password: %s\n", apPassword);
  Serial.printf("IP: %s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("MAC: %s\n", WiFi.softAPmacAddress().c_str());
  Serial.printf("Channel: %d\n", WiFi.channel());
  Serial.println("📍 Connect and visit either:");
  Serial.println("http://config.esp");
  Serial.println("http://192.168.4.1");
  Serial.println("http://192.168.4.1/debug");

  // 5. Start DNS server with captive portal redirection
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  if (!dnsServer.start(DNS_PORT, "*", apIP)) {
    Serial.println("❌ DNS Server failed to start!");
    blinkPattern(3, 500, 500);
    delay(1000);
    ESP.restart();
  }

  // 6. Reset and configure web server
  server.stop();
  delay(100);
  server.close();

  // Set the root handler to handleRoot_configmode

  if (WiFi.getMode() & WIFI_AP) {
    server.on("/", handleRoot_configmode);
  } else {
    server.on("/", handleRoot_main);
  }

  // 9. Save handler
  server.on("/save", HTTP_POST, []() {
    if (server.hasArg("ssid")) {
      ssid = server.arg("ssid");
      password = server.arg("password");
      apiURL = server.arg("api_url");

      // Save to EEPROM
      saveString(ssid, EEPROM_SSID, 32);
      saveString(password, EEPROM_PASSWORD, 64);
      saveString(apiURL, EEPROM_API_URL, 128);

      // Send response
      String page = R"html(
<!DOCTYPE html>
<html>
<head>
  <title>Settings Saved</title>
  <meta http-equiv="refresh" content="5;url=/">
  <style>
    body { font-family: Arial; text-align: center; padding: 40px; }
    .success { color: #2ecc71; font-size: 24px; }
  </style>
</head>
<body>
  <div class="success">Settings Saved</div>
  <p>Device will restart and attempt to connect...</p>
  <p>You'll be redirected back to the config page if connection fails.</p>
</body>
</html>
      )html";
      server.send(200, "text/html", page);

      delay(1000);
      ESP.restart();
    } else {
      server.send(400, "text/plain", "Bad Request: SSID missing");
    }
  });

  // 10. Reset handler
  server.on("/reset", []() {
    String page = R"html(
<!DOCTYPE html>
<html>
<head>
  <title>Reset Complete</title>
  <meta http-equiv="refresh" content="5;url=/">
  <style>
    body { font-family: Arial; text-align: center; padding: 40px; }
    .warning { color: #e74c3c; font-size: 24px; }
  </style>
</head>
<body>
  <div class="warning">⚠️ Settings Reset</div>
  <p>Device restored to factory defaults and will restart...</p>
</body>
</html>
    )html";
    server.send(200, "text/html", page);

    handleResetSettings();
    delay(1000);
    ESP.restart();
  });

  // 11. Start server
  server.begin();
  Serial.println("✅ Web server started successfully");

  // 12. Main loop with status monitoring
  unsigned long lastStatusTime = 0;
  while (true) {
    dnsServer.processNextRequest();
    server.handleClient();

    // Print status every 10 seconds
    if (millis() - lastStatusTime > 10000) {
      lastStatusTime = millis();
      Serial.printf("[Status] Clients: %d | Heap: %d bytes\n",
                    WiFi.softAPgetStationNum(), ESP.getFreeHeap());
    }

    delay(10);
  }
}




void normalSetup() {
  // Load RTC
  if (!ESP.rtcUserMemoryRead(0, (uint32_t*)&rtcData, sizeof(rtcData))) {
    rtcData.bootCount = 0;
    rtcData.internetUp = 0;
    rtcData.apiSuccess = 0;
  }
  rtcData.bootCount++;
  ESP.rtcUserMemoryWrite(0, (uint32_t*)&rtcData, sizeof(rtcData));

  // Pins
  pinMode(pinD4, OUTPUT);
  pinMode(pinD7, OUTPUT);
  pinMode(pinD8, OUTPUT);
  digitalWrite(pinD4, LOW);
  digitalWrite(pinD7, LOW);
  digitalWrite(pinD8, LOW);

  // Hostname
  WiFi.hostname(hostName);

  // mDNS
  if (MDNS.begin(hostName)) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("ota", "tcp", 8266);
    Serial.printf("✅ mDNS: http://%s.local/\n", hostName);
  }

  // OTA
  ArduinoOTA.setHostname(hostName);
  ArduinoOTA.onStart([]() {
    blinkPattern(5, 100, 100);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\n✅ OTA Complete");
  });
  ArduinoOTA.onError([](ota_error_t err) {
    Serial.printf("❌ OTA Error: %d\n", err);
  });
  ArduinoOTA.begin();
  Serial.println("🚀 OTA Ready");

  // Web Server
  //server.on("/", handleRoot_main);
  if (WiFi.status() == WL_CONNECTED) {
    server.on("/", handleRoot_main);
    server.on("/reboot", []() {
      server.send(200, "text/plain", "Rebooting...");
      delay(500);
      ESP.restart();
    });
    server.on("/sendApiRequest", []() {
      if (server.hasArg("url")) {
        String url = server.arg("url");
        WiFiClient client;
        HTTPClient http;
        String response = "Request to: " + url + "\n\n";
        
        if (http.begin(client, url)) {
          int httpCode = http.GET();
          response += "HTTP Status Code: " + String(httpCode) + "\n";
          
          if (httpCode > 0) {
            response += "Response:\n" + http.getString();
          } else {
            response += "Error: " + http.errorToString(httpCode);
          }
          http.end();
        } else {
          response += "Unable to connect to server";
        }
        
        server.send(200, "text/plain", response);
      } else {
        server.send(400, "text/plain", "Missing URL parameter");
      }
    });
  } else server.on("/", handleRoot_configmode);



  server.on("/configmode", handleRoot_configmode);
  server.on("/save", HTTP_POST, []() {
    if (server.hasArg("ssid")) {
      ssid = server.arg("ssid");
      password = server.arg("password");
      apiURL = server.arg("api_url");

      // Save to EEPROM
      saveString(ssid, EEPROM_SSID, 32);
      saveString(password, EEPROM_PASSWORD, 64);
      saveString(apiURL, EEPROM_API_URL, 128);

      // Send response
      String page = R"html(
<!DOCTYPE html>
<html>
<head>
  <title>Settings Saved</title>
  <meta http-equiv="refresh" content="5;url=/">
  <style>
    body { font-family: Arial; text-align: center; padding: 40px; }
    .success { color: #2ecc71; font-size: 24px; }
  </style>
</head>
<body>
  <div class="success">Settings Saved</div>
  <p>Device will restart and attempt to connect...</p>
  <p>You'll be redirected back to the config page if connection fails.</p>
</body>
</html>
      )html";
      server.send(200, "text/html", page);

      delay(1000);
      ESP.restart();
    } else {
      server.send(400, "text/plain", "Bad Request: SSID missing");
    }
  });

  // 10. Reset handler
  server.on("/reset", []() {
    String page = R"html(
<!DOCTYPE html>
<html>
<head>
  <title>Reset Complete</title>
  <meta http-equiv="refresh" content="5;url=/">
  <style>
    body { font-family: Arial; text-align: center; padding: 40px; }
    .warning { color: #e74c3c; font-size: 24px; }
  </style>
</head>
<body>
  <div class="warning">⚠️ Settings Reset</div>
  <p>Device restored to factory defaults and will restart...</p>
</body>
</html>
    )html";
    server.send(200, "text/html", page);

    handleResetSettings();
    delay(1000);
    ESP.restart();
  });
  server.begin();

  Serial.print("🌐 Web UI: http://");
  Serial.print(WiFi.localIP());
  Serial.printf(" or http://%s.local/\n", hostName);

  lastCheckTime = millis();
}

void setup() {
  Serial.begin(115200);
  while (!Serial)
    ;

  EEPROM.begin(EEPROM_SIZE);

  // Load saved settings
  ssid = loadString(EEPROM_SSID, 32);
  password = loadString(EEPROM_PASSWORD, 64);
  apiURL = loadString(EEPROM_API_URL, 128);

  // Default if empty
  if (ssid.length() == 0) {
    ssid = "redmi4xx";
    password = "komkritc";
    apiURL = "http://login.npu.ac.th/login?dst=http://www.msftconnecttest.com/redirect&popup=true&username=3449900299486&password=komkritc1975";
    saveString(ssid, EEPROM_SSID, 32);
    saveString(password, EEPROM_PASSWORD, 64);
    saveString(apiURL, EEPROM_API_URL, 128);
  }

  Serial.println("\n\n🔧 ESP8266 Ultimate Monitor v9.0");
  Serial.println("📶 Loaded: " + ssid);
  Serial.println("📶 Loaded: " + password);
  Serial.println("📶 Loaded: " + apiURL);

  // Reset button (D6)
  pinMode(pinD6, INPUT_PULLUP);
  if (digitalRead(pinD6) == LOW) {
    delay(500);  // Debounce
    if (digitalRead(pinD6) == LOW) {
      handleResetSettings();
      ESP.restart();
    }
  }

  // Try to connect
  if (connectToSavedWiFi(15000)) {
    normalSetup();
    performInternetCheck();
  } else {
    configMode();
  }
}


void loop() {
  ArduinoOTA.handle();
  server.handleClient();

  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck >= 10000) {
    lastWiFiCheck = millis();
    checkWiFiHealth();
  }

  if (WiFi.status() == WL_CONNECTED) {
    unsigned long currentTime = millis();
    if (currentTime - lastCheckTime >= checkInterval) {
      lastCheckTime = currentTime;
      performInternetCheck();
      if (internetOK) { sendGetRequest(apiURL); }
    }
  }
  yield();
}

void sendGetRequest(String apiURL) {

  WiFiClient client;
  HTTPClient http;
  Serial.print("\nSending GET request to: ");
  Serial.println(apiURL);

  if (http.begin(client, apiURL)) {
    int httpCode = http.GET();
    Serial.printf("HTTP Status Code: %d\n", httpCode);

    // If request was successful (code will be > 0)
    if (httpCode > 0) {
      // Print the response
      String payload = http.getString();
      Serial.println("Response:");
      Serial.println(payload);
    } else {
      Serial.printf("GET request failed, error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
  } else {
    Serial.println("Unable to connect to server");
  }
}