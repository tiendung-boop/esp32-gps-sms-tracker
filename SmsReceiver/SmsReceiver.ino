// Doc SMS toa do gui toi tu GpsSmsSender.ino (dinh dang "Toa do:
// lat1,lon1;lat2,lon2;..." - 1 SMS co the gop nhieu diem lay mau trong 1
// phut, xem buildLocationMessage() trong sketch do) qua module SIM 4G
// A7680C, roi chuyen tiep TUNG diem len vi-tri-server
// (lam_viec/vi-tri-server/server.js) bang HTTP GET /api/pos co ky HMAC (1
// request/diem) — GIONG HET co che GpsPosSender.ino dang dung, chi khac
// nguon toa do la tu SMS thay vi tu GPS truc tiep. Nho vay thiet bi gui SMS
// (khong co WiFi, o hien truong) van hien realtime tren dashboard /thu
// chung voi cac thiet bi WiFi khac.
//
// Dat sketch nay o mot ESP32 RIENG, o noi CO WiFi (van phong/nha) — khac voi
// GpsSmsSender.ino la thiet bi gan xe, KHONG co WiFi nen moi phai di duong
// SMS. ESP32 nay dung 1 SIM RIENG, so dien thoai cua no chinh la PHONE_NUMBER
// khai bao trong GpsSmsSender/secrets.h (so nhan tin).
//
// Wiring (A7680C -> ESP32) — GIU NGUYEN wiring da xac nhan hoat dong trong
// ATBridge.ino / SmsSender.ino (khong dung GPS o sketch nay nen khong co
// xung dot UART nhu GpsSmsSender.ino):
//   VBAT/VCC -> nguon ngoai rieng 3.4-4.2V, chiu dong dinh ~2A (KHONG lay tu
//               chan 3.3V/5V/VIN cua ESP32).
//   GND      -> GND chung voi ESP32.
//   TXD      -> GPIO26 (ESP32 RX1)
//   RXD      -> GPIO27 (ESP32 TX1)
//   PWRKEY/RST -> khong noi GPIO nao (de troi), xem ATBridge.ino.
//
// Thu vien can: khong can cai them qua Library Manager (giong SmsSender.ino,
// tu parse phan hoi AT). Phan goi HTTP dung WiFi.h/HTTPClient/WiFiClientSecure/
// mbedtls co san trong loi arduino-esp32 (giong GpsPosSender.ino) — sketch nay
// can THEM root_ca_bundle.h trong cung thu muc (copy tu GpsPosSender/, cung 1
// noi dung, khong lien quan gi server cu the nao).
//
// secrets.h can co: WIFI_SSID, WIFI_PASSWORD, API_SECRET (giong het
// GpsPosSender/secrets.h — PHAI khop API_SECRET voi server va voi
// GpsPosSender neu dung chung 1 server).

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include "mbedtls/md.h"
#include "secrets.h"
#include "root_ca_bundle.h"

#define MODEM_RX_PIN 26
#define MODEM_TX_PIN 27
#define MODEM_BAUD 115200

// Dia chi vi-tri-server — copy dung gia tri dang dung trong GpsPosSender.ino
// (Cloudflare quick tunnel doi moi lan restart, phai sua lai + nap lai sketch
// khi doi — xem ghi chu trong GpsPosSender.ino).
#define SERVER_BASE "https://cleaner-coverage-gnu-scanner.trycloudflare.com"
#define ROOM_CODE "000"

// Chu ky kiem tra tin nhan moi. Khong can qua nhanh: ben gui (GpsSmsSender.ino)
// chi gui toi da 1 tin/phut.
#define SMS_POLL_INTERVAL_MS (20UL * 1000UL)  // 20 giay

// (2026-09-08) GpsSmsSender.ino gio gop nhieu diem GPS lay mau trong 1 phut
// vao CHUNG 1 tin SMS (cach nhau boi ';', xem buildLocationMessage() ben
// do) thay vi 1 diem/tin nhu truoc — MAX_POINTS_PER_SMS phai >= so o toi da
// ben gui co the gui (SLOTS_PER_SEND trong GpsSmsSender.ino, hien la 6) de
// khong bi cat bot o khi parse.
#define MAX_POINTS_PER_SMS 8

