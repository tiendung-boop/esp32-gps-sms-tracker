// Doc vi tri tu GPS (NEO-6M) va dinh ky gui ve so dien thoai chi dinh bang
// SMS qua module SIM 4G A7680C (SIMCom), dieu khien qua tap lenh AT — ket
// hop GpsPosSender.ino (doc GPS) + SmsSender.ino (gui SMS) trong cung 1
// sketch. Khong dung WiFi/HTTP: day la duong du phong khi khong co WiFi,
// chi can co song di dong (xem ghi chu trong SmsSender.ino).
//
// Wiring — CAN 2 UART CUNG LUC (khac voi GpsPosSender/SmsSender vi ca hai
// deu tung dung rieng le GPIO16/17). (2026-09-04) Da doi cho: GPS ve lai
// GPIO16/17, SIM chuyen sang GPIO26/27 (nguoc lai voi ban truoc do):
//   NEO-6M (GPS)      -> Serial2 tren GPIO16/17:
//     VCC -> 3.3V (hoac 5V, xem chu in tren module)
//     GND -> GND
//     TX  -> GPIO16 (ESP32 RX2)
//     RX  -> GPIO17 (ESP32 TX2)
//   A7680C (SIM 4G)   -> Serial1 tren GPIO26/27:
//     VBAT/VCC -> nguon ngoai rieng 3.4-4.2V, chiu dong dinh ~2A (KHONG lay
//                 tu chan 3.3V/5V/VIN cua ESP32).
//     GND      -> GND chung voi ESP32.
//     TXD      -> GPIO26 (ESP32 RX1)
//     RXD      -> GPIO27 (ESP32 TX1)
//     PWRKEY/RST -> khong noi GPIO nao (de troi), xem ATBridge.ino.
//
// Thu vien can: TinyGPSPlus (Library Manager -> tim "TinyGPSPlus").
//
// SIM: dung SIM khong khoa PIN. So dien thoai nhan tin de trong secrets.h.

#include <TinyGPSPlus.h>
#include "secrets.h"

#define GPS_RX_PIN 16
#define GPS_TX_PIN 17
#define GPS_BAUD 9600

#define MODEM_RX_PIN 26
#define MODEM_TX_PIN 27
#define MODEM_BAUD 115200

// Chu ky gui SMS vi tri. SMS ton tien/cuoc — dung chinh sua qua ngan.
// (2026-09-08) Rut tu 5 phut xuong 1 phut, nhung KHONG gui rieng 1 toa do
// hien tai moi lan nhu truoc: giua 2 lan gui, GPS duoc lay mau vao bo nho
// RAM (pointSlots, xem duoi) moi SAMPLE_INTERVAL_MS, roi khi den han
// SEND_INTERVAL_MS thi gui GOP het cac diem da tich luy trong 1 tin SMS duy
// nhat (cach nhau boi ';', xem buildLocationMessage()) - vua giu duoc vet di
// chuyen chi tiet hon trong khoang 1 phut do, vua khong ton them SMS (van 1
// tin/phut). Bo nho chi la RAM (khong phai flash): du lieu chi can song sot
// toi lan gui ke tiep (1 phut), khong can ben vung qua reset nguon.
#define SEND_INTERVAL_MS (1UL * 60UL * 1000UL)  // 1 phut

// Khoang cach giua 2 lan lay mau GPS vao bo nho RAM (pointSlots). Chon 10s
// de vua du day 1 tin SMS gop (~6 diem/phut, ~139 ky tu voi toa do Da Nang -
// van trong gioi han 160 ky tu cua 1 doan SMS GSM-7, khong can chia doan),
// vua du thua de thay ro huong di chuyen tren ban do thay vi chi 1 diem/phut.
// PHAI khop voi SAMPLE_INTERVAL_SEC ben SmsReceiver.ino/SmsReceiverUSB*.ino
// (tinh bang giay) - ben nhan dua vao gia tri nay de tinh tuoi tung diem, xem
// giai thich o BufferedSlot/buildLocationMessage() ben duoi.
#define SAMPLE_INTERVAL_MS (10UL * 1000UL)  // 10 giay

// So o CO DINH trong 1 chu ky gui - luon = SEND_INTERVAL_MS / SAMPLE_INTERVAL_MS
// (hien 60s/10s = 6), tinh tu 2 hang so tren de khong bao gio bi lech neu sau
// nay doi 1 trong 2 gia tri do.
#define SLOTS_PER_SEND ((int)(SEND_INTERVAL_MS / SAMPLE_INTERVAL_MS))

