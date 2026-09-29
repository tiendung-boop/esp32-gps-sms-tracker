// Access Point (AP) mode test.
// ESP32 creates its own WiFi network instead of joining an existing one.
// Connect your phone/laptop to that network, then browse to the IP shown
// in Serial Monitor (default is 192.168.4.1) to see "Xin chào".

#include <WiFi.h>
#include <WebServer.h>

const char* AP_SSID = "ESP32_GPS";
const char* AP_PASSWORD = "12345678";  // WPA2 requires at least 8 characters

WebServer server(80);

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", "<h1>Xin chào</h1>");
}

void setup() {
  Serial.begin(115200);

  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("Access Point started. SSID: ");
  Serial.println(AP_SSID);
  Serial.print("IP address: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.begin();
  Serial.println("Web server started.");
}

void loop() {
  server.handleClient();
}