// PHAI khop SAMPLE_INTERVAL_MS/1000 ben GpsSmsSender.ino - dung de tinh tuoi
// (giay truoc) cua tung diem toa do dua vao vi tri cua no trong danh sach o,
// xem splitIntoSlots()/processMessage() ben duoi.
#define SAMPLE_INTERVAL_SEC 10

// Ten hien thi (tuy chon) cho tung so may gui toi, chi de danh sach thiet bi
// tren dashboard /thu de doc hon so dien thoai tho. Them dong moi neu co
// nhieu xe/thiet bi GpsSmsSender cung gui ve so nay. So dien thoai ghi KHONG
// dau '+', khong dau cach (vd "84987654321"). Khong khop dong nao -> dung
// luon so dien thoai lam ten.
struct KnownSender {
  const char* phoneDigits;
  const char* name;
};
KnownSender KNOWN_SENDERS[] = {
  // { "84987654321", "Xe may 1" },
};

HardwareSerial modemSerial(2);
WiFiClientSecure secureClient;
unsigned long lastPollAt = 0;
bool timeSynced = false;

// ---- AT command helpers (giu nguyen logic tu SmsSender.ino/ATBridge.ino) ----

bool sendATCommand(const char* command, const char* expected, unsigned long timeoutMs) {
  while (modemSerial.available()) modemSerial.read();  // xa rac cu trong buffer
  Serial.print("-> ");
  Serial.println(command);
  modemSerial.println(command);
  String resp = "";
  unsigned long start = millis();
  bool ok = false;
  while (millis() - start < timeoutMs) {
    while (modemSerial.available()) {
      char c = modemSerial.read();
      resp += c;
      Serial.write(c);
    }
    if (resp.indexOf(expected) != -1) {
      ok = true;
      break;
    }
    delay(1);  // nhuong CPU, tranh ESP32 tu reset do task watchdog khi cho lau
  }
  Serial.println(ok ? "\n[OK]" : "\n[TIMEOUT/FAIL]");
  return ok;
}

// Gui lenh AT+CMGL="REC UNREAD" va tra ve nguyen van phan hoi tho (nhieu
// dong, nhieu tin nhan) de tu parse tay ben duoi — AT+CMGL tra ve nhieu tin
// cung luc nen khong dung waitForResponse() don gian nhu cac lenh 1 dong.
String readSmsList(unsigned long timeoutMs) {
  while (modemSerial.available()) modemSerial.read();
  Serial.println("-> AT+CMGL=\"REC UNREAD\"");
  modemSerial.println("AT+CMGL=\"REC UNREAD\"");
  String resp = "";
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (modemSerial.available()) {
      char c = modemSerial.read();
      resp += c;
      Serial.write(c);
    }
    if (resp.indexOf("\r\nOK\r\n") != -1 || resp.indexOf("ERROR") != -1) break;
    delay(1);
  }
  return resp;
}

// Lay noi dung trong dau nhay kep thu n (dem tu 0) tren 1 dong, vd dung de
// tach so dien thoai (truong "oa") tu dong header "+CMGL: 1,"REC
// UNREAD","+84987654321",,"26/08/14,10:23:45+28"".
String extractQuotedField(const String& line, int n) {
  int idx = 0;
  int count = 0;
  while (true) {
    int q1 = line.indexOf('"', idx);
    if (q1 == -1) return "";
    int q2 = line.indexOf('"', q1 + 1);
    if (q2 == -1) return "";
    if (count == n) return line.substring(q1 + 1, q2);
    count++;
    idx = q2 + 1;
  }
}