// Chu ky bao trang thai GPS len Serial Monitor (chi de debug tren laptop,
// khong lien quan gi den SEND_INTERVAL_MS/SMS).
#define GPS_STATUS_INTERVAL_MS 2000UL  // 2 giay

// true: echo thang cau NMEA tho nhan tu module GPS ra Serial Monitor, de
// nhin tan mat module dang gui gi — phan biet duoc "co cau NMEA hop le
// nhung 0 ve tinh" (binh thuong, dang do song, cu doi/dua ra ngoai troi)
// voi "toan ky tu rac" (sai GPS_BAUD) hoac "khong co gi" (day/anten hong).
// Xong debug thi doi lai false cho do rac Serial Monitor.
#define GPS_RAW_DEBUG true

HardwareSerial gpsSerial(2);
HardwareSerial modemSerial(1);
TinyGPSPlus gps;
unsigned long lastActionAt = 0;
unsigned long lastGpsStatusAt = 0;
unsigned long lastSampleAt = 0;
unsigned long gpsBytesSinceStatus = 0;  // dem byte tho tu module GPS trong 1 chu ky status
bool firstFixSent = false;  // true sau khi da gui SMS ngay lan dau vua co fix

// Bo nho RAM tam giu cac diem GPS lay mau duoc giua 2 lan gui SMS - xem giai
// thich o SEND_INTERVAL_MS/SAMPLE_INTERVAL_MS phia tren.
//
// (2026-09-08) Moi o ung voi DUNG 1 lan lay mau theo dinh ky (tick) trong 1
// phut, KE CA khi khong bat duoc fix GPS luc do (hasFix=false, "o loi") -
// khac ban truoc (chi luu diem CO fix, lang le bo qua khi mat fix, nen so o
// dung co the it hon SLOTS_PER_SEND ma khong ro vi sao). Giu ca o loi la de
// ben nhan (SmsReceiver*.ino) biet CHINH XAC o thu may (trong SLOTS_PER_SEND
// o) ung voi tick nao, tu do tinh dung tuoi (thoi gian) cua tung diem toa do
// - neu chi gui cac diem CO fix (bo qua lang le cac o mat fix), ben nhan se
// khong biet duoc cac o bi mat nam o dau trong day, tinh sai khoang cach
// thoi gian giua cac diem con lai.
struct BufferedSlot {
  bool hasFix;
  double lat;
  double lon;
};
BufferedSlot pointSlots[SLOTS_PER_SEND];
int slotsFilled = 0;     // so o da lay mau trong chu ky hien tai (0..SLOTS_PER_SEND)
int validSlotCount = 0;  // trong so o da lay mau, bao nhieu o CO fix (de biet co gi de gui SMS khong)

// Lay mau vao o TIEP THEO cua pointSlots: neu dang co fix GPS hop le thi ghi
// toa do, khong thi danh dau o do la "loi" (van chiem 1 o, KHONG bo qua - xem
// giai thich o BufferedSlot). Goi moi SAMPLE_INTERVAL_MS trong loop().
void sampleGpsPoint() {
  if (slotsFilled >= SLOTS_PER_SEND) return;  // da du o cho chu ky nay, cho den han gui
  BufferedSlot& slot = pointSlots[slotsFilled];
  slot.hasFix = gps.location.isValid();
  if (slot.hasFix) {
    slot.lat = gps.location.lat();
    slot.lon = gps.location.lng();
    validSlotCount++;
  }
  slotsFilled++;
}

// ---- Phan gui AT command / SMS, giu nguyen logic da kiem chung trong
// ---- SmsSender.ino ----

bool waitForResponse(const char* expected, unsigned long timeoutMs) {
  String response = "";
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (modemSerial.available()) {
      char c = modemSerial.read();
      response += c;
      Serial.write(c);
    }
    if (response.indexOf(expected) != -1) {
      return true;
    }
    delay(1);  // nhuong CPU, tranh ESP32 tu reset do task watchdog khi cho lau
  }
  return false;
}

bool sendATCommand(const char* command, const char* expected, unsigned long timeoutMs) {
  while (modemSerial.available()) modemSerial.read();  // xa rac cu trong buffer
  Serial.print("-> ");
  Serial.println(command);
  modemSerial.println(command);
  bool ok = waitForResponse(expected, timeoutMs);
  Serial.println(ok ? "\n[OK]" : "\n[TIMEOUT/FAIL]");
  return ok;
}

