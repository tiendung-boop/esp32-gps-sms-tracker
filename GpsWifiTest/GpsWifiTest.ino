// WiFi (station mode) + GPS test.
// ESP32 joins your WiFi network and serves a page showing "Xin chào"
// plus the latest GPS fix from a NEO-6M module.
//
// Wiring (NEO-6M -> ESP32):
//   VCC -> 3.3V (or 5V, check your module's silkscreen)
//   GND -> GND
//   TX  -> GPIO16 (ESP32 RX2)
//   RX  -> GPIO17 (ESP32 TX2)   (optional, GPS rarely needs data from ESP32)
//
// Library needed: TinyGPSPlus (Library Manager -> search "TinyGPSPlus")

#include <WiFi.h>
#include <WebServer.h>
#include <TinyGPSPlus.h>
#include "secrets.h"

#define GPS_RX_PIN 16
#define GPS_TX_PIN 17
#define GPS_BAUD 9600

HardwareSerial gpsSerial(2);
TinyGPSPlus gps;
WebServer server(80);

void handleRoot() {
  String html = "<h1>Xin chào</h1>";
  if (gps.location.isValid()) {
    html += "<p>Latitude: " + String(gps.location.lat(), 6) + "</p>";
    html += "<p>Longitude: " + String(gps.location.lng(), 6) + "</p>";
    html += "<p>Satellites: " + String(gps.satellites.value()) + "</p>";
  } else {
    html += "<p>Chưa có tín hiệu GPS (đang tìm vệ tinh, cần ở ngoài trời, nhìn thấy bầu trời)...</p>";
  }
  server.send(200, "text/html; charset=utf-8", html);
}

void setup() {
  Serial.begin(115200);
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected! IP address: ");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.begin();
  Serial.println("Web server started.");
}

void loop() {
  while (gpsSerial.available() > 0) {
    if (gps.encode(gpsSerial.read())) {
      if (gps.location.isValid()) {
        Serial.print("Lat: ");
        Serial.print(gps.location.lat(), 6);
        Serial.print("  Long: ");
        Serial.print(gps.location.lng(), 6);
        Serial.print("  Satellites: ");
        Serial.println(gps.satellites.value());
      } else {
        Serial.print("Đang tìm vệ tinh... satellites=");
        Serial.print(gps.satellites.isValid() ? gps.satellites.value() : 0);
        Serial.print("  HDOP=");
        Serial.println(gps.hdop.isValid() ? gps.hdop.hdop() : -1);
      }
    }
  }

  // Nếu không có NMEA nào lọt qua trong 5s, khả năng cao là lỗi dây/baud, không phải đang tìm vệ tinh.
  static unsigned long lastCharsCheck = 0;
  static unsigned long lastCharsSeen = 0;
  if (millis() - lastCharsCheck > 5000) {
    lastCharsCheck = millis();
    unsigned long chars = gps.charsProcessed();
    if (chars == lastCharsSeen) {
      Serial.println("CẢNH BÁO: không nhận được byte nào từ GPS trong 5s qua — kiểm tra dây TX/RX, nguồn, baud rate.");
    }
    lastCharsSeen = chars;
  }

  server.handleClient();
}