// Thu parse 1 "o" (da trim khoang trang) dang "lat,lon" (lat trong [-90,90],
// lon trong [-180,180]). Tra ve false neu khong dung dinh dang - KHONG coi
// day la loi parse, don gian la 1 o "LOI" (GPS mat fix luc do, xem
// GpsSmsSender.ino/BufferedSlot) hoac rac khac.
bool parseLatLonToken(const String& token, double& lat, double& lon) {
  int comma = token.indexOf(',');
  if (comma == -1) return false;
  String num1 = token.substring(0, comma);
  String num2 = token.substring(comma + 1);
  num1.trim();
  num2.trim();
  if (num1.indexOf('.') == -1 || num2.indexOf('.') == -1) return false;
  for (unsigned int i = 0; i < num1.length(); i++) {
    char c = num1[i];
    if (!isDigit(c) && c != '.' && c != '-') return false;
  }
  for (unsigned int i = 0; i < num2.length(); i++) {
    char c = num2[i];
    if (!isDigit(c) && c != '.' && c != '-') return false;
  }
  double a = num1.toDouble();
  double b = num2.toDouble();
  if (a < -90.0 || a > 90.0 || b < -180.0 || b > 180.0) return false;
  lat = a;
  lon = b;
  return true;
}

struct ParsedSlot {
  bool hasCoord;
  double lat;
  double lon;
};

// Mang toan cuc chua cac o da tach - splitIntoSlots() ghi thang vao day thay
// vi nhan ParsedSlot* lam tham so. Ly do: Arduino IDE tu sinh prototype cho
// MOI ham va chen ngay sau #include cuoi cung o dau file - TRUOC ca cho
// "struct ParsedSlot" nay duoc dinh nghia - nen 1 ham co tham so kieu
// ParsedSlot* se gay loi bien dich "'ParsedSlot' has not been declared" (xem
// prototype tu sinh do dung kieu chua khai bao). Giu ham khong co tham so
// kieu struct rieng trong chu ky (giong cach GpsSmsSender.ino dung mang toan
// cuc pointSlots[] voi BufferedSlot) tranh han loi nay.
ParsedSlot parsedSlots[MAX_POINTS_PER_SMS];

// Tach body SMS thanh cac "o" cach nhau boi ';' (bo qua tien to "Toa do: "
// truoc dau ':' dau tien, neu co) - khop dinh dang GpsSmsSender.ino gui, xem
// buildLocationMessage() ben do. Ghi ket qua vao parsedSlots[] o tren. Moi o
// hoac la "lat,lon" (hasCoord=true) hoac la "LOI"/rac khac (hasCoord=false) -
// GIU NGUYEN vi tri cua o rong/loi (khong bo qua) de tinh dung tuoi tung
// diem: o cuoi cung (slotCount-1) la gan luc gui SMS nhat, o thu i cach do
// (slotCount-1-i)*SAMPLE_INTERVAL_SEC giay - xem processMessage(). Tra ve
// tong so o tach duoc (toi da maxSlots).
int splitIntoSlots(const String& body, int maxSlots) {
  int colon = body.indexOf(':');
  String rest = (colon == -1) ? body : body.substring(colon + 1);

  int count = 0;
  int p = 0;
  int len = rest.length();
  while (count < maxSlots) {
    int semi = rest.indexOf(';', p);
    String token = (semi == -1) ? rest.substring(p) : rest.substring(p, semi);
    token.trim();

    double lat, lon;
    parsedSlots[count].hasCoord = parseLatLonToken(token, lat, lon);
    if (parsedSlots[count].hasCoord) {
      parsedSlots[count].lat = lat;
      parsedSlots[count].lon = lon;
    }
    count++;

    if (semi == -1) break;
    p = semi + 1;
  }
  return count;
}

// Chi giu chu so trong so dien thoai (bo dau '+') — deviceId/toSign di qua
// query string HTTP, va dau '+' trong query string bi hieu la ky tu cach
// (chuan application/x-www-form-urlencoded, server.js dung URLSearchParams
// cung theo chuan nay) nen PHAI bo truoc khi dung, khong chi de trang trí.
String digitsOnly(const String& s) {
  String out = "";
  for (unsigned int i = 0; i < s.length(); i++) {
    if (isDigit(s[i])) out += s[i];
  }
  return out;
}

String nameForPhone(const String& digits) {
  for (unsigned int i = 0; i < sizeof(KNOWN_SENDERS) / sizeof(KNOWN_SENDERS[0]); i++) {
    if (digits.equals(KNOWN_SENDERS[i].phoneDigits)) return String(KNOWN_SENDERS[i].name);
  }
  return digits;
}

