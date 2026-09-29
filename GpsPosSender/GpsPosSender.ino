// Đọc GPS (NEO-6M) rồi gửi toạ độ lên vi-tri-server (lam_viec/vi-tri-server/server.js)
// qua API HTTP GET /api/pos — dùng đường dành cho "thiết bị không trình duyệt"
// (xem HUONG_DAN.txt mục "THIET BI KHONG CO TRINH DUYET"). Không mở web server
// nội bộ như GpsWifiTest — sketch này chỉ có nhiệm vụ đẩy vị trí đi.
//
// Wiring (NEO-6M -> ESP32), giống GpsWifiTest:
//   VCC -> 3.3V (hoặc 5V, xem chữ in trên module)
//   GND -> GND
//   TX  -> GPIO16 (ESP32 RX2)
//   RX  -> GPIO17 (ESP32 TX2)
//
// Thư viện cần: TinyGPSPlus (Library Manager -> tìm "TinyGPSPlus"). Các phần
// còn lại (mbedtls HMAC, cert bundle) đã có sẵn trong lõi arduino-esp32,
// không cần cài thêm — chỉ cần root_ca_bundle.h nằm cùng thư mục sketch này.
//
// Bảo mật (2026-08-07): mỗi request /api/pos được ký HMAC-SHA256 bằng
// API_SECRET (secrets.h, phải khớp với server — xem HUONG_DAN.txt mục
// "XAC THUC HMAC"), giờ lấy qua NTP để chống replay, và TLS được xác thực
// bằng bộ root CA thật (root_ca_bundle.h) thay vì bỏ qua chứng chỉ.

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <TinyGPSPlus.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include "mbedtls/md.h"
#include "secrets.h"
#include "root_ca_bundle.h"

#define GPS_RX_PIN 16
#define GPS_TX_PIN 17
#define GPS_BAUD 9600

// Địa chỉ vi-tri-server. Nếu dùng Cloudflare quick tunnel (trycloudflare.com),
// LINK NÀY ĐỔI MỖI LẦN chạy lại "cloudflared tunnel --url ..." — phải sửa dòng
// dưới rồi nạp lại sketch mỗi lần đổi. Test cùng mạng WiFi thì thay bằng
// "http://<IP-LAN-của-server>:8877" cho ổn định hơn (xem HUONG_DAN.txt).
#define SERVER_BASE "https://cleaner-coverage-gnu-scanner.trycloudflare.com"

#define ROOM_CODE "000"
#define DEVICE_ID "ESP32"
#define DEVICE_NAME "ESP32"

#define SEND_INTERVAL_MS (3UL * 60UL * 1000UL)  // 3 phút

HardwareSerial gpsSerial(2);
TinyGPSPlus gps;
WiFiClientSecure secureClient;
unsigned long lastActionAt = 0;

void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected! IP address: ");
  Serial.println(WiFi.localIP());
}

// Đồng bộ giờ thật qua NTP — bắt buộc để "ts" trong mỗi request có nghĩa
// (server so ts với giờ của nó để chặn replay, xem MAX_CLOCK_SKEW_MS trong
// server.js). Không dùng millis()/RTC nội bộ vì nó không phải giờ thật.
void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Dong bo gio NTP");
  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 8 * 3600 * 2 && attempts < 40) {  // ~20s toi da, tranh treo neu mang chan NTP
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    attempts++;
  }
  Serial.println();
  if (now < 8 * 3600 * 2) {
    Serial.println("CANH BAO: khong dong bo duoc gio NTP - server se tu choi request (sai ts).");
  }
}

// HMAC-SHA256(key, msg) dạng hex — chuỗi ký phải khớp CHÍNH XÁC cách
// server.js tính lại (xem verifySig() trong server.js).
String hmacSha256Hex(const char *key, const String &msg) {
  uint8_t hmacResult[32];
  const mbedtls_md_info_t *mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mdInfo, 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char *)key, strlen(key));
  mbedtls_md_hmac_update(&ctx, (const unsigned char *)msg.c_str(), msg.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);

  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", hmacResult[i]);
  hex[64] = '\0';
  return String(hex);
}

void sendPosition() {
  unsigned long ts = (unsigned long)time(nullptr);
  String latStr = String(gps.location.lat(), 6);
  String lonStr = String(gps.location.lng(), 6);

  String toSign = String(ROOM_CODE) + "|" + DEVICE_ID + "|" + String(ts) + "|" + latStr + "|" + lonStr;
  String sig = hmacSha256Hex(API_SECRET, toSign);

  String url = String(SERVER_BASE) + "/api/pos?room=" + ROOM_CODE
               + "&deviceId=" + DEVICE_ID + "&name=" + DEVICE_NAME
               + "&lat=" + latStr + "&lon=" + lonStr
               + "&ts=" + String(ts) + "&sig=" + sig;
  if (gps.speed.isValid()) url += "&speed=" + String(gps.speed.mps(), 2);
  if (gps.course.isValid()) url += "&heading=" + String(gps.course.deg(), 1);

  HTTPClient http;
  bool began = url.startsWith("https://") ? http.begin(secureClient, url) : http.begin(url);
  if (!began) {
    Serial.println("HTTPClient begin() thất bại (kiểm tra SERVER_BASE).");
    return;
  }

  int code = http.GET();
  if (code > 0) {
    Serial.print("Gửi vị trí -> HTTP ");
    Serial.print(code);
    Serial.print(": ");
    Serial.println(http.getString());
  } else {
    Serial.print("Gửi vị trí thất bại: ");
    Serial.println(http.errorToString(code));
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  // Kiểm tra chứng chỉ TLS thật bằng bộ root CA công khai (root_ca_bundle.h,
  // sinh từ danh sách Mozilla) thay vì setInsecure() — chặn được tấn công
  // MITM (giả mạo server) mà setInsecure() bỏ qua hoàn toàn.
  secureClient.setCACertBundle(root_ca_bundle, root_ca_bundle_len);
  connectWiFi();
  syncTime();
}

void loop() {
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (millis() - lastActionAt >= SEND_INTERVAL_MS) {
    lastActionAt = millis();
    if (gps.location.isValid()) {
      sendPosition();
    } else {
      Serial.print("Chưa có fix GPS, satellites=");
      Serial.println(gps.satellites.isValid() ? gps.satellites.value() : 0);
    }
  }
}
