// Bien the KHONG WIFI/INTERNET cua SmsReceiver.ino - doc SMS toa do gui toi
// tu GpsSmsSender.ino (dinh dang "Toa do: lat1,lon1;lat2,lon2;..." - 1 SMS
// co the gop nhieu diem lay mau trong 1 phut) qua module SIM 4G A7680C, roi
// in ra CONG USB (Serial thuong, toc do 115200) duoi dang 1 dong/diem de may
// tinh doc - KHONG chuyen tiep qua WiFi/HTTP nhu SmsReceiver.ino goc.
//
// Dung khi may tinh xem ban do (doc-sms-qua-esp32-usb.js +
// nhan-sms-tu-dong-tool.html trong lam_viec/) khong co internet - ESP32 nay
// cam thang vao may tinh do bang day USB, script tren may doc dong du lieu
// qua cong COM roi day len trang ban do offline. Xem HUONG_DAN.txt muc
// "NHAN SMS TU DONG (khong can internet)".
//
// Wiring (A7680C -> ESP32) - GIU NGUYEN nhu SmsReceiver.ino/ATBridge.ino:
//   VBAT/VCC -> nguon ngoai rieng 3.4-4.2V, chiu dong dinh ~2A (KHONG lay tu
//               chan 3.3V/5V/VIN cua ESP32).
//   GND      -> GND chung voi ESP32.
//   TXD      -> GPIO26 (ESP32 RX1)
//   RXD      -> GPIO27 (ESP32 TX1)
//   PWRKEY/RST -> khong noi GPIO nao (de troi), xem ATBridge.ino.
//
// Thu vien can: khong can cai them qua Library Manager - tu parse phan hoi AT
// (giong SmsReceiver.ino/SmsSender.ino), KHONG dung WiFi.h/HTTPClient nen
// KHONG can secrets.h/root_ca_bundle.h nhu ban WiFi.
//
// Dinh dang dong in ra USB khi doc duoc toa do hop le (1 dong/diem, ket thuc
// \n - neu 1 SMS gop nhieu diem thi in nhieu dong lien tiep, theo dung thu
// tu lay mau cu truoc moi sau):
//   TOADO|<deviceId>|<ten>|<lat>|<lon>|<secondsAgo>
// vd: TOADO|sms-84987654321|Xe may 1|16.047238|108.219700|20
// (secondsAgo = so giay TRUOC luc SMS duoc gui ma diem nay thuc su duoc GPS
// lay mau tren xe, dua vao vi tri cua diem do trong danh sach nhieu diem cua
// 1 SMS - xem SAMPLE_INTERVAL_SEC/splitIntoSlots() ben duoi va
// buildLocationMessage() trong GpsSmsSender.ino. doc-sms-qua-esp32-usb.js
// dung gia tri nay de tinh ts that cho tung diem thay vi gan chung "bay gio"
// cho ca 6 diem. Dau '|' lam ky tu phan cach - KHONG dat '|' trong ten o
// KNOWN_SENDERS ben duoi vi se lam sai dinh dang dong nay.)

#include <string.h>

#define MODEM_RX_PIN 26
#define MODEM_TX_PIN 27
#define MODEM_BAUD 115200

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

// Ten hien thi (tuy chon) cho tung so may gui toi - xem giai thich trong
// SmsReceiver.ino. So dien thoai ghi KHONG dau '+', khong dau cach. KHONG
// dung ky tu '|' trong ten (xem dinh dang dong o tren).
struct KnownSender {
  const char* phoneDigits;
  const char* name;
};
KnownSender KNOWN_SENDERS[] = {
  // { "84987654321", "Xe may 1" },
};

HardwareSerial modemSerial(2);
unsigned long lastPollAt = 0;

// ---- AT command helpers (giu nguyen logic tu SmsReceiver.ino/SmsSender.ino/ATBridge.ino) ----

bool sendATCommand(const char* command, const char* expected, unsigned long timeoutMs) {
  while (modemSerial.available()) modemSerial.read();  // xa rac cu trong buffer
  modemSerial.println(command);
  String resp = "";
  unsigned long start = millis();
  bool ok = false;
  while (millis() - start < timeoutMs) {
    while (modemSerial.available()) {
      char c = modemSerial.read();
      resp += c;
    }
    if (resp.indexOf(expected) != -1) {
      ok = true;
      break;
    }
    delay(1);  // nhuong CPU, tranh ESP32 tu reset do task watchdog khi cho lau
  }
  return ok;
}

// Gui lenh AT+CMGL="REC UNREAD" va tra ve nguyen van phan hoi tho (nhieu
// dong, nhieu tin nhan) de tu parse tay ben duoi.
String readSmsList(unsigned long timeoutMs) {
  while (modemSerial.available()) modemSerial.read();
  modemSerial.println("AT+CMGL=\"REC UNREAD\"");
  String resp = "";
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (modemSerial.available()) {
      char c = modemSerial.read();
      resp += c;
    }
    if (resp.indexOf("\r\nOK\r\n") != -1 || resp.indexOf("ERROR") != -1) break;
    delay(1);
  }
  return resp;
}

// Lay noi dung trong dau nhay kep thu n (dem tu 0) tren 1 dong.
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

// ---- Xu ly danh sach SMS ----

void processMessage(int index, const String& phone, const String& body) {
  String digits = digitsOnly(phone);
  int slotCount = splitIntoSlots(body, MAX_POINTS_PER_SMS);
  if (slotCount > 0) {
    String deviceId = "sms-" + digits;
    String name = nameForPhone(digits);
    // 1 SMS co the gop nhieu diem (xem GpsSmsSender.ino) - in 1 dong TOADO|
    // cho MOI diem, theo dung thu tu lay mau (cu truoc, moi sau). Bo qua cac
    // o "LOI" (GPS mat fix luc do). O cuoi cung (slotCount-1) la gan luc gui
    // SMS nhat; o thu i cach do (slotCount-1-i)*SAMPLE_INTERVAL_SEC giay -
    // gui kem secondsAgo de script ben may tinh tinh dung ts that cho tung
    // diem. Dong duy nhat may tinh (doc-sms-qua-esp32-usb.js) can doc - xem
    // dinh dang o dau file. Khong xen log/debug nao khac vao giua de tranh
    // script ben kia parse nham.
    for (int i = 0; i < slotCount; i++) {
      if (!parsedSlots[i].hasCoord) continue;
      int secondsAgo = (slotCount - 1 - i) * SAMPLE_INTERVAL_SEC;
      Serial.print("TOADO|");
      Serial.print(deviceId);
      Serial.print("|");
      Serial.print(name);
      Serial.print("|");
      Serial.print(parsedSlots[i].lat, 6);
      Serial.print("|");
      Serial.print(parsedSlots[i].lon, 6);
      Serial.print("|");
      Serial.println(secondsAgo);
    }
  }

  // Xoa tin nhan da xu ly du thanh cong hay khong - xem ly do trong
  // SmsReceiver.ino (SIM chi luu duoc so luong tin gioi han).
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

  unsigned long start = millis();
  while (millis() - start < 10000) {  // cho toi da 10s de module bat song AT
    if (sendATCommand("AT", "OK", 1000)) break;
    delay(500);
  }
  sendATCommand("AT+CPIN?", "READY", 3000);
}

void loop() {
  if (millis() - lastPollAt >= SMS_POLL_INTERVAL_MS) {
    lastPollAt = millis();
    pollAndForwardSms();
  }
}