bool sendSms(const char* phoneNumber, const String& message) {
  if (!sendATCommand("AT+CPIN?", "READY", 3000)) {
    Serial.println("SIM chua san sang: kiem tra da lap SIM dung chieu, khong khoa PIN.");
    return false;
  }
  sendATCommand("AT+CMGF=1", "OK", 2000);        // che do SMS dang text
  sendATCommand("AT+CSCS=\"GSM\"", "OK", 2000);  // bang ma ky tu
  // (2026-08-14) Neu gap "+CMS ERROR: Network timeout" o day du CSQ/CREG/
  // CEREG/CSCA deu binh thuong VA dien thoai thuong voi cung SIM gui SMS
  // duoc ngay: KHONG PHAI do CGSMS chon sai duong CS/PS. Nguyen nhan thuc
  // te gap phai la nha mang (Viettel) khoa gui SMS tren SIM cho toi khi
  // xac thuc "doi may" (SIM chuyen sang thiet bi/IMEI la la module nay) -
  // vao app/tong dai nha mang xac nhan doi may la het, khong can sua code.
  // Chi tiet dieu tra + logic retry PS/CS xem sendSms() trong SmsSender.ino.
  sendATCommand("AT+CGSMS=0", "OK", 2000);

  while (modemSerial.available()) modemSerial.read();
  String cmd = String("AT+CMGS=\"") + phoneNumber + "\"";
  Serial.print("-> ");
  Serial.println(cmd);
  modemSerial.println(cmd);
  if (!waitForResponse(">", 5000)) {
    Serial.println("Module khong tra ve dau nhac '>' de nhap noi dung tin nhan.");
    return false;
  }

  modemSerial.print(message);
  modemSerial.write(0x1A);  // Ctrl+Z ket thuc va gui tin nhan
  bool sent = waitForResponse("+CMGS", 15000);
  Serial.println(sent ? "\n[Da gui SMS]" : "\n[Gui SMS that bai]");
  return sent;
}

// ---- Phan GPS ----

// Ghep noi dung SMS tu pointSlots: moi o cach nhau boi ';', o CO fix ghi
// "lat,lon", o KHONG co fix ghi "LOI" (xem BufferedSlot) - vd "Toa do:
// 16.047238,108.219700;LOI;16.047500,108.220100". Ben nhan dua vao VI TRI
// cua tung o trong danh sach (khong phai chi dem so o co toa do) de tinh
// dung tuoi tung diem - xem splitIntoSlots() trong SmsReceiver*.ino.
//
// (2026-09-08) LUON lap du SLOTS_PER_SEND o (KHONG chi lap toi slotsFilled)
// - neu den han gui ma chua kip lay du 6 o (vd vong loop() vua bi block vai
// giay trong luc cho phan hoi AT+CMGS cua lan gui truoc, an bot thoi gian
// lay mau cua chu ky nay), cac o CHUA KIP lay mau cung duoc tinh la "LOI"
// giong o mat fix, KHONG bo trong/rut ngan danh sach. Dam bao SMS gui di
// LUON co dung 6 o co dinh, de vi tri cua o trong danh sach (dung de ben
// nhan suy ra tuoi tung diem) khong bao gio bi sai lech vi thieu o.
String buildLocationMessage() {
  String msg = "Toa do: ";
  for (int i = 0; i < SLOTS_PER_SEND; i++) {
    if (i > 0) msg += ";";
    if (i < slotsFilled && pointSlots[i].hasFix) {
      msg += String(pointSlots[i].lat, 6);
      msg += ",";
      msg += String(pointSlots[i].lon, 6);
    } else {
      msg += "LOI";
    }
  }
  return msg;
}

void sendLocationSms(const char* logPrefix) {
  String msg = buildLocationMessage();
  Serial.print(logPrefix);
  Serial.println(msg);
  sendSms(PHONE_NUMBER, msg);
}

