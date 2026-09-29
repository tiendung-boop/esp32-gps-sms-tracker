// Gui SMS toi mot so dien thoai bang module SIM 4G A7680C (SIMCom), dieu
// khien qua tap lenh AT tren UART — khong can them thu vien ngoai (khac voi
// TinyGPSPlus/WiFiClientSecure o cac sketch khac trong repo, o day tu parse
// phan hoi AT la du va gon hon).
//
// Wiring (A7680C -> ESP32):
//   VBAT/VCC -> nguon ngoai rieng 3.4-4.2V, chiu duoc dong DINH ~2A luc phat
//               song (KHONG lay tu chan 3.3V/5V/VIN cua ESP32 — sut ap se
//               lam module reset lien tuc khi gui SMS/goi).
//   GND      -> GND chung voi ESP32.
//   TXD      -> GPIO26 (ESP32 RX1)
//   RXD      -> GPIO27 (ESP32 TX1)
//   PWRKEY / RST -> KHONG noi GPIO nao ca (da xac nhan bang ATBridge.ino:
//               module nay tu bat nguon va tra loi AT ngay ma khong can
//               dieu khien chan nao). Neu ban co day noi tu ESP32 sang chan
//               RST/PWRKEY cua module (vi du GPIO4), RUT day do ra — de no
//               troi (khong noi GPIO nao) — vi mot so board dung chan do lam
//               RST that, ep GPIO xuong/len sai chieu se giu module trong
//               trang thai reset vinh vien (dung trieu chung "AT" timeout
//               lien tuc ma khong tra loi gi ca).
//
// SIM: dung SIM khong khoa PIN (hoac da AT+CPIN="xxxx" truoc). So dien thoai
// dich (dinh dang quoc te +84...) de trong secrets.h, khong hardcode o day.

#include "secrets.h"

#define MODEM_RX_PIN 26
#define MODEM_TX_PIN 27
#define MODEM_BAUD 115200

// "+CMS ERROR: Network timeout" nhieu khi la nghen mang tam thoi, khong
// phai loi vinh vien -> thu lai nhieu lan cach nhau vai giay thay vi bo
// cuoc ngay lan dau.
#define SMS_MAX_ATTEMPTS 8
#define SMS_RETRY_DELAY_MS 8000

HardwareSerial modemSerial(2);

const char* SMS_MESSAGE = "Xin chao tu ESP32 + A7680C!";

// Doc phan hoi tu module trong toi da timeoutMs, in ra Serial Monitor de
// debug, tra ve true ngay khi thay chuoi "expected" xuat hien.
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

// Gui 1 lan qua duong SMS hien dang cau hinh (AT+CGSMS). Tra ve false neu
// khong thay dau nhac '>' hoac khong thay "+CMGS" xac nhan (vd bao loi
// "+CMS ERROR: ..." trong response, in ra Serial boi waitForResponse).
bool trySendSmsOnce(const char* phoneNumber, const char* message) {
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

bool sendSms(const char* phoneNumber, const char* message) {
  if (!sendATCommand("AT+CPIN?", "READY", 3000)) {
    Serial.println("SIM chua san sang: kiem tra da lap SIM dung chieu, khong khoa PIN.");
    return false;
  }
  sendATCommand("AT+CMGF=1", "OK", 2000);        // che do SMS dang text
  sendATCommand("AT+CSCS=\"GSM\"", "OK", 2000);  // bang ma ky tu

  // (2026-08-14) Da dieu tra ky "+CMS ERROR: Network timeout" lap lai o day:
  // CSQ/CREG/CEREG/COPS deu binh thuong, CSCA (SMSC) dung y het so may dien
  // thoai thuong (cung SIM) dang dung de gui SMS thanh cong ngay lap tuc ->
  // KHONG PHAI do CGSMS chon sai duong CS/PS, khong phai do SMSC, khong
  // phai do mang/song. Nguyen nhan thuc su: nha mang (Viettel) tam khoa gui
  // SMS tren SIM cho toi khi thue bao xac thuc "doi may" (SIM chuyen tu
  // dien thoai sang module A7680C = doi IMEI) qua app/tong dai nha mang -
  // xac thuc xong la gui duoc binh thuong, khong lien quan code o day.
  // Doan retry PS/CS ben duoi giu lai nhu 1 lop du phong hop ly cho cac loi
  // mang that/tam thoi trong tuong lai, nhung KHONG phai fix cho loi tren.
  for (int attempt = 1; attempt <= SMS_MAX_ATTEMPTS; attempt++) {
    Serial.print("=== Thu gui SMS lan ");
    Serial.print(attempt);
    Serial.print("/");
    Serial.print(SMS_MAX_ATTEMPTS);
    Serial.println(" ===");

    sendATCommand("AT+CGSMS=0", "OK", 2000);  // 0 = GPRS/PS
    if (trySendSmsOnce(phoneNumber, message)) return true;

    Serial.println("Gui qua PS/GPRS that bai, thu lai qua CS...");
    sendATCommand("AT+CGSMS=1", "OK", 2000);  // 1 = CS
    if (trySendSmsOnce(phoneNumber, message)) return true;

    if (attempt < SMS_MAX_ATTEMPTS) {
      Serial.print("Ca hai duong deu that bai, cho ");
      Serial.print(SMS_RETRY_DELAY_MS / 1000);
      Serial.println("s roi thu lai...");
      delay(SMS_RETRY_DELAY_MS);
    }
  }
  Serial.println("Da thu het so lan, khong gui duoc SMS.");
  return false;
}

void setup() {
  Serial.begin(115200);
  modemSerial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(3000);  // cho module boot xong sau khi ESP32 vua duoc cap nguon/reset

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
    Serial.println("Khong lien lac duoc voi module. Kiem tra day noi, nguon.");
    return;
  }

  // Ca PS lan CS deu bao "+CMS ERROR: Network timeout" nhieu lan lien tiep
  // -> khong phai nghen mang tam thoi, can xem cac thong so mang/SMSC nay
  // de biet dung nguyen nhan (dac biet AT+CSCA?: SMSC rong/sai la nguyen
  // nhan pho bien nhat gay dung loi nay tren moi lan gui).
  Serial.println("=== Chan doan mang/SIM ===");
  sendATCommand("AT+CSCA?", "OK", 3000);   // so SMSC dang cau hinh
  sendATCommand("AT+CSQ", "OK", 3000);     // cuong do song
  sendATCommand("AT+CREG?", "OK", 3000);   // dang ky mang 2G/3G
  sendATCommand("AT+CEREG?", "OK", 3000);  // dang ky mang LTE
  sendATCommand("AT+COPS?", "OK", 5000);   // nha mang dang bat duoc
  Serial.println("=== Het chan doan, bat dau gui SMS ===");

  sendSms(PHONE_NUMBER, SMS_MESSAGE);
}

void loop() {
  // Demo chi gui 1 lan trong setup(). Muon gui lai: nhan nut RESET tren ESP32,
  // hoac tu goi lai sendSms(PHONE_NUMBER, "...") theo dieu kien cua ban (vi du
  // ket hop voi GPS de gui vi tri qua SMS khi mat WiFi).
}
