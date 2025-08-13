
# 💧 ESP8266 Ultimate Network Monitor
![Project Screenshot](https://github.com/komkritc/ESP8266-Network-Monitor/blob/main/images/mainpage.jpg)


📌 Overview
**The ESP8266 Ultimate Network Monitor is a robust WiFi monitoring device that continuously checks your network and internet connectivity, providing visual feedback and a web-based dashboard. It features automatic reconnection, API endpoint monitoring, and a configuration interface.**

✨ Features
- WiFi Monitoring: Automatic reconnection when WiFi drops
- Internet Connectivity Check: Ping tests to major services
- API Endpoint Monitoring: Verifies your API endpoints are reachable
- Web Interface: Beautiful dashboard with real-time status
- OTA Updates: Update firmware over-the-air
- mDNS Support: Access via espmonitor.local-
- Hardware Indicators: LED status lights for quick visual feedback
- Configuration Portal: Easy setup via captive portal (192.168.4.1)

🛠️ Hardware Requirements
- ESP8266 (NodeMCU, Wemos D1 Mini, etc.)
- 3x LEDs (or use built-in LEDs if available)
- 1x Push button (for reset functionality)
-Resistors as needed for LEDs
<img src="https://github.com/komkritc/ESP8266-Network-Monitor/blob/main/images/pcb.jpg" width="30%" alt="PCB Design">

Pin Connections:
GPIO	Pin	Function
2	D4	Status LED (blinking)
12	D6	Reset button (pull to GND)
13	D7	No internet indicator
15	D8	Internet OK indicator

🌐 Web Interface
Access the dashboard via:
-Device IP address (shown in Serial Monitor)
-http://espmonitor.local (if mDNS works on your network)
-Dashboard Features:
-Real-time network status
-Signal strength indicator
-API endpoint tester
-Reboot button
-Config mode switch
🔧 Configuration
-Default Settings
-AP SSID: ESP8266_Config
-AP Password: 12345678
-Web Interface: http://espmonitor.local or device IP
-Resetting to Defaults
-Hold the reset button (D6/GPIO12) for >5 seconds to reset

⚙️ Normal Operation
-Checks internet connectivity every 30 seconds
-Blinks status LED during checks
-Solid green LED (D8) when internet is available
-Solid red LED (D7) when internet is down

Automatically attempts to reconnect if connection is lost