// ---- WiFi + gio NTP + HMAC + gui /api/pos (giu nguyen logic da kiem chung
// ---- trong GpsPosSender.ino) ----

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected! IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Khong ket noi duoc WiFi, se thu lai o vong lap sau.");
  }
}

void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Dong bo gio NTP");
  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 8 * 3600 * 2 && attempts < 40) {  // ~20s toi da
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    attempts++;
  }
  Serial.println();
  timeSynced = now >= 8 * 3600 * 2;
  if (!timeSynced) {
    Serial.println("CANH BAO: khong dong bo duoc gio NTP - server se tu choi request (sai ts).");
  }
}

String hmacSha256Hex(const char* key, const String& msg) {
  uint8_t hmacResult[32];
  const mbedtls_md_info_t* mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mdInfo, 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key, strlen(key));
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)msg.c_str(), msg.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);

  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", hmacResult[i]);
  hex[64] = '\0';
  return String(hex);
}

// (2026-09-08) ts gio duoc TRUYEN VAO tu processMessage() thay vi tu goi
// time(nullptr) o day - vi 1 SMS co the gop nhieu diem lay mau cach nhau toi
// SAMPLE_INTERVAL_SEC giay tren xe, nhung ca 6 diem do lai duoc xu ly/forward
// gan nhu cung luc (chua toi 1 giay) khi SMS toi ben nhan. Neu van goi
// time(nullptr) rieng cho tung diem o day, ca 6 diem se bi gan gan nhu CUNG 1
// ts (luc SMS duoc xu ly), sai lech so voi ts THAT su ma diem do duoc lay
// mau - xem processMessage().
void forwardPosition(const String& deviceId, const String& name, double lat, double lon, unsigned long ts) {
  if (WiFi.status() != WL_CONNECTED || !timeSynced) {
    Serial.println("Bo qua chuyen tiep: chua co WiFi/gio dong bo.");
    return;
  }
  String latStr = String(lat, 6);
  String lonStr = String(lon, 6);

  String toSign = String(ROOM_CODE) + "|" + deviceId + "|" + String(ts) + "|" + latStr + "|" + lonStr;
  String sig = hmacSha256Hex(API_SECRET, toSign);

  String url = String(SERVER_BASE) + "/api/pos?room=" + ROOM_CODE
               + "&deviceId=" + deviceId + "&name=" + name
               + "&lat=" + latStr + "&lon=" + lonStr
               + "&ts=" + String(ts) + "&sig=" + sig;

  HTTPClient http;
  bool began = url.startsWith("https://") ? http.begin(secureClient, url) : http.begin(url);
  if (!began) {
    Serial.println("HTTPClient begin() that bai (kiem tra SERVER_BASE).");
    return;
  }
  int code = http.GET();
  if (code > 0) {
    Serial.print("Chuyen tiep vi tri -> HTTP ");
    Serial.print(code);
    Serial.print(": ");
    Serial.println(http.getString());
  } else {
    Serial.print("Chuyen tiep vi tri that bai: ");
    Serial.println(http.errorToString(code));
  }
  http.end();
}

// ---- Xu ly danh sach SMS ----