// In trang thai GPS len Serial Monitor (USB, laptop) de theo doi truc tiep
// luc dat module o cho moi / cho bat song: da co fix chua, dang dung bao
// nhieu ve tinh de tinh vi tri, va HDOP (do chinh xac — cang thap cang tot,
// duoi ~2 la tot, tren ~5 la kem). TinyGPSPlus chi biet SO LUONG ve tinh dang
// dung (tu cau NMEA GPGGA), khong biet ID/tin hieu tung ve tinh rieng le (can
// parse them cau GPGSV, khong lam trong sketch nay).
//
// "Chua co fix, 0 ve tinh" tu no KHONG phan biet duoc "module co ket noi
// nhung dang do song" voi "module khong ket noi/khong nhan duoc gi ca" (day
// noi sai, sai baud, mat nguon...) — module GPS that van lien tuc phat cau
// NMEA (vd $GPGGA rong) moi giay du chua co fix. Vi vay them dong "Ket noi
// module" dua tren SO BYTE THO nhan duoc tren gpsSerial trong chu ky nay
// (gpsBytesSinceStatus, dem o loop() truoc khi dua vao gps.encode()):
//   >0 byte  -> module co gui du lieu (co ket noi UART that su).
//   0 byte   -> khong nhan duoc gi -> kiem tra day TX/RX co dao nguoc GPS TX
//               phai noi GPIO26 (RX cua ESP32) khong, GND chung, nguon module,
//               va GPS_BAUD (mac dinh NEO-6M la 9600).
// Neu co byte nhung sentencesWithFix()/charsProcessed() khong tang qua nhieu
// chu ky lien tiep (vd toan ky tu la rac), nhieu kha nang sai GPS_BAUD.
void printGpsStatus() {
  bool linkAlive = gpsBytesSinceStatus > 0;
  Serial.print("[GPS] Ket noi module: ");
  Serial.print(linkAlive ? "CO (dang nhan du lieu)" : "KHONG nhan duoc du lieu - kiem tra day/nguon/baud!");
  Serial.print(" (");
  Serial.print(gpsBytesSinceStatus);
  Serial.print(" byte/");
  Serial.print(GPS_STATUS_INTERVAL_MS / 1000);
  Serial.println("s)");
  gpsBytesSinceStatus = 0;

  Serial.print("[GPS] ");
  Serial.print(gps.location.isValid() ? "FIX OK" : "Chua co fix");
  Serial.print(" | ve tinh dang dung=");
  Serial.print(gps.satellites.isValid() ? gps.satellites.value() : 0);
  Serial.print(" | HDOP=");
  if (gps.hdop.isValid()) {
    Serial.print(gps.hdop.value() / 100.0, 1);
  } else {
    Serial.print("N/A");
  }
  if (gps.location.isValid()) {
    Serial.print(" | lat=");
    Serial.print(gps.location.lat(), 6);
    Serial.print(" lon=");
    Serial.print(gps.location.lng(), 6);
    Serial.print(" | tuoi fix=");
    Serial.print(gps.location.age());
    Serial.print("ms");
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
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
}

void loop() {
  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    gpsBytesSinceStatus++;
    if (GPS_RAW_DEBUG) Serial.write(c);  // xem thang cau NMEA tho, xem huong dan dau file
    gps.encode(c);
  }

  if (millis() - lastGpsStatusAt >= GPS_STATUS_INTERVAL_MS) {
    lastGpsStatusAt = millis();
    printGpsStatus();
  }

  if (millis() - lastSampleAt >= SAMPLE_INTERVAL_MS) {
    lastSampleAt = millis();
    sampleGpsPoint();
  }

  if (!firstFixSent) {
    // Lan dau tien: gui ngay khi vua bat duoc fix, khong doi het SEND_INTERVAL_MS.
    if (gps.location.isValid()) {
      if (slotsFilled == 0) sampleGpsPoint();  // dam bao co it nhat 1 o de gui
      sendLocationSms("Gui SMS vi tri (lan dau, vua bat duoc fix): ");
      slotsFilled = 0;
      validSlotCount = 0;
      firstFixSent = true;
      lastActionAt = millis();   // moc thoi gian cho chu ky 1 phut ke tiep
      lastSampleAt = millis();
    }
  } else if (millis() - lastActionAt >= SEND_INTERVAL_MS) {
    lastActionAt = millis();
    if (validSlotCount > 0) {
      sendLocationSms("Gui SMS vi tri: ");
    } else {
      Serial.println("Bo qua gui SMS lan nay: khong co o nao bat duoc fix GPS trong 1 phut qua (xem [GPS] status o tren).");
    }
    // Luon reset bo dem sau moi chu ky gui (du gui thanh cong hay khong) -
    // giu don gian, khong retry/gui lai diem cu, giong tinh than cac phan
    // khac cua sketch nay (xem ghi chu AT+CGSMS o sendSms()).
    slotsFilled = 0;
    validSlotCount = 0;
  }
}