void processMessage(int index, const String& phone, const String& body) {
  String digits = digitsOnly(phone);
  int slotCount = splitIntoSlots(body, MAX_POINTS_PER_SMS);
  String deviceId = "sms-" + digits;
  String name = nameForPhone(digits);
  unsigned long nowEpoch = (unsigned long)time(nullptr);

  // 1 SMS co the gop nhieu diem (xem GpsSmsSender.ino) - chuyen tiep TUNG
  // diem bang 1 request rieng, theo dung thu tu lay mau (cu truoc, moi sau).
  // O cuoi cung (slotCount-1) la gan luc gui SMS nhat; o thu i cach do
  // (slotCount-1-i)*SAMPLE_INTERVAL_SEC giay - dung de tinh ts THAT (gan
  // dung) cho tung diem thay vi gan chung 1 ts "bay gio" cho ca 6 diem.
  int forwarded = 0;
  for (int i = 0; i < slotCount; i++) {
    if (!parsedSlots[i].hasCoord) continue;  // o "LOI" - GPS mat fix luc do, bo qua
    unsigned long secondsAgo = (unsigned long)(slotCount - 1 - i) * SAMPLE_INTERVAL_SEC;
    unsigned long pointTs = (nowEpoch > secondsAgo) ? (nowEpoch - secondsAgo) : nowEpoch;
    Serial.print("SMS #");
    Serial.print(index);
    Serial.print(" tu ");
    Serial.print(phone);
    Serial.print(" -> toa do ");
    Serial.print(parsedSlots[i].lat, 6);
    Serial.print(",");
    Serial.print(parsedSlots[i].lon, 6);
    Serial.print(" (");
    Serial.print(secondsAgo);
    Serial.println("s truoc)");
    forwardPosition(deviceId, name, parsedSlots[i].lat, parsedSlots[i].lon, pointTs);
    forwarded++;
  }
  if (forwarded == 0) {
    Serial.print("SMS #");
    Serial.print(index);
    Serial.print(" tu ");
    Serial.print(phone);
    Serial.println(" khong co o nao chua toa do hop le (co the toan bo la \"LOI\" - GPS mat fix suot phut do), bo qua (van xoa de don SIM).");
  }

  // Xoa tin nhan da xu ly du thanh cong hay khong — SIM chi luu duoc so
  // luong gioi han (thuong ~50 tin), khong xoa se day storage va chan nhan
  // tin moi. Chap nhan mat 1 lan cap nhat neu WiFi/server dang loi luc do,
  // vi ben gui se gui lai sau toi da 5 phut (SEND_INTERVAL_MS trong
  // GpsSmsSender.ino) — don gian hon nhieu so voi co che retry/ACK day du.
  String delCmd = "AT+CMGD=" + String(index);
  sendATCommand(delCmd.c_str(), "OK", 3000);
}

void pollAndForwardSms() {
  sendATCommand("AT+CMGF=1", "OK", 2000);        // che do SMS dang text
  sendATCommand("AT+CSCS=\"GSM\"", "OK", 2000);  // bang ma ky tu

  String resp = readSmsList(8000);
  if (resp.indexOf("+CMGL:") == -1) return;  // khong co tin nhan chua doc nao

  int pos = 0;
  while (true) {
    int hdrStart = resp.indexOf("+CMGL:", pos);
    if (hdrStart == -1) break;
    int hdrEnd = resp.indexOf('\n', hdrStart);
    if (hdrEnd == -1) break;
    String header = resp.substring(hdrStart, hdrEnd);

    int nextHdr = resp.indexOf("+CMGL:", hdrEnd);
    int okPos = resp.indexOf("\r\nOK", hdrEnd);
    int bodyEnd;
    if (nextHdr != -1 && (okPos == -1 || nextHdr < okPos)) {
      bodyEnd = nextHdr;
    } else if (okPos != -1) {
      bodyEnd = okPos;
    } else {
      bodyEnd = resp.length();
    }
    String body = resp.substring(hdrEnd + 1, bodyEnd);
    body.trim();

    int commaAfterIdx = header.indexOf(',');
    int index = header.substring(header.indexOf(':') + 1, commaAfterIdx).toInt();
    String phone = extractQuotedField(header, 1);  // 0=stat, 1=oa (so goi den)

    processMessage(index, phone, body);

    pos = bodyEnd;
  }
}

void setup() {
  Serial.begin(115200);
  modemSerial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(3000);  // cho module SIM boot xong sau khi ESP32 vua duoc cap nguon/reset

  bool alive = false;
  unsigned long start = millis();
  while (millis() - start < 10000) {  // cho toi da 10s de module bat song AT
    if (sendATCommand("AT", "OK", 1000)) {
      alive = true;
      break;
    }
    delay(500);
  }
  if (!alive) {
    Serial.println("Khong lien lac duoc voi module SIM. Kiem tra day noi, nguon.");
  }
  sendATCommand("AT+CPIN?", "READY", 3000);

  secureClient.setCACertBundle(root_ca_bundle, root_ca_bundle_len);
  connectWiFi();
  syncTime();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!timeSynced) syncTime();

  if (millis() - lastPollAt >= SMS_POLL_INTERVAL_MS) {
    lastPollAt = millis();
    pollAndForwardSms();
  }
}
